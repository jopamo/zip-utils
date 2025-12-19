#define _XOPEN_SOURCE 700

#include "writer.h"
#include "reader.h" /* For zu_load_central_directory */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <fnmatch.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#endif

#include "crc32.h"
#include "fileio.h"
#include "zip_headers.h"
#include "zipcrypto.h"
#include "bzip2_shim.h"
#include "zlib_shim.h"

#define ZU_EXTRA_ZIP64 0x0001
#define ZU_IO_CHUNK (64 * 1024)

static const char* compression_method_name(uint16_t method) {
    switch (method) {
        case 0:
            return "store";
        case 12:
            return "bzip2";
        case 8:
        default:
            return "deflate";
    }
}

/* Extra fields that store platform attributes we drop when -X is set
   Keep structural extras (e.g., Zip64) so entries stay readable */
static bool should_strip_attr_extra(uint16_t tag) {
    switch (tag) {
        case ZU_EXTRA_ZIP64:
            return false;
        case 0x5455: /* Extended timestamp */
        case 0x5855: /* Info-ZIP Unix (old) */
        case 0x7875: /* Info-ZIP Unix (UID/GID) */
        case 0x756e: /* ASi Unix */
        case 0x000a: /* NTFS attributes/timestamps */
            return true;
        default:
            return false;
    }
}

/* Always returns an owned buffer when extra_len > 0
   Callers may always free(*out_extra) when non-NULL */
static int filter_extra_for_exclude(const uint8_t* extra, uint16_t extra_len, uint8_t** out_extra, uint16_t* out_len) {
    if (!out_extra || !out_len) {
        return ZU_STATUS_USAGE;
    }

    *out_extra = NULL;
    *out_len = 0;

    if (!extra || extra_len == 0) {
        return ZU_STATUS_OK;
    }

    uint8_t* buf = malloc(extra_len);
    if (!buf) {
        return ZU_STATUS_OOM;
    }

    size_t pos = 0;
    size_t out = 0;

    while (pos + 4 <= extra_len) {
        uint16_t tag = 0;
        uint16_t sz = 0;
        memcpy(&tag, extra + pos, sizeof(tag));
        memcpy(&sz, extra + pos + 2, sizeof(sz));
        size_t end = pos + 4 + (size_t)sz;

        if (end > extra_len) {
            memcpy(buf, extra, extra_len);
            *out_extra = buf;
            *out_len = extra_len;
            return ZU_STATUS_OK;
        }

        if (!should_strip_attr_extra(tag)) {
            memcpy(buf + out, extra + pos, 4 + (size_t)sz);
            out += 4 + (size_t)sz;
        }

        pos = end;
    }

    *out_extra = buf;
    *out_len = (uint16_t)out;
    return ZU_STATUS_OK;
}

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
    uint16_t int_attr;     /* Internal file attributes */
    bool zip64;
    uint32_t disk_start;
    char* comment;
    uint16_t comment_len;
    time_t atime;
    time_t mtime;
    time_t ctime;
    uint32_t uid;
    uint32_t gid;
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
    bool is_symlink;
    char* link_target;
    size_t link_target_len;
} zu_input_info;

/* -------------------------------------------------------------------------
 * Helper Functions
 * ------------------------------------------------------------------------- */

static int zu_write_output(ZContext* ctx, const void* ptr, size_t size);
static void progress_log(ZContext* ctx, const char* fmt, ...);

static uint16_t get_extra_len(const ZContext* ctx, bool zip64, bool is_lh, const zu_writer_entry* e) {
    uint16_t len = 0;
    if (zip64) {
        if (is_lh) {
            len += 4 + 2 * sizeof(uint64_t);
        }
        else if (e) {
            uint16_t zv = 0;
            if (e->uncomp_size >= 0xffffffffu || e->zip64)
                zv++;
            if (e->comp_size >= 0xffffffffu || e->zip64)
                zv++;
            if (e->lho_offset >= 0xffffffffu || e->zip64)
                zv++;
            if (zv > 0)
                len += (uint16_t)(4 + zv * sizeof(uint64_t));
        }
    }
    if (!ctx->exclude_extra_attrs) {
        /* UT: 4 (header) + 1 (flags) + N*4 (timestamps) */
        len += 4 + 1 + (is_lh ? 3 : 1) * 4;
        /* Ux: 4 (header) + 1 (version) + 1 (uid size) + 4 (uid) + 1 (gid size) + 4 (gid) = 15 */
        if (is_lh) {
            len += 15;
        }
    }
    return len;
}

