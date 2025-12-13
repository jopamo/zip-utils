#define _XOPEN_SOURCE 700

#include "reader.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include <bzlib.h>
#include <fnmatch.h>
#include <time.h>
#include <sys/time.h>

#include "fileio.h"
#include "crc32.h"
#include "zlib_shim.h"
#include "zip_headers.h"
#include "zipcrypto.h"

#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#endif

#define ZU_EXTRA_ZIP64 0x0001
#define ZU_IO_CHUNK (64 * 1024)

/*
 * ZIP reader and extractor
 *
 * This translation unit implements the read-side of the zip-utils toolset
 * It is responsible for
 * - locating the end of central directory (EOCD) record
 * - parsing central directory entries into in-memory representations
 * - supporting Zip64 expansion via the Zip64 extra field and Zip64 EOCD records
 * - listing entries in "unzip" mode and "zipinfo" compatible modes
 * - extracting or testing entries, including CRC verification
 * - handling classic PKZIP encryption (ZipCrypto) when present
 *
 * Important invariants
 * - All archive reads happen through ctx->in_file which is opened by zu_open_input
 * - Extraction writes either to stdout or to a filesystem path built from ctx policy
 * - The extractor rejects path traversal patterns and absolute paths
 * - Integrity testing verifies uncompressed byte count and CRC32
 *
 * Limitations in this layer
 * - Only store (0), deflate (8), and bzip2 (12) are supported
 * - Data descriptor handling depends on sizes resolved from the central directory
 * - ZipCrypto is supported, strong encryption is not
 */

typedef struct {
    uint64_t uncomp_size;
    uint64_t comp_size;
    uint64_t lho_offset;
} zu_zip64_extra;

typedef struct {
    uint64_t cd_offset;
    uint64_t entries_total;
} zu_cd_info;

/*
 * Convert DOS date/time values stored in ZIP headers into a time_t
 *
 * DOS stores seconds in 2-second resolution
 * The conversion uses mktime so results are in local time and DST is inferred
 */
static time_t dos_to_unix_time(uint16_t dos_date, uint16_t dos_time) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = ((dos_date >> 9) & 0x7f) + 80;
    t.tm_mon = ((dos_date >> 5) & 0x0f) - 1;
    t.tm_mday = dos_date & 0x1f;
    t.tm_hour = (dos_time >> 11) & 0x1f;
    t.tm_min = (dos_time >> 5) & 0x3f;
    t.tm_sec = (dos_time & 0x1f) * 2;
    t.tm_isdst = -1;

    return mktime(&t);
}

/*
 * Map the "version made by" host id (upper byte) to the classic zipinfo abbreviations
 */
static const char* zi_host_abbrev(uint16_t version_made) {
    int host = (version_made >> 8) & 0xff;
    switch (host) {
        case 0:
            return "fat";
        case 1:
            return "ami";
        case 2:
            return "vms";
        case 3:
            return "unx";
        case 6:
            return "hpfs";
        case 7:
            return "mac";
        case 10:
            return "ntfs";
        case 14:
            return "vfat";
        case 19:
            return "osx";
        default:
            return "???";
    }
}

/*
 * Format "version made by" as "M.m host"
 * - M.m comes from the lower byte (eg 20 -> 2.0)
 * - host is derived from the upper byte
 */
static void zi_format_creator(uint16_t version_made, char* out, size_t len) {
    int ver_raw = version_made & 0xff;
    int major = ver_raw / 10;
    int minor = ver_raw % 10;
    const char* host = zi_host_abbrev(version_made);
    snprintf(out, len, "%d.%d %s", major, minor, host);
}

/*
 * Derive a printable unix-style permission string from external attributes
 *
 * The ZIP central directory stores platform-specific "external attributes"
 * For Unix creators, the upper 16 bits are the st_mode value
 * If that field is zero, we synthesize a reasonable default based on file type
 */
static void zi_format_permissions(uint32_t ext_attr, bool is_dir, char out[11]) {
    mode_t mode = (mode_t)((ext_attr >> 16) & 0xffff);
    if (mode == 0) {
        mode = (is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    }

    out[0] = S_ISDIR(mode) ? 'd' : '-';

    const mode_t masks[9] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};

    const char chars[3] = {'r', 'w', 'x'};

    for (int i = 0; i < 9; ++i) {
        out[i + 1] = (mode & masks[i]) ? chars[i % 3] : '-';
    }

    out[10] = '\0';
}

/*
 * Heuristic used by zipinfo formatting
 * - "t" means likely text
 * - "b" means likely binary
 *
 * The decision is based on
 * - directory names
 * - a small list of well-known text file basenames
 * - file extensions commonly used for text sources/configs
 */
static bool name_is_text(const char* name) {
    if (!name)
        return false;

    size_t len = strlen(name);
    if (len == 0)
        return false;

    if (name[len - 1] == '/')
        return true;

    static const char* text_names[] = {"README", "LICENSE", "COPYING", "Makefile", "Dockerfile"};

    for (size_t i = 0; i < sizeof(text_names) / sizeof(text_names[0]); ++i) {
        if (strcasecmp(name, text_names[i]) == 0)
            return true;
    }

    const char* dot = strrchr(name, '.');
    if (!dot || dot == name)
        return false;

    const char* ext = dot + 1;

    static const char* text_exts[] = {"txt", "md",  "markdown", "c",    "cc",   "cpp", "cxx", "h",    "hpp",   "hh",     "rs",    "go",  "py", "rb",   "java", "js",
                                      "mjs", "cjs", "ts",       "tsx",  "html", "htm", "css", "scss", "json",  "yaml",   "yml",   "xml", "sh", "bash", "zsh",  "ksh",
                                      "ps1", "ini", "cfg",      "conf", "toml", "csv", "tsv", "sql",  "proto", "gradle", "cmake", "mak", "mk", "log",  "tex"};

    for (size_t i = 0; i < sizeof(text_exts) / sizeof(text_exts[0]); ++i) {
        if (strcasecmp(ext, text_exts[i]) == 0)
            return true;
    }

    return false;
}

/*
 * Format zipinfo-style per-entry flags
 *
 * Output is two characters plus NUL
 * - First char: 't' or 'b', uppercased if encrypted
 * - Second char: indicates extra fields and data-descriptor usage
 *   - '-' none
 *   - 'l' data descriptor present (bit 3)
 *   - 'x' extra fields present
 *   - 'X' both data descriptor and extra fields present
 */
static void zi_format_flags(const zu_central_header* hdr, const char* name, char out[3]) {
    bool encrypted = (hdr->flags & 0x0001) != 0;
    bool is_text = name_is_text(name);

    char txt = is_text ? 't' : 'b';
    if (encrypted) {
        txt = (char)toupper((unsigned char)txt);
    }

    char extra = '-';
    if (hdr->flags & 0x0008) {
        extra = 'l';
    }
    if (hdr->extra_len > 0) {
        extra = (extra == 'l') ? 'X' : 'x';
    }

    out[0] = txt;
    out[1] = extra;
    out[2] = '\0';
}

/*
 * Map compression method ids to zipinfo abbreviations
 */
static void zi_format_method(uint16_t method, char* out, size_t len) {
    const char* name = "unkn";
    switch (method) {
        case 0:
            name = "stor";
            break;
        case 8:
            name = "defl";
            break;
        case 9:
            name = "defS";
            break;
        case 12:
            name = "bzip";
            break;
        default:
            name = "unkn";
            break;
    }
    snprintf(out, len, "%s", name);
}

