#define _XOPEN_SOURCE 700

#include "writer.h"
#include "reader.h" /* For zu_load_central_directory */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <fnmatch.h>

#include "crc32.h"
#include "fileio.h"
#include "zip_headers.h"
#include "zipcrypto.h"
#include "bzip2_shim.h"

#define ZU_EXTRA_ZIP64 0x0001
#define ZU_IO_CHUNK (64 * 1024)

/* -------------------------------------------------------------------------
 * Internal Structures
 * ------------------------------------------------------------------------- */

typedef struct {
    char* name;
    uint32_t crc32;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t lho_offset;
    uint16_t method;
    uint16_t flags;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t ext_attr;     /* External file attributes */
    uint16_t version_made; /* Version made by */
    bool zip64;
    char* comment;
    uint16_t comment_len;
} zu_writer_entry;

typedef struct {
    zu_writer_entry* items;
    size_t len;
    size_t cap;
} zu_entry_list;

typedef struct {
    struct stat st;
    bool size_known;
    bool is_stdin;
} zu_input_info;

/* -------------------------------------------------------------------------
 * Helper Functions
 * ------------------------------------------------------------------------- */

static void free_entries(zu_entry_list* list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->len; ++i) {
        free(list->items[i].name);
        free(list->items[i].comment);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int ensure_entry_capacity(zu_entry_list* list) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        zu_writer_entry* new_items = realloc(list->items, new_cap * sizeof(zu_writer_entry));
        if (!new_items) {
            return ZU_STATUS_OOM;
        }
        list->items = new_items;
        list->cap = new_cap;
    }
    return ZU_STATUS_OK;
}

static bool path_is_stdin(const char* path) {
    return path && strcmp(path, "-") == 0;
}

static bool should_store_by_suffix(const ZContext* ctx, const char* path) {
    if (!ctx || ctx->no_compress_suffixes.len == 0 || !path) {
        return false;
    }
    const char* dot = strrchr(path, '.');
    if (!dot || *(dot + 1) == '\0') {
        return false;
    }
    const char* ext = dot + 1;
    for (size_t i = 0; i < ctx->no_compress_suffixes.len; ++i) {
        const char* suf = ctx->no_compress_suffixes.items[i];
        if (!suf || *suf == '\0')
            continue;
        const char* cmp = suf;
        if (*cmp == '.')
            cmp++;
        if (strcasecmp(ext, cmp) == 0) {
            return true;
        }
    }
    return false;
}

static const char* basename_component(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash && slash[1] != '\0') {
        return slash + 1;
    }
    return path;
}

static int translate_buffer(ZContext* ctx, const uint8_t* in, size_t in_len, uint8_t** out_buf, size_t* out_len, bool* prev_cr) {
    if (!ctx || ctx->line_mode == ZU_LINE_NONE) {
        *out_buf = (uint8_t*)in;
        *out_len = in_len;
        return ZU_STATUS_OK;
    }

    size_t cap = ctx->line_mode == ZU_LINE_LF_TO_CRLF ? (in_len * 2 + 1) : (in_len + 1);
    uint8_t* out = malloc(cap);
    if (!out)
        return ZU_STATUS_OOM;

    size_t o = 0;
    for (size_t i = 0; i < in_len; ++i) {
        uint8_t b = in[i];
        if (ctx->line_mode == ZU_LINE_LF_TO_CRLF) {
            if (b == '\n' && !*prev_cr) {
                out[o++] = '\r';
                out[o++] = '\n';
            }
            else {
                out[o++] = b;
            }
            *prev_cr = (b == '\r');
        }
        else { /* CRLF_TO_LF */
            if (*prev_cr) {
                if (b == '\n') {
                    out[o++] = '\n';
                    *prev_cr = false;
                    continue;
                }
                out[o++] = '\r';
                *prev_cr = false;
            }
            if (b == '\r') {
                *prev_cr = true;
                continue;
            }
            out[o++] = b;
        }
    }
    if (ctx->line_mode == ZU_LINE_CRLF_TO_LF && *prev_cr) {
        out[o++] = '\r';
        *prev_cr = false;
    }

    *out_buf = out;
    *out_len = o;
    return ZU_STATUS_OK;
}

static char* make_temp_path(const ZContext* ctx, const char* target_path) {
    const char* base = basename_component(target_path);
    const char* dir_sep = strrchr(target_path, '/');
    char dirbuf[PATH_MAX];
    const char* dir = ".";
    if (ctx->temp_dir) {
        dir = ctx->temp_dir;
    }
    else if (dir_sep) {
        size_t len = (size_t)(dir_sep - target_path);
        if (len >= sizeof(dirbuf))
            len = sizeof(dirbuf) - 1;
        memcpy(dirbuf, target_path, len);
        dirbuf[len] = '\0';
        dir = dirbuf;
    }

    size_t len = strlen(dir) + strlen(base) + 6 + 3; /* / + .tmp + nul */
    char* path = malloc(len);
    if (!path)
        return NULL;
    snprintf(path, len, "%s/%s.tmp", dir, base);
    return path;
}

static int stage_translated(ZContext* ctx, const char* path, bool is_stdin, FILE** out_fp, char** out_path, uint32_t* crc_out, uint64_t* size_out) {
    char* tpath = make_temp_path(ctx, path ? path : "stdin");
    if (!tpath)
        return ZU_STATUS_OOM;
    int fd = mkstemp(tpath);
    if (fd < 0) {
        free(tpath);
        return ZU_STATUS_IO;
    }
    FILE* fp = fdopen(fd, "wb+");
    if (!fp) {
        close(fd);
        unlink(tpath);
        free(tpath);
        return ZU_STATUS_IO;
    }

    FILE* src = is_stdin ? stdin : fopen(path, "rb");
    if (!src) {
        fclose(fp);
        unlink(tpath);
        free(tpath);
        return ZU_STATUS_IO;
    }

    uint8_t* buf = malloc(ZU_IO_CHUNK);
    if (!buf) {
        if (!is_stdin)
            fclose(src);
        fclose(fp);
        unlink(tpath);
        free(tpath);
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t total = 0;
    bool prev_cr = false;
    int rc = ZU_STATUS_OK;
    size_t got;
    while ((got = fread(buf, 1, ZU_IO_CHUNK, src)) > 0) {
        uint8_t* out = NULL;
        size_t out_len = 0;
        rc = translate_buffer(ctx, buf, got, &out, &out_len, &prev_cr);
        if (rc != ZU_STATUS_OK)
            break;
        crc = zu_crc32(out, out_len, crc);
        total += out_len;
        if (fwrite(out, 1, out_len, fp) != out_len) {
            rc = ZU_STATUS_IO;
        }
        if (out != buf)
            free(out);
        if (rc != ZU_STATUS_OK)
            break;
    }
    if (rc == ZU_STATUS_OK && ferror(src)) {
        rc = ZU_STATUS_IO;
    }
    free(buf);
    if (!is_stdin)
        fclose(src);

    if (rc != ZU_STATUS_OK) {
        fclose(fp);
        unlink(tpath);
        free(tpath);
        return rc;
    }

    if (fseeko(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        unlink(tpath);
        free(tpath);
        return ZU_STATUS_IO;
    }

    *out_fp = fp;
    *out_path = tpath;
    if (crc_out)
        *crc_out = crc;
    if (size_out)
        *size_out = total;
    return ZU_STATUS_OK;
}

static int rename_or_copy(const char* src, const char* dst) {
    if (rename(src, dst) == 0) {
        return 0;
    }
    if (errno != EXDEV) {
        return -1;
    }

    FILE* in = fopen(src, "rb");
    FILE* out = fopen(dst, "wb");
    if (!in || !out) {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        return -1;
    }
    uint8_t buf[8192];
    size_t got;
    int rc = 0;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            rc = -1;
            break;
        }
    }
    if (ferror(in) || ferror(out))
        rc = -1;
    fclose(in);
    fclose(out);
    if (rc == 0)
        unlink(src);
    return rc;
}