static int write_extra_fields(ZContext* ctx, bool zip64, uint64_t uncomp_size, uint64_t comp_size, bool is_lh, const zu_writer_entry* e) {
    if (zip64) {
        uint16_t header_id = ZU_EXTRA_ZIP64;
        if (is_lh) {
            uint16_t data_len = 16;
            uint64_t sizes[2] = {uncomp_size, comp_size};
            if (zu_write_output(ctx, &header_id, 2) != ZU_STATUS_OK || zu_write_output(ctx, &data_len, 2) != ZU_STATUS_OK || zu_write_output(ctx, sizes, sizeof(sizes)) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
        }
        else {
            uint64_t zip64_vals[3];
            uint16_t zv = 0;
            if (e->uncomp_size >= 0xffffffffu || e->zip64)
                zip64_vals[zv++] = e->uncomp_size;
            if (e->comp_size >= 0xffffffffu || e->zip64)
                zip64_vals[zv++] = e->comp_size;
            if (e->lho_offset >= 0xffffffffu || e->zip64)
                zip64_vals[zv++] = e->lho_offset;

            if (zv > 0) {
                uint16_t data_len = (uint16_t)(zv * sizeof(uint64_t));
                if (zu_write_output(ctx, &header_id, 2) != ZU_STATUS_OK || zu_write_output(ctx, &data_len, 2) != ZU_STATUS_OK ||
                    zu_write_output(ctx, zip64_vals, zv * sizeof(uint64_t)) != ZU_STATUS_OK) {
                    return ZU_STATUS_IO;
                }
            }
        }
    }

    if (!ctx->exclude_extra_attrs) {
        /* UT: Extended Timestamp */
        uint16_t ut_id = 0x5455;
        uint16_t ut_len = 1 + (is_lh ? 3 : 1) * 4;
        uint8_t ut_flags = is_lh ? 0x07 : 0x01; /* mtime, atime, ctime in LH; mtime only in CD */
        if (zu_write_output(ctx, &ut_id, 2) != ZU_STATUS_OK || zu_write_output(ctx, &ut_len, 2) != ZU_STATUS_OK || zu_write_output(ctx, &ut_flags, 1) != ZU_STATUS_OK) {
            return ZU_STATUS_IO;
        }
        uint32_t m = (uint32_t)e->mtime;
        if (zu_write_output(ctx, &m, 4) != ZU_STATUS_OK) {
            return ZU_STATUS_IO;
        }
        if (is_lh) {
            uint32_t a = (uint32_t)e->atime;
            uint32_t c = (uint32_t)e->ctime;
            if (zu_write_output(ctx, &a, 4) != ZU_STATUS_OK || zu_write_output(ctx, &c, 4) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
        }

        /* Ux: New Unix Extra Field (only in Local Header) */
        if (is_lh) {
            uint16_t ux_id = 0x7875;
            uint16_t ux_len = 11;
            uint8_t ux_ver = 1;
            uint8_t uid_size = 4;
            uint8_t gid_size = 4;
            uint32_t uid = e->uid;
            uint32_t gid = e->gid;
            if (zu_write_output(ctx, &ux_id, 2) != ZU_STATUS_OK || zu_write_output(ctx, &ux_len, 2) != ZU_STATUS_OK || zu_write_output(ctx, &ux_ver, 1) != ZU_STATUS_OK ||
                zu_write_output(ctx, &uid_size, 1) != ZU_STATUS_OK || zu_write_output(ctx, &uid, 4) != ZU_STATUS_OK || zu_write_output(ctx, &gid_size, 1) != ZU_STATUS_OK ||
                zu_write_output(ctx, &gid, 4) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
        }
    }

    return ZU_STATUS_OK;
}

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

static int effective_deflate_level(const ZContext* ctx);

static bool copy_mode_keep(const ZContext* ctx, const char* name) {
    if (!ctx || !name) {
        return false;
    }
    int flags = ctx->match_case ? 0 : FNM_CASEFOLD;
    bool match_inputs = ctx->include.len == 0;
    for (size_t i = 0; i < ctx->include.len && !match_inputs; ++i) {
        if (fnmatch(ctx->include.items[i], name, flags) == 0) {
            match_inputs = true;
        }
    }
    if (!match_inputs) {
        return false;
    }
    return zu_should_include(ctx, name);
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

static void make_attrs(const ZContext* ctx, const struct stat* st, bool is_dir, uint32_t* ext_attr, uint16_t* version_made) {
    uint32_t ext = 0;
    uint16_t vmade = (3 << 8) | 20;

    if (ctx->exclude_extra_attrs) {
        ext = 0;
        vmade = 20; /* host = FAT */
    }
    else {
        uint8_t dos_attr = 0;
        if (is_dir)
            dos_attr |= 0x10;
        if (!(st->st_mode & S_IWUSR))
            dos_attr |= 0x01;
        ext = ((uint32_t)st->st_mode << 16) | dos_attr;
    }

    if (ext_attr)
        *ext_attr = ext;
    if (version_made)
        *version_made = vmade;
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

static void update_newest_mtime(ZContext* ctx, time_t t) {
    if (!ctx)
        return;
    if (!ctx->newest_mtime_valid || t > ctx->newest_mtime) {
        ctx->newest_mtime = t;
        ctx->newest_mtime_valid = true;
    }
}

static int apply_mtime(const char* path, time_t t) {
    struct utimbuf times = {
        .actime = t,
        .modtime = t,
    };
    return utime(path, &times);
}

static time_t dos_to_unix_time(uint16_t dos_date, uint16_t dos_time) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = ((dos_date >> 9) & 0x7f) + 80; /* years since 1900 */
    t.tm_mon = ((dos_date >> 5) & 0x0f) - 1;   /* 0-11 */
    t.tm_mday = dos_date & 0x1f;               /* 1-31 */
    t.tm_hour = (dos_time >> 11) & 0x1f;       /* 0-23 */
    t.tm_min = (dos_time >> 5) & 0x3f;         /* 0-59 */
    t.tm_sec = (dos_time & 0x1f) * 2;          /* 0-58 even */
    t.tm_isdst = -1;
    return mktime(&t);
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

static uint32_t current_disk_index(const ZContext* ctx) {
    (void)ctx;
    return 0;
}

static bool file_is_likely_text(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return false;
    uint8_t buf[4096];
    size_t got = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (got == 0)
        return true; /* empty file */
    for (size_t i = 0; i < got; i++) {
        if (buf[i] == 0)
            return false;
    }
    return true;
}

/* Quick in-memory probe to see if deflate would beat store for small files
   This keeps fast-write streaming while still avoiding pointless compression */
static int deflate_outperforms_store(const ZContext* ctx, const zu_input_info* info, const char* path, bool* out_deflate) {
    if (!ctx || !info || !path || !out_deflate) {
        return ZU_STATUS_USAGE;
    }
    *out_deflate = true;

    size_t size = (size_t)info->st.st_size;
    if (size == 0) {
        *out_deflate = false;
        return ZU_STATUS_OK;
    }
    uint8_t* buf = malloc(size);
    if (!buf) {
        return ZU_STATUS_OOM;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        free(buf);
        return ZU_STATUS_IO;
    }

    size_t got = fread(buf, 1, size, fp);
    bool read_error = (got != size) || ferror(fp);
    fclose(fp);
    if (read_error) {
        free(buf);
        return ZU_STATUS_IO;
    }

    uint8_t* comp = NULL;
    size_t comp_len = 0;
    int level = effective_deflate_level(ctx);
    int rc = zu_deflate_buffer(buf, size, level, &comp, &comp_len);
    free(buf);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    *out_deflate = comp_len < size;
    free(comp);
    return ZU_STATUS_OK;
}

static bool should_compress_file(ZContext* ctx, const zu_input_info* info, const char* path) {
    if (!ctx) {
        return false;
    }
    if (ctx->compression_method == 0 || ctx->compression_level == 0) {
        return false;
    }
    if (ctx->line_mode != ZU_LINE_NONE) {
        return false; /* Info-ZIP stores when translating lines */
    }
    if (info && info->is_stdin) {
        return false; /* Streamed stdin entries are stored to avoid data descriptors */
    }
    if (path && should_store_by_suffix(ctx, path)) {
        return false;
    }
    /* Store very small non-text files in fast-write mode to reduce overhead */
    if (ctx->fast_write && info && info->size_known && (uint64_t)info->st.st_size <= (uint64_t)512 && (!path || !file_is_likely_text(path))) {
        return false;
    }
    if (ctx->fast_write && info && info->size_known) {
        if (!info->is_stdin && ctx->compression_method == 8 && ctx->line_mode == ZU_LINE_NONE && (uint64_t)info->st.st_size <= (uint64_t)32 * 1024) {
            bool skip_probe = false;
            /* Text files compress well at any level */
            if (file_is_likely_text(path)) {
                skip_probe = true;
            }
            if (!skip_probe) {
                bool deflate_better = true;
                if (deflate_outperforms_store(ctx, info, path, &deflate_better) == ZU_STATUS_OK && !deflate_better) {
                    return false;
                }
            }
        }
        if (ctx->compression_level <= 1 && (uint64_t)info->st.st_size >= (uint64_t)64 * 1024 && (!path || !file_is_likely_text(path))) {
            return false; /* fast-write level 0/1: store larger incompressible candidates */
        }
    }
    return true;
}

static int effective_deflate_level(const ZContext* ctx) {
    if (!ctx)
        return Z_DEFAULT_COMPRESSION;
    int lvl = (ctx->compression_level < 0 || ctx->compression_level > 9) ? Z_DEFAULT_COMPRESSION : ctx->compression_level;
    if (ctx->fast_write) {
        if (lvl <= 1)
            return 1;
        if (lvl > 3)
            return 3;
    }
    return lvl;
}

static void progress_log(ZContext* ctx, const char* fmt, ...) {
    if (!ctx || !fmt) {
        return;
    }
    if (ctx->quiet) {
        return;
    }
    FILE* stream = ctx->output_to_stdout ? stderr : stdout;
    va_list args;
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);
    if (ctx->log_file) {
        va_start(args, fmt);
        vfprintf(ctx->log_file, fmt, args);
        va_end(args);
        fflush(ctx->log_file);
    }
}

static const char* method_label(uint16_t method) {
    switch (method) {
        case 0:
            return "stored";
        case 12:
            return "bzipped";
        case 8:
        default:
            return "deflated";
    }
}

static int compression_percent(uint64_t comp_size, uint64_t uncomp_size) {
    if (uncomp_size == 0) {
        return 0;
    }
    int64_t delta = (int64_t)uncomp_size - (int64_t)comp_size;
    /* Info-ZIP rounds using integer math with an extra digit of precision */
    int64_t pct_times_ten = ((delta * 1000) / (int64_t)uncomp_size) + 5;
    int pct = (int)(pct_times_ten / 10);
    if (pct < -99)
        pct = -99;
    return pct;
}

static void log_entry_action(ZContext* ctx, const char* action, const char* name, uint16_t method, uint64_t comp_size, uint64_t uncomp_size) {
    if (!ctx || !action || !name) {
        return;
    }
    const char* label = method_label(method);
    int pct = compression_percent(comp_size, uncomp_size);
    progress_log(ctx, "  %s: %s (%s %d%%)\n", action, name, label, pct);
}

static void log_delete_action(ZContext* ctx, const char* name) {
    if (!ctx || !name) {
        return;
    }
    progress_log(ctx, "deleting: %s\n", name);
}

static int zu_write_output(ZContext* ctx, const void* ptr, size_t size) {
    if (!ctx || !ctx->out_file)
        return ZU_STATUS_IO;

    if (size == 0) {
        return ZU_STATUS_OK;
    }

    if (fwrite(ptr, 1, size, ctx->out_file) != size) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write output failed");
        return ZU_STATUS_IO;
    }
    ctx->current_offset += size;
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
        info->st.st_mode = S_IFIFO | 0600;
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
        if (ctx->store_symlinks) {
            info->is_symlink = true;
            info->size_known = true;
            info->st = st;
            char linkbuf[PATH_MAX];
            ssize_t llen = readlink(path, linkbuf, sizeof(linkbuf) - 1);
            if (llen < 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "readlink '%s': %s", path, strerror(errno));
                zu_context_set_error(ctx, ZU_STATUS_IO, msg);
                return ZU_STATUS_IO;
            }
            linkbuf[llen] = '\0';
            info->link_target = strndup(linkbuf, (size_t)llen);
            if (!info->link_target) {
                zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating symlink target failed");
                return ZU_STATUS_OOM;
            }
            info->link_target_len = (size_t)llen;
            info->st.st_size = (off_t)info->link_target_len;
            info->is_stdin = false;
            return ZU_STATUS_OK;
        }

        if (!ctx->allow_symlinks) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "refusing to follow symlink (use -y to store it)");
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

static int compute_crc_and_size(ZContext* ctx, const char* path, uint32_t* crc_out, uint64_t* size_out, bool translate) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }

    uint8_t* buf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    if (!buf) {
        fclose(fp);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating read buffer failed");
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t total = 0;
    size_t got = 0;
    bool prev_cr = false;
    int rc = ZU_STATUS_OK;

    while ((got = fread(buf, 1, ZU_IO_CHUNK, fp)) > 0) {
        if (translate) {
            uint8_t* tbuf = buf;
            size_t tlen = got;
            rc = translate_buffer(ctx, buf, got, &tbuf, &tlen, &prev_cr);
            if (rc != ZU_STATUS_OK) {
                break;
            }
            crc = zu_crc32(tbuf, tlen, crc);
            total += tlen;
            if (tbuf != buf) {
                free(tbuf);
            }
        }
        else {
            crc = zu_crc32(buf, got, crc);
            total += got;
        }
    }

    if (rc == ZU_STATUS_OK && ferror(fp)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "read '%s' failed", path);
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        rc = ZU_STATUS_IO;
    }

    fclose(fp);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    if (crc_out)
        *crc_out = crc;
    if (size_out)
        *size_out = total;
    return ZU_STATUS_OK;
}