/*
 * Format DOS date/time fields into zipinfo-compatible timestamps
 *
 * decimal=true produces "YYMMDD.HHMMSS"
 * decimal=false produces a "dd-Mon-yy HH:MM" style output when month is valid
 */
static void zi_format_datetime(uint16_t dos_date, uint16_t dos_time, bool decimal, char* out, size_t len) {
    unsigned year = ((dos_date >> 9) & 0x7f) + 1980;
    unsigned month = (dos_date >> 5) & 0x0f;
    unsigned day = dos_date & 0x1f;
    unsigned hour = (dos_time >> 11) & 0x1f;
    unsigned minute = (dos_time >> 5) & 0x3f;
    unsigned second = (dos_time & 0x1f) * 2;

    static const char* months[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (decimal) {
        snprintf(out, len, "%02u%02u%02u.%02u%02u%02u", year % 100, month, day, hour, minute, second);
    }
    else if (month < sizeof(months) / sizeof(months[0])) {
        snprintf(out, len, "%02u-%s-%02u %02u:%02u", day, months[month], year % 100, hour, minute);
    }
    else {
        snprintf(out, len, "%04u-%02u-%02u %02u:%02u", year, month, day, hour, minute);
    }
}

/*
 * Compute the displayed compression ratio used by zipinfo
 *
 * The ratio is "percentage removed", not "percentage remaining"
 * - 0 means no savings
 * - values are clamped to [0, 999.9] for display sanity
 */
static double zi_ratio(uint64_t comp, uint64_t uncomp) {
    if (uncomp == 0)
        return 0.0;

    double removed = 100.0 - ((double)comp * 100.0 / (double)uncomp);
    if (removed < 0.0)
        removed = 0.0;
    if (removed > 999.9)
        removed = 999.9;

    return removed;
}

/*
 * Decide whether to emulate zipinfo's simple pager behavior
 *
 * This is only enabled when
 * - zipinfo mode is active
 * - -M / allow pager is set
 * - stdout is a terminal
 */
static bool zi_should_page(const ZContext* ctx) {
    return ctx->zipinfo_mode && ctx->zi_allow_pager && isatty(STDOUT_FILENO);
}

/*
 * Print a formatted line and optionally enforce a "--More--" prompt
 *
 * Return value
 * - 0 to continue
 * - 1 when the user requested abort (q/Q)
 */
static int zi_print_line(const ZContext* ctx, size_t* line_count, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (zi_should_page(ctx)) {
        (*line_count)++;
        if (*line_count >= 22) {
            fputs("--More--", stderr);
            fflush(stderr);
            int ch = getchar();
            fputs("\r        \r", stderr);
            fflush(stderr);
            *line_count = 0;
            if (ch == 'q' || ch == 'Q') {
                return 1;
            }
        }
    }

    return 0;
}

/*
 * Print a central directory entry in one of the supported zipinfo layouts
 *
 * The printed fields are derived from the central header and the resolved Zip64 sizes
 */
static int zi_print_entry(const ZContext* ctx, size_t* line_count, const zu_central_header* hdr, const char* name, uint64_t comp_size, uint64_t uncomp_size) {
    bool is_dir = name && name[strlen(name) - 1] == '/';

    char perms[11];
    zi_format_permissions(hdr->ext_attr, is_dir, perms);

    char creator[16];
    zi_format_creator(hdr->version_made, creator, sizeof(creator));

    char flags[3];
    zi_format_flags(hdr, name, flags);

    char method[8];
    zi_format_method(hdr->method, method, sizeof(method));

    char when[32];
    zi_format_datetime(hdr->mod_date, hdr->mod_time, ctx->zi_decimal_time, when, sizeof(when));

    double ratio = zi_ratio(comp_size, uncomp_size);

    switch (ctx->zi_format) {
        case ZU_ZI_FMT_NAMES:
            return zi_print_line(ctx, line_count, "%s\n", name);

        case ZU_ZI_FMT_MEDIUM:
            return zi_print_line(ctx, line_count, "%-10s %-10s %10" PRIu64 " %2s %5.0f%% %-4s %s %s\n", perms, creator, uncomp_size, flags, ratio, method, when, name);

        case ZU_ZI_FMT_LONG:
        case ZU_ZI_FMT_VERBOSE:
            return zi_print_line(ctx, line_count, "%-10s %-10s %10" PRIu64 " %2s %10" PRIu64 " %-4s %s %s\n", perms, creator, uncomp_size, flags, comp_size, method, when, name);

        case ZU_ZI_FMT_SHORT:
        default:
            return zi_print_line(ctx, line_count, "%-10s %-10s %10" PRIu64 " %2s %-4s %s %s\n", perms, creator, uncomp_size, flags, method, when, name);
    }
}

/*
 * Find the EOCD signature by scanning backwards from end-of-file
 *
 * ZIP allows an arbitrary-length comment, so EOCD is located by searching
 * the last 64 KiB + EOCD header size region for the end signature
 *
 * Return value
 * - 0 and *pos_out set on success
 * - -1 on failure
 */
static int find_eocd(FILE* f, off_t* pos_out) {
    if (fseeko(f, 0, SEEK_END) != 0)
        return -1;

    off_t end = ftello(f);
    if (end < 0)
        return -1;

    off_t max_scan = end < (off_t)(0x10000 + sizeof(zu_end_central)) ? end : (off_t)(0x10000 + sizeof(zu_end_central));

    if (fseeko(f, end - max_scan, SEEK_SET) != 0)
        return -1;

    size_t buf_len = (size_t)max_scan;
    char* buf = malloc(buf_len);
    if (!buf)
        return -1;

    size_t readlen = fread(buf, 1, buf_len, f);
    if (readlen != buf_len) {
        free(buf);
        return -1;
    }

    for (ssize_t i = (ssize_t)buf_len - (ssize_t)sizeof(uint32_t); i >= 0; --i) {
        uint32_t sig = 0;
        memcpy(&sig, buf + i, sizeof(sig));
        if (sig == ZU_SIG_END) {
            *pos_out = end - buf_len + i;
            free(buf);
            return 0;
        }
    }

    free(buf);
    return -1;
}

/*
 * Best-effort central directory recovery used by -FF mode
 *
 * Strategy
 * - Scan the file sequentially for local file header signatures
 * - For each local header, read the filename and capture metadata needed to
 *   synthesize a "central directory" view in ctx->existing_entries
 *
 * Caveats
 * - Entries using data descriptors do not store sizes in the local header
 * - This function attempts to estimate sizes by looking at the next local header offset
 * - Extra field lengths matter for correct size estimation, so they must be tracked
 *
 * Output
 * - Populates ctx->existing_entries with zu_existing_entry objects
 * - Returns OK if at least one entry was recovered
 */
static int zu_recover_central_directory(ZContext* ctx) {
    if (ctx->verbose)
        zu_log(ctx, "Scanning for local headers (-FF)...\n");

    if (fseeko(ctx->in_file, 0, SEEK_SET) != 0)
        return ZU_STATUS_IO;

    uint8_t buf[ZU_IO_CHUNK];
    off_t current = 0;
    size_t got;
    int entries_found = 0;

    while ((got = fread(buf, 1, sizeof(buf), ctx->in_file)) > 0) {
        if (got < 4)
            break;

        for (size_t i = 0; i < got - 3; ++i) {
            uint32_t s;
            memcpy(&s, buf + i, 4);
            if (s != ZU_SIG_LOCAL)
                continue;

            off_t lho_offset = current + (off_t)i;

            if (fseeko(ctx->in_file, lho_offset, SEEK_SET) != 0)
                break;

            zu_local_header lho;
            if (fread(&lho, 1, sizeof(lho), ctx->in_file) != sizeof(lho))
                break;

            char* name = malloc(lho.name_len + 1);
            if (!name)
                return ZU_STATUS_OOM;

            if (fread(name, 1, lho.name_len, ctx->in_file) != lho.name_len) {
                free(name);
                break;
            }
            name[lho.name_len] = '\0';

            if (fseeko(ctx->in_file, lho.extra_len, SEEK_CUR) != 0) {
                free(name);
                break;
            }

            uint64_t comp_size = lho.comp_size;
            uint64_t uncomp_size = lho.uncomp_size;

            if (lho.flags & 8) {
                comp_size = 0;
            }

            zu_existing_entry* entry = calloc(1, sizeof(zu_existing_entry));
            if (!entry) {
                free(name);
                return ZU_STATUS_OOM;
            }

            entry->hdr.signature = ZU_SIG_CENTRAL;
            entry->hdr.version_made = 20;
            entry->hdr.version_needed = lho.version_needed;
            entry->hdr.flags = lho.flags;
            entry->hdr.method = lho.method;
            entry->hdr.mod_time = lho.mod_time;
            entry->hdr.mod_date = lho.mod_date;
            entry->hdr.crc32 = lho.crc32;
            entry->hdr.comp_size = (uint32_t)comp_size;
            entry->hdr.uncomp_size = (uint32_t)uncomp_size;
            entry->hdr.name_len = lho.name_len;
            entry->hdr.extra_len = lho.extra_len;
            entry->hdr.lho_offset = (uint32_t)lho_offset;

            entry->name = name;
            entry->comp_size = comp_size;
            entry->uncomp_size = uncomp_size;
            entry->lho_offset = (uint64_t)lho_offset;

            if (ctx->existing_entries.len == ctx->existing_entries.cap) {
                size_t new_cap = ctx->existing_entries.cap == 0 ? 16 : ctx->existing_entries.cap * 2;
                char** new_items = realloc(ctx->existing_entries.items, new_cap * sizeof(char*));
                if (!new_items) {
                    free(name);
                    free(entry);
                    return ZU_STATUS_OOM;
                }
                ctx->existing_entries.items = new_items;
                ctx->existing_entries.cap = new_cap;
            }
            ctx->existing_entries.items[ctx->existing_entries.len++] = (char*)entry;
            entries_found++;

            if (!(lho.flags & 8) && comp_size > 0) {
                fseeko(ctx->in_file, (off_t)comp_size, SEEK_CUR);
            }

            current = ftello(ctx->in_file);
            i = (size_t)-1;
            got = fread(buf, 1, sizeof(buf), ctx->in_file);
            if (got < 4) {
                got = 0;
                break;
            }
        }

        if (got == 0)
            break;

        current += (off_t)got - 3;
        fseeko(ctx->in_file, current, SEEK_SET);
    }

    // Fix up unknown sizes for data-descriptor entries using next header boundary
    for (size_t k = 0; k < ctx->existing_entries.len; ++k) {
        zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[k];
        if ((e->hdr.flags & 8) && e->comp_size == 0) {
            uint64_t next_offset = 0;
            if (k + 1 < ctx->existing_entries.len) {
                next_offset = ((zu_existing_entry*)ctx->existing_entries.items[k + 1])->lho_offset;
            }
            else {
                fseeko(ctx->in_file, 0, SEEK_END);
                next_offset = (uint64_t)ftello(ctx->in_file);
            }

            uint64_t start_data = e->lho_offset + 30 + e->hdr.name_len + e->hdr.extra_len;
            if (start_data < next_offset) {
                e->comp_size = next_offset - start_data;
                e->hdr.comp_size = (uint32_t)e->comp_size;
            }
        }
    }

    return entries_found > 0 ? ZU_STATUS_OK : ZU_STATUS_IO;
}

/*
 * Read central directory location and entry count into info
 *
 * Also optionally loads the archive comment from EOCD unless a user-supplied
 * comment is already present in ctx (zip -z behavior)
 *
 * Zip64 handling
 * - If EOCD contains sentinel values, look for the Zip64 locator and Zip64 EOCD
 * - Replace cd_offset and entries_total with Zip64 values
 */
static int read_cd_info(ZContext* ctx, zu_cd_info* info, bool load_comment) {
    off_t eocd_pos;
    if (find_eocd(ctx->in_file, &eocd_pos) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "missing end of central directory");
        return ZU_STATUS_IO;
    }

    if (fseeko(ctx->in_file, eocd_pos, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek failed");
        return ZU_STATUS_IO;
    }

    zu_end_central endrec;
    size_t got = fread(&endrec, 1, sizeof(endrec), ctx->in_file);
    if (got != sizeof(endrec) || endrec.signature != ZU_SIG_END) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "failed to read EOCD");
        return ZU_STATUS_IO;
    }

    info->entries_total = endrec.entries_total;
    info->cd_offset = endrec.cd_offset;

    if (!load_comment) {
        free(ctx->zip_comment);
        ctx->zip_comment = NULL;
        ctx->zip_comment_len = 0;
    }
    else if (ctx->zip_comment_specified) {
        // keep user-supplied comment
    }
    else if (endrec.comment_len > 0) {
        char* comment = malloc(endrec.comment_len + 1);
        if (!comment) {
            zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating archive comment failed");
            return ZU_STATUS_OOM;
        }
        size_t got_comment = fread(comment, 1, endrec.comment_len, ctx->in_file);
        if (got_comment != endrec.comment_len) {
            free(comment);
            zu_context_set_error(ctx, ZU_STATUS_IO, "reading archive comment failed");
            return ZU_STATUS_IO;
        }
        comment[endrec.comment_len] = '\0';
        free(ctx->zip_comment);
        ctx->zip_comment = comment;
        ctx->zip_comment_len = endrec.comment_len;
    }
    else {
        free(ctx->zip_comment);
        ctx->zip_comment = NULL;
        ctx->zip_comment_len = 0;
    }

    int need_zip64 = endrec.entries_total == 0xffff || endrec.cd_offset == 0xffffffff || endrec.cd_size == 0xffffffff;

    if (!need_zip64)
        return ZU_STATUS_OK;

    off_t locator_pos = eocd_pos - (off_t)sizeof(zu_end64_locator);
    if (locator_pos < 0 || fseeko(ctx->in_file, locator_pos, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "zip64 locator seek failed");
        return ZU_STATUS_IO;
    }

    zu_end64_locator locator;
    got = fread(&locator, 1, sizeof(locator), ctx->in_file);
    if (got != sizeof(locator) || locator.signature != ZU_SIG_END64LOC) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "zip64 locator missing");
        return ZU_STATUS_IO;
    }

    if (fseeko(ctx->in_file, (off_t)locator.eocd64_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "zip64 eocd seek failed");
        return ZU_STATUS_IO;
    }

    zu_end_central64 end64;
    got = fread(&end64, 1, sizeof(end64), ctx->in_file);
    if (got != sizeof(end64) || end64.signature != ZU_SIG_END64) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "zip64 eocd read failed");
        return ZU_STATUS_IO;
    }

    info->entries_total = end64.entries_total;
    info->cd_offset = end64.cd_offset;
    return ZU_STATUS_OK;
}