static void msdos_datetime(const struct stat* st, uint16_t* out_time, uint16_t* out_date) {
    time_t t = st->st_mtime;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) {
        *out_time = 0;
        *out_date = 0;
        return;
    }
    int year = tmv.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
    }
    *out_date = (uint16_t)(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    *out_time = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
}

static uint64_t zip64_trigger_bytes(void) {
    const char* env = getenv("ZU_TEST_ZIP64_TRIGGER");
    if (!env || *env == '\0') {
        return (uint64_t)UINT32_MAX + 1ULL;
    }
    char* end = NULL;
    unsigned long long val = strtoull(env, &end, 10);
    if (*end == '\0' && val > 0) {
        return (uint64_t)val;
    }
    return (uint64_t)UINT32_MAX + 1ULL;
}

/* -------------------------------------------------------------------------
 * Split Archive Helpers
 * ------------------------------------------------------------------------- */

static char* get_split_path(const char* base, uint32_t index) {
    char* path = strdup(base);
    if (!path)
        return NULL;

    /* If base is "archive.zip.tmp" and index is 1, we want "archive.z01.tmp" */
    char* zip = strstr(path, ".zip");
    if (zip) {
        zip[1] = 'z';
        zip[2] = (char)('0' + (index / 10) % 10);
        zip[3] = (char)('0' + (index % 10));
    }
    else {
        // Fallback: insert .zXX before last .tmp
        size_t len = strlen(path);
        if (len > 4 && strcmp(path + len - 4, ".tmp") == 0) {
            char* new_path = malloc(len + 5);
            if (new_path) {
                strncpy(new_path, path, len - 4);
                sprintf(new_path + len - 4, ".z%02d.tmp", index);
                free(path);
                path = new_path;
            }
        }
    }
    return path;
}

static int zu_open_next_split(ZContext* ctx) {
    if (ctx->out_file) {
        fclose(ctx->out_file);
        ctx->out_file = NULL;
    }

    ctx->split_disk_index++;
    char* next_path = get_split_path(ctx->temp_write_path, ctx->split_disk_index);
    if (!next_path)
        return ZU_STATUS_OOM;

    ctx->out_file = fopen(next_path, "wb");
    if (!ctx->out_file) {
        free(next_path);
        zu_context_set_error(ctx, ZU_STATUS_IO, "create split file failed");
        return ZU_STATUS_IO;
    }

    if (ctx->verbose) {
        zu_log(ctx, "creating split file: %s\n", next_path);
    }

    free(next_path);
    ctx->split_written = 0;
    return ZU_STATUS_OK;
}

static int zu_write_output(ZContext* ctx, const void* ptr, size_t size) {
    if (!ctx->out_file)
        return ZU_STATUS_IO;

    const uint8_t* data = (const uint8_t*)ptr;
    size_t remaining = size;

    while (remaining > 0) {
        if (ctx->split_size > 0 && ctx->split_written + remaining > ctx->split_size) {
            size_t can_write = (size_t)(ctx->split_size - ctx->split_written);
            if (can_write > 0) {
                if (fwrite(data, 1, can_write, ctx->out_file) != can_write) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "write split failed");
                    return ZU_STATUS_IO;
                }
                ctx->split_written += can_write;
                data += can_write;
                remaining -= can_write;
            }

            if (ctx->split_pause) {
                printf("Split point reached. Insert disk %d and press Enter...", ctx->split_disk_index + 2);
                while (getchar() != '\n')
                    ;
            }

            if (zu_open_next_split(ctx) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
        }
        else {
            if (fwrite(data, 1, remaining, ctx->out_file) != remaining) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write output failed");
                return ZU_STATUS_IO;
            }
            ctx->split_written += remaining;
            remaining = 0;
        }
    }
    return ZU_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * I/O Helpers
 * ------------------------------------------------------------------------- */

static int describe_input(ZContext* ctx, const char* path, zu_input_info* info) {
    if (!ctx || !path || !info) {
        return ZU_STATUS_USAGE;
    }

    memset(info, 0, sizeof(*info));

    if (path_is_stdin(path)) {
        info->is_stdin = true;
        info->size_known = false;
        info->st.st_mode = S_IFIFO;
        info->st.st_mtime = time(NULL);
        return ZU_STATUS_OK;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "stat '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }
    if (S_ISLNK(st.st_mode)) {
        if (!ctx->allow_symlinks) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "refusing to follow symlink (use flag to allow)");
            return ZU_STATUS_USAGE;
        }
        if (stat(path, &st) != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "stat target '%s': %s", path, strerror(errno));
            zu_context_set_error(ctx, ZU_STATUS_IO, msg);
            return ZU_STATUS_IO;
        }
    }
    if (S_ISDIR(st.st_mode)) {
        /* directories allowed */
    }
    else if (S_ISFIFO(st.st_mode)) {
        if (!ctx->allow_fifo) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "refusing fifo (use flag to allow)");
            return ZU_STATUS_USAGE;
        }
    }
    else if (!S_ISREG(st.st_mode)) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "only regular files are supported");
        return ZU_STATUS_USAGE;
    }
    if (st.st_size < 0) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "negative file size reported");
        return ZU_STATUS_USAGE;
    }

    info->st = st;
    info->size_known = !S_ISFIFO(st.st_mode);
    if (S_ISDIR(st.st_mode)) {
        info->st.st_size = 0;
        info->size_known = true;
    }
    info->is_stdin = false;
    return ZU_STATUS_OK;
}

static int compute_crc_and_size(ZContext* ctx, const char* path, uint32_t* crc_out, uint64_t* size_out) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    uint8_t* buf = malloc(ZU_IO_CHUNK);
    if (!buf) {
        fclose(fp);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating read buffer failed");
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t total = 0;
    size_t got = 0;
    while ((got = fread(buf, 1, ZU_IO_CHUNK, fp)) > 0) {
        crc = zu_crc32(buf, got, crc);
        total += got;
    }

    if (ferror(fp)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "read '%s' failed", path);
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        free(buf);
        fclose(fp);
        return ZU_STATUS_IO;
    }

    free(buf);
    fclose(fp);
    if (crc_out)
        *crc_out = crc;
    if (size_out)
        *size_out = total;
    return ZU_STATUS_OK;
}

