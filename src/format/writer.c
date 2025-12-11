#define _XOPEN_SOURCE 700

#include "writer.h"
#include "reader.h" /* For zu_load_central_directory */

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
#include <fnmatch.h>

#include "crc32.h"
#include "fileio.h"
#include "zip_headers.h"
#include "zipcrypto.h"

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

static const char* basename_component(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash && slash[1] != '\0') {
        return slash + 1;
    }
    return path;
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
    if (S_ISFIFO(st.st_mode)) {
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

static int deflate_to_temp(ZContext* ctx, const char* path, int level, FILE** temp_out, uint32_t* crc_out, uint64_t* uncomp_out, uint64_t* comp_out) {
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

static int write_file_data(ZContext* ctx, const char* path, FILE* staged, FILE* out, uint64_t expected_size, zu_zipcrypto_ctx* zc) {
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
        if (fwrite(buf, 1, got, out) != got) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write file data failed");
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

static int copy_existing_entry(ZContext* ctx, const zu_existing_entry* e, FILE* out, uint64_t* written_out) {
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
        if (fwrite(buf, 1, got, out) != got) {
            free(buf);
            zu_context_set_error(ctx, ZU_STATUS_IO, "write failed during entry copy");
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

static int write_central_directory(ZContext* ctx, FILE* out, const zu_entry_list* entries, uint64_t cd_offset, uint64_t* cd_size_out, bool* needs_zip64_out) {
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
            .comment_len = 0,
            .disk_start = 0,
            .int_attr = 0,
            .ext_attr = e->ext_attr,
            .lho_offset = offset32,
        };

        if (fwrite(&ch, 1, sizeof(ch), out) != sizeof(ch) || fwrite(e->name, 1, name_len, out) != name_len) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write central directory failed");
            return ZU_STATUS_IO;
        }
        if (zv > 0) {
            uint16_t header_id = ZU_EXTRA_ZIP64;
            uint16_t data_len = (uint16_t)(zv * sizeof(uint64_t));
            if (fwrite(&header_id, 1, sizeof(header_id), out) != sizeof(header_id) || fwrite(&data_len, 1, sizeof(data_len), out) != sizeof(data_len) ||
                fwrite(zip64_vals, sizeof(uint64_t), zv, out) != zv) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write central directory failed");
                return ZU_STATUS_IO;
            }
        }
        cd_size += sizeof(ch) + name_len + extra_len;
    }
    if (cd_size > UINT32_MAX)
        needs_zip64 = true;
    if (cd_size_out)
        *cd_size_out = cd_size;
    if (needs_zip64_out)
        *needs_zip64_out = needs_zip64;
    return ZU_STATUS_OK;
}

static int write_end_central(ZContext* ctx, FILE* out, const zu_entry_list* entries, uint64_t cd_offset, uint64_t cd_size, bool needs_zip64) {
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
    if (fwrite(&endrec, 1, sizeof(endrec), out) != sizeof(endrec)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write EOCD failed");
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static int write_end_central64(ZContext* ctx, FILE* out, const zu_entry_list* entries, uint64_t cd_offset, uint64_t cd_size) {
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
        if (zu_load_central_directory(ctx) == ZU_STATUS_OK) {
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
        /* For each file in include list, check if it exists in archive */
        /* Note: This simplistic loop matches filenames. Ideally we map them. */

        /* We are not updating existing_entries structures with new data here.
           Instead, we will build a separate list of "files to write".
           But we need to know if we should skip writing a file because the existing one is newer (-f/-u). */
    }

    /* 3. Open Output */
    char* temp_path = NULL;
    const char* target_path = ctx->output_path ? ctx->output_path : ctx->archive_path;
    FILE* out = NULL;
    if (ctx->output_to_stdout) {
        out = stdout;
    }
    else {
        /* Create temp file next to target path */
        int len = strlen(target_path) + 10;
        temp_path = malloc(len);
        snprintf(temp_path, len, "%s.tmp", target_path);
        out = fopen(temp_path, "wb");
        if (!out) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "create temp file failed");
            free(temp_path);
            return ZU_STATUS_IO;
        }
        ctx->out_file = out;
    }

    zu_entry_list entries = {0};
    uint64_t offset = 0;
    int rc = ZU_STATUS_OK;
    uint64_t zip64_trigger = zip64_trigger_bytes();

    /* 4. Write Entries */

    /* 4a. Write New/Updated Files (unless delete mode) */
    if (!ctx->difference_mode) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            const char* path = ctx->include.items[i];
            const char* stored = ctx->store_paths ? path : basename_component(path);

            /* Check if this file replaces an existing one */
            zu_existing_entry* existing = NULL;
            if (existing_loaded) {
                for (size_t j = 0; j < ctx->existing_entries.len; ++j) {
                    zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[j];
                    if (!e->delete && strcmp(e->name, stored) == 0) {
                        existing = e;
                        break;
                    }
                }
            }

            /* Perform Update/Freshen Checks */
            zu_input_info info;
            if (describe_input(ctx, path, &info) != ZU_STATUS_OK) {
                /* If file not found, skip or error? Standard zip warns and skips.
                   For now, we abort on error as per original writer. */
                rc = ctx->last_error;
                goto cleanup;
            }

            /* Time Filtering */
            if (ctx->has_filter_after && info.st.st_mtime < ctx->filter_after) {
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "skipping %s (older than -t)\n", path);
                continue;
            }
            if (ctx->has_filter_before && info.st.st_mtime >= ctx->filter_before) {
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "skipping %s (newer than -tt)\n", path);
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
                        continue;
                    }
                }
                /* We are replacing existing. Mark it for deletion. */
                existing->delete = true;
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "updating: %s\n", stored);
            }
            else {
                /* New file */
                if (ctx->freshen) {
                    /* Freshen only updates existing. Skip new. */
                    continue;
                }
                if (ctx->verbose || ctx->log_info)
                    zu_log(ctx, "adding: %s\n", stored);
            }

            /* Proceed to write this file */
            /* Copy-paste from original writer loop */
            uint16_t dos_time = 0, dos_date = 0;
            msdos_datetime(&info.st, &dos_time, &dos_date);

            uint32_t crc = 0;
            uint64_t uncomp_size = 0, comp_size = 0;
            uint16_t method = 0, flags = 0;
            FILE* staged = NULL;

            bool compress = ctx->compression_level > 0;
            if (info.st.st_size == 0)
                compress = false;
            method = compress ? 8 : 0;

            if (compress) {
                rc = deflate_to_temp(ctx, path, ctx->compression_level, &staged, &crc, &uncomp_size, &comp_size);
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
            size_t name_len = strlen(stored);

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

            if (fwrite(&lho, 1, sizeof(lho), out) != sizeof(lho) || fwrite(stored, 1, name_len, out) != name_len) {
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
                if (fwrite(&header_id, 1, 2, out) != 2 || fwrite(&data_len, 1, 2, out) != 2 || fwrite(sizes, 8, 2, out) != 2) {
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
                if (fwrite(header, 1, 12, out) != 12) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "write encryption header failed");
                    rc = ZU_STATUS_IO;
                    if (staged)
                        fclose(staged);
                    goto cleanup;
                }
            }

            rc = write_file_data(ctx, path, staged, out, payload_size, pzc);
            if (staged)
                fclose(staged);
            if (rc != ZU_STATUS_OK)
                goto cleanup;

            ensure_entry_capacity(&entries);
            entries.items[entries.len].name = strdup(stored);
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
            entries.len++;

            offset += sizeof(lho) + name_len + extra_len + comp_size;
        }
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
                rc = copy_existing_entry(ctx, e, out, &written);
                if (rc != ZU_STATUS_OK)
                    goto cleanup;

                ensure_entry_capacity(&entries);
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
                entries.len++;

                offset += written;
            }
        }
    }

    /* 5. Write Central Directory */
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

cleanup:
    free_entries(&entries);
    if (ctx->out_file)
        fclose(ctx->out_file);
    ctx->out_file = NULL;
    if (ctx->in_file)
        fclose(ctx->in_file);
    ctx->in_file = NULL;

    if (rc == ZU_STATUS_OK && temp_path) {
        if (rename(temp_path, target_path) != 0) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "rename temp file failed");

            rc = ZU_STATUS_IO;
        }
    }

    else if (temp_path) {
        unlink(temp_path);
    }

    free(temp_path);

    return rc;
}