/*
 * Apply include/exclude logic and optionally track which include patterns matched
 *
 * Behavior
 * - Exclude patterns always win
 * - If ctx->include is empty, everything not excluded matches
 * - Otherwise name must match at least one include glob
 * - When include_hits is provided, mark the indices that matched for post-run warnings
 */
static bool match_and_track(const ZContext* ctx, const char* name, bool* include_hits) {
    int flags = ctx->match_case ? 0 : FNM_CASEFOLD;

    for (size_t i = 0; i < ctx->exclude.len; ++i) {
        if (fnmatch(ctx->exclude.items[i], name, flags) == 0) {
            return false;
        }
    }

    if (ctx->include.len == 0) {
        return true;
    }

    bool matched = false;
    for (size_t i = 0; i < ctx->include.len; ++i) {
        if (fnmatch(ctx->include.items[i], name, flags) == 0) {
            matched = true;
            if (include_hits) {
                include_hits[i] = true;
            }
        }
    }

    return matched;
}

/*
 * Detect unsafe archive paths
 *
 * Rejects
 * - absolute paths
 * - any ".." segment that would traverse upward
 *
 * This prevents classic zip slip attacks during extraction
 */
static bool path_has_traversal(const char* name) {
    if (!name)
        return true;

    if (name[0] == '/')
        return true;

    for (const char* p = name; *p; ++p) {
        if (p[0] == '.' && p[1] == '.' && (p == name || p[-1] == '/') && (p[2] == '/' || p[2] == '\0')) {
            return true;
        }
    }

    return false;
}