static int compress_to_temp(ZContext* ctx, const char* path, int method, int level, FILE** temp_out, uint32_t* crc_out, uint64_t* uncomp_out, uint64_t* comp_out, bool translate) {
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

    uint8_t* inbuf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    uint8_t* outbuf = zu_get_io_buffer2(ctx, ZU_IO_CHUNK);
    if (!inbuf || !outbuf) {
        fclose(in);
        fclose(tmp);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating compression buffers failed");
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t total_in = 0;
    size_t got = 0;
    int rc = ZU_STATUS_OK;
    bool prev_cr = false;

    if (method == 8) { /* Deflate */
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int lvl = effective_deflate_level(ctx);
        int zrc = deflateInit2(&strm, lvl, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        if (zrc != Z_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "compression init failed");
            goto comp_done;
        }

        while ((got = fread(inbuf, 1, ZU_IO_CHUNK, in)) > 0) {
            uint8_t* next_in = inbuf;
            size_t next_len = got;

            if (translate) {
                rc = translate_buffer(ctx, inbuf, got, &next_in, &next_len, &prev_cr);
                if (rc != ZU_STATUS_OK) {
                    deflateEnd(&strm);
                    goto comp_done;
                }
            }

            crc = zu_crc32(next_in, next_len, crc);
            total_in += next_len;
            strm.next_in = next_in;
            strm.avail_in = (uInt)next_len;
            do {
                strm.next_out = outbuf;
                strm.avail_out = (uInt)ZU_IO_CHUNK;
                zrc = deflate(&strm, Z_NO_FLUSH);
                if (zrc != Z_OK) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "compression failed");
                    deflateEnd(&strm);
                    if (translate && next_in != inbuf)
                        free(next_in);
                    goto comp_done;
                }
                size_t have = ZU_IO_CHUNK - strm.avail_out;
                if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "write compressed data failed");
                    deflateEnd(&strm);
                    if (translate && next_in != inbuf)
                        free(next_in);
                    goto comp_done;
                }
            } while (strm.avail_out == 0);

            if (translate && next_in != inbuf) {
                free(next_in);
            }
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
            uint8_t* next_in = inbuf;
            size_t next_len = got;

            if (translate) {
                rc = translate_buffer(ctx, inbuf, got, &next_in, &next_len, &prev_cr);
                if (rc != ZU_STATUS_OK) {
                    BZ2_bzCompressEnd(&strm);
                    goto comp_done;
                }
            }

            crc = zu_crc32(next_in, next_len, crc);
            total_in += next_len;
            strm.next_in = (char*)next_in;
            strm.avail_in = (unsigned int)next_len;
            do {
                strm.next_out = (char*)outbuf;
                strm.avail_out = (unsigned int)ZU_IO_CHUNK;
                bzrc = BZ2_bzCompress(&strm, BZ_RUN);
                if (bzrc != BZ_RUN_OK) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "bzip2 compression failed");
                    BZ2_bzCompressEnd(&strm);
                    if (translate && next_in != inbuf)
                        free(next_in);
                    goto comp_done;
                }
                size_t have = ZU_IO_CHUNK - strm.avail_out;
                if (have > 0 && fwrite(outbuf, 1, have, tmp) != have) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "write compressed data failed");
                    BZ2_bzCompressEnd(&strm);
                    if (translate && next_in != inbuf)
                        free(next_in);
                    goto comp_done;
                }
            } while (strm.avail_out == 0);

            if (translate && next_in != inbuf) {
                free(next_in);
            }
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

static int write_file_data(ZContext* ctx, const char* path, FILE* staged, uint64_t expected_size, zu_zipcrypto_ctx* zc, bool translate) {
    FILE* src = staged ? staged : fopen(path, "rb");
    if (!src) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, msg);
        return ZU_STATUS_IO;
    }
    uint8_t* buf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    if (!buf) {
        if (!staged)
            fclose(src);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating write buffer failed");
        return ZU_STATUS_OOM;
    }
    uint64_t written = 0;
    size_t got = 0;
    int rc = ZU_STATUS_OK;
    bool prev_cr = false;

    while ((got = fread(buf, 1, ZU_IO_CHUNK, src)) > 0) {
        uint8_t* next_out = buf;
        size_t next_len = got;

        if (!staged && translate) {
            rc = translate_buffer(ctx, buf, got, &next_out, &next_len, &prev_cr);
            if (rc != ZU_STATUS_OK) {
                break;
            }
        }

        if (zc) {
            zu_zipcrypto_encrypt(zc, next_out, next_len);
        }
        if (zu_write_output(ctx, next_out, next_len) != ZU_STATUS_OK) {
            rc = ZU_STATUS_IO;
            if (!staged && translate && next_out != buf)
                free(next_out);
            break;
        }
        written += next_len;

        if (!staged && translate && next_out != buf) {
            free(next_out);
        }
    }

    if (rc != ZU_STATUS_OK) {
        if (!staged)
            fclose(src);
        return rc;
    }

    if (ferror(src)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "read failed while writing output");
        if (!staged)
            fclose(src);
        return ZU_STATUS_IO;
    }
    if (!staged)
        fclose(src);
    if (expected_size != UINT64_MAX && written != expected_size) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "input size changed during write");
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static int write_symlink_data(ZContext* ctx, const char* target, size_t len, zu_zipcrypto_ctx* zc) {
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > ZU_IO_CHUNK)
            chunk = ZU_IO_CHUNK;
        uint8_t buf[ZU_IO_CHUNK];
        memcpy(buf, target + written, chunk);
        if (zc) {
            zu_zipcrypto_encrypt(zc, buf, chunk);
        }
        if (zu_write_output(ctx, buf, chunk) != ZU_STATUS_OK) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "write symlink target failed");
            return ZU_STATUS_IO;
        }
        written += chunk;
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

typedef struct {
    char* path;
    FILE* file;
    uint64_t size;
    uint32_t crc32;
    bool is_text;
} zu_stdin_stage;

static void free_stdin_stage(zu_stdin_stage* staged) {
    if (!staged)
        return;
    if (staged->file)
        fclose(staged->file);
    if (staged->path) {
        unlink(staged->path);
        free(staged->path);
    }
    staged->file = NULL;
    staged->path = NULL;
    staged->size = 0;
    staged->crc32 = 0;
    staged->is_text = false;
}

static int copy_fast_path(ZContext* ctx, uint64_t start_offset, uint64_t total_len, uint64_t* written_out) {
#ifdef __linux__
    if (!ctx || !ctx->in_file || !ctx->out_file)
        return ZU_STATUS_USAGE;

    int infd = fileno(ctx->in_file);
    int outfd = fileno(ctx->out_file);
    if (infd < 0 || outfd < 0)
        return ZU_STATUS_NOT_IMPLEMENTED;

    if (fflush(ctx->out_file) != 0)
        return ZU_STATUS_IO;

    off_t in_off = (off_t)start_offset;
    off_t out_start = lseek(outfd, 0, SEEK_CUR);
    if (out_start < 0)
        return ZU_STATUS_NOT_IMPLEMENTED;

    uint64_t remaining = total_len;
    while (remaining > 0) {
        size_t to_copy = remaining > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)remaining;
        ssize_t sent = sendfile(outfd, infd, &in_off, to_copy);
        if (sent < 0) {
            if (errno == ENOSYS || errno == EINVAL || errno == EXDEV)
                return ZU_STATUS_NOT_IMPLEMENTED;
            zu_context_set_error(ctx, ZU_STATUS_IO, "sendfile failed");
            return ZU_STATUS_IO;
        }
        if (sent == 0) {
            /* EOF reached before expected size */
            zu_context_set_error(ctx, ZU_STATUS_IO, "short read during sendfile");
            return ZU_STATUS_IO;
        }
        remaining -= (uint64_t)sent;
        ctx->current_offset += (uint64_t)sent;
    }

    if (fseeko(ctx->out_file, (off_t)ctx->current_offset, SEEK_SET) != 0)
        return ZU_STATUS_IO;
    if (fseeko(ctx->in_file, in_off, SEEK_SET) != 0)
        return ZU_STATUS_IO;

    if (written_out)
        *written_out = total_len;
    return ZU_STATUS_OK;
