#define _XOPEN_SOURCE 700

#include "writer.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "crc32.h"
#include "fileio.h"
#include "zip_headers.h"

#define ZU_EXTRA_ZIP64 0x0001
#define ZU_IO_CHUNK (64 * 1024)

typedef struct {
    char *name;
    uint32_t crc32;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t lho_offset;
    uint16_t method;
    uint16_t flags;
    uint16_t mod_time;
    uint16_t mod_date;
    bool zip64;
} zu_writer_entry;

typedef struct {
    zu_writer_entry *items;
    size_t len;
    size_t cap;
} zu_entry_list;

typedef struct {
    struct stat st;
    bool size_known;
    bool is_stdin;
} zu_input_info;

static void free_entries(zu_entry_list *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->len; ++i) {
        free(list->items[i].name);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int ensure_entry_capacity(zu_entry_list *list) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        zu_writer_entry *new_items = realloc(list->items, new_cap * sizeof(zu_writer_entry));
        if (!new_items) {
            return ZU_STATUS_OOM;
        }
        list->items = new_items;
        list->cap = new_cap;
    }
    return ZU_STATUS_OK;
}

static bool path_is_stdin(const char *path) {
    return path && strcmp(path, "-") == 0;
}

static void msdos_datetime(const struct stat *st, uint16_t *out_time, uint16_t *out_date) {
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

static const char *basename_component(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash && slash[1] != '\0') {
        return slash + 1;
    }
    return path;
}

static uint64_t zip64_trigger_bytes(void) {
    const char *env = getenv("ZU_TEST_ZIP64_TRIGGER");
    if (!env || *env == '\0') {
        return (uint64_t)UINT32_MAX + 1ULL;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(env, &end, 10);
    if (errno != 0 || end == env) {
        return (uint64_t)UINT32_MAX + 1ULL;
    }
    if (val == 0) {
        return 1;
    }
    return (uint64_t)val;
}

static int describe_input(ZContext *ctx, const char *path, zu_input_info *info) {
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
    if (S_ISFIFO(st.st_mode)) {
        if (!ctx->allow_fifo) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "refusing fifo (use flag to allow)");
            return ZU_STATUS_USAGE;
        }
    } else if (!S_ISREG(st.st_mode)) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "only regular files are supported");
        return ZU_STATUS_USAGE;
    }
    if (st.st_size < 0) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "negative file size reported");
        return ZU_STATUS_USAGE;
    }

    info->st = st;
    info->size_known = !S_ISFIFO(st.st_mode);
    info->is_stdin = false;
    return ZU_STATUS_OK;
}

static int compute_crc_and_size(ZContext *ctx, const char *path, uint32_t *crc_out, uint64_t *size_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    uint8_t *buf = malloc(ZU_IO_CHUNK);
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
    if (crc_out) {
        *crc_out = crc;
    }
    if (size_out) {
        *size_out = total;
    }
    return ZU_STATUS_OK;
}


static int open_input_stream(ZContext *ctx, const char *path, FILE **fp_out, bool *should_close_out) {
    if (!ctx || !path || !fp_out) {
        return ZU_STATUS_USAGE;
    }
    *fp_out = NULL;
    if (should_close_out) {
        *should_close_out = false;
    }

    if (path_is_stdin(path)) {
        *fp_out = stdin;
        return ZU_STATUS_OK;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    *fp_out = fp;
    if (should_close_out) {
        *should_close_out = true;
    }
    return ZU_STATUS_OK;
}

static int stream_stored_data(ZContext *ctx, FILE *src, FILE *out, uint32_t *crc_out, uint64_t *size_out) {
    if (!src || !out) {
        return ZU_STATUS_USAGE;
    }
    uint8_t *buf = malloc(ZU_IO_CHUNK);
    if (!buf) {
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating streaming buffer failed");
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t total = 0;
    size_t got = 0;
    while ((got = fread(buf, 1, ZU_IO_CHUNK, src)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "stream write failed");
            free(buf);
            return ZU_STATUS_IO;
        }
        crc = zu_crc32(buf, got, crc);
        total += got;
    }

    if (ferror(src)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "read failed during streaming write");
        free(buf);
        return ZU_STATUS_IO;
    }

    free(buf);
    if (crc_out) {
        *crc_out = crc;
    }
    if (size_out) {
        *size_out = total;
    }
    return ZU_STATUS_OK;
}