static int compress_to_temp(ZContext* ctx, const char* path, int method, int level, FILE** temp_out, uint32_t* crc_out, uint64_t* uncomp_out, uint64_t* comp_out) {
    if (!temp_out) {
        return ZU_STATUS_USAGE;
    }

    FILE* in = fopen(path, "rb");
    if (!in) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    FILE* tmp = tmpfile();
    if (!tmp) {
        fclose(in);
        zu_context_set_error(ctx, ZU_STATUS_IO, "creating temp file failed");
        return ZU_STATUS_IO;
    }

    uint8_t* inbuf = malloc(ZU_IO_CHUNK);
    uint8_t* outbuf = malloc(ZU_IO_CHUNK);
    if (!inbuf || !outbuf) {
        free(inbuf);
        free(outbuf);
        fclose(in);
        fclose(tmp);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating compression buffers failed");
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t total_in = 0;
    size_t got = 0;
    int rc = ZU_STATUS_OK;

    if (method == 8) { /* Deflate */
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int lvl = (level < 0 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
        int zrc = deflateInit2(&strm, lvl, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        if (zrc != Z_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "compression init failed");
            goto comp_done;
        }

        while ((got = fread(inbuf, 1, ZU_IO_CHUNK, in)) > 0) {
            crc = zu_crc32(inbuf, got, crc);
            total_in += got;
            strm.next_in = inbuf;
            strm.avail_in = (uInt)got;
            do {
                strm.next_out = outbuf;
                strm.avail_out = (uInt)ZU_IO_CHUNK;
                zrc = deflate(&strm, Z_NO_FLUSH);
                if (zrc != Z_OK) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "compression failed");
                    deflateEnd(&strm);
                    goto comp_done;
                }
                size_t have = ZU_IO_CHUNK - strm.avail_out;
                if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "write compressed data failed");
                    deflateEnd(&strm);
                    goto comp_done;
                }
            } while (strm.avail_out == 0);
        }
        if (ferror(in)) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "read failed during compression");
            deflateEnd(&strm);
            goto comp_done;
        }
        do {
            strm.next_out = outbuf;
            strm.avail_out = (uInt)ZU_IO_CHUNK;
            zrc = deflate(&strm, Z_FINISH);
            if (zrc != Z_OK && zrc != Z_STREAM_END) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "compression finish failed");
                deflateEnd(&strm);
                goto comp_done;
            }
            size_t have = ZU_IO_CHUNK - strm.avail_out;
            if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "write compressed data failed");
                deflateEnd(&strm);
                goto comp_done;
            }
        } while (zrc != Z_STREAM_END);
        deflateEnd(&strm);
    }
    else if (method == 12) { /* Bzip2 */
        bz_stream strm;
        memset(&strm, 0, sizeof(strm));
        int lvl = (level < 1 || level > 9) ? 9 : level;
        int bzrc = BZ2_bzCompressInit(&strm, lvl, 0, 30);
        if (bzrc != BZ_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "bzip2 init failed");
            goto comp_done;
        }

        while ((got = fread(inbuf, 1, ZU_IO_CHUNK, in)) > 0) {
            crc = zu_crc32(inbuf, got, crc);
            total_in += got;
            strm.next_in = (char*)inbuf;
            strm.avail_in = (unsigned int)got;
            do {
                strm.next_out = (char*)outbuf;
                strm.avail_out = (unsigned int)ZU_IO_CHUNK;
                bzrc = BZ2_bzCompress(&strm, BZ_RUN);
                if (bzrc != BZ_RUN_OK) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "bzip2 compression failed");
                    BZ2_bzCompressEnd(&strm);
                    goto comp_done;
                }
                size_t have = ZU_IO_CHUNK - strm.avail_out;
                if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "write compressed data failed");
                    BZ2_bzCompressEnd(&strm);
                    goto comp_done;
                }
            } while (strm.avail_out == 0);
        }
        if (ferror(in)) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "read failed during compression");
            BZ2_bzCompressEnd(&strm);
            goto comp_done;
        }
        do {
            strm.next_out = (char*)outbuf;
            strm.avail_out = (unsigned int)ZU_IO_CHUNK;
            bzrc = BZ2_bzCompress(&strm, BZ_FINISH);
            if (bzrc != BZ_FINISH_OK && bzrc != BZ_STREAM_END) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "bzip2 finish failed");
                BZ2_bzCompressEnd(&strm);
                goto comp_done;
            }
            size_t have = ZU_IO_CHUNK - strm.avail_out;
            if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "write compressed data failed");
                BZ2_bzCompressEnd(&strm);
                goto comp_done;
            }
        } while (bzrc != BZ_STREAM_END);
        BZ2_bzCompressEnd(&strm);
    }
    else {
        rc = ZU_STATUS_NOT_IMPLEMENTED;
        zu_context_set_error(ctx, rc, "unsupported compression method");
    }

comp_done:
    free(inbuf);
    free(outbuf);
    fclose(in);
    if (rc != ZU_STATUS_OK) {
        fclose(tmp);
        return rc;
    }
    if (fflush(tmp) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "temp file flush failed");
        fclose(tmp);
        return ZU_STATUS_IO;
    }
    off_t pos = ftello(tmp);
    if (pos < 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "ftello failed");
        fclose(tmp);
        return ZU_STATUS_IO;
    }
    if (fseeko(tmp, 0, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "temp file rewind failed");
        fclose(tmp);
        return ZU_STATUS_IO;
    }
    if (crc_out)
        *crc_out = crc;
    if (uncomp_out)
        *uncomp_out = total_in;
    if (comp_out)
        *comp_out = (uint64_t)pos;
    *temp_out = tmp;
    return ZU_STATUS_OK;
}