/*
 * Ensure that a directory exists
 *
 * - If the path exists and is a directory: OK
 * - If the path does not exist: try mkdir(0755)
 * - Otherwise: usage/io error depending on failure mode
 */
static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ZU_STATUS_OK : ZU_STATUS_USAGE;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return ZU_STATUS_OK;
    }
    return ZU_STATUS_IO;
}

/*
 * Create every parent directory needed for a file path
 *
 * The function walks the string and temporarily terminates it at each '/'
 * calling ensure_dir on each prefix
 */
static int ensure_parent_dirs(const char* path) {
    char* dup = strdup(path);
    if (!dup)
        return ZU_STATUS_OOM;

    for (char* p = dup + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (dup[0] != '\0' && ensure_dir(dup) != ZU_STATUS_OK) {
                free(dup);
                return ZU_STATUS_IO;
            }
            *p = '/';
        }
    }

    free(dup);
    return ZU_STATUS_OK;
}

/*
 * Build an extraction target path according to ctx settings
 *
 * Rules
 * - If ctx->store_paths is false, strip directories and keep only basename
 * - If ctx->target_dir is set, prefix the output path with it
 * - Returns a malloc'd string or NULL on OOM
 */
static char* build_output_path(const ZContext* ctx, const char* name) {
    const char* name_part = name;

    if (!ctx->store_paths) {
        const char* slash = strrchr(name, '/');
        name_part = slash ? slash + 1 : name;
    }

    const char* base = ctx->target_dir ? ctx->target_dir : "";
    size_t base_len = strlen(base);
    bool need_sep = base_len > 0 && base[base_len - 1] != '/';

    size_t total = base_len + (need_sep ? 1 : 0) + strlen(name_part) + 1;
    char* out = malloc(total);
    if (!out)
        return NULL;

    if (base_len == 0) {
        snprintf(out, total, "%s", name_part);
    }
    else if (need_sep) {
        snprintf(out, total, "%s/%s", base, name_part);
    }
    else {
        snprintf(out, total, "%s%s", base, name_part);
    }

    return out;
}

/*
 * Resolve Zip64 sizes from the Zip64 extra field
 *
 * The central header uses 0xffffffff sentinel values when a field is stored in Zip64
 * This parser walks the extra blocks until it finds the Zip64 tag (0x0001)
 * It then reads the present values in the correct order:
 * - uncompressed size (if needed)
 * - compressed size (if needed)
 * - local header offset (if needed)
 */
static void resolve_zip64_sizes(const zu_central_header* hdr, const unsigned char* extra, size_t extra_len, uint64_t* comp_out, uint64_t* uncomp_out, uint64_t* lho_out) {
    uint64_t comp = hdr->comp_size;
    uint64_t uncomp = hdr->uncomp_size;
    uint64_t lho = hdr->lho_offset;

    bool need_uncomp = hdr->uncomp_size == 0xffffffffu;
    bool need_comp = hdr->comp_size == 0xffffffffu;
    bool need_lho = hdr->lho_offset == 0xffffffffu;

    size_t pos = 0;
    while ((need_uncomp || need_comp || need_lho) && pos + 4 <= extra_len) {
        uint16_t tag = 0;
        uint16_t sz = 0;
        memcpy(&tag, extra + pos, 2);
        memcpy(&sz, extra + pos + 2, 2);
        pos += 4;

        if (pos + sz > extra_len)
            break;

        if (tag == ZU_EXTRA_ZIP64) {
            size_t zpos = pos;

            if (need_uncomp && zpos + sizeof(uint64_t) <= pos + sz) {
                memcpy(&uncomp, extra + zpos, sizeof(uint64_t));
                zpos += sizeof(uint64_t);
                need_uncomp = false;
            }
            if (need_comp && zpos + sizeof(uint64_t) <= pos + sz) {
                memcpy(&comp, extra + zpos, sizeof(uint64_t));
                zpos += sizeof(uint64_t);
                need_comp = false;
            }
            if (need_lho && zpos + sizeof(uint64_t) <= pos + sz) {
                memcpy(&lho, extra + zpos, sizeof(uint64_t));
                need_lho = false;
            }

            break;
        }

        pos += sz;
    }

    if (comp_out)
        *comp_out = comp;
    if (uncomp_out)
        *uncomp_out = uncomp;
    if (lho_out)
        *lho_out = lho;
}

/*
 * Read one central directory entry from ctx->in_file
 *
 * Outputs
 * - hdr: central header struct
 * - name_out: malloc'd filename string
 * - extra_out/extra_len_out: optional malloc'd extra field buffer
 * - comment_out/comment_len_out: optional malloc'd comment string
 * - comp_size_out/uncomp_size_out/lho_offset_out: resolved values with Zip64 support
 *
 * Memory behavior
 * - name_out is always allocated on success
 * - extra/comment are only allocated if their output pointers are provided
 */