#else
    (void)ctx;
    (void)start_offset;
    (void)total_len;
    (void)written_out;
    return ZU_STATUS_NOT_IMPLEMENTED;
#endif
}

static int stage_stdin_to_temp(ZContext* ctx, zu_stdin_stage* staged) {
    if (!ctx || !staged) {
        return ZU_STATUS_USAGE;
    }
    memset(staged, 0, sizeof(*staged));

    const char* dir = ctx->temp_dir ? ctx->temp_dir : P_tmpdir;
    if (!dir || *dir == '\0') {
        dir = ".";
    }
    char tmpl[PATH_MAX];
    int n = snprintf(tmpl, sizeof(tmpl), "%s/zipstdin-XXXXXX", dir);
    if (n <= 0 || (size_t)n >= sizeof(tmpl)) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "temp path too long for stdin staging");
        return ZU_STATUS_IO;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "creating temp file for stdin failed");
        return ZU_STATUS_IO;
    }
    FILE* fp = fdopen(fd, "wb+");
    if (!fp) {
        close(fd);
        unlink(tmpl);
        zu_context_set_error(ctx, ZU_STATUS_IO, "opening temp file for stdin failed");
        return ZU_STATUS_IO;
    }

    uint8_t* buf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    if (!buf) {
        fclose(fp);
        unlink(tmpl);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating stdin staging buffer failed");
        return ZU_STATUS_OOM;
    }

    bool prev_cr = false;
    uint64_t total = 0;
    uint32_t crc = 0;
    staged->is_text = true;
    int rc = ZU_STATUS_OK;

    size_t got = 0;
    while ((got = fread(buf, 1, ZU_IO_CHUNK, stdin)) > 0) {
        uint8_t* translated = buf;
        size_t translated_len = got;
        if (ctx->line_mode != ZU_LINE_NONE) {
            rc = translate_buffer(ctx, buf, got, &translated, &translated_len, &prev_cr);
            if (rc != ZU_STATUS_OK) {
                break;
            }
        }
        if (staged->is_text) {
            for (size_t i = 0; i < translated_len; ++i) {
                if (translated[i] == 0) {
                    staged->is_text = false;
                    break;
                }
            }
        }
        crc = zu_crc32(translated, translated_len, crc);
        total += translated_len;
        if (fwrite(translated, 1, translated_len, fp) != translated_len) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "write to stdin temp failed");
        }
        if (translated != buf) {
            free(translated);
        }
        if (rc != ZU_STATUS_OK) {
            break;
        }
    }

    if (rc == ZU_STATUS_OK && ferror(stdin)) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "read from stdin failed");
    }
    if (rc == ZU_STATUS_OK && fflush(fp) != 0) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "flush stdin temp failed");
    }
    if (rc == ZU_STATUS_OK && fseeko(fp, 0, SEEK_SET) != 0) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "rewind stdin temp failed");
    }

    if (rc != ZU_STATUS_OK) {
        fclose(fp);
        unlink(tmpl);
        return rc;
    }

    staged->path = strdup(tmpl);
    staged->file = fp;
    staged->size = total;
    staged->crc32 = crc;
    return ZU_STATUS_OK;
}