static int write_file_data(ZContext* ctx, const char* path, FILE* staged, uint64_t expected_size, zu_zipcrypto_ctx* zc) {
    FILE* src = staged ? staged : fopen(path, "rb");
    if (!src) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }
    uint8_t* buf = malloc(ZU_IO_CHUNK);
    if (!buf) {
        if (!staged)
            fclose(src);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating write buffer failed");
        return ZU_STATUS_OOM;
    }
    uint64_t written = 0;
    size_t got = 0;
    while ((got = fread(buf, 1, ZU_IO_CHUNK, src)) > 0) {
        if (zc) {
            zu_zipcrypto_encrypt(zc, buf, got);
        }
        if (zu_write_output(ctx, buf, got) != ZU_STATUS_OK) {
            free(buf);
            if (!staged)
                fclose(src);
            return ZU_STATUS_IO;
        }
        written += got;
    }
    if (ferror(src)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "read failed while writing output");
        free(buf);
        if (!staged)
            fclose(src);
        return ZU_STATUS_IO;
    }
    free(buf);
    if (!staged)
        fclose(src);
    if (expected_size != UINT64_MAX && written != expected_size) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "input size changed during write");
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static int write_data_descriptor(ZContext* ctx, uint32_t crc, uint64_t comp_size, uint64_t uncomp_size, bool use_zip64) {
    if (use_zip64 || comp_size > UINT32_MAX || uncomp_size > UINT32_MAX) {
        zu_data_descriptor64 dd64 = {
            .signature = ZU_SIG_DESCRIPTOR,
            .crc32 = crc,
            .comp_size = comp_size,
            .uncomp_size = uncomp_size,
        };
        if (zu_write_output(ctx, &dd64, sizeof(dd64)) != ZU_STATUS_OK) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write data descriptor failed");
            return ZU_STATUS_IO;
        }
        return ZU_STATUS_OK;
    }

    zu_data_descriptor dd = {
        .signature = ZU_SIG_DESCRIPTOR,
        .crc32 = crc,
        .comp_size = (uint32_t)comp_size,
        .uncomp_size = (uint32_t)uncomp_size,
    };
    if (zu_write_output(ctx, &dd, sizeof(dd)) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write data descriptor failed");
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static int write_streaming_entry(ZContext* ctx,
                                 const char* path,
                                 const char* stored,
                                 const zu_input_info* info,
                                 uint16_t dos_time,
                                 uint16_t dos_date,
                                 uint64_t zip64_trigger,
                                 uint64_t* offset,
                                 zu_entry_list* entries) {
    if (!ctx || !stored || !info || !offset || !entries) {
        return ZU_STATUS_USAGE;
    }

    bool compress = ctx->compression_level > 0 && ctx->compression_method != 0;
    if (should_store_by_suffix(ctx, path)) {
        compress = false;
    }
    if (info->size_known && info->st.st_size == 0) {
        compress = false;
    }
    uint16_t method = compress ? ctx->compression_method : 0;
    uint16_t flags = 0x0008; /* data descriptor */

    uint32_t ext_attr = 0;
    uint16_t version_made = 20;
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    version_made = (3 << 8) | 20;
    uint8_t dos_attr = 0;
    if (S_ISDIR(info->st.st_mode))
        dos_attr |= 0x10;
    if (!(info->st.st_mode & S_IWUSR))
        dos_attr |= 0x01;
    ext_attr = ((uint32_t)info->st.st_mode << 16) | dos_attr;
#endif

    if (ctx->encrypt && ctx->password) {
        flags |= 1;
    }

    size_t name_len = strlen(stored);
    uint16_t version_needed = (method == 0 ? 10 : 20);
    if (*offset >= zip64_trigger) {
        version_needed = 45;
    }

    zu_local_header lho = {
        .signature = ZU_SIG_LOCAL,
        .version_needed = version_needed,
        .flags = flags,
        .method = method,
        .mod_time = dos_time,
        .mod_date = dos_date,
        .crc32 = 0,
        .comp_size = 0,
        .uncomp_size = 0,
        .name_len = (uint16_t)name_len,
        .extra_len = 0,
    };

    if (zu_write_output(ctx, &lho, sizeof(lho)) != ZU_STATUS_OK || zu_write_output(ctx, stored, name_len) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write local header failed");
        return ZU_STATUS_IO;
    }

    zu_zipcrypto_ctx zc;
    zu_zipcrypto_ctx* pzc = NULL;
    uint64_t comp_size = 0;
    uint64_t uncomp_size = 0;
    uint32_t crc = 0;

    if (flags & 1) {
        zu_zipcrypto_init(&zc, ctx->password);
        pzc = &zc;
        uint8_t header[12];
        for (int k = 0; k < 12; ++k)
            header[k] = (uint8_t)(rand() & 0xff);
        header[11] = (uint8_t)(dos_time >> 8); /* bit 3 set: use mod_time high byte */
        zu_zipcrypto_encrypt(&zc, header, 12);
        if (zu_write_output(ctx, header, 12) != ZU_STATUS_OK) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write encryption header failed");
            return ZU_STATUS_IO;
        }
        comp_size += 12;
    }

    FILE* src = info->is_stdin ? stdin : fopen(path, "rb");
    if (!src) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    uint8_t* inbuf = malloc(ZU_IO_CHUNK);
    uint8_t* outbuf = compress ? malloc(ZU_IO_CHUNK) : NULL;
    if (!inbuf || (compress && !outbuf)) {
        free(inbuf);
        free(outbuf);
        if (src != stdin)
            fclose(src);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating streaming buffers failed");
        return ZU_STATUS_OOM;
    }

    int rc = ZU_STATUS_OK;
    if (!compress) {
        size_t got = 0;
        while ((got = fread(inbuf, 1, ZU_IO_CHUNK, src)) > 0) {
            crc = zu_crc32(inbuf, got, crc);
            uncomp_size += got;
            if (pzc) {
                zu_zipcrypto_encrypt(pzc, inbuf, got);
            }
            if (zu_write_output(ctx, inbuf, got) != ZU_STATUS_OK) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "write output failed");
                break;
            }
            comp_size += got;
        }
        if (rc == ZU_STATUS_OK && ferror(src)) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "read failed during streaming copy");
        }
    }
    else if (method == 8) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int lvl = (ctx->compression_level < 0 || ctx->compression_level > 9) ? Z_DEFAULT_COMPRESSION : ctx->compression_level;
        int zrc = deflateInit2(&strm, lvl, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        if (zrc != Z_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "compression init failed");
        }
        else {
            size_t got = 0;
            while ((got = fread(inbuf, 1, ZU_IO_CHUNK, src)) > 0) {
                crc = zu_crc32(inbuf, got, crc);
                uncomp_size += got;
                strm.next_in = inbuf;
                strm.avail_in = (uInt)got;
                do {
                    strm.next_out = outbuf;
                    strm.avail_out = (uInt)ZU_IO_CHUNK;
                    zrc = deflate(&strm, Z_NO_FLUSH);
                    if (zrc != Z_OK && zrc != Z_STREAM_END) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "compression failed");
                        break;
                    }
                    size_t have = ZU_IO_CHUNK - strm.avail_out;
                    if (have > 0) {
                        if (pzc) {
                            zu_zipcrypto_encrypt(pzc, outbuf, have);
                        }
                        if (zu_write_output(ctx, outbuf, have) != ZU_STATUS_OK) {
                            rc = ZU_STATUS_IO;
                            zu_context_set_error(ctx, rc, "write compressed output failed");
                            break;
                        }
                        comp_size += have;
                    }
                } while (strm.avail_out == 0);
                if (rc != ZU_STATUS_OK) {
                    break;
                }
            }
            if (rc == ZU_STATUS_OK && ferror(src)) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "read failed during streaming compression");
            }
            if (rc == ZU_STATUS_OK) {
                do {
                    strm.next_out = outbuf;
                    strm.avail_out = (uInt)ZU_IO_CHUNK;
                    zrc = deflate(&strm, Z_FINISH);
                    if (zrc != Z_OK && zrc != Z_STREAM_END) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "compression finish failed");
                        break;
                    }
                    size_t have = ZU_IO_CHUNK - strm.avail_out;
                    if (have > 0) {
                        if (pzc) {
                            zu_zipcrypto_encrypt(pzc, outbuf, have);
                        }
                        if (zu_write_output(ctx, outbuf, have) != ZU_STATUS_OK) {
                            rc = ZU_STATUS_IO;
                            zu_context_set_error(ctx, rc, "write compressed output failed");
                            break;
                        }
                        comp_size += have;
                    }
                } while (zrc != Z_STREAM_END && rc == ZU_STATUS_OK);
            }
            deflateEnd(&strm);
        }
    }
    else if (method == 12) {
        bz_stream strm;
        memset(&strm, 0, sizeof(strm));
        int lvl = (ctx->compression_level < 1 || ctx->compression_level > 9) ? 9 : ctx->compression_level;
        int bzrc = BZ2_bzCompressInit(&strm, lvl, 0, 30);
        if (bzrc != BZ_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "bzip2 init failed");
        }
        else {
            size_t got = 0;
            while ((got = fread(inbuf, 1, ZU_IO_CHUNK, src)) > 0) {
                crc = zu_crc32(inbuf, got, crc);
                uncomp_size += got;
                strm.next_in = (char*)inbuf;
                strm.avail_in = (unsigned int)got;
                do {
                    strm.next_out = (char*)outbuf;
                    strm.avail_out = (unsigned int)ZU_IO_CHUNK;
                    bzrc = BZ2_bzCompress(&strm, BZ_RUN);
                    if (bzrc != BZ_RUN_OK) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "bzip2 compression failed");
                        break;
                    }
                    size_t have = ZU_IO_CHUNK - strm.avail_out;
                    if (have > 0) {
                        if (pzc) {
                            zu_zipcrypto_encrypt(pzc, outbuf, have);
                        }
                        if (zu_write_output(ctx, outbuf, have) != ZU_STATUS_OK) {
                            rc = ZU_STATUS_IO;
                            zu_context_set_error(ctx, rc, "write compressed output failed");
                            break;
                        }
                        comp_size += have;
                    }
                } while (strm.avail_out == 0);
                if (rc != ZU_STATUS_OK) {
                    break;
                }
            }
            if (rc == ZU_STATUS_OK && ferror(src)) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "read failed during streaming compression");
            }
            if (rc == ZU_STATUS_OK) {
                do {
                    strm.next_out = (char*)outbuf;
                    strm.avail_out = (unsigned int)ZU_IO_CHUNK;
                    bzrc = BZ2_bzCompress(&strm, BZ_FINISH);
                    if (bzrc != BZ_FINISH_OK && bzrc != BZ_STREAM_END) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "bzip2 finish failed");
                        break;
                    }
                    size_t have = ZU_IO_CHUNK - strm.avail_out;
                    if (have > 0) {
                        if (pzc) {
                            zu_zipcrypto_encrypt(pzc, outbuf, have);
                        }
                        if (zu_write_output(ctx, outbuf, have) != ZU_STATUS_OK) {
                            rc = ZU_STATUS_IO;
                            zu_context_set_error(ctx, rc, "write compressed output failed");
                            break;
                        }
                        comp_size += have;
                    }
                } while (bzrc != BZ_STREAM_END && rc == ZU_STATUS_OK);
            }
            BZ2_bzCompressEnd(&strm);
        }
    }
    else {
        rc = ZU_STATUS_NOT_IMPLEMENTED;
        zu_context_set_error(ctx, rc, "unsupported streaming compression method");
    }

    if (src != stdin) {
        fclose(src);
    }
    free(inbuf);
    free(outbuf);

    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    bool need_zip64 = comp_size >= zip64_trigger || uncomp_size >= zip64_trigger || *offset >= zip64_trigger;
    if (write_data_descriptor(ctx, crc, comp_size, uncomp_size, need_zip64) != ZU_STATUS_OK) {
        return ZU_STATUS_IO;
    }
    uint64_t desc_len = need_zip64 ? sizeof(zu_data_descriptor64) : sizeof(zu_data_descriptor);

    if (ensure_entry_capacity(entries) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating entry list failed");
        return ZU_STATUS_OOM;
    }

    entries->items[entries->len].name = strdup(stored);
    entries->items[entries->len].crc32 = crc;
    entries->items[entries->len].comp_size = comp_size;
    entries->items[entries->len].uncomp_size = uncomp_size;
    entries->items[entries->len].lho_offset = *offset;
    entries->items[entries->len].method = method;
    entries->items[entries->len].mod_time = dos_time;
    entries->items[entries->len].mod_date = dos_date;
    entries->items[entries->len].ext_attr = ext_attr;
    entries->items[entries->len].version_made = need_zip64 ? (uint16_t)((3 << 8) | 45) : version_made;
    entries->items[entries->len].zip64 = need_zip64;
    entries->items[entries->len].flags = flags;
    entries->items[entries->len].comment = NULL;
    entries->items[entries->len].comment_len = 0;
    entries->len++;

    uint64_t header_len = sizeof(lho) + name_len + lho.extra_len;
    *offset += header_len + comp_size + desc_len;
    return ZU_STATUS_OK;
}