static int read_central_entry(ZContext* ctx,
                              zu_central_header* hdr,
                              char** name_out,
                              unsigned char** extra_out,
                              uint16_t* extra_len_out,
                              char** comment_out,
                              uint16_t* comment_len_out,
                              uint64_t* comp_size_out,
                              uint64_t* uncomp_size_out,
                              uint64_t* lho_offset_out) {
    size_t got = fread(hdr, 1, sizeof(*hdr), ctx->in_file);
    if (got != sizeof(*hdr) || hdr->signature != ZU_SIG_CENTRAL) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "bad central header");
        return ZU_STATUS_IO;
    }

    char* name = malloc(hdr->name_len + 1);
    if (!name) {
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating filename failed");
        return ZU_STATUS_OOM;
    }

    size_t name_len = fread(name, 1, hdr->name_len, ctx->in_file);
    if (name_len != hdr->name_len) {
        free(name);
        zu_context_set_error(ctx, ZU_STATUS_IO, "short read on filename");
        return ZU_STATUS_IO;
    }
    name[hdr->name_len] = '\0';

    uint16_t extra_to_read = hdr->extra_len;
    unsigned char* extra_buf = NULL;

    bool need_zip64 = hdr->comp_size == 0xffffffffu || hdr->uncomp_size == 0xffffffffu || hdr->lho_offset == 0xffffffffu;

    if (extra_to_read > 0) {
        extra_buf = malloc(extra_to_read);
        if (!extra_buf) {
            free(name);
            zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating extra field failed");
            return ZU_STATUS_OOM;
        }
        size_t extra_got = fread(extra_buf, 1, extra_to_read, ctx->in_file);
        if (extra_got != extra_to_read) {
            free(name);
            free(extra_buf);
            zu_context_set_error(ctx, ZU_STATUS_IO, "short read on extra field");
            return ZU_STATUS_IO;
        }
    }

    if (need_zip64 && extra_to_read == 0) {
        free(name);
        free(extra_buf);
        zu_context_set_error(ctx, ZU_STATUS_IO, "missing Zip64 extra for large entry");
        return ZU_STATUS_IO;
    }

    if (extra_out && extra_len_out) {
        *extra_out = extra_buf;
        *extra_len_out = extra_to_read;
    }

    if (hdr->comment_len > 0) {
        if (comment_out && comment_len_out) {
            char* cbuf = malloc(hdr->comment_len + 1);
            if (!cbuf) {
                free(name);
                free(extra_buf);
                zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating file comment failed");
                return ZU_STATUS_OOM;
            }

            size_t cgot = fread(cbuf, 1, hdr->comment_len, ctx->in_file);
            if (cgot != hdr->comment_len) {
                free(name);
                free(extra_buf);
                free(cbuf);
                zu_context_set_error(ctx, ZU_STATUS_IO, "short read on file comment");
                return ZU_STATUS_IO;
            }

            cbuf[hdr->comment_len] = '\0';
            *comment_out = cbuf;
            *comment_len_out = hdr->comment_len;
        }
        else {
            if (fseeko(ctx->in_file, (off_t)hdr->comment_len, SEEK_CUR) != 0) {
                free(name);
                free(extra_buf);
                zu_context_set_error(ctx, ZU_STATUS_IO, "seek past comment failed");
                return ZU_STATUS_IO;
            }
        }
    }
    else {
        if (comment_out && comment_len_out) {
            *comment_out = NULL;
            *comment_len_out = 0;
        }
    }

    uint64_t comp64 = hdr->comp_size;
    uint64_t uncomp64 = hdr->uncomp_size;
    uint64_t lho64 = hdr->lho_offset;

    if (extra_to_read > 0) {
        resolve_zip64_sizes(hdr, extra_buf, extra_to_read, &comp64, &uncomp64, &lho64);
    }

    if (!extra_out || !extra_len_out) {
        free(extra_buf);
    }

    if (comp_size_out)
        *comp_size_out = comp64;
    if (uncomp_size_out)
        *uncomp_size_out = uncomp64;
    if (lho_offset_out)
        *lho_offset_out = lho64;

    *name_out = name;
    return ZU_STATUS_OK;
}

/*
 * Verbose zipinfo printing for ZU_ZI_FMT_VERBOSE
 *
 * Prints the normal listing line and then prints additional metadata
 * - "version needed" and general header details
 * - size and crc
 * - a summary of extra field tags
 * - optional comment body when enabled
 */
static int zi_print_verbose_entry(const ZContext* ctx,
                                  size_t* line_count,
                                  const zu_central_header* hdr,
                                  const char* name,
                                  const unsigned char* extra,
                                  uint16_t extra_len,
                                  const char* comment,
                                  uint16_t comment_len,
                                  uint64_t comp_size,
                                  uint64_t uncomp_size,
                                  uint64_t lho_offset) {
    if (zi_print_entry(ctx, line_count, hdr, name, comp_size, uncomp_size) != 0)
        return 1;

    int ver_needed_major = hdr->version_needed / 10;
    int ver_needed_minor = hdr->version_needed % 10;

    if (zi_print_line(ctx, line_count, "    version needed: %d.%d  flags: 0x%04x  method: %u  offset: %" PRIu64 "\n", ver_needed_major, ver_needed_minor, hdr->flags, hdr->method, lho_offset) != 0) {
        return 1;
    }

    if (zi_print_line(ctx, line_count, "    sizes: comp=%" PRIu64 "  uncomp=%" PRIu64 "  crc=%08" PRIx32 "\n", comp_size, uncomp_size, hdr->crc32) != 0) {
        return 1;
    }

    if (extra_len > 0) {
        if (zi_print_line(ctx, line_count, "    extra fields: %u bytes\n", extra_len) != 0)
            return 1;

        size_t pos = 0;
        while (pos + 4 <= extra_len) {
            uint16_t tag = 0;
            uint16_t sz = 0;
            memcpy(&tag, extra + pos, 2);
            memcpy(&sz, extra + pos + 2, 2);
            if (zi_print_line(ctx, line_count, "      tag 0x%04x (%u bytes)\n", tag, sz) != 0)
                return 1;
            pos += 4 + sz;
        }
    }
    else {
        if (zi_print_line(ctx, line_count, "    extra fields: none\n") != 0)
            return 1;
    }

    if (comment && comment_len > 0 && ctx->zi_show_comments) {
        if (zi_print_line(ctx, line_count, "    comment: %.*s\n", (int)comment_len, comment) != 0)
            return 1;
    }

    return zi_print_line(ctx, line_count, "\n");
}

/*
 * Extract or test a single entry selected by the central directory
 *
 * Workflow
 * - Validate entry name to prevent path traversal
 * - Seek to local header using lho_offset and skip to file data
 * - Handle directory entries by creating output directories when appropriate
 * - For files, stream the compressed data through a decoder
 *   - store: straight copy
 *   - deflate: zlib inflate with raw window bits
 *   - bzip2: bz2 streaming decompressor
 * - Update CRC as bytes are produced and optionally write to output
 * - Verify CRC and uncompressed byte count against central directory values
 * - When writing to filesystem, restore permissions and timestamps when possible
 */