static int deflate_stream(ZContext *ctx,
                          FILE *src,
                          FILE *out,
                          int level,
                          uint32_t *crc_out,
                          uint64_t *uncomp_out,
                          uint64_t *comp_out) {
    if (!src || !out) {
        return ZU_STATUS_USAGE;
    }

    uint8_t *inbuf = malloc(ZU_IO_CHUNK);
    uint8_t *outbuf = malloc(ZU_IO_CHUNK);
    if (!inbuf || !outbuf) {
        free(inbuf);
        free(outbuf);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating streaming buffers failed");
        return ZU_STATUS_OOM;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int lvl = (level < 0 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
    int zrc = deflateInit2(&strm, lvl, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (zrc != Z_OK) {
        free(inbuf);
        free(outbuf);
        zu_context_set_error(ctx, ZU_STATUS_IO, "compression init failed");
        return ZU_STATUS_IO;
    }

    uint32_t crc = 0;
    uint64_t total_in = 0;
    uint64_t total_out = 0;
    size_t got = 0;
    int rc = ZU_STATUS_OK;

    while ((got = fread(inbuf, 1, ZU_IO_CHUNK, src)) > 0) {
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
                goto stream_deflate_done;
            }
            size_t have = ZU_IO_CHUNK - strm.avail_out;
            if (have > 0) {
                if (fwrite(outbuf, 1, have, out) != have) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "stream write failed");
                    goto stream_deflate_done;
                }
                total_out += have;
            }
        } while (strm.avail_out == 0);
    }

    if (ferror(src)) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "read failed during compression");
        goto stream_deflate_done;
    }

    do {
        strm.next_out = outbuf;
        strm.avail_out = (uInt)ZU_IO_CHUNK;
        zrc = deflate(&strm, Z_FINISH);
        if (zrc != Z_OK && zrc != Z_STREAM_END) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "compression finish failed");
            goto stream_deflate_done;
        }
        size_t have = ZU_IO_CHUNK - strm.avail_out;
        if (have > 0) {
            if (fwrite(outbuf, 1, have, out) != have) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "stream write failed");
                goto stream_deflate_done;
            }
            total_out += have;
        }
    } while (zrc != Z_STREAM_END);

stream_deflate_done:
    deflateEnd(&strm);
    free(inbuf);
    free(outbuf);

    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    if (crc_out) {
        *crc_out = crc;
    }
    if (uncomp_out) {
        *uncomp_out = total_in;
    }
    if (comp_out) {
        *comp_out = total_out;
    }

    return ZU_STATUS_OK;
}

static uint64_t data_descriptor_length(bool zip64) {
    return zip64 ? (uint64_t)(sizeof(uint32_t) + sizeof(uint32_t) + 2 * sizeof(uint64_t))
                 : (uint64_t)(sizeof(uint32_t) * 4);
}

static int write_data_descriptor(ZContext *ctx, FILE *out, uint32_t crc, uint64_t comp_size, uint64_t uncomp_size, bool zip64) {
    if (!out) {
        return ZU_STATUS_USAGE;
    }
    if (zip64) {
        zu_data_descriptor64 dd = {
            .signature = ZU_SIG_DESCRIPTOR,
            .crc32 = crc,
            .comp_size = comp_size,
            .uncomp_size = uncomp_size,
        };
        if (fwrite(&dd, 1, sizeof(dd), out) != sizeof(dd)) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write Zip64 data descriptor failed");
            return ZU_STATUS_IO;
        }
        return ZU_STATUS_OK;
    }

    if (comp_size > UINT32_MAX || uncomp_size > UINT32_MAX) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "sizes exceed limit for standard data descriptor");
        return ZU_STATUS_USAGE;
    }

    zu_data_descriptor dd = {
        .signature = ZU_SIG_DESCRIPTOR,
        .crc32 = crc,
        .comp_size = (uint32_t)comp_size,
        .uncomp_size = (uint32_t)uncomp_size,
    };
    if (fwrite(&dd, 1, sizeof(dd), out) != sizeof(dd)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write data descriptor failed");
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static int patch_zip64_extra_sizes(ZContext *ctx, FILE *out, uint64_t extra_pos, uint64_t uncomp_size, uint64_t comp_size) {
    if (!out) {
        return ZU_STATUS_USAGE;
    }

    off_t resume = ftello(out);
    if (resume < 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "ftello failed while patching Zip64 extra");
        return ZU_STATUS_IO;
    }

    if (fseeko(out, (off_t)(extra_pos + 4), SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to Zip64 extra failed");
        return ZU_STATUS_IO;
    }

    uint64_t sizes[2] = {uncomp_size, comp_size};
    if (fwrite(sizes, sizeof(uint64_t), 2, out) != 2) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "rewrite Zip64 extra failed");
        return ZU_STATUS_IO;
    }

    if (fseeko(out, resume, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "restore output position failed");
        return ZU_STATUS_IO;
    }

    return ZU_STATUS_OK;
}