static int copy_existing_entry(ZContext* ctx, const zu_existing_entry* e, uint64_t* written_out) {
    if (fseeko(ctx->in_file, (off_t)e->lho_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to old LHO failed");
        return ZU_STATUS_IO;
    }

    zu_local_header lho;
    if (fread(&lho, 1, sizeof(lho), ctx->in_file) != sizeof(lho) || lho.signature != ZU_SIG_LOCAL) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "read old LHO failed");
        return ZU_STATUS_IO;
    }

    /* We need to copy LHO + name + extra + data.
     * We know size of name/extra from LHO.
     * We know size of data from CD entry (e->comp_size). */

    uint64_t header_len = sizeof(lho) + lho.name_len + lho.extra_len;
    uint64_t total_to_copy = header_len + e->comp_size;

    /* Rewind to start of LHO */
    if (fseeko(ctx->in_file, (off_t)e->lho_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to old LHO start failed");
        return ZU_STATUS_IO;
    }

    uint8_t* buf = malloc(ZU_IO_CHUNK);
    if (!buf)
        return ZU_STATUS_OOM;

    uint64_t remaining = total_to_copy;
    while (remaining > 0) {
        size_t to_read = remaining > ZU_IO_CHUNK ? ZU_IO_CHUNK : (size_t)remaining;
        size_t got = fread(buf, 1, to_read, ctx->in_file);
        if (got != to_read) {
            free(buf);
            zu_context_set_error(ctx, ZU_STATUS_IO, "short read during entry copy");
            return ZU_STATUS_IO;
        }
        if (zu_write_output(ctx, buf, got) != ZU_STATUS_OK) {
            free(buf);
            return ZU_STATUS_IO;
        }
        remaining -= got;
    }
    free(buf);

    if (written_out)
        *written_out = total_to_copy;
    return ZU_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * Central Directory Writing
 * ------------------------------------------------------------------------- */

static int write_central_directory(ZContext* ctx, const zu_entry_list* entries, uint64_t cd_offset, uint64_t* cd_size_out, bool* needs_zip64_out) {
    uint64_t cd_size = 0;
    bool needs_zip64 = entries->len > 0xffff || cd_offset > UINT32_MAX;

    for (size_t i = 0; i < entries->len; ++i) {
        const zu_writer_entry* e = &entries->items[i];
        size_t name_len = strlen(e->name);

        bool entry_zip64 = e->zip64 || e->comp_size > UINT32_MAX || e->uncomp_size > UINT32_MAX || e->lho_offset > UINT32_MAX;
        if (entry_zip64)
            needs_zip64 = true;

        uint64_t zip64_vals[3];
        size_t zv = 0;
        bool need_uncomp64 = entry_zip64 || e->uncomp_size > UINT32_MAX;
        bool need_comp64 = entry_zip64 || e->comp_size > UINT32_MAX;
        bool need_off64 = entry_zip64 || e->lho_offset > UINT32_MAX;

        if (need_uncomp64)
            zip64_vals[zv++] = e->uncomp_size;
        if (need_comp64)
            zip64_vals[zv++] = e->comp_size;
        if (need_off64)
            zip64_vals[zv++] = e->lho_offset;

        uint32_t comp32 = need_comp64 ? 0xffffffffu : (uint32_t)e->comp_size;
        uint32_t uncomp32 = need_uncomp64 ? 0xffffffffu : (uint32_t)e->uncomp_size;
        uint32_t offset32 = need_off64 ? 0xffffffffu : (uint32_t)e->lho_offset;

        uint16_t extra_len = zv > 0 ? (uint16_t)(4 + zv * sizeof(uint64_t)) : 0;
        uint16_t version_needed = entry_zip64 ? 45 : (e->method == 0 ? 10 : 20);
        uint16_t comment_len = e->comment_len;

        zu_central_header ch = {
            .signature = ZU_SIG_CENTRAL,
            .version_made = e->version_made,
            .version_needed = version_needed,
            .flags = e->flags,
            .method = e->method,
            .mod_time = e->mod_time,
            .mod_date = e->mod_date,
            .crc32 = e->crc32,
            .comp_size = comp32,
            .uncomp_size = uncomp32,
            .name_len = (uint16_t)name_len,
            .extra_len = extra_len,
            .comment_len = comment_len,
            .disk_start = 0,
            .int_attr = 0,
            .ext_attr = e->ext_attr,
            .lho_offset = offset32,
        };

        /* For split archives, we need to set disk_start properly */
        if (ctx->split_size > 0) {
            /* This is simplified. Ideally we track which disk the LHO is on.
               The current architecture doesn't easily track LHO disk location.
               We stored lho_offset as global offset?
               If lho_offset is global, we need to map it to disk number?
               Standard zip stores disk number where file starts.

               In this implementation, we don't easily know the disk number for a previous file unless we tracked it.
               We can add `disk_start` to zu_writer_entry.
            */
            // TODO: Track disk number. For now leaving as 0 which is incorrect for multi-volume.
        }

        if (zu_write_output(ctx, &ch, sizeof(ch)) != ZU_STATUS_OK || zu_write_output(ctx, e->name, name_len) != ZU_STATUS_OK) {
            return ZU_STATUS_IO;
        }
        if (zv > 0) {
            uint16_t header_id = ZU_EXTRA_ZIP64;
            uint16_t data_len = (uint16_t)(zv * sizeof(uint64_t));
            if (zu_write_output(ctx, &header_id, sizeof(header_id)) != ZU_STATUS_OK || zu_write_output(ctx, &data_len, sizeof(data_len)) != ZU_STATUS_OK ||
                zu_write_output(ctx, zip64_vals, zv * sizeof(uint64_t)) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
        }
        if (comment_len > 0 && !e->comment) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "entry comment missing");
            return ZU_STATUS_IO;
        }
        if (comment_len > 0 && e->comment) {
            if (zu_write_output(ctx, e->comment, comment_len) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
        }
        cd_size += sizeof(ch) + name_len + extra_len + comment_len;
    }
    if (cd_size > UINT32_MAX)
        needs_zip64 = true;
    if (cd_size_out)
        *cd_size_out = cd_size;
    if (needs_zip64_out)
        *needs_zip64_out = needs_zip64;
    return ZU_STATUS_OK;
}

static int write_end_central(ZContext* ctx, const zu_entry_list* entries, uint64_t cd_offset, uint64_t cd_size, bool needs_zip64, const char* comment, uint16_t comment_len) {
    uint16_t entry_count = entries->len > 0xffff ? 0xffff : (uint16_t)entries->len;
    zu_end_central endrec = {
        .signature = ZU_SIG_END,
        .disk_num = (uint16_t)(ctx->split_disk_index > 0xffff ? 0xffff : ctx->split_disk_index),  // Should be number of this disk
        .disk_start = 0,                                                                          // Should be disk where CD starts
        .entries_disk = entry_count,                                                              // Should be entries on this disk
        .entries_total = entry_count,
        .cd_size = (needs_zip64 || cd_size > UINT32_MAX) ? 0xffffffffu : (uint32_t)cd_size,
        .cd_offset = (needs_zip64 || cd_offset > UINT32_MAX) ? 0xffffffffu : (uint32_t)cd_offset,
        .comment_len = comment_len,
    };
    /* Fixup split fields if needed */
    /* Note: Ideally we track disk_start and disk_num properly */

    if (zu_write_output(ctx, &endrec, sizeof(endrec)) != ZU_STATUS_OK) {
        return ZU_STATUS_IO;
    }
    if (comment_len > 0 && comment) {
        if (zu_write_output(ctx, comment, comment_len) != ZU_STATUS_OK) {
            return ZU_STATUS_IO;
        }
    }
    return ZU_STATUS_OK;
}

static int write_end_central64(ZContext* ctx, const zu_entry_list* entries, uint64_t cd_offset, uint64_t cd_size) {
    zu_end_central64 end64 = {
        .signature = ZU_SIG_END64,
        .size = (uint64_t)(sizeof(zu_end_central64) - 12),
        .version_made = 45,
        .version_needed = 45,
        .disk_num = ctx->split_disk_index,
        .disk_start = 0,  // Disk where CD starts
        .entries_disk = (uint64_t)entries->len,
        .entries_total = (uint64_t)entries->len,
        .cd_size = cd_size,
        .cd_offset = cd_offset,
    };
    if (zu_write_output(ctx, &end64, sizeof(end64)) != ZU_STATUS_OK) {
        return ZU_STATUS_IO;
    }
    zu_end64_locator locator = {
        .signature = ZU_SIG_END64LOC,
        .disk_num = ctx->split_disk_index,  // Disk where EOCD64 is
        .eocd64_offset = cd_offset + cd_size,
        .total_disks = ctx->split_disk_index + 1,
    };
    if (zu_write_output(ctx, &locator, sizeof(locator)) != ZU_STATUS_OK) {
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * Main Logic
 * ------------------------------------------------------------------------- */

int zu_modify_archive(ZContext* ctx) {
    if (!ctx || !ctx->archive_path) {
        return ZU_STATUS_USAGE;
    }
    srand(time(NULL));

    /* 1. Load existing archive if requested and it exists */
    bool existing_loaded = false;
    struct stat st;
    if (stat(ctx->archive_path, &st) == 0 && ctx->modify_archive) {
        if (ctx->existing_loaded) {
            existing_loaded = true;
            if (!ctx->in_file) {
                if (zu_open_input(ctx, ctx->archive_path) != ZU_STATUS_OK) {
                    return ZU_STATUS_IO;
                }
            }
        }
        else if (zu_load_central_directory(ctx) == ZU_STATUS_OK) {
            existing_loaded = true;
        }
    }

    /* 2. Process Input Files and Mark Changes */
    /* If difference_mode, ctx->include contains items to delete.
       Otherwise, ctx->include contains items to add/update. */

    if (ctx->difference_mode) {
        /* Mark entries for deletion */
        for (size_t i = 0; i < ctx->include.len; ++i) {
            const char* pattern = ctx->include.items[i];
            for (size_t j = 0; j < ctx->existing_entries.len; ++j) {
                zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[j];
                if (!e->delete && fnmatch(pattern, e->name, 0) == 0) {
                    e->delete = true;
                }
            }
        }
    }
    else {
        /* Add / Update Mode */

        /* 2a. Expand directories if recursion enabled */
        if (zu_expand_args(ctx) != ZU_STATUS_OK) {
            return ZU_STATUS_OOM;
        }

        /* 2b. File Sync (-FS) deletion pass */
        if (ctx->filesync) {
            /* Sync mode: delete entries that are in archive but NOT on filesystem.
               In standard zip, this scope is limited to the input arguments.
               Since we expanded arguments, we can check against FS directly.
               However, standard behavior is safer: Iterate existing entries, check if they exist on FS. */
            for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
                zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
                if (e->delete)
                    continue;

                /* Check if file exists */
                struct stat st;
                if (lstat(e->name, &st) != 0) {
                    /* File missing, delete from archive */
                    e->delete = true;
                    if (ctx->verbose || ctx->log_info) {
                        zu_log(ctx, "deleting: %s\n", e->name);
                    }
                }
            }
        }

        /* For each file in include list, check if it exists in archive */
        /* Note: This simplistic loop matches filenames. Ideally we map them. */

        /* We are not updating existing_entries structures with new data here.
           Instead, we will build a separate list of "files to write".
           But we need to know if we should skip writing a file because the existing one is newer (-f/-u). */
    }

    /* 3. Open Output */
    char* temp_path = NULL;
    const char* target_path = ctx->output_path ? ctx->output_path : ctx->archive_path;

    if (ctx->output_to_stdout) {
        ctx->out_file = stdout;
    }
    else {
        temp_path = make_temp_path(ctx, target_path);
        if (!temp_path) {
            return ZU_STATUS_OOM;
        }

        ctx->temp_write_path = temp_path;  // Store for split logic

        if (ctx->split_size > 0) {
            ctx->split_disk_index = 0;  // Will be incremented to 1
            if (zu_open_next_split(ctx) != ZU_STATUS_OK) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "create split temp file failed");
                free(temp_path);
                return ZU_STATUS_IO;
            }
        }
        else {
            ctx->out_file = fopen(temp_path, "wb");
            if (!ctx->out_file) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "create temp file failed");
                free(temp_path);
                return ZU_STATUS_IO;
            }
        }
    }

    zu_entry_list entries = {0};
    uint64_t offset = 0;
    int rc = ZU_STATUS_OK;
    size_t added = 0;
    char* staged_path_global = NULL;
    uint64_t zip64_trigger = zip64_trigger_bytes();

    /* 4. Write Entries */

    /* 4a. Write New/Updated Files (unless delete mode) */
    if (!ctx->difference_mode) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            const char* path = ctx->include.items[i];
            /* Exclude/include pattern filtering */
            if (!zu_should_include(ctx, path)) {
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "skipping %s (excluded)\n", path);
                continue;
            }
            const char* stored = ctx->store_paths ? path : basename_component(path);
            char* allocated = NULL;
            const char* entry_name = stored;

            /* Perform Update/Freshen Checks */
            zu_input_info info;
            if (describe_input(ctx, path, &info) != ZU_STATUS_OK) {
                /* If file not found, skip with warning (standard zip behavior). */
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "zip: %s not found or not readable\n", path);
                continue;
            }

            /* If directory, ensure entry name ends with '/' */
            if (S_ISDIR(info.st.st_mode)) {
                size_t len = strlen(stored);
                if (len == 0 || stored[len - 1] != '/') {
                    allocated = malloc(len + 2);
                    if (!allocated) {
                        zu_context_set_error(ctx, ZU_STATUS_OOM, "out of memory");
                        rc = ZU_STATUS_OOM;
                        goto cleanup;
                    }
                    memcpy(allocated, stored, len);
                    allocated[len] = '/';
                    allocated[len + 1] = '\0';
                    entry_name = allocated;
                }
            }

            /* Check if this file replaces an existing one */
            zu_existing_entry* existing = NULL;
            if (existing_loaded) {
                for (size_t j = 0; j < ctx->existing_entries.len; ++j) {
                    zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[j];
                    if (!e->delete && strcmp(e->name, entry_name) == 0) {
                        existing = e;
                        break;
                    }
                }
            }

            /* Time Filtering */
            if (ctx->has_filter_after && info.st.st_mtime < ctx->filter_after) {
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "skipping %s (older than -t)\n", path);
                free(allocated);
                continue;
            }
            if (ctx->has_filter_before && info.st.st_mtime >= ctx->filter_before) {
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "skipping %s (newer than -tt)\n", path);
                free(allocated);
                continue;
            }

            if (existing) {
                if (ctx->freshen || ctx->update) {
                    /* Compare times */
                    uint16_t dos_time = 0, dos_date = 0;
                    msdos_datetime(&info.st, &dos_time, &dos_date);

                    /* Simple comparison (this ignores high-res timestamps for now) */
                    bool input_newer = true;
                    if (dos_date < existing->hdr.mod_date || (dos_date == existing->hdr.mod_date && dos_time <= existing->hdr.mod_time)) {
                        input_newer = false;
                    }

                    if (!input_newer) {
                        /* Input is older or same, skip it. Keep existing. */
                        if (ctx->verbose || ctx->log_info)
                            zu_log(ctx, "skipping %s (not newer)\n", path);
                        free(allocated);
                        continue;
                    }
                }
                /* We are replacing existing. Mark it for deletion. */
                existing->delete = true;
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "updating: %s\n", entry_name);
                added++;
            }
            else {
                /* New file */
                if (ctx->freshen) {
                    /* Freshen only updates existing. Skip new. */
                    free(allocated);
                    continue;
                }
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "adding: %s\n", entry_name);
                added++;
            }

            /* Proceed to write this file */
            /* Copy-paste from original writer loop */
            uint16_t dos_time = 0, dos_date = 0;
            msdos_datetime(&info.st, &dos_time, &dos_date);

            bool streaming = info.is_stdin || !info.size_known;
            bool needs_translation = ctx->line_mode != ZU_LINE_NONE;
            if (needs_translation && streaming) {
                streaming = false; /* stage for translation */
            }
            if (streaming) {
                rc = write_streaming_entry(ctx, path, entry_name, &info, dos_time, dos_date, zip64_trigger, &offset, &entries);
                if (rc != ZU_STATUS_OK)
                    goto cleanup;
                if (ctx->remove_source && !info.is_stdin) {
                    unlink(path);
                }
                free(allocated);
                continue;
            }

            uint32_t crc = 0;
            uint64_t uncomp_size = 0, comp_size = 0;
            uint16_t method = 0, flags = 0;
            FILE* staged = NULL;
            char* staged_path = NULL;

            if (S_ISDIR(info.st.st_mode)) {
                /* Directories have zero size, no CRC, store method */
                crc = 0;
                uncomp_size = 0;
                comp_size = 0;
                method = 0;
            }
            else {
                bool compress = ctx->compression_level > 0;
                if (info.st.st_size == 0)
                    compress = false;
                if (should_store_by_suffix(ctx, path)) {
                    compress = false;
                }

                if (compress) {
                    method = ctx->compression_method;
                    if (method == 0) {
                        compress = false;
                    }
                }
                else {
                    method = 0;
                }

                if (needs_translation) {
                    FILE* trans_fp = NULL;
                    rc = stage_translated(ctx, path, info.is_stdin, &trans_fp, &staged_path, &crc, &uncomp_size);
                    if (rc != ZU_STATUS_OK) {
                        goto cleanup;
                    }
                    staged = trans_fp;
                    staged_path_global = staged_path;
                    info.size_known = true;
                    comp_size = uncomp_size;

                    if (compress) {
                        FILE* staged_comp = NULL;
                        uint32_t ccrc = 0;
                        uint64_t cuncomp = 0, ccomp = 0;
                        rc = compress_to_temp(ctx, staged_path, method, ctx->compression_level, &staged_comp, &ccrc, &cuncomp, &ccomp);
                        fclose(staged);
                        staged = staged_comp;
                        comp_size = ccomp;
                        uncomp_size = cuncomp;
                        crc = ccrc;
                        if (rc != ZU_STATUS_OK) {
                            goto cleanup;
                        }
                    }
                }
                else {
                    if (compress) {
                        rc = compress_to_temp(ctx, path, method, ctx->compression_level, &staged, &crc, &uncomp_size, &comp_size);
                    }
                    else {
                        rc = compute_crc_and_size(ctx, path, &crc, &uncomp_size);
                        comp_size = uncomp_size;
                    }
                    if (rc != ZU_STATUS_OK) {
                        if (staged)
                            fclose(staged);
                        goto cleanup;
                    }
                }
            }

            uint64_t payload_size = comp_size;
            zu_zipcrypto_ctx zc;
            zu_zipcrypto_ctx* pzc = NULL;
            if (ctx->encrypt && ctx->password) {
                flags |= 1;
                comp_size += 12;
            }

            bool zip64_lho = (comp_size >= zip64_trigger || uncomp_size >= zip64_trigger || offset >= zip64_trigger);
            uint16_t extra_len = zip64_lho ? (uint16_t)(4 + 2 * sizeof(uint64_t)) : 0;
            uint16_t version_needed = zip64_lho ? 45 : (method == 0 ? 10 : 20);
            size_t name_len = strlen(entry_name);

            uint32_t ext_attr = 0;
            uint16_t version_made = 20; /* Default: FAT/DOS, version 2.0 */
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
            version_made = (3 << 8) | (zip64_lho ? 45 : 20); /* Host 3 (Unix) */
            uint8_t dos_attr = 0;
            if (S_ISDIR(info.st.st_mode))
                dos_attr |= 0x10;
            if (!(info.st.st_mode & S_IWUSR))
                dos_attr |= 0x01;
            ext_attr = ((uint32_t)info.st.st_mode << 16) | dos_attr;