static int extract_or_test_entry(ZContext* ctx, const zu_central_header* hdr, const char* name, bool test_only, uint64_t comp_size, uint64_t uncomp_size, uint64_t lho_offset) {
    size_t name_len = strlen(name);
    bool is_dir = name_len > 0 && name[name_len - 1] == '/';

    if (path_has_traversal(name)) {
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "unsafe path in archive entry");
        return ZU_STATUS_USAGE;
    }

    if (fseeko(ctx->in_file, (off_t)lho_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to local header failed");
        return ZU_STATUS_IO;
    }

    zu_local_header lho;
    size_t got = fread(&lho, 1, sizeof(lho), ctx->in_file);
    if (got != sizeof(lho) || lho.signature != ZU_SIG_LOCAL) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "bad local header");
        return ZU_STATUS_IO;
    }

    off_t data_offset = (off_t)lho_offset + (off_t)sizeof(lho) + lho.name_len + lho.extra_len;
    if (fseeko(ctx->in_file, data_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to file data failed");
        return ZU_STATUS_IO;
    }

    if (is_dir) {
        if (!test_only) {
            if (ctx->output_to_stdout)
                return ZU_STATUS_OK;

            if (!ctx->store_paths)
                return ZU_STATUS_OK;

            char* out_path = build_output_path(ctx, name);
            if (!out_path) {
                zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating output path failed");
                return ZU_STATUS_OOM;
            }

            size_t out_len = strlen(out_path);
            if (out_len > 0 && out_path[out_len - 1] == '/') {
                out_path[out_len - 1] = '\0';
            }

            int dir_rc = ensure_dir(out_path);
            free(out_path);
            if (dir_rc != ZU_STATUS_OK) {
                zu_context_set_error(ctx, dir_rc, "creating directory failed");
                return dir_rc;
            }
        }
        return ZU_STATUS_OK;
    }

    bool encrypted = (hdr->flags & 1) != 0;
    zu_zipcrypto_ctx zc;
    if (encrypted) {
        if (!ctx->password) {
            zu_context_set_error(ctx, ZU_STATUS_PASSWORD_REQUIRED, "password required");
            return ZU_STATUS_PASSWORD_REQUIRED;
        }

        zu_zipcrypto_init(&zc, ctx->password);

        uint8_t header[12];
        if (fread(header, 1, 12, ctx->in_file) != 12) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "reading encryption header failed");
            return ZU_STATUS_IO;
        }

        zu_zipcrypto_decrypt(&zc, header, 12);

        uint8_t check = (uint8_t)((hdr->flags & 8) ? (hdr->mod_time >> 8) : (hdr->crc32 >> 24));
        if (header[11] != check) {
            zu_context_set_error(ctx, ZU_STATUS_BAD_PASSWORD, "incorrect password");
            return ZU_STATUS_BAD_PASSWORD;
        }

        if (comp_size < 12) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "encrypted entry too small");
            return ZU_STATUS_IO;
        }
        comp_size -= 12;
    }

    uint8_t* in_buf = zu_get_io_buffer(ctx, ZU_IO_CHUNK);
    uint8_t* out_buf = zu_get_io_buffer2(ctx, ZU_IO_CHUNK);
    if (!in_buf || !out_buf) {
        zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating buffers failed");
        return ZU_STATUS_OOM;
    }

    uint32_t crc = 0;
    uint64_t written = 0;

    FILE* fp = NULL;
    char* out_path = NULL;

    if (!test_only) {
        if (!ctx->quiet)
            printf("  %s: %s\n", hdr->method == 8 ? "inflating" : "extracting", name);
        if (ctx->output_to_stdout) {
            fp = stdout;
        }
        else {
            out_path = build_output_path(ctx, name);
            if (!out_path) {
                zu_context_set_error(ctx, ZU_STATUS_OOM, "allocating output path failed");
                return ZU_STATUS_OOM;
            }

            if (ensure_parent_dirs(out_path) != ZU_STATUS_OK) {
                free(out_path);
                zu_context_set_error(ctx, ZU_STATUS_IO, "creating parent directories failed");
                return ZU_STATUS_IO;
            }

            fp = fopen(out_path, "wb");
            if (!fp) {
                free(out_path);
                zu_context_set_error(ctx, ZU_STATUS_IO, "open output file failed");
                return ZU_STATUS_IO;
            }
        }
    }

    int rc = ZU_STATUS_OK;

    if (hdr->method == 0) {
        uint64_t remaining = comp_size;
        while (remaining > 0) {
            size_t chunk = remaining > ZU_IO_CHUNK ? ZU_IO_CHUNK : (size_t)remaining;
            size_t got_data = fread(in_buf, 1, chunk, ctx->in_file);
            if (got_data != chunk) {
                rc = ZU_STATUS_IO;
                zu_context_set_error(ctx, rc, "short read on stored data");
                break;
            }

            if (encrypted) {
                zu_zipcrypto_decrypt(&zc, in_buf, got_data);
            }

            crc = zu_crc32(in_buf, got_data, crc);

            if (!test_only) {
                if (fwrite(in_buf, 1, got_data, fp) != got_data) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "write output file failed");
                    break;
                }
            }

            written += got_data;
            remaining -= got_data;
        }
    }
    else if (hdr->method == 8) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        int zrc = inflateInit2(&strm, -MAX_WBITS);
        if (zrc != Z_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "inflate init failed");
        }
        else {
            uint64_t remaining = comp_size;
            zrc = Z_OK;

            while (rc == ZU_STATUS_OK && remaining > 0) {
                size_t to_read = remaining > ZU_IO_CHUNK ? ZU_IO_CHUNK : (size_t)remaining;
                size_t got_data = fread(in_buf, 1, to_read, ctx->in_file);
                if (got_data != to_read) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "short read on compressed data");
                    break;
                }

                if (encrypted) {
                    zu_zipcrypto_decrypt(&zc, in_buf, got_data);
                }

                remaining -= got_data;

                strm.next_in = in_buf;
                strm.avail_in = (uInt)got_data;

                do {
                    strm.next_out = out_buf;
                    strm.avail_out = (uInt)ZU_IO_CHUNK;

                    zrc = inflate(&strm, Z_NO_FLUSH);
                    if (zrc != Z_OK && zrc != Z_STREAM_END) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "inflate failed");
                        break;
                    }

                    size_t have = ZU_IO_CHUNK - strm.avail_out;
                    if (have > 0) {
                        crc = zu_crc32(out_buf, have, crc);

                        if (!test_only) {
                            if (fwrite(out_buf, 1, have, fp) != have) {
                                rc = ZU_STATUS_IO;
                                zu_context_set_error(ctx, rc, "write output file failed");
                                break;
                            }
                        }

                        written += have;
                    }
                } while (strm.avail_out == 0);

                if (rc != ZU_STATUS_OK || zrc == Z_STREAM_END)
                    break;
            }

            if (rc == ZU_STATUS_OK) {
                if (zrc == Z_STREAM_END && remaining > 0) {
                    if (fseeko(ctx->in_file, (off_t)remaining, SEEK_CUR) != 0) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "seek past compressed data failed");
                    }
                }
                else if (zrc != Z_STREAM_END) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "inflate did not reach stream end");
                }
            }

            inflateEnd(&strm);
        }
    }
    else if (hdr->method == 12) {
        bz_stream strm;
        memset(&strm, 0, sizeof(strm));

        int bzrc = BZ2_bzDecompressInit(&strm, 0, 0);
        if (bzrc != BZ_OK) {
            rc = ZU_STATUS_IO;
            zu_context_set_error(ctx, rc, "bzDecompressInit failed");
        }
        else {
            uint64_t remaining = comp_size;
            bzrc = BZ_OK;

            while (rc == ZU_STATUS_OK && remaining > 0) {
                size_t to_read = remaining > ZU_IO_CHUNK ? ZU_IO_CHUNK : (size_t)remaining;
                size_t got_data = fread(in_buf, 1, to_read, ctx->in_file);
                if (got_data != to_read) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "short read on compressed data");
                    break;
                }

                if (encrypted) {
                    zu_zipcrypto_decrypt(&zc, in_buf, got_data);
                }

                remaining -= got_data;

                strm.next_in = (char*)in_buf;
                strm.avail_in = (unsigned int)got_data;

                do {
                    strm.next_out = (char*)out_buf;
                    strm.avail_out = (unsigned int)ZU_IO_CHUNK;

                    bzrc = BZ2_bzDecompress(&strm);
                    if (bzrc != BZ_OK && bzrc != BZ_STREAM_END) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "bzip2 decompression failed");
                        break;
                    }

                    size_t have = ZU_IO_CHUNK - (size_t)strm.avail_out;
                    if (have > 0) {
                        crc = zu_crc32(out_buf, have, crc);

                        if (!test_only) {
                            if (fwrite(out_buf, 1, have, fp) != have) {
                                rc = ZU_STATUS_IO;
                                zu_context_set_error(ctx, rc, "write output file failed");
                                break;
                            }
                        }

                        written += have;
                    }
                } while (strm.avail_out == 0);

                if (rc != ZU_STATUS_OK || bzrc == BZ_STREAM_END)
                    break;
            }

            if (rc == ZU_STATUS_OK) {
                if (bzrc == BZ_STREAM_END && remaining > 0) {
                    if (fseeko(ctx->in_file, (off_t)remaining, SEEK_CUR) != 0) {
                        rc = ZU_STATUS_IO;
                        zu_context_set_error(ctx, rc, "seek past compressed data failed");
                    }
                }
                else if (bzrc != BZ_STREAM_END) {
                    rc = ZU_STATUS_IO;
                    zu_context_set_error(ctx, rc, "bzip2 did not reach stream end");
                }
            }

            BZ2_bzDecompressEnd(&strm);
        }
    }
    else {
        rc = ZU_STATUS_NOT_IMPLEMENTED;
        zu_context_set_error(ctx, rc, "compression method not supported");
    }

    if (fp && fp != stdout) {
        fclose(fp);

        if (rc == ZU_STATUS_OK) {
            mode_t mode = (mode_t)((hdr->ext_attr >> 16) & 0xffff);
            if (mode != 0) {
                if (chmod(out_path, mode) != 0) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "failed to set file permissions");
                    rc = ZU_STATUS_IO;
                }
            }

            if (rc == ZU_STATUS_OK) {
                time_t mtime = dos_to_unix_time(hdr->mod_date, hdr->mod_time);
                struct timeval times[2];
                times[0].tv_sec = mtime;
                times[0].tv_usec = 0;
                times[1].tv_sec = mtime;
                times[1].tv_usec = 0;

                if (utimes(out_path, times) != 0) {
                    zu_context_set_error(ctx, ZU_STATUS_IO, "failed to set file timestamps");
                    rc = ZU_STATUS_IO;
                }
            }
        }
    }

    free(out_path);

    if (rc != ZU_STATUS_OK)
        return rc;

    if (written != uncomp_size) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "uncompressed size mismatch");
        return ZU_STATUS_IO;
    }

    if (crc != hdr->crc32) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "crc mismatch");
        return ZU_STATUS_IO;
    }

    return ZU_STATUS_OK;
}