static int write_streaming_entry(ZContext* ctx,
                                 const char* path,
                                 const char* stored,
                                 const zu_input_info* info,
                                 uint16_t dos_time,
                                 uint16_t dos_date,
                                 uint64_t entry_lho_offset,
                                 uint32_t entry_disk_start,
                                 uint64_t zip64_trigger,
                                 const zu_existing_entry* existing,
                                 uint64_t* offset,
                                 zu_entry_list* entries) {
    if (!ctx || !stored || !info || !offset || !entries) {
        return ZU_STATUS_USAGE;
    }

    bool size_unknown = !info->size_known;
    uint64_t size_hint = info->size_known ? (uint64_t)info->st.st_size : 0;
    bool compress = should_compress_file(ctx, info, path);
    if (info->size_known && info->st.st_size == 0) {
        compress = false;
    }
    uint16_t method = compress ? ctx->compression_method : 0;
    uint16_t flags = 0x0008; /* data descriptor */

    uint32_t ext_attr = 0;
    uint16_t version_made = 20;
    make_attrs(ctx, &info->st, S_ISDIR(info->st.st_mode), &ext_attr, &version_made);

    if (ctx->encrypt && ctx->password) {
        flags |= 1;
    }

    size_t name_len = strlen(stored);

    bool header_zip64 = size_unknown || *offset >= zip64_trigger || size_hint >= zip64_trigger;

    uint16_t version_needed = header_zip64 ? 45 : (method == 0 ? 10 : 20);

    uint16_t extra_len = get_extra_len(ctx, header_zip64, true, NULL);

    zu_local_header lho = {
        .signature = ZU_SIG_LOCAL,
        .version_needed = version_needed,
        .flags = flags,
        .method = method,
        .mod_time = dos_time,
        .mod_date = dos_date,
        .crc32 = 0,
        .comp_size = header_zip64 ? 0xffffffffu : 0,
        .uncomp_size = header_zip64 ? 0xffffffffu : 0,
        .name_len = (uint16_t)name_len,
        .extra_len = extra_len,
    };

    if (zu_write_output(ctx, &lho, sizeof(lho)) != ZU_STATUS_OK || zu_write_output(ctx, stored, name_len) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write local header failed");
        return ZU_STATUS_IO;
    }

    zu_writer_entry tmp_e = {
        .mtime = info->st.st_mtime,
        .atime = info->st.st_atime,
        .ctime = info->st.st_ctime,
        .uid = (uint32_t)info->st.st_uid,
        .gid = (uint32_t)info->st.st_gid,
    };

    if (write_extra_fields(ctx, header_zip64, 0, 0, true, &tmp_e) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write extra fields failed");
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
        header[11] = (uint8_t)(dos_time >> 8);
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

    uint8_t* inbuf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    uint8_t* outbuf = compress ? zu_get_io_buffer2(ctx, ZU_IO_CHUNK) : NULL;
    if (!inbuf || (compress && !outbuf)) {
        if (src != stdin)
            fclose(src);
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating streaming buffers failed");
        return ZU_STATUS_OOM;
    }

    int rc = ZU_STATUS_OK;
    bool prev_cr = false;

    if (!compress) {
        size_t got = 0;
        while ((got = fread(inbuf, 1, ZU_IO_CHUNK, src)) > 0) {
            uint8_t* translated = inbuf;
            size_t translated_len = got;
            if (ctx->line_mode != ZU_LINE_NONE) {
                rc = translate_buffer(ctx, inbuf, got, &translated, &translated_len, &prev_cr);
                if (rc != ZU_STATUS_OK)
                    break;
            }
            crc = zu_crc32(translated, translated_len, crc);
            uncomp_size += translated_len;
            if (pzc) {
                zu_zipcrypto_encrypt(pzc, translated, translated_len);
            }
            if (zu_write_output(ctx, translated, translated_len) != ZU_STATUS_OK) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "write output failed");
            }
            if (translated != inbuf)
                free(translated);
            if (rc != ZU_STATUS_OK)
                break;
            comp_size += translated_len;
        }
        if (rc == ZU_STATUS_OK && ferror(src)) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "read failed during streaming copy");
        }
    }
    else if (method == 8) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int lvl = effective_deflate_level(ctx);
        int zrc = deflateInit2(&strm, lvl, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        if (zrc != Z_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "compression init failed");
        }
        else {
            size_t got = 0;
            while ((got = fread(inbuf, 1, ZU_IO_CHUNK, src)) > 0) {
                uint8_t* translated = inbuf;
                size_t translated_len = got;
                if (ctx->line_mode != ZU_LINE_NONE) {
                    rc = translate_buffer(ctx, inbuf, got, &translated, &translated_len, &prev_cr);
                    if (rc != ZU_STATUS_OK)
                        break;
                }
                crc = zu_crc32(translated, translated_len, crc);
                uncomp_size += translated_len;
                strm.next_in = translated;
                strm.avail_in = (uInt)translated_len;
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
                if (translated != inbuf)
                    free(translated);
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
                    size_t have = ZU_IO_CHUNK - (size_t)strm.avail_out;
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
                    size_t have = ZU_IO_CHUNK - (size_t)strm.avail_out;
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
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    bool need_zip64 = header_zip64 || comp_size >= zip64_trigger || uncomp_size >= zip64_trigger || *offset >= zip64_trigger;
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
    entries->items[entries->len].lho_offset = entry_lho_offset;
    entries->items[entries->len].method = method;
    entries->items[entries->len].mod_time = dos_time;
    entries->items[entries->len].mod_date = dos_date;
    entries->items[entries->len].ext_attr = ext_attr;
    uint16_t made_field = need_zip64 ? (uint16_t)((version_made & 0xff00u) | 45) : version_made;
    entries->items[entries->len].version_made = made_field;
    entries->items[entries->len].int_attr = 0;
    entries->items[entries->len].zip64 = need_zip64;
    entries->items[entries->len].flags = flags;
    entries->items[entries->len].disk_start = entry_disk_start;
    entries->items[entries->len].comment = NULL;
    entries->items[entries->len].comment_len = 0;
    entries->items[entries->len].mtime = info->st.st_mtime;
    entries->items[entries->len].atime = info->st.st_atime;
    entries->items[entries->len].ctime = info->st.st_ctime;
    entries->items[entries->len].uid = (uint32_t)info->st.st_uid;
    entries->items[entries->len].gid = (uint32_t)info->st.st_gid;
    if (existing && existing->comment && existing->comment_len > 0) {
        entries->items[entries->len].comment = malloc(existing->comment_len);
        if (!entries->items[entries->len].comment) {
            zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating entry comment failed");
            free(entries->items[entries->len].name);
            entries->items[entries->len].name = NULL;
            return ZU_STATUS_OOM;
        }
        memcpy(entries->items[entries->len].comment, existing->comment, existing->comment_len);
        entries->items[entries->len].comment_len = existing->comment_len;
    }
    entries->len++;

    log_entry_action(ctx, "adding", stored, method, comp_size, uncomp_size);

    uint64_t header_len = sizeof(lho) + name_len + extra_len;
    *offset += header_len + comp_size + desc_len;
    return ZU_STATUS_OK;
}

static int write_stdin_staged_entry(ZContext* ctx,
                                    const char* stored,
                                    const zu_input_info* info,
                                    uint16_t dos_time,
                                    uint16_t dos_date,
                                    uint64_t entry_lho_offset,
                                    uint32_t entry_disk_start,
                                    const zu_existing_entry* existing,
                                    uint64_t* offset,
                                    zu_entry_list* entries) {
    if (!ctx || !stored || !info || !offset || !entries) {
        return ZU_STATUS_USAGE;
    }

    zu_stdin_stage staged = {0};
    int rc = stage_stdin_to_temp(ctx, &staged);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    zu_input_info staged_info = *info;
    staged_info.size_known = true;
    staged_info.is_stdin = false;
    staged_info.st.st_size = (off_t)staged.size;

    bool compress = should_compress_file(ctx, &staged_info, stored);
    uint16_t method = compress ? ctx->compression_method : 0;
    if (method == 0) {
        compress = false;
    }

    uint64_t uncomp_size = staged.size;
    uint64_t comp_size = staged.size;
    uint32_t crc = staged.crc32;
    FILE* payload = staged.file;
    FILE* staged_comp = NULL;

    if (compress) {
        uint32_t crc_tmp = 0;
        uint64_t uncomp_tmp = 0, comp_tmp = 0;
        rc = compress_to_temp(ctx, staged.path, method, ctx->compression_level, &staged_comp, &crc_tmp, &uncomp_tmp, &comp_tmp, false);
        if (rc != ZU_STATUS_OK) {
            free_stdin_stage(&staged);
            return rc;
        }
        crc = crc_tmp;
        uncomp_size = uncomp_tmp;
        comp_size = comp_tmp;
        payload = staged_comp;
        if (comp_size >= uncomp_size) {
            fclose(staged_comp);
            staged_comp = NULL;
            payload = staged.file;
            comp_size = uncomp_size;
            method = 0;
            compress = false;
        }
    }

    zu_zipcrypto_ctx zc;
    zu_zipcrypto_ctx* pzc = NULL;
    uint16_t flags = 0;
    uint64_t payload_size = comp_size;
    if (ctx->encrypt && ctx->password) {
        flags |= 1;
        comp_size += 12;
    }

    uint64_t zip64_trigger = zip64_trigger_bytes();
    bool zip64_lho = (uncomp_size >= zip64_trigger || comp_size >= zip64_trigger || entry_lho_offset >= zip64_trigger);

    uint32_t ext_attr = 0;
    uint16_t version_made = 20;
    make_attrs(ctx, &info->st, S_ISDIR(info->st.st_mode), &ext_attr, &version_made);
    version_made = (uint16_t)((version_made & 0xff00u) | (zip64_lho ? 45 : 30));
    uint16_t int_attr = staged.is_text ? 1 : 0;

    size_t name_len = strlen(stored);
    uint16_t extra_len = get_extra_len(ctx, zip64_lho, true, NULL);

    zu_local_header lho = {
        .signature = ZU_SIG_LOCAL,
        .version_needed = zip64_lho ? 45 : (method == 0 ? 10 : 20),
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

    if (zu_write_output(ctx, &lho, sizeof(lho)) != ZU_STATUS_OK || zu_write_output(ctx, stored, name_len) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write local header failed");
        rc = ZU_STATUS_IO;
        goto cleanup;
    }

    zu_writer_entry tmp_e = {
        .mtime = info->st.st_mtime,
        .atime = info->st.st_atime,
        .ctime = info->st.st_ctime,
        .uid = (uint32_t)info->st.st_uid,
        .gid = (uint32_t)info->st.st_gid,
    };

    if (write_extra_fields(ctx, zip64_lho, uncomp_size, comp_size, true, &tmp_e) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "write extra fields failed");
        rc = ZU_STATUS_IO;
        goto cleanup;
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
            goto cleanup;
        }
    }

    if (payload && fseeko(payload, 0, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "rewind staged data failed");
        rc = ZU_STATUS_IO;
        goto cleanup;
    }

    rc = write_file_data(ctx, staged.path, payload, payload_size, pzc, false);
    if (rc != ZU_STATUS_OK) {
        goto cleanup;
    }

    if (ensure_entry_capacity(entries) != ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating entry list failed");
        rc = ZU_STATUS_OOM;
        goto cleanup;
    }

    entries->items[entries->len].name = strdup(stored);
    entries->items[entries->len].crc32 = crc;
    entries->items[entries->len].comp_size = comp_size;
    entries->items[entries->len].uncomp_size = uncomp_size;
    entries->items[entries->len].lho_offset = entry_lho_offset;
    entries->items[entries->len].method = method;
    entries->items[entries->len].mod_time = dos_time;
    entries->items[entries->len].mod_date = dos_date;
    entries->items[entries->len].ext_attr = ext_attr;
    entries->items[entries->len].version_made = version_made;
    entries->items[entries->len].int_attr = int_attr;
    entries->items[entries->len].zip64 = zip64_lho;
    entries->items[entries->len].flags = flags;
    entries->items[entries->len].disk_start = entry_disk_start;
    entries->items[entries->len].comment = NULL;
    entries->items[entries->len].comment_len = 0;
    entries->items[entries->len].mtime = info->st.st_mtime;
    entries->items[entries->len].atime = info->st.st_atime;
    entries->items[entries->len].ctime = info->st.st_ctime;
    entries->items[entries->len].uid = (uint32_t)info->st.st_uid;
    entries->items[entries->len].gid = (uint32_t)info->st.st_gid;
    if (existing && existing->comment && existing->comment_len > 0) {
        entries->items[entries->len].comment = malloc(existing->comment_len);
        if (!entries->items[entries->len].comment) {
            zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating entry comment failed");
            free(entries->items[entries->len].name);
            entries->items[entries->len].name = NULL;
            rc = ZU_STATUS_OOM;
            goto cleanup;
        }
        memcpy(entries->items[entries->len].comment, existing->comment, existing->comment_len);
        entries->items[entries->len].comment_len = existing->comment_len;
    }
    entries->len++;

    log_entry_action(ctx, "adding", stored, method, comp_size, uncomp_size);

    uint64_t header_len = sizeof(lho) + name_len + extra_len;
    *offset += header_len + comp_size;
    rc = ZU_STATUS_OK;

cleanup:
    if (payload && payload == staged_comp) {
        fclose(payload);
    }
    free_stdin_stage(&staged);
    return rc;
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

    uint64_t header_len = sizeof(lho) + lho.name_len + lho.extra_len;
    uint64_t total_to_copy = header_len + e->comp_size;

    if (!ctx->exclude_extra_attrs) {
        int fast_rc = copy_fast_path(ctx, e->lho_offset, total_to_copy, written_out);
        if (fast_rc == ZU_STATUS_OK)
            return ZU_STATUS_OK;
        if (fast_rc != ZU_STATUS_NOT_IMPLEMENTED)
            return fast_rc;

        if (fseeko(ctx->in_file, (off_t)e->lho_offset, SEEK_SET) != 0) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "seek to old LHO start failed");
            return ZU_STATUS_IO;
        }
        uint8_t* buf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
        if (!buf)
            return ZU_STATUS_OOM;
        (void)posix_fadvise(fileno(ctx->in_file), (off_t)e->lho_offset, (off_t)total_to_copy, POSIX_FADV_SEQUENTIAL);

        uint64_t remaining = total_to_copy;
        while (remaining > 0) {
            size_t to_read = remaining > ZU_IO_CHUNK ? ZU_IO_CHUNK : (size_t)remaining;
            size_t got = fread(buf, 1, to_read, ctx->in_file);
            if (got != to_read) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "short read during entry copy");
                return ZU_STATUS_IO;
            }
            if (zu_write_output(ctx, buf, got) != ZU_STATUS_OK) {
                return ZU_STATUS_IO;
            }
            remaining -= got;
        }

        if (written_out)
            *written_out = total_to_copy;
        return ZU_STATUS_OK;
    }

    size_t name_len = lho.name_len;
    size_t extra_len = lho.extra_len;
    char* name_buf = NULL;
    uint8_t* extra_buf = NULL;
    uint8_t* filtered_extra = NULL;
    uint8_t* buf = NULL;
    int rc = ZU_STATUS_OK;

    if (name_len > 0) {
        name_buf = malloc(name_len);
        if (!name_buf) {
            return ZU_STATUS_OOM;
        }
        if (fread(name_buf, 1, name_len, ctx->in_file) != name_len) {
            free(name_buf);
            zu_context_set_error(ctx, ZU_STATUS_IO, "read old filename failed");
            return ZU_STATUS_IO;
        }
    }

    if (extra_len > 0) {
        extra_buf = malloc(extra_len);
        if (!extra_buf) {
            free(name_buf);
            return ZU_STATUS_OOM;
        }
        if (fread(extra_buf, 1, extra_len, ctx->in_file) != extra_len) {
            free(name_buf);
            free(extra_buf);
            zu_context_set_error(ctx, ZU_STATUS_IO, "read old extra failed");
            return ZU_STATUS_IO;
        }
    }

    uint16_t filtered_len = (uint16_t)extra_len;
    if (extra_len > 0) {
        rc = filter_extra_for_exclude(extra_buf, (uint16_t)extra_len, &filtered_extra, &filtered_len);
        if (rc != ZU_STATUS_OK) {
            free(name_buf);
            free(extra_buf);
            zu_context_set_error(ctx, rc, "filter extra failed");
            return rc;
        }
    }

    lho.extra_len = filtered_len;

    if (zu_write_output(ctx, &lho, sizeof(lho)) != ZU_STATUS_OK) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "write filtered LHO failed");
        goto cleanup;
    }
    if (name_len > 0 && zu_write_output(ctx, name_buf, name_len) != ZU_STATUS_OK) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "write filtered name failed");
        goto cleanup;
    }
    if (filtered_len > 0 && zu_write_output(ctx, filtered_extra, filtered_len) != ZU_STATUS_OK) {
        rc = ZU_STATUS_IO;
        zu_context_set_error(ctx, rc, "write filtered extra failed");
        goto cleanup;
    }

    buf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    if (!buf) {
        rc = ZU_STATUS_OOM;
        zu_context_set_error(ctx, rc, "allocating copy buffer failed");
        goto cleanup;
    }

    uint64_t remaining = e->comp_size;
    while (remaining > 0) {
        size_t to_read = remaining > ZU_IO_CHUNK ? ZU_IO_CHUNK : (size_t)remaining;
        size_t got = fread(buf, 1, to_read, ctx->in_file);
        if (got != to_read) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "short read during entry data copy");
            goto cleanup;
        }
        if (zu_write_output(ctx, buf, got) != ZU_STATUS_OK) {
            rc = ZU_STATUS_IO;
            goto cleanup;
        }
        remaining -= got;
    }

    if (written_out)
        *written_out = sizeof(lho) + name_len + filtered_len + e->comp_size;