#endif

            zu_local_header lho = {
                .signature = ZU_SIG_LOCAL,
                .version_needed = version_needed,
                .flags = flags,
                .method = method,
                .mod_time = dos_time,
                .mod_date = dos_date,
                .crc32 = crc,
                .comp_size = zip64_lho ? 0xffffffffu : (uint32_t)comp_size,
                .uncomp_size = zip64_lho ? 0xffffffffu : (uint32_t)uncomp_size,
                .name_len = (uint16_t)name_len,
                .extra_len = extra_len,
            };

            if (zu_write_output(ctx, &lho, sizeof(lho)) != ZU_STATUS_OK || zu_write_output(ctx, entry_name, name_len) != ZU_STATUS_OK) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write local header failed");
                rc = ZU_STATUS_IO;
                if (staged)
                    fclose(staged);
                goto cleanup;
            }

            if (zip64_lho) {
                uint16_t header_id = ZU_EXTRA_ZIP64;
                uint16_t data_len = 16;
                uint64_t sizes[2] = {uncomp_size, comp_size};
                if (zu_write_output(ctx, &header_id, 2) != ZU_STATUS_OK || zu_write_output(ctx, &data_len, 2) != ZU_STATUS_OK || zu_write_output(ctx, sizes, 16) != ZU_STATUS_OK) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "write Zip64 extra failed");
                    rc = ZU_STATUS_IO;
                    if (staged)
                        fclose(staged);
                    goto cleanup;
                }
            }

            if (ctx->encrypt && ctx->password) {
                zu_zipcrypto_init(&zc, ctx->password);
                pzc = &zc;
                uint8_t header[12];
                for (int k = 0; k < 12; ++k)
                    header[k] = (uint8_t)(rand() & 0xff);
                header[11] = (uint8_t)(crc >> 24);
                zu_zipcrypto_encrypt(&zc, header, 12);
                if (zu_write_output(ctx, header, 12) != ZU_STATUS_OK) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "write encryption header failed");
                    rc = ZU_STATUS_IO;
                    if (staged)
                        fclose(staged);
                    goto cleanup;
                }
            }

            if (!S_ISDIR(info.st.st_mode)) {
                rc = write_file_data(ctx, path, staged, payload_size, pzc);
                if (staged)
                    fclose(staged);
                if (rc != ZU_STATUS_OK)
                    goto cleanup;
            }
            else {
                /* Directory has no data to write */
                if (staged)
                    fclose(staged);
            }
            if (ctx->remove_source && !info.is_stdin) {
                unlink(path);
            }
            if (staged_path_global) {
                unlink(staged_path_global);
                free(staged_path_global);
                staged_path_global = NULL;
            }

            if (ensure_entry_capacity(&entries) != ZU_STATUS_OK) {
                rc = ZU_STATUS_OOM;
                goto cleanup;
            }
            entries.items[entries.len].name = strdup(entry_name);
            entries.items[entries.len].crc32 = crc;
            entries.items[entries.len].comp_size = comp_size;
            entries.items[entries.len].uncomp_size = uncomp_size;
            entries.items[entries.len].lho_offset = offset;
            entries.items[entries.len].method = method;
            entries.items[entries.len].mod_time = dos_time;
            entries.items[entries.len].mod_date = dos_date;
            entries.items[entries.len].ext_attr = ext_attr;
            entries.items[entries.len].version_made = version_made;
            entries.items[entries.len].zip64 = zip64_lho;
            entries.items[entries.len].flags = flags;
            entries.items[entries.len].comment = NULL;
            entries.items[entries.len].comment_len = 0;
            if (existing && existing->comment && existing->comment_len > 0) {
                entries.items[entries.len].comment = malloc(existing->comment_len);
                if (!entries.items[entries.len].comment) {
                    rc = ZU_STATUS_OOM;
                    goto cleanup;
                }
                memcpy(entries.items[entries.len].comment, existing->comment, existing->comment_len);
                entries.items[entries.len].comment_len = existing->comment_len;
            }
            entries.len++;

            offset += sizeof(lho) + name_len + extra_len + comp_size;

            free(allocated);
        }
    }

    bool existing_changes = false;
    if (existing_loaded) {
        for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
            zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
            if (e->changed) {
                existing_changes = true;
                break;
            }
        }
    }

    /* Check if any files were added/updated or metadata changed */
    if (added == 0 && !ctx->difference_mode && !existing_changes && !ctx->zip_comment_specified) {
        rc = ZU_STATUS_NO_FILES;
        goto cleanup;
    }

    /* 4b. Write Kept Existing Entries */
    if (existing_loaded) {
        for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
            zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
            if (!e->delete) {
                /* Copy entry */
                uint64_t written = 0;
                /* Before copying, we must temporarily update e->lho_offset in our struct
                   to be the NEW offset? No, we need old offset to read, but we need to
                   record NEW offset for the new CD.
                   Wait, copy_existing_entry uses e->lho_offset to READ.
                   So we must not change e->lho_offset before calling it. */

                uint64_t new_offset = offset;
                rc = copy_existing_entry(ctx, e, &written);
                if (rc != ZU_STATUS_OK)
                    goto cleanup;

                if (ensure_entry_capacity(&entries) != ZU_STATUS_OK) {
                    rc = ZU_STATUS_OOM;
                    goto cleanup;
                }
                entries.items[entries.len].name = strdup(e->name);
                entries.items[entries.len].crc32 = e->hdr.crc32;
                entries.items[entries.len].comp_size = e->comp_size;
                entries.items[entries.len].uncomp_size = e->uncomp_size;
                entries.items[entries.len].lho_offset = new_offset; /* THE NEW OFFSET */
                entries.items[entries.len].method = e->hdr.method;
                entries.items[entries.len].mod_time = e->hdr.mod_time;
                entries.items[entries.len].mod_date = e->hdr.mod_date;
                entries.items[entries.len].ext_attr = e->hdr.ext_attr;
                entries.items[entries.len].version_made = e->hdr.version_made;
                /* Logic to detect zip64 from existing data */
                entries.items[entries.len].zip64 = (e->comp_size >= 0xffffffffu || e->uncomp_size >= 0xffffffffu || new_offset >= 0xffffffffu);
                entries.items[entries.len].flags = e->hdr.flags;
                entries.items[entries.len].comment = NULL;
                entries.items[entries.len].comment_len = 0;
                if (e->comment && e->comment_len > 0) {
                    entries.items[entries.len].comment = malloc(e->comment_len);
                    if (!entries.items[entries.len].comment) {
                        rc = ZU_STATUS_OOM;
                        goto cleanup;
                    }
                    memcpy(entries.items[entries.len].comment, e->comment, e->comment_len);
                    entries.items[entries.len].comment_len = e->comment_len;
                }
                entries.len++;

                offset += written;
            }
        }
    }

    /* 5. Write Central Directory */
    uint64_t cd_offset = offset;
    uint64_t cd_size = 0;
    bool need_zip64 = false;
    rc = write_central_directory(ctx, &entries, cd_offset, &cd_size, &need_zip64);
    if (rc == ZU_STATUS_OK && need_zip64) {
        rc = write_end_central64(ctx, &entries, cd_offset, cd_size);
    }
    if (rc == ZU_STATUS_OK) {
        uint16_t comment_len = 0;
        if (ctx->zip_comment_len > UINT16_MAX) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "archive comment too large");
            rc = ZU_STATUS_USAGE;
        }
        else {
            comment_len = (uint16_t)ctx->zip_comment_len;
            rc = write_end_central(ctx, &entries, cd_offset, cd_size, need_zip64, ctx->zip_comment, comment_len);
        }
    }