/*
 * List archive entries
 *
 * Behavior
 * - Opens the archive, locates the central directory, and walks each entry
 * - Filters entries through include/exclude matching
 * - In unzip mode, prints matching filenames
 * - In zipinfo mode, prints formatted listings and optional archive comment
 * - Tracks which include patterns matched so we can warn about misses
 */
int zu_list_archive(ZContext* ctx) {
    if (!ctx || !ctx->archive_path)
        return ZU_STATUS_USAGE;

    struct stat st;
    uint64_t archive_size = 0;
    if (stat(ctx->archive_path, &st) == 0) {
        archive_size = (uint64_t)st.st_size;
    }

    int rc = zu_open_input(ctx, ctx->archive_path);
    if (rc != ZU_STATUS_OK)
        return rc;

    zu_cd_info cdinfo;
    rc = read_cd_info(ctx, &cdinfo, ctx->zipinfo_mode && ctx->zi_show_comments);
    if (rc != ZU_STATUS_OK) {
        zu_close_files(ctx);
        return rc;
    }

    if (fseeko(ctx->in_file, (off_t)cdinfo.cd_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to central directory failed");
        zu_close_files(ctx);
        return ZU_STATUS_IO;
    }

    bool* include_hits = NULL;
    if (ctx->include.len > 0) {
        include_hits = calloc(ctx->include.len, sizeof(bool));
        if (!include_hits) {
            zu_close_files(ctx);
            return ZU_STATUS_OOM;
        }
    }

    uint64_t matched = 0;
    uint64_t total_comp = 0;
    uint64_t total_uncomp = 0;
    size_t pager_lines = 0;

    if (ctx->zipinfo_mode && ctx->zi_header && !ctx->quiet) {
        if (zi_print_line(ctx, &pager_lines, "Archive:  %s   %" PRIu64 " bytes   %" PRIu64 " files\n", ctx->archive_path, archive_size, cdinfo.entries_total) != 0) {
            zu_close_files(ctx);
            free(include_hits);
            return ZU_STATUS_OK;
        }
        if (ctx->zi_list_entries) {
            if (zi_print_line(ctx, &pager_lines, "\n") != 0) {
                zu_close_files(ctx);
                free(include_hits);
                return ZU_STATUS_OK;
            }
        }
    }

    for (uint64_t i = 0; i < cdinfo.entries_total; ++i) {
        zu_central_header hdr;
        char* name = NULL;
        unsigned char* extra = NULL;
        uint16_t extra_len = 0;
        char* comment = NULL;
        uint16_t comment_len = 0;

        bool need_meta = ctx->zipinfo_mode && (ctx->zi_format == ZU_ZI_FMT_VERBOSE || ctx->zi_show_comments);

        uint64_t comp_size = 0;
        uint64_t uncomp_size = 0;
        uint64_t lho_offset = 0;

        rc = read_central_entry(ctx, &hdr, &name, need_meta ? &extra : NULL, need_meta ? &extra_len : NULL, need_meta ? &comment : NULL, need_meta ? &comment_len : NULL, &comp_size, &uncomp_size,
                                &lho_offset);
        if (rc != ZU_STATUS_OK) {
            zu_close_files(ctx);
            free(extra);
            free(comment);
            free(name);
            free(include_hits);
            return rc;
        }

        bool match = match_and_track(ctx, name, include_hits);
        if (match) {
            matched++;
            total_comp += comp_size;
            total_uncomp += uncomp_size;

            if (!ctx->quiet) {
                if (ctx->zipinfo_mode) {
                    if (ctx->zi_list_entries) {
                        if (ctx->zi_format == ZU_ZI_FMT_VERBOSE) {
                            if (zi_print_verbose_entry(ctx, &pager_lines, &hdr, name, extra, extra_len, comment, comment_len, comp_size, uncomp_size, lho_offset) != 0) {
                                free(extra);
                                free(comment);
                                free(name);
                                break;
                            }
                        }
                        else {
                            if (zi_print_entry(ctx, &pager_lines, &hdr, name, comp_size, uncomp_size) != 0) {
                                free(extra);
                                free(comment);
                                free(name);
                                break;
                            }
                        }
                    }
                }
                else {
                    printf("%s\n", name);
                }
            }
        }

        free(extra);
        free(comment);
        free(name);
    }

    if (ctx->zipinfo_mode) {
        if (ctx->zi_footer && !ctx->quiet) {
            double ratio = zi_ratio(total_comp, total_uncomp);
            zi_print_line(ctx, &pager_lines, "%" PRIu64 " files, %" PRIu64 " bytes uncompressed, %" PRIu64 " bytes compressed:  %0.1f%%\n", matched, total_uncomp, total_comp, ratio);
        }

        if (ctx->zi_show_comments && ctx->zip_comment && ctx->zip_comment_len > 0 && !ctx->quiet) {
            zi_print_line(ctx, &pager_lines, "\nzipfile comment:\n");
            zi_print_line(ctx, &pager_lines, "%.*s\n", (int)ctx->zip_comment_len, ctx->zip_comment);
        }

        zu_close_files(ctx);

        int final_rc = ZU_STATUS_OK;
        if (include_hits && ctx->include.len > 0) {
            for (size_t i = 0; i < ctx->include.len; ++i) {
                if (!include_hits[i]) {
                    fprintf(stderr, "caution: filename not matched:  %s\n", ctx->include.items[i]);
                    final_rc = ZU_STATUS_NO_FILES;
                }
            }
        }

        free(include_hits);
        return final_rc;
    }

    if (ctx->verbose && !ctx->quiet) {
        printf("Total entries: %" PRIu64 "\n", matched);
    }

    zu_close_files(ctx);

    int final_rc = ZU_STATUS_OK;
    if (include_hits && ctx->include.len > 0) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            if (!include_hits[i]) {
                fprintf(stderr, "caution: filename not matched:  %s\n", ctx->include.items[i]);
                final_rc = ZU_STATUS_NO_FILES;
            }
        }
    }

    free(include_hits);
    return final_rc;
}