static int write_file_data(ZContext *ctx, const char *path, FILE *staged, FILE *out, uint64_t expected_size) {
    FILE *src = staged ? staged : fopen(path, "rb");
    if (!src) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    uint8_t *buf = malloc(ZU_IO_CHUNK);
    if (!buf) {
        if (!staged) {
            fclose(src);
        }
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating write buffer failed");
        return ZU_STATUS_OOM;
    }

    uint64_t written = 0;
    size_t got = 0;
    while ((got = fread(buf, 1, ZU_IO_CHUNK, src)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write file data failed");
            free(buf);
            if (!staged) {
                fclose(src);
            }
            return ZU_STATUS_IO;
        }
        written += got;
    }

    if (ferror(src)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "read failed while writing output");
        free(buf);
        if (!staged) {
            fclose(src);
        }
        return ZU_STATUS_IO;
    }

    free(buf);
    if (!staged) {
        fclose(src);
    }

    if (expected_size != UINT64_MAX && written != expected_size) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "input size changed during write");
        return ZU_STATUS_IO;
    }

    return ZU_STATUS_OK;
}

static int deflate_to_temp(ZContext *ctx,
                           const char *path,
                           int level,
                           FILE **temp_out,
                           uint32_t *crc_out,
                           uint64_t *uncomp_out,
                           uint64_t *comp_out) {
    if (!temp_out) {
        return ZU_STATUS_USAGE;
    }

    FILE *in = fopen(path, "rb");
    if (!in) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    FILE *tmp = tmpfile();
    if (!tmp) {
        fclose(in);
        zu_context_set_error(ctx, ZU_STATUS_IO, "creating temp file failed");
        return ZU_STATUS_IO;
    }

    uint8_t *inbuf = malloc(ZU_IO_CHUNK);
    uint8_t *outbuf = malloc(ZU_IO_CHUNK);
    if (!inbuf || !outbuf) {
        free(inbuf);
        free(outbuf);
        fclose(in);
        fclose(tmp);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating compression buffers failed");
        return ZU_STATUS_OOM;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int lvl = (level < 0 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
    int zrc = deflateInit2(&strm, lvl, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (zrc != Z_OK) {
        free(inbuf);
        free(outbuf);
        fclose(in);
        fclose(tmp);
        zu_context_set_error(ctx, ZU_STATUS_IO, "compression init failed");
        return ZU_STATUS_IO;
    }

    uint32_t crc = 0;
    uint64_t total_in = 0;
    size_t got = 0;
    int rc = ZU_STATUS_OK;

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
                goto deflate_done;
            }
            size_t have = ZU_IO_CHUNK - strm.avail_out;
            if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "write compressed data failed");
                goto deflate_done;
            }
        } while (strm.avail_out == 0);
    }

    if (ferror(in)) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "read failed during compression");
        goto deflate_done;
    }

    /* Finish the stream. */
    do {
        strm.next_out = outbuf;
        strm.avail_out = (uInt)ZU_IO_CHUNK;
        zrc = deflate(&strm, Z_FINISH);
        if (zrc != Z_OK && zrc != Z_STREAM_END) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "compression finish failed");
            goto deflate_done;
        }
        size_t have = ZU_IO_CHUNK - strm.avail_out;
        if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "write compressed data failed");
            goto deflate_done;
        }
    } while (zrc != Z_STREAM_END);

deflate_done:
    deflateEnd(&strm);
    free(inbuf);
    free(outbuf);
    fclose(in);

    if (rc != ZU_STATUS_OK) {
        fclose(tmp);
        return rc;
    }

    if (fflush(tmp) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "flush compressed data failed");
        fclose(tmp);
        return ZU_STATUS_IO;
    }
    off_t pos = ftello(tmp);
    if (pos < 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "query compressed size failed");
        fclose(tmp);
        return ZU_STATUS_IO;
    }
    if (fseeko(tmp, 0, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "rewind compressed data failed");
        fclose(tmp);
        return ZU_STATUS_IO;
    }

    if (crc_out) {
        *crc_out = crc;
    }
    if (uncomp_out) {
        *uncomp_out = total_in;
    }
    if (comp_out) {
        *comp_out = (uint64_t)pos;
    }
    *temp_out = tmp;
    return ZU_STATUS_OK;
}