cleanup:
    if (staged_path_global) {
        unlink(staged_path_global);
        free(staged_path_global);
        staged_path_global = NULL;
    }
    free_entries(&entries);
    if (ctx->out_file && ctx->out_file != stdout)
        fclose(ctx->out_file);
    ctx->out_file = NULL;
    if (ctx->in_file)
        fclose(ctx->in_file);
    ctx->in_file = NULL;

    if (rc == ZU_STATUS_OK && temp_path) {
        if (ctx->split_size > 0) {
            /* Rename all split parts */
            /* Current split_disk_index is the last one */
            for (uint32_t i = 1; i <= ctx->split_disk_index; ++i) {
                char* old_name = get_split_path(temp_path, i);
                char* new_name = NULL;
                if (i == ctx->split_disk_index) {
                    new_name = strdup(target_path);
                }
                else {
                    new_name = get_split_path(target_path, i);
                }

                if (rename_or_copy(old_name, new_name) != 0) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "rename split part failed");
                    rc = ZU_STATUS_IO;
                    free(old_name);
                    free(new_name);
                    break;
                }
                free(old_name);
                free(new_name);
            }
        }
        else {
            if (rename_or_copy(temp_path, target_path) != 0) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "rename temp file failed");
                rc = ZU_STATUS_IO;
            }
        }
    }
    else if (temp_path) {
        /* Cleanup temps on error */
        if (ctx->split_size > 0) {
            for (uint32_t i = 1; i <= ctx->split_disk_index; ++i) {
                char* p = get_split_path(temp_path, i);
                unlink(p);
                free(p);
            }
        }
        else {
            unlink(temp_path);
        }
    }

    free(temp_path);

    return rc;
}