/*
 * Walk all central directory entries and either test or extract those that match
 *
 * The central directory must be read sequentially
 * Extraction seeks away from the central directory into file data, so we
 * snapshot the next central directory position and seek back after each entry
 *
 * Post-pass behavior
 * - Emit "filename not matched" warnings for include patterns that never matched
 */
static int walk_entries(ZContext* ctx, bool test_only) {
    if (!ctx || !ctx->archive_path)
        return ZU_STATUS_USAGE;

    bool* include_hits = NULL;
    if (ctx->include.len > 0) {
        include_hits = calloc(ctx->include.len, sizeof(bool));
        if (!include_hits)
            return ZU_STATUS_OOM;
    }

    int rc = zu_open_input(ctx, ctx->archive_path);
    if (rc != ZU_STATUS_OK) {
        free(include_hits);
        return rc;
    }

    zu_cd_info cdinfo;
    rc = read_cd_info(ctx, &cdinfo, false);
    if (rc != ZU_STATUS_OK) {
        zu_close_files(ctx);
        free(include_hits);
        return rc;
    }

    if (fseeko(ctx->in_file, (off_t)cdinfo.cd_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to central directory failed");
        zu_close_files(ctx);
        free(include_hits);
        return ZU_STATUS_IO;
    }

    for (uint64_t i = 0; i < cdinfo.entries_total; ++i) {
        zu_central_header hdr;
        char* name = NULL;
        uint64_t comp_size = 0;
        uint64_t uncomp_size = 0;
        uint64_t lho_offset = 0;

        rc = read_central_entry(ctx, &hdr, &name, NULL, NULL, NULL, NULL, &comp_size, &uncomp_size, &lho_offset);
        if (rc != ZU_STATUS_OK) {
            zu_close_files(ctx);
            free(include_hits);
            free(name);
            return rc;
        }

        off_t next_cd_pos = ftello(ctx->in_file);
        if (next_cd_pos < 0) {
            free(name);
            zu_close_files(ctx);
            free(include_hits);
            zu_context_set_error(ctx, ZU_STATUS_IO, "ftello failed");
            return ZU_STATUS_IO;
        }

        if (match_and_track(ctx, name, include_hits)) {
            rc = extract_or_test_entry(ctx, &hdr, name, test_only, comp_size, uncomp_size, lho_offset);
        }
        else {
            rc = ZU_STATUS_OK;
        }

        free(name);

        if (rc != ZU_STATUS_OK) {
            zu_close_files(ctx);
            free(include_hits);
            return rc;
        }

        if (fseeko(ctx->in_file, next_cd_pos, SEEK_SET) != 0) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "seek to next central header failed");
            zu_close_files(ctx);
            free(include_hits);
            return ZU_STATUS_IO;
        }
    }

    zu_close_files(ctx);

    int final_rc = ZU_STATUS_OK;
    if (include_hits && ctx->include.len > 0) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            if (!include_hits[i]) {
                fprintf(stderr, "caution: filename not matched:  %s\n", ctx->include.items[i]);
                final_rc = ZU_STATUS_NO_FILES;
            }
        }
    }

    free(include_hits);
    return final_rc;
}

/*
 * Public entry points used by unzip/zip drivers
 */
int zu_test_archive(ZContext* ctx) {
    return walk_entries(ctx, true);
}

int zu_extract_archive(ZContext* ctx) {
    return walk_entries(ctx, false);
}

/*
 * Load the central directory into ctx->existing_entries for modification workflows
 *
 * This path is used by the writer when it needs to read and then rewrite an archive
 * It parses each central directory record and stores a full zu_existing_entry object
 *
 * Failure and recovery
 * - If reading CD fails and ctx->fix_fix_archive is set, attempt recovery by scanning locals
 */
int zu_load_central_directory(ZContext* ctx) {
    if (!ctx || !ctx->archive_path)
        return ZU_STATUS_USAGE;

    ctx->existing_loaded = false;

    int rc = zu_open_input(ctx, ctx->archive_path);
    if (rc != ZU_STATUS_OK)
        return rc;

    zu_cd_info cdinfo;
    rc = read_cd_info(ctx, &cdinfo, true);
    if (rc != ZU_STATUS_OK) {
        if (ctx->fix_fix_archive) {
            return zu_recover_central_directory(ctx);
        }
        zu_close_files(ctx);
        return rc;
    }

    if (fseeko(ctx->in_file, (off_t)cdinfo.cd_offset, SEEK_SET) != 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "seek to central directory failed");
        zu_close_files(ctx);
        return ZU_STATUS_IO;
    }

    for (uint64_t i = 0; i < cdinfo.entries_total; ++i) {
        zu_central_header hdr;
        char* name = NULL;
        unsigned char* extra = NULL;
        uint16_t extra_len = 0;
        char* comment = NULL;
        uint16_t comment_len = 0;
        uint64_t comp_size = 0;
        uint64_t uncomp_size = 0;
        uint64_t lho_offset = 0;

        rc = read_central_entry(ctx, &hdr, &name, &extra, &extra_len, &comment, &comment_len, &comp_size, &uncomp_size, &lho_offset);
        if (rc != ZU_STATUS_OK) {
            free(name);
            free(extra);
            free(comment);
            zu_close_files(ctx);
            return rc;
        }

        zu_existing_entry* entry = malloc(sizeof(zu_existing_entry));
        if (!entry) {
            free(name);
            free(extra);
            free(comment);
            zu_close_files(ctx);
            return ZU_STATUS_OOM;
        }

        entry->hdr = hdr;
        entry->name = name;
        entry->extra = extra;
        entry->extra_len = extra_len;
        entry->comment = comment;
        entry->comment_len = comment_len;
        entry->comp_size = comp_size;
        entry->uncomp_size = uncomp_size;
        entry->lho_offset = lho_offset;
        entry->delete = false;
        entry->changed = false;

        if (ctx->existing_entries.len == ctx->existing_entries.cap) {
            size_t new_cap = ctx->existing_entries.cap == 0 ? 16 : ctx->existing_entries.cap * 2;
            char** new_items = realloc(ctx->existing_entries.items, new_cap * sizeof(char*));
            if (!new_items) {
                free(entry->name);
                free(entry->extra);
                free(entry->comment);
                free(entry);
                zu_close_files(ctx);
                return ZU_STATUS_OOM;
            }
            ctx->existing_entries.items = new_items;
            ctx->existing_entries.cap = new_cap;
        }

        ctx->existing_entries.items[ctx->existing_entries.len++] = (char*)entry;
    }

    ctx->existing_loaded = true;
    return ZU_STATUS_OK;
}