static int write_end_central(ZContext *ctx, FILE *out, const zu_entry_list *entries, uint64_t cd_offset, uint64_t cd_size, bool needs_zip64) {
    uint16_t entry_count = entries->len > 0xffff ? 0xffff : (uint16_t)entries->len;
    zu_end_central endrec = {
        .signature = ZU_SIG_END,
        .disk_num = 0,
        .disk_start = 0,
        .entries_disk = entry_count,
        .entries_total = entry_count,
        .cd_size = (needs_zip64 || cd_size > UINT32_MAX) ? 0xffffffffu : (uint32_t)cd_size,
        .cd_offset = (needs_zip64 || cd_offset > UINT32_MAX) ? 0xffffffffu : (uint32_t)cd_offset,
        .comment_len = 0,
    };

    size_t wrote = fwrite(&endrec, 1, sizeof(endrec), out);
    if (wrote != sizeof(endrec)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write EOCD failed");
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static int write_end_central64(ZContext *ctx, FILE *out, const zu_entry_list *entries, uint64_t cd_offset, uint64_t cd_size) {
    zu_end_central64 end64 = {
        .signature = ZU_SIG_END64,
        .size = (uint64_t)(sizeof(zu_end_central64) - 12),
        .version_made = 45,
        .version_needed = 45,
        .disk_num = 0,
        .disk_start = 0,
        .entries_disk = (uint64_t)entries->len,
        .entries_total = (uint64_t)entries->len,
        .cd_size = cd_size,
        .cd_offset = cd_offset,
    };

    if (fwrite(&end64, 1, sizeof(end64), out) != sizeof(end64)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write Zip64 EOCD failed");
        return ZU_STATUS_IO;
    }

    zu_end64_locator locator = {
        .signature = ZU_SIG_END64LOC,
        .disk_num = 0,
        .eocd64_offset = cd_offset + cd_size,
        .total_disks = 1,
    };
    if (fwrite(&locator, 1, sizeof(locator), out) != sizeof(locator)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write Zip64 locator failed");
        return ZU_STATUS_IO;
    }

    return ZU_STATUS_OK;
}

static int write_central_directory(ZContext *ctx,
                                   FILE *out,
                                   const zu_entry_list *entries,
                                   uint64_t cd_offset,
                                   uint64_t *cd_size_out,
                                   bool *needs_zip64_out) {
    uint64_t cd_size = 0;
    bool needs_zip64 = entries->len > 0xffff || cd_offset > UINT32_MAX;

    for (size_t i = 0; i < entries->len; ++i) {
        const zu_writer_entry *e = &entries->items[i];
        size_t name_len = strlen(e->name);
        if (name_len == 0 || name_len > 0xffff) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "invalid filename length in central directory");
            return ZU_STATUS_USAGE;
        }

        bool entry_zip64 = e->zip64 || e->comp_size > UINT32_MAX || e->uncomp_size > UINT32_MAX || e->lho_offset > UINT32_MAX;
        if (entry_zip64) {
            needs_zip64 = true;
        }

        uint64_t zip64_vals[3];
        size_t zv = 0;
        bool need_uncomp64 = entry_zip64 || e->uncomp_size > UINT32_MAX;
        bool need_comp64 = entry_zip64 || e->comp_size > UINT32_MAX;
        bool need_off64 = entry_zip64 || e->lho_offset > UINT32_MAX;

        if (need_uncomp64) {
            zip64_vals[zv++] = e->uncomp_size;
        }
        if (need_comp64) {
            zip64_vals[zv++] = e->comp_size;
        }
        if (need_off64) {
            zip64_vals[zv++] = e->lho_offset;
        }

        uint32_t comp32 = need_comp64 ? 0xffffffffu : (uint32_t)e->comp_size;
        uint32_t uncomp32 = need_uncomp64 ? 0xffffffffu : (uint32_t)e->uncomp_size;
        uint32_t offset32 = need_off64 ? 0xffffffffu : (uint32_t)e->lho_offset;

        uint16_t extra_len = zv > 0 ? (uint16_t)(4 + zv * sizeof(uint64_t)) : 0;
        uint16_t version_needed = entry_zip64 ? 45 : (e->method == 0 ? 10 : 20);
        uint16_t version_made = entry_zip64 ? 45 : 20;

        zu_central_header ch = {
            .signature = ZU_SIG_CENTRAL,
            .version_made = version_made,
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
            .comment_len = 0,
            .disk_start = 0,
            .int_attr = 0,
            .ext_attr = 0,
            .lho_offset = offset32,
        };

        if (fwrite(&ch, 1, sizeof(ch), out) != sizeof(ch) ||
            fwrite(e->name, 1, name_len, out) != name_len) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write central directory failed");
            return ZU_STATUS_IO;
        }

        if (zv > 0) {
            uint16_t header_id = ZU_EXTRA_ZIP64;
            uint16_t data_len = (uint16_t)(zv * sizeof(uint64_t));
            if (fwrite(&header_id, 1, sizeof(header_id), out) != sizeof(header_id) ||
                fwrite(&data_len, 1, sizeof(data_len), out) != sizeof(data_len) ||
                fwrite(zip64_vals, sizeof(uint64_t), zv, out) != zv) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write central directory failed");
                return ZU_STATUS_IO;
            }
        }

        cd_size += sizeof(ch) + name_len + extra_len;
    }

    if (cd_size > UINT32_MAX) {
        needs_zip64 = true;
    }
    if (cd_size_out) {
        *cd_size_out = cd_size;
    }
    if (needs_zip64_out) {
        *needs_zip64_out = needs_zip64;
    }
    return ZU_STATUS_OK;
}