cleanup:
    free(name_buf);
    free(filtered_extra);
    free(extra_buf);
    return rc;
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

        bool entry_zip64 = e->zip64;
        bool need_uncomp64 = entry_zip64 || e->uncomp_size >= 0xffffffffu;
        bool need_comp64 = entry_zip64 || e->comp_size >= 0xffffffffu;
        bool need_off64 = entry_zip64 || e->lho_offset >= 0xffffffffu;
        bool need_zip64_extra = need_uncomp64 || need_comp64 || need_off64;
        if (entry_zip64 || need_zip64_extra)
            needs_zip64 = true;

        uint32_t comp32 = need_comp64 ? 0xffffffffu : (uint32_t)e->comp_size;
        uint32_t uncomp32 = need_uncomp64 ? 0xffffffffu : (uint32_t)e->uncomp_size;
        uint32_t offset32 = need_off64 ? 0xffffffffu : (uint32_t)e->lho_offset;

        uint16_t extra_len = get_extra_len(ctx, need_zip64_extra, false, e);
        uint16_t version_needed = (entry_zip64 || need_zip64_extra) ? 45 : (e->method == 0 ? 10 : 20);
        uint16_t comment_len = e->comment_len;
        uint16_t disk_start = (uint16_t)((e->disk_start > 0xffffu) ? 0xffffu : e->disk_start);

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
            .disk_start = disk_start,
            .int_attr = e->int_attr,
            .ext_attr = e->ext_attr,
            .lho_offset = offset32,
        };

        if (zu_write_output(ctx, &ch, sizeof(ch)) != ZU_STATUS_OK || zu_write_output(ctx, e->name, name_len) != ZU_STATUS_OK) {
            return ZU_STATUS_IO;
        }

        if (write_extra_fields(ctx, need_zip64_extra, e->uncomp_size, e->comp_size, false, e) != ZU_STATUS_OK) {
            return ZU_STATUS_IO;
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
    (void)ctx;
    (void)needs_zip64;

    uint16_t disk_id = 0;
    uint16_t entry_count = entries->len > 0xffff ? 0xffff : (uint16_t)entries->len;
    zu_end_central endrec = {
        .signature = ZU_SIG_END,
        .disk_num = disk_id,
        .disk_start = disk_id,
        .entries_disk = entry_count,
        .entries_total = entry_count,
        .cd_size = (cd_size > UINT32_MAX) ? 0xffffffffu : (uint32_t)cd_size,
        .cd_offset = (cd_offset > UINT32_MAX) ? 0xffffffffu : (uint32_t)cd_offset,
        .comment_len = comment_len,
    };

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
    (void)ctx;
    uint32_t disk_count = 1;
    uint16_t disk_id = 0;

    uint16_t version_made = 45;
    for (size_t i = 0; i < entries->len; ++i) {
        if (entries->items[i].version_made > version_made) {
            version_made = entries->items[i].version_made;
        }
    }

    zu_end_central64 end64 = {
        .signature = ZU_SIG_END64,
        .size = (uint64_t)(sizeof(zu_end_central64) - 12),
        .version_made = version_made,
        .version_needed = 45,
        .disk_num = disk_id,
        .disk_start = disk_id,
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
        .disk_num = disk_id,
        .eocd64_offset = cd_offset + cd_size,
        .total_disks = disk_count,
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
    ctx->current_offset = 0;
    char* temp_path = NULL;
    const char* target_path = ctx->output_path ? ctx->output_path : ctx->archive_path;
    int rc = ZU_STATUS_OK;

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

    size_t copy_selected = 0;
    if (ctx->copy_mode) {
        if (!ctx->output_path) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "copy mode requires --out");
            return ZU_STATUS_USAGE;
        }
        if (!existing_loaded) {
            zu_context_set_error(ctx, ZU_STATUS_USAGE, "copy mode requires an existing archive");
            return ZU_STATUS_USAGE;
        }
        for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
            zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
            bool keep = copy_mode_keep(ctx, e->name);
            e->delete = !keep;
            e->changed = keep || e->changed;
            if (keep) {
                copy_selected++;
            }
        }
    }

    size_t delete_selected = 0;
    if (ctx->difference_mode) {
        bool time_filter_applied = ctx->has_filter_before || ctx->has_filter_after;
        for (size_t i = 0; i < ctx->include.len; ++i) {
            const char* pattern = ctx->include.items[i];
            for (size_t j = 0; j < ctx->existing_entries.len; ++j) {
                zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[j];
                if (!e->delete && fnmatch(pattern, e->name, 0) == 0) {
                    if (time_filter_applied) {
                        time_t mtime = dos_to_unix_time(e->hdr.mod_date, e->hdr.mod_time);
                        if (ctx->has_filter_after && mtime < ctx->filter_after) {
                            continue;
                        }
                        if (ctx->has_filter_before && mtime >= ctx->filter_before) {
                            continue;
                        }
                    }
                    e->delete = true;
                    e->changed = true;
                    delete_selected++;
                    log_delete_action(ctx, e->name);
                }
            }
        }
        if (delete_selected == 0) {
            rc = ZU_STATUS_NO_FILES;
            if (!ctx->quiet && target_path) {
                printf("\nzip error: Nothing to do! (%s)\n", target_path);
            }
            goto cleanup;
        }
    }
    else if (!ctx->copy_mode) {
        if (zu_expand_args(ctx) != ZU_STATUS_OK) {
            return ZU_STATUS_OOM;
        }

        if (ctx->filesync) {
            for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
                zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
                if (e->delete)
                    continue;

                struct stat st2;
                if (lstat(e->name, &st2) != 0) {
                    e->delete = true;
                    e->changed = true;
                    log_delete_action(ctx, e->name);
                }
            }
        }
    }

    if (!ctx->dry_run) {
        if (ctx->output_to_stdout) {
            ctx->out_file = stdout;
            ctx->current_offset = 0;
        }
        else {
            temp_path = make_temp_path(ctx, target_path);
            if (!temp_path) {
                return ZU_STATUS_OOM;
            }

            ctx->out_file = fopen(temp_path, "wb");
            if (!ctx->out_file) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "create temp file failed");
                free(temp_path);
                return ZU_STATUS_IO;
            }
            ctx->current_offset = 0;
        }
    }

    zu_entry_list entries = {0};
    uint64_t offset = 0;
    size_t added = 0;
    bool skipped_by_update = false;
    uint64_t zip64_trigger = zip64_trigger_bytes();
    if (ctx->copy_mode) {
        added = copy_selected;
    }

    if (!ctx->difference_mode && !ctx->copy_mode) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            const char* path = ctx->include.items[i];

            if (!zu_should_include(ctx, path)) {
                if (ctx->verbose || ctx->log_info || ctx->dry_run)
                    zu_log(ctx, "skipping %s (excluded)\n", path);
                continue;
            }

            const char* stored = ctx->store_paths ? path : basename_component(path);
            char* allocated = NULL;
            const char* entry_name = stored;

            zu_input_info info;
            if (describe_input(ctx, path, &info) != ZU_STATUS_OK) {
                if (ctx->verbose || ctx->log_info || ctx->dry_run)
                    zu_log(ctx, "zip: %s not found or not readable\n", path);
                continue;
            }
            bool is_symlink = info.is_symlink;

            if (S_ISDIR(info.st.st_mode)) {
                if (ctx->no_dir_entries) {
                    free(allocated);
                    if (info.link_target)
                        free(info.link_target);
                    continue;
                }
                size_t len = strlen(stored);
                if (len == 0 || stored[len - 1] != '/') {
                    allocated = malloc(len + 2);
                    if (!allocated) {
                        zu_context_set_error(ctx, ZU_STATUS_OOM, "out of memory");
                        rc = ZU_STATUS_OOM;
                        if (info.link_target)
                            free(info.link_target);
                        goto cleanup;
                    }
                    memcpy(allocated, stored, len);
                    allocated[len] = '/';
                    allocated[len + 1] = '\0';
                    entry_name = allocated;
                }
            }

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

            if (ctx->has_filter_after && info.st.st_mtime < ctx->filter_after) {
                if (ctx->verbose || ctx->log_info || ctx->dry_run)
                    zu_log(ctx, "skipping %s (older than -t)\n", path);
                if (info.link_target)
                    free(info.link_target);
                free(allocated);
                continue;
            }
            if (ctx->has_filter_before && info.st.st_mtime >= ctx->filter_before) {
                if (ctx->verbose || ctx->log_info || ctx->dry_run)
                    zu_log(ctx, "skipping %s (newer than -tt)\n", path);
                if (info.link_target)
                    free(info.link_target);
                free(allocated);
                continue;
            }

            if (existing) {
                if (ctx->freshen || ctx->update) {
                    uint16_t dos_time = 0, dos_date = 0;
                    msdos_datetime(&info.st, &dos_time, &dos_date);

                    bool process_entry = true;

                    if (ctx->filesync) {
                        bool time_differs = (dos_date != existing->hdr.mod_date || dos_time != existing->hdr.mod_time);
                        bool size_differs = (info.st.st_size != (off_t)existing->hdr.uncomp_size);
                        if (!time_differs && !size_differs) {
                            process_entry = false;
                        }
                    }
                    else {
                        if (dos_date < existing->hdr.mod_date || (dos_date == existing->hdr.mod_date && dos_time <= existing->hdr.mod_time)) {
                            process_entry = false;
                        }
                    }

                    if (!process_entry) {
                        if (ctx->verbose || ctx->log_info || ctx->dry_run)
                            zu_log(ctx, "skipping %s (not newer/changed)\n", path);
                        skipped_by_update = true;
                        if (info.link_target)
                            free(info.link_target);
                        free(allocated);
                        continue;
                    }
                }
                existing->delete = true;
                existing->changed = true;
                if (ctx->verbose || ctx->log_info || ctx->dry_run)
                    zu_log(ctx, "updating: %s\n", entry_name);
                added++;
            }
            else {
                if (ctx->freshen) {
                    if (info.link_target)
                        free(info.link_target);
                    free(allocated);
                    continue;
                }
                if (ctx->verbose || ctx->log_info || ctx->dry_run)
                    zu_log(ctx, "adding: %s\n", entry_name);
                added++;
            }

            uint16_t dos_time = 0, dos_date = 0;
            msdos_datetime(&info.st, &dos_time, &dos_date);

            update_newest_mtime(ctx, info.st.st_mtime);

            /* Stream only when necessary (stdin/unknown size/line mode) or when fast-write is requested */
            bool streaming = false;
            if (!S_ISDIR(info.st.st_mode) && !is_symlink) {
                if (info.is_stdin || !info.size_known || ctx->line_mode != ZU_LINE_NONE || ctx->fast_write) {
                    streaming = true;
                }
            }
            bool compress = false;
            if (S_ISDIR(info.st.st_mode)) {
                compress = false;
            }
            else if (streaming) {
                compress = should_compress_file(ctx, &info, path);
                if (info.size_known && info.st.st_size == 0) {
                    compress = false;
                }
            }
            else {
                compress = should_compress_file(ctx, &info, path);
                if (info.st.st_size == 0)
                    compress = false;
                if (is_symlink) {
                    compress = false;
                }
            }

            bool translate = false;
            if (ctx->line_mode != ZU_LINE_NONE && !S_ISDIR(info.st.st_mode) && !is_symlink && !streaming && !info.is_stdin) {
                if (file_is_likely_text(path)) {
                    translate = true;
                }
            }

            const char* prefix = ctx->dry_run ? "plan" : (existing ? "updating" : "adding");
            const char* method_desc = compress ? compression_method_name(ctx->compression_method) : "store";
            if (ctx->verbose || ctx->log_info || ctx->dry_run) {
                zu_log(ctx, "%s %s via %s%s%s%s\n", prefix, entry_name, method_desc, streaming ? " (streaming)" : "", translate ? " (translated)" : "",
                       is_symlink ? " [symlink]" : (S_ISDIR(info.st.st_mode) ? " [dir]" : ""));
            }

            if (ctx->dry_run) {
                if (info.link_target)
                    free(info.link_target);
                free(allocated);
                continue;
            }

            uint32_t entry_disk_start = current_disk_index(ctx);
            uint64_t entry_lho_offset = ctx->current_offset;

            if (streaming) {
                if (info.is_stdin) {
                    rc = write_stdin_staged_entry(ctx, entry_name, &info, dos_time, dos_date, entry_lho_offset, entry_disk_start, existing, &offset, &entries);
                }
                else {
                    rc = write_streaming_entry(ctx, path, entry_name, &info, dos_time, dos_date, entry_lho_offset, entry_disk_start, zip64_trigger, existing, &offset, &entries);
                }
                if (rc != ZU_STATUS_OK)
                    goto cleanup;
                if (ctx->remove_source && !info.is_stdin) {
                    unlink(path);
                }
                if (info.link_target)
                    free(info.link_target);
                free(allocated);
                continue;
            }

            uint32_t crc = 0;
            uint64_t uncomp_size = 0, comp_size = 0;
            uint16_t method = 0, flags = 0;
            FILE* staged = NULL;

            if (S_ISDIR(info.st.st_mode)) {
                if (!ctx->store_paths) {
                    free(allocated);
                    if (info.link_target)
                        free(info.link_target);
                    continue;
                }
                crc = 0;
                uncomp_size = 0;
                comp_size = 0;
                method = 0;
            }
            else {
                method = compress ? ctx->compression_method : 0;
                if (method == 0)
                    compress = false;

                if (compress) {
                    rc = compress_to_temp(ctx, path, method, ctx->compression_level, &staged, &crc, &uncomp_size, &comp_size, translate);
                    if (rc == ZU_STATUS_OK && comp_size >= uncomp_size) {
                        fclose(staged);
                        staged = NULL;
                        method = 0;
                        compress = false;
                        comp_size = uncomp_size;
                    }
                }
                else if (is_symlink) {
                    crc = zu_crc32((const uint8_t*)info.link_target, info.link_target_len, 0);
                    uncomp_size = info.link_target_len;
                    comp_size = uncomp_size;
                }
                else {
                    rc = compute_crc_and_size(ctx, path, &crc, &uncomp_size, translate);
                    comp_size = uncomp_size;
                }

                if (rc != ZU_STATUS_OK) {
                    if (staged)
                        fclose(staged);
                    if (info.link_target)
                        free(info.link_target);
                    goto cleanup;
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
            uint16_t extra_len = get_extra_len(ctx, zip64_lho, true, NULL);
            uint16_t version_needed = zip64_lho ? 45 : (method == 0 ? 10 : 20);
            size_t name_len = strlen(entry_name);

            uint32_t ext_attr = 0;
            uint16_t version_made = 20;
            make_attrs(ctx, &info.st, S_ISDIR(info.st.st_mode), &ext_attr, &version_made);
            if (zip64_lho) {
                version_made = (uint16_t)((version_made & 0xff00u) | 45);
            }

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
                if (info.link_target)
                    free(info.link_target);
                goto cleanup;
            }

            zu_writer_entry tmp_e = {
                .mtime = info.st.st_mtime,
                .atime = info.st.st_atime,
                .ctime = info.st.st_ctime,
                .uid = (uint32_t)info.st.st_uid,
                .gid = (uint32_t)info.st.st_gid,
            };

            if (write_extra_fields(ctx, zip64_lho, uncomp_size, comp_size, true, &tmp_e) != ZU_STATUS_OK) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write extra fields failed");
                rc = ZU_STATUS_IO;
                if (staged)
                    fclose(staged);
                if (info.link_target)
                    free(info.link_target);
                goto cleanup;
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
                    if (info.link_target)
                        free(info.link_target);
                    goto cleanup;
                }
            }

            if (!S_ISDIR(info.st.st_mode)) {
                if (is_symlink) {
                    rc = write_symlink_data(ctx, info.link_target ? info.link_target : "", info.link_target_len, pzc);
                }
                else {
                    rc = write_file_data(ctx, path, staged, payload_size, pzc, translate);
                }
                if (staged)
                    fclose(staged);
                if (rc != ZU_STATUS_OK) {
                    if (info.link_target)
                        free(info.link_target);
                    goto cleanup;
                }
            }
            else {
                if (staged)
                    fclose(staged);
            }

            if (ctx->remove_source && !info.is_stdin) {
                unlink(path);
            }

            if (ensure_entry_capacity(&entries) != ZU_STATUS_OK) {
                rc = ZU_STATUS_OOM;
                if (info.link_target)
                    free(info.link_target);
                goto cleanup;
            }

            entries.items[entries.len].name = strdup(entry_name);
            entries.items[entries.len].crc32 = crc;
            entries.items[entries.len].comp_size = comp_size;
            entries.items[entries.len].uncomp_size = uncomp_size;
            entries.items[entries.len].lho_offset = entry_lho_offset;
            entries.items[entries.len].method = method;
            entries.items[entries.len].mod_time = dos_time;
            entries.items[entries.len].mod_date = dos_date;
            entries.items[entries.len].ext_attr = ext_attr;
            entries.items[entries.len].version_made = version_made;
            entries.items[entries.len].int_attr = 0;
            entries.items[entries.len].zip64 = zip64_lho;
            entries.items[entries.len].flags = flags;
            entries.items[entries.len].disk_start = entry_disk_start;
            entries.items[entries.len].comment = NULL;
            entries.items[entries.len].comment_len = 0;
            entries.items[entries.len].mtime = info.st.st_mtime;
            entries.items[entries.len].atime = info.st.st_atime;
            entries.items[entries.len].ctime = info.st.st_ctime;
            entries.items[entries.len].uid = (uint32_t)info.st.st_uid;
            entries.items[entries.len].gid = (uint32_t)info.st.st_gid;
            if (existing && existing->comment && existing->comment_len > 0) {
                entries.items[entries.len].comment = malloc(existing->comment_len);
                if (!entries.items[entries.len].comment) {
                    rc = ZU_STATUS_OOM;
                    if (info.link_target)
                        free(info.link_target);
                    goto cleanup;
                }
                memcpy(entries.items[entries.len].comment, existing->comment, existing->comment_len);
                entries.items[entries.len].comment_len = existing->comment_len;
            }
            entries.len++;

            log_entry_action(ctx, "adding", entry_name, method, comp_size, uncomp_size);

            offset += sizeof(lho) + name_len + extra_len + comp_size;

            if (info.link_target) {
                free(info.link_target);
                info.link_target = NULL;
            }
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

    if (added == 0 && !ctx->difference_mode && !existing_changes && !ctx->zip_comment_specified && !ctx->set_archive_mtime && !ctx->fix_archive && !ctx->fix_fix_archive) {
        rc = ZU_STATUS_NO_FILES;
        /* -FS returns 0 when fully synced; -u/-f return 12 when nothing done */
        if (skipped_by_update && ctx->filesync) {
            rc = ZU_STATUS_OK;
        }

        bool suppress_msg = (rc == ZU_STATUS_OK && !ctx->filesync);
        if (!suppress_msg && !ctx->quiet && target_path && !ctx->update && !ctx->freshen) {
            printf("\nzip error: Nothing to do! (%s)\n", target_path);
        }
        else if (ctx->filesync && !ctx->quiet && target_path) {
            printf("\nzip error: Nothing to do! (%s)\n", target_path);
        }
    }

    if (ctx->dry_run) {
        goto cleanup;
    }
    if (rc != ZU_STATUS_OK) {
        goto cleanup;
    }

    if (existing_loaded) {
        for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
            zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
            if (!e->delete) {
                uint64_t written = 0;
                uint64_t new_offset = offset;
                uint32_t entry_disk_start = current_disk_index(ctx);
                uint64_t entry_lho_offset = ctx->current_offset;

                if (ctx->copy_mode && !ctx->quiet) {
                    progress_log(ctx, " copying: %s\n", e->name);
                }

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
                entries.items[entries.len].lho_offset = entry_lho_offset;
                entries.items[entries.len].method = e->hdr.method;
                entries.items[entries.len].mod_time = e->hdr.mod_time;
                entries.items[entries.len].mod_date = e->hdr.mod_date;

                uint32_t ext_attr = ctx->exclude_extra_attrs ? 0 : e->hdr.ext_attr;
                uint16_t version_made = ctx->exclude_extra_attrs ? (uint16_t)(e->hdr.version_made & 0x00ffu) : e->hdr.version_made;

                entries.items[entries.len].ext_attr = ext_attr;
                entries.items[entries.len].version_made = version_made;
                entries.items[entries.len].int_attr = e->hdr.int_attr;
                entries.items[entries.len].zip64 = (e->comp_size >= 0xffffffffu || e->uncomp_size >= 0xffffffffu || new_offset >= 0xffffffffu);
                entries.items[entries.len].flags = e->hdr.flags;
                entries.items[entries.len].disk_start = entry_disk_start;
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
                update_newest_mtime(ctx, dos_to_unix_time(e->hdr.mod_date, e->hdr.mod_time));
            }
        }
    }

    uint64_t cd_offset = ctx->current_offset;
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
    free_entries(&entries);

    if (ctx->out_file && ctx->out_file != stdout)
        fclose(ctx->out_file);
    ctx->out_file = NULL;

    if (ctx->in_file)
        fclose(ctx->in_file);
    ctx->in_file = NULL;

    if (rc == ZU_STATUS_OK && temp_path) {
        if (rename_or_copy(temp_path, target_path) != 0) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "rename temp file failed");
            rc = ZU_STATUS_IO;
        }
    }

    if (rc == ZU_STATUS_OK && ctx->set_archive_mtime && ctx->newest_mtime_valid && !ctx->output_to_stdout) {
        if (apply_mtime(target_path, ctx->newest_mtime) != 0) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "failed to set archive mtime");
            rc = ZU_STATUS_IO;
        }
    }
    else if (rc != ZU_STATUS_OK && temp_path) {
        unlink(temp_path);
    }

    free(temp_path);
    return rc;
}