int zu_write_archive(ZContext *ctx) {
    if (!ctx || !ctx->archive_path || ctx->include.len == 0) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "archive path and at least one input are required");
        return ZU_STATUS_USAGE;
    }

    FILE *out = NULL;
    bool close_out = false;
    if (ctx->output_to_stdout) {
        out = stdout;
    } else {
        int rc = zu_open_output(ctx, ctx->archive_path, "wb");
        if (rc != ZU_STATUS_OK) {
            return rc;
        }
        out = ctx->out_file;
        close_out = true;
    }

    zu_entry_list entries = {0};
    uint64_t offset = 0;
    uint64_t zip64_trigger = zip64_trigger_bytes();
    int rc = ZU_STATUS_OK;

    for (size_t i = 0; i < ctx->include.len; ++i) {
        const char *path = ctx->include.items[i];
        const char *stored = ctx->store_paths ? path : basename_component(path);
        size_t name_len = strlen(stored);
        if (name_len == 0 || name_len > 0xffff) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "invalid filename length");
            rc = ZU_STATUS_USAGE;
            break;
        }

        zu_input_info info;
        rc = describe_input(ctx, path, &info);
        if (rc != ZU_STATUS_OK) {
            break;
        }

        uint16_t dos_time = 0;
        uint16_t dos_date = 0;
        msdos_datetime(&info.st, &dos_time, &dos_date);

        bool streaming = !info.size_known;
        
        uint32_t crc = 0;
        uint64_t uncomp_size = 0;
        uint64_t comp_size = 0;
        uint16_t method = 0;
        uint16_t flags = 0;
        FILE *staged = NULL;
        FILE *src_stream = NULL;
        bool close_src = false;

        bool compress = ctx->compression_level > 0;
        if (!streaming && info.st.st_size == 0) {
            compress = false;
        }
        method = compress ? 8 : 0;

        if (streaming) {
            rc = open_input_stream(ctx, path, &src_stream, &close_src);
            if (rc != ZU_STATUS_OK) break;
            flags |= 0x0008; /* Data Descriptor */
        } else {
            if (compress) {
                rc = deflate_to_temp(ctx, path, ctx->compression_level, &staged, &crc, &uncomp_size, &comp_size);
            } else {
                rc = compute_crc_and_size(ctx, path, &crc, &uncomp_size);
                comp_size = uncomp_size;
            }
            if (rc != ZU_STATUS_OK) {
                if (staged) fclose(staged);
                break;
            }
        }

        bool zip64_lho = false;
        if (streaming) {
            zip64_lho = true; /* Always use Zip64 structures for streaming to be safe */
        } else {
            zip64_lho = (comp_size >= zip64_trigger || uncomp_size >= zip64_trigger || offset >= zip64_trigger);
        }

        uint16_t extra_len = zip64_lho ? (uint16_t)(4 + 2 * sizeof(uint64_t)) : 0;
        uint16_t version_needed = zip64_lho ? 45 : (method == 0 ? 10 : 20);

        zu_local_header lho = {
            .signature = ZU_SIG_LOCAL,
            .version_needed = version_needed,
            .flags = flags,
            .method = method,
            .mod_time = dos_time,
            .mod_date = dos_date,
            .crc32 = streaming ? 0 : crc,
            .comp_size = streaming ? 0 : (zip64_lho ? 0xffffffffu : (uint32_t)comp_size),
            .uncomp_size = streaming ? 0 : (zip64_lho ? 0xffffffffu : (uint32_t)uncomp_size),
            .name_len = (uint16_t)name_len,
            .extra_len = extra_len,
        };

        if (fwrite(&lho, 1, sizeof(lho), out) != sizeof(lho) || fwrite(stored, 1, name_len, out) != name_len) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write local header failed");
            rc = ZU_STATUS_IO;
            goto entry_cleanup;
        }

        uint64_t zip64_extra_pos = 0;
        if (zip64_lho) {
            off_t pos = ftello(out);
            if (pos >= 0) {
                zip64_extra_pos = (uint64_t)pos;
            }
            
            uint16_t header_id = ZU_EXTRA_ZIP64;
            uint16_t data_len = (uint16_t)(2 * sizeof(uint64_t));
            uint64_t sizes[2] = {streaming ? 0 : uncomp_size, streaming ? 0 : comp_size};
            
            if (fwrite(&header_id, 1, sizeof(header_id), out) != sizeof(header_id) ||
                fwrite(&data_len, 1, sizeof(data_len), out) != sizeof(data_len) ||
                fwrite(sizes, sizeof(uint64_t), 2, out) != 2) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write Zip64 extra failed");
                rc = ZU_STATUS_IO;
                goto entry_cleanup;
            }
        }

        if (streaming) {
            if (compress) {
                rc = deflate_stream(ctx, src_stream, out, ctx->compression_level, &crc, &uncomp_size, &comp_size);
            } else {
                rc = stream_stored_data(ctx, src_stream, out, &crc, &uncomp_size);
                comp_size = uncomp_size;
            }
        } else {
            rc = write_file_data(ctx, path, staged, out, comp_size);
        }

        if (rc != ZU_STATUS_OK) goto entry_cleanup;

        if (streaming) {
            rc = write_data_descriptor(ctx, out, crc, comp_size, uncomp_size, true);
            if (rc != ZU_STATUS_OK) goto entry_cleanup;
            
            /* Attempt to patch LHO if seekable, but ignore failure (e.g. pipe output) */
            if (zip64_lho && zip64_extra_pos > 0) {
                /* We ignore the return code here because inability to patch (non-seekable output) 
                 * is expected and valid for streaming. */
                patch_zip64_extra_sizes(ctx, out, zip64_extra_pos, uncomp_size, comp_size);
            }
            offset += data_descriptor_length(true);
        }

        if (ensure_entry_capacity(&entries) != ZU_STATUS_OK) {
            zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating entry list failed");
            rc = ZU_STATUS_OOM;
            goto entry_cleanup;
        }

        entries.items[entries.len].name = malloc(name_len + 1);
        if (!entries.items[entries.len].name) {
            zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating entry name failed");
            rc = ZU_STATUS_OOM;
            goto entry_cleanup;
        }
        memcpy(entries.items[entries.len].name, stored, name_len + 1);
        entries.items[entries.len].crc32 = crc;
        entries.items[entries.len].comp_size = comp_size;
        entries.items[entries.len].uncomp_size = uncomp_size;
        entries.items[entries.len].lho_offset = offset;
        entries.items[entries.len].method = method;
        entries.items[entries.len].mod_time = dos_time;
        entries.items[entries.len].mod_date = dos_date;
        entries.items[entries.len].zip64 = zip64_lho;
        entries.items[entries.len].flags = flags;
        entries.len += 1;

        offset += (uint64_t)sizeof(lho) + name_len + extra_len + comp_size;

entry_cleanup:
        if (staged) fclose(staged);
        if (src_stream && close_src) fclose(src_stream);
        if (rc != ZU_STATUS_OK) break;
    }

    if (rc == ZU_STATUS_OK) {
        uint64_t cd_offset = offset;
        uint64_t cd_size = 0;
        bool need_zip64 = false;
        rc = write_central_directory(ctx, out, &entries, cd_offset, &cd_size, &need_zip64);
        if (rc == ZU_STATUS_OK && need_zip64) {
            rc = write_end_central64(ctx, out, &entries, cd_offset, cd_size);
        }
        if (rc == ZU_STATUS_OK) {
            rc = write_end_central(ctx, out, &entries, cd_offset, cd_size, need_zip64);
        }
    }

    if (close_out) {
        zu_close_files(ctx);
    }

    free_entries(&entries);
    return rc;
}
