#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

#include "ctx.h"
#include "ops.h"
#include "ziputils.h"
#include "reader.h"

static time_t parse_date(const char* str) {
    struct tm tm = {0};
    char* end;
    // Initialize tm to avoid garbage
    tm.tm_isdst = -1;

    // Try ISO 8601: yyyy-mm-dd
    end = strptime(str, "%Y-%m-%d", &tm);
    if (end && *end == '\0') {
        // Validate month/day ranges (mktime will normalize, but we can reject invalid)
        if (tm.tm_mon >= 0 && tm.tm_mon <= 11 && tm.tm_mday >= 1 && tm.tm_mday <= 31) {
            return mktime(&tm);
        }
    }

    // Try mmddyyyy
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;
    end = strptime(str, "%m%d%Y", &tm);
    if (end && *end == '\0') {
        // Validate month/day ranges
        if (tm.tm_mon >= 0 && tm.tm_mon <= 11 && tm.tm_mday >= 1 && tm.tm_mday <= 31) {
            return mktime(&tm);
        }
    }

    return (time_t)-1;
}

static int read_stdin_names(ZContext* ctx) {
    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, stdin)) != -1) {
        if (linelen > 0 && line[linelen - 1] == '\n') {
            line[linelen - 1] = '\0';
            linelen--;
        }
        if (linelen > 0) {
            if (zu_strlist_push(&ctx->include, line) != 0) {
                free(line);
                return ZU_STATUS_OOM;
            }
        }
    }
    free(line);
    if (ferror(stdin)) {
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

static bool is_alias(const char* argv0, const char* name) {
    const char* base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    return strcmp(base, name) == 0;
}

static int map_exit_code(int status) {
    switch (status) {
        case ZU_STATUS_OK:
            return 0;
        case ZU_STATUS_USAGE:
            return 16;
        case ZU_STATUS_IO:
            return 2;
        case ZU_STATUS_OOM:
            return 5;
        case ZU_STATUS_NO_FILES:
            return 12;
        case ZU_STATUS_NOT_IMPLEMENTED:
            return 3;
        default:
            return 3;
    }
}

static void warn_stub(ZContext* ctx, const char* opt, const char* detail) {
    char buf[256];
    snprintf(buf, sizeof(buf), "zip: %s is stubbed (Info-ZIP 3.0 parity pending): %s", opt, detail ? detail : "behavior incomplete");
    zu_warn_once(ctx, buf);
}

static void emit_zip_stub_warnings(ZContext* ctx, bool invoked_as_zipcloak) {
    if (ctx->fix_archive || ctx->fix_fix_archive) {
        warn_stub(ctx, ctx->fix_fix_archive ? "-FF" : "-F", "recovery semantics are still under validation");
    }
    if (invoked_as_zipcloak) {
        warn_stub(ctx, "zipcloak", "only encrypts new writes; existing entries are left untouched");
    }
}

static const char* compression_method_name(int method) {
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

static void trace_effective_zip_defaults(ZContext* ctx) {
    zu_trace_option(ctx, "effective compression: %s level %d", compression_method_name(ctx->compression_method), ctx->compression_level);
    zu_trace_option(ctx, "paths: %s (recursive %s)", ctx->store_paths ? "preserve" : "junk", ctx->recursive ? "on" : "off");
    const char* target = ctx->output_to_stdout ? "stdout" : (ctx->output_path ? ctx->output_path : (ctx->archive_path ? ctx->archive_path : "(unset)"));
    zu_trace_option(ctx, "output target: %s", target);
    const char* mode = ctx->difference_mode ? "delete" : (ctx->freshen ? "freshen" : (ctx->update ? "update" : (ctx->filesync ? "filesync" : "create/modify")));
    zu_trace_option(ctx, "mode: %s%s%s%s", mode, ctx->remove_source ? " +move" : "", ctx->encrypt ? " +encrypt" : "", ctx->dry_run ? " +dry-run" : "");
    zu_trace_option(ctx, "quiet level: %d, verbose: %s", ctx->quiet_level, ctx->verbose ? "on" : "off");
}

static void emit_option_trace(const char* tool, ZContext* ctx) {
    if (!(ctx->verbose || ctx->log_info || ctx->dry_run)) {
        return;
    }
    if (ctx->option_events.len == 0) {
        return;
    }
    zu_log(ctx, "%s option resolution:\n", tool);
    for (size_t i = 0; i < ctx->option_events.len; ++i) {
        zu_log(ctx, "  %s\n", ctx->option_events.items[i]);
    }
}

static const char* zipnote_archive_label = "(zip file comment below this line)";

static void zipnote_emit_comment(const char* data, size_t len) {
    if (!data || len == 0) {
        printf("\n");
        return;
    }
    size_t i = 0;
    while (i < len) {
        size_t line_end = i;
        while (line_end < len && data[line_end] != '\n')
            line_end++;
        size_t line_len = line_end - i;
        if (line_len > 0 && data[i] == '@')
            putchar('@');
        fwrite(data + i, 1, line_len, stdout);
        putchar('\n');
        i = line_end + 1;
    }
}

static int zipnote_list(ZContext* ctx) {
    int rc = zu_load_central_directory(ctx);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
        zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
        printf("@ %s\n", e->name);
        zipnote_emit_comment(e->comment, e->comment_len);
        printf("@\n");
    }

    printf("@ %s\n", zipnote_archive_label);
    zipnote_emit_comment(ctx->zip_comment, ctx->zip_comment_len);
    printf("@\n");

    return ZU_STATUS_OK;
}

typedef struct {
    char* name;
    char* comment;
    size_t comment_len;
    bool is_archive;
} zipnote_edit;

static void zipnote_edit_free(zipnote_edit* e) {
    if (!e)
        return;
    free(e->name);
    free(e->comment);
    free(e);
}

static int zipnote_add_edit(zipnote_edit*** edits, size_t* len, size_t* cap, zipnote_edit* e) {
    if (*len == *cap) {
        size_t new_cap = *cap == 0 ? 8 : *cap * 2;
        zipnote_edit** np = realloc(*edits, new_cap * sizeof(zipnote_edit*));
        if (!np)
            return ZU_STATUS_OOM;
        *edits = np;
        *cap = new_cap;
    }
    (*edits)[(*len)++] = e;
    return ZU_STATUS_OK;
}

static int zipnote_parse(ZContext* ctx, zipnote_edit*** edits_out, size_t* edits_len_out) {
    (void)ctx;
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t got;
    zipnote_edit** edits = NULL;
    size_t edits_len = 0, edits_cap = 0;

    char* cur_name = NULL;
    char* comment_buf = NULL;
    size_t comment_len = 0, comment_cap = 0;

    while ((got = getline(&line, &line_cap, stdin)) != -1) {
        if (got > 0 && line[got - 1] == '\n') {
            line[got - 1] = '\0';
            got--;
        }

        if (line[0] == '@' && line[1] != '@') {
            if (cur_name) {
                zipnote_edit* e = calloc(1, sizeof(zipnote_edit));
                if (!e) {
                    free(line);
                    free(comment_buf);
                    free(cur_name);
                    for (size_t i = 0; i < edits_len; ++i)
                        zipnote_edit_free(edits[i]);
                    free(edits);
                    return ZU_STATUS_OOM;
                }
                e->name = cur_name;
                e->comment = comment_buf;
                e->comment_len = comment_len;
                e->is_archive = (strcmp(cur_name, zipnote_archive_label) == 0);
                int add_rc = zipnote_add_edit(&edits, &edits_len, &edits_cap, e);
                if (add_rc != ZU_STATUS_OK) {
                    zipnote_edit_free(e);
                    free(line);
                    for (size_t i = 0; i < edits_len; ++i)
                        zipnote_edit_free(edits[i]);
                    free(edits);
                    return add_rc;
                }
                cur_name = NULL;
                comment_buf = NULL;
                comment_len = 0;
                comment_cap = 0;
            }

            const char* name = line + 1;
            while (*name == ' ')
                name++;
            if (*name == '\0') {
                free(cur_name);
                cur_name = NULL;
                continue;
            }
            cur_name = strdup(name);
            comment_len = 0;
            comment_cap = 0;
            free(comment_buf);
            comment_buf = NULL;
            continue;
        }

        const char* data = line;
        size_t data_len = (size_t)got;
        if (line[0] == '@' && line[1] == '@') {
            data = line + 1;
            data_len -= 1;
        }

        if (!cur_name) {
            continue;
        }

        if (comment_len + data_len + 1 > comment_cap) {
            size_t new_cap = comment_cap == 0 ? 256 : comment_cap * 2;
            while (new_cap < comment_len + data_len + 1)
                new_cap *= 2;
            char* nb = realloc(comment_buf, new_cap);
            if (!nb) {
                free(line);
                free(comment_buf);
                free(cur_name);
                for (size_t i = 0; i < edits_len; ++i)
                    zipnote_edit_free(edits[i]);
                free(edits);
                return ZU_STATUS_OOM;
            }
            comment_buf = nb;
            comment_cap = new_cap;
        }
        memcpy(comment_buf + comment_len, data, data_len);
        comment_len += data_len;
        comment_buf[comment_len++] = '\n';
    }

    if (cur_name) {
        zipnote_edit* e = calloc(1, sizeof(zipnote_edit));
        if (!e) {
            free(line);
            free(comment_buf);
            free(cur_name);
            for (size_t i = 0; i < edits_len; ++i)
                zipnote_edit_free(edits[i]);
            free(edits);
            return ZU_STATUS_OOM;
        }
        e->name = cur_name;
        e->comment = comment_buf;
        e->comment_len = comment_len;
        e->is_archive = (strcmp(cur_name, zipnote_archive_label) == 0);
        int add_rc = zipnote_add_edit(&edits, &edits_len, &edits_cap, e);
        if (add_rc != ZU_STATUS_OK) {
            zipnote_edit_free(e);
            free(line);
            for (size_t i = 0; i < edits_len; ++i)
                zipnote_edit_free(edits[i]);
            free(edits);
            return add_rc;
        }
        cur_name = NULL;
        comment_buf = NULL;
        comment_len = 0;
        comment_cap = 0;
    }

    free(line);
    free(comment_buf);
    free(cur_name);

    *edits_out = edits;
    *edits_len_out = edits_len;
    return ZU_STATUS_OK;
}

static zu_existing_entry* zipnote_find_entry(ZContext* ctx, const char* name) {
    for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
        zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static int zipnote_apply(ZContext* ctx) {
    int rc = zu_load_central_directory(ctx);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    zipnote_edit** edits = NULL;
    size_t edits_len = 0;
    rc = zipnote_parse(ctx, &edits, &edits_len);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    bool seen_archive = false;
    for (size_t i = 0; i < edits_len; ++i) {
        zipnote_edit* e = edits[i];
        if (e->is_archive) {
            free(ctx->zip_comment);
            ctx->zip_comment = e->comment;
            ctx->zip_comment_len = e->comment_len;
            ctx->zip_comment_specified = true;
            e->comment = NULL;
            seen_archive = true;
            continue;
        }

        zu_existing_entry* existing = zipnote_find_entry(ctx, e->name);
        if (!existing) {
            fprintf(stderr, "zipnote: warning: entry not found: %s\n", e->name);
            continue;
        }

        free(existing->comment);
        existing->comment = e->comment;
        existing->comment_len = (uint16_t)e->comment_len;
        existing->changed = true;
        e->comment = NULL;
    }

    if (!seen_archive) {
        ctx->zip_comment_specified = false;
    }

    for (size_t i = 0; i < edits_len; ++i)
        zipnote_edit_free(edits[i]);
    free(edits);

    ctx->existing_loaded = true;
    return zu_modify_archive(ctx);
}

static int read_zip_comment(ZContext* ctx) {
    if (!ctx) {
        return ZU_STATUS_USAGE;
    }

    size_t cap = 1024;
    size_t len = 0;
    char* buf = malloc(cap);
    if (!buf) {
        return ZU_STATUS_OOM;
    }

    size_t got = 0;
    while ((got = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += got;
        if (len == cap) {
            size_t new_cap = cap * 2;
            char* nb = realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                return ZU_STATUS_OOM;
            }
            buf = nb;
            cap = new_cap;
        }
    }
    if (ferror(stdin)) {
        free(buf);
        return ZU_STATUS_IO;
    }

    if (len > UINT16_MAX) {
        free(buf);
        return ZU_STATUS_USAGE;
    }

    if (len == cap) {
        char* nb = realloc(buf, cap + 1);
        if (!nb) {
            free(buf);
            return ZU_STATUS_OOM;
        }
        buf = nb;
        cap += 1;
    }
    buf[len] = '\0';
    free(ctx->zip_comment);
    ctx->zip_comment = buf;
    ctx->zip_comment_len = len;
    return ZU_STATUS_OK;
}

static void print_usage(FILE* to, const char* argv0) {
    fprintf(to,
            "Usage: %s [options] archive.zip [inputs...]\n"
            "\n"
            "Info-ZIP–style CLI (archiving work still in progress).\n"
            "\n"
            "Modes:\n"
            "  • Default: modify/create archive.zip\n"
            "  • Filter: with no archive or inputs, read stdin and write zip to stdout\n"
            "  • Input name \"-\": treat as file data from stdin; \"-@\": read names from stdin; \"--\": end options\n"
            "\n"
            "Options parsed:\n"
            "  Paths:      -r recurse, -j junk paths\n"
            "  Update:     -u update newer, -f freshen, -m move, -d delete patterns, -FS filesync\n"
            "  Select:     -x patterns..., -i patterns... (lists end at next option/--)\n"
            "  Dates:      -t mmddyyyy (after), -tt mmddyyyy (before)\n"
            "  Compress:   -0..-9 level, -Z method (deflate/store/bzip2), -n suffixes store-only\n"
            "  Output:     -O path (write elsewhere), -b dir (temp dir), -o archive mtime=max entry, \"-\" for stdout\n"
            "  Split:      -s size[kmgt], -sp pause between parts\n"
            "  Logging:    -lf file, -la append, -li info-level\n"
            "  Dry-run:    --dry-run (log what would happen without writing)\n"
            "  Text:       -l LF->CRLF, -ll CRLF->LF\n"
            "  Entries:    -D no dir entries, -X strip extra attrs, -y store symlinks as links\n"
            "  Quiet/Verb: -q (stackable) / -v, -T test after write\n"
            "  Fix:        -F / -FF\n"
            "  Encrypt:    -e, -P password\n"
            "  Comments:   -z (zipfile comment from stdin)\n"
            "  Zipnote:    -w (when invoked as zipnote)\n"
            "  Not yet:    -c -A -J\n"
            "  --help      Show this help\n",
            argv0);
}

static bool is_cluster_flag(char c) {
    switch (c) {
        case 'r':
        case 'j':
        case 'T':
        case 'q':
        case 'v':
        case 'm':
        case 'd':
        case 'f':
        case 'u':
        case 'D':
        case 'X':
        case 'y':
        case 'e':
        case 'l':
            return true;
        default:
            break;
    }
    return (c >= '0' && c <= '9');
}

static int apply_cluster_flag(char c, ZContext* ctx) {
    switch (c) {
        case 'r':
            ctx->recursive = true;
            zu_trace_option(ctx, "-r recurse into directories");
            return ZU_STATUS_OK;
        case 'j':
            ctx->store_paths = false;
            zu_trace_option(ctx, "-j junk paths");
            return ZU_STATUS_OK;
        case 'T':
            ctx->test_integrity = true;
            zu_trace_option(ctx, "-T test after write");
            return ZU_STATUS_OK;
        case 'q':
            ctx->quiet_level++;
            ctx->quiet = ctx->quiet_level > 0;
            ctx->verbose = false;
            zu_trace_option(ctx, "-q quiet level %d (verbose off)", ctx->quiet_level);
            return ZU_STATUS_OK;
        case 'v':
            ctx->verbose = true;
            zu_trace_option(ctx, "-v verbose enabled");
            return ZU_STATUS_OK;
        case 'm':
            ctx->remove_source = true;
            zu_trace_option(ctx, "-m move sources after writing");
            return ZU_STATUS_OK;
        case 'd':
            ctx->difference_mode = true;
            zu_trace_option(ctx, "-d delete patterns from archive");
            return ZU_STATUS_OK;
        case 'f':
            ctx->freshen = true;
            zu_trace_option(ctx, "-f freshen existing entries");
            return ZU_STATUS_OK;
        case 'u':
            ctx->update = true;
            zu_trace_option(ctx, "-u update newer entries");
            return ZU_STATUS_OK;
        case 'D':
            ctx->no_dir_entries = true;
            zu_trace_option(ctx, "-D suppress directory entries");
            return ZU_STATUS_OK;
        case 'X':
            ctx->exclude_extra_attrs = true;
            zu_trace_option(ctx, "-X drop extra attributes");
            return ZU_STATUS_OK;
        case 'y':
            ctx->store_symlinks = true;
            ctx->allow_symlinks = true;
            zu_trace_option(ctx, "-y store symlinks as links");
            return ZU_STATUS_OK;
        case 'e':
            ctx->encrypt = true;
            zu_trace_option(ctx, "-e enable encryption");
            return ZU_STATUS_OK;
        case 'l':
            ctx->line_mode = ZU_LINE_LF_TO_CRLF;
            zu_trace_option(ctx, "-l translate LF->CRLF");
            return ZU_STATUS_OK;
        default:
            break;
    }

    if (c >= '0' && c <= '9') {
        ctx->compression_level = c - '0';
        zu_trace_option(ctx, "compression level set to %d", ctx->compression_level);
        return ZU_STATUS_OK;
    }

    return ZU_STATUS_USAGE;
}

static int parse_pattern_list(ZContext* ctx, int argc, char** argv, int* idx, bool* endopts, bool include) {
    int i = *idx + 1;
    bool any = false;

    for (; i < argc; ++i) {
        const char* tok = argv[i];
        if (!*endopts && strcmp(tok, "--") == 0) {
            *endopts = true;
            ++i;
            break;
        }
        if (!*endopts && tok[0] == '-' && tok[1] != '\0') {
            break;
        }
        if (include) {
            if (zu_strlist_push(&ctx->include_patterns, tok) != 0)
                return ZU_STATUS_OOM;
        }
        else {
            if (zu_strlist_push(&ctx->exclude, tok) != 0)
                return ZU_STATUS_OOM;
        }
        any = true;
    }

    if (!any) {
        fprintf(stderr, "zip: option requires one or more patterns\n");
        return ZU_STATUS_USAGE;
    }

    *idx = i - 1;
    return ZU_STATUS_OK;
}

static int parse_pattern_list_with_first(ZContext* ctx, const char* first, int argc, char** argv, int* idx, bool* endopts, bool include) {
    if (first) {
        if (include) {
            if (zu_strlist_push(&ctx->include_patterns, first) != 0)
                return ZU_STATUS_OOM;
        }
        else {
            if (zu_strlist_push(&ctx->exclude, first) != 0)
                return ZU_STATUS_OOM;
        }
        return ZU_STATUS_OK;
    }
    return parse_pattern_list(ctx, argc, argv, idx, endopts, include);
}

static int push_suffixes(ZContext* ctx, const char* str) {
    if (!str || !*str)
        return ZU_STATUS_OK;
    const char* p = str;
    while (*p) {
        const char* start = p;
        while (*p && *p != ':')
            p++;
        size_t len = (size_t)(p - start);
        char* suf = strndup(start, len);
        if (!suf)
            return ZU_STATUS_OOM;
        if (zu_strlist_push(&ctx->no_compress_suffixes, suf) != 0) {
            free(suf);
            return ZU_STATUS_OOM;
        }
        free(suf);
        p += (*p == ':') ? 1 : 0;
    }
    return ZU_STATUS_OK;
}

static int parse_suffix_list(ZContext* ctx, const char* first, int argc, char** argv, int* idx, bool* endopts) {
    (void)endopts;
    if (first) {
        return push_suffixes(ctx, first);
    }
    if (*idx + 1 >= argc) {
        fprintf(stderr, "zip: -n requires suffix list\n");
        return ZU_STATUS_USAGE;
    }
    const char* tok = argv[++(*idx)];
    return push_suffixes(ctx, tok);
}

static int parse_long_option(const char* tok, int argc, char** argv, int* idx, ZContext* ctx) {
    ctx->used_long_option = true;
    const char* name = tok + 2;
    const char* value = NULL;
    char namebuf[64];
    const char* eq = strchr(name, '=');
    if (eq) {
        size_t len = (size_t)(eq - name);
        if (len >= sizeof(namebuf))
            len = sizeof(namebuf) - 1;
        memcpy(namebuf, name, len);
        namebuf[len] = '\0';
        name = namebuf;
        value = eq + 1;
    }

    if (strcmp(name, "dry-run") == 0) {
        ctx->dry_run = true;
        ctx->verbose = true;
        ctx->quiet = false;
        zu_trace_option(ctx, "--dry-run enable plan-only mode");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "recurse-paths") == 0) {
        ctx->recursive = true;
        zu_trace_option(ctx, "--recurse-paths");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "test") == 0) {
        ctx->test_integrity = true;
        zu_trace_option(ctx, "--test after write");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "test-command") == 0) {
        if (!value) {
            if (*idx + 1 >= argc) {
                fprintf(stderr, "zip: --test-command requires an argument\n");
                return ZU_STATUS_USAGE;
            }
            value = argv[++(*idx)];
        }
        free(ctx->test_command);
        ctx->test_command = strdup(value);
        if (!ctx->test_command)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "--test-command");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "quiet") == 0) {
        ctx->quiet_level++;
        ctx->quiet = ctx->quiet_level > 0;
        ctx->verbose = false;
        zu_trace_option(ctx, "--quiet level %d (verbose off)", ctx->quiet_level);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "verbose") == 0) {
        ctx->verbose = true;
        zu_trace_option(ctx, "--verbose");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "encrypt") == 0) {
        ctx->encrypt = true;
        zu_trace_option(ctx, "--encrypt");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "password") == 0) {
        if (!value) {
            if (*idx + 1 >= argc) {
                fprintf(stderr, "zip: --password requires an argument\n");
                return ZU_STATUS_USAGE;
            }
            value = argv[++(*idx)];
        }
        free(ctx->password);
        ctx->password = strdup(value);
        if (!ctx->password)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "--password (provided)");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "help") == 0) {
        print_usage(stdout, argv[0]);
        return ZU_STATUS_USAGE;
    }
    if (strcmp(name, "output-file") == 0) {
        if (!value) {
            if (*idx + 1 >= argc) {
                fprintf(stderr, "zip: --output-file requires an argument\n");
                return ZU_STATUS_USAGE;
            }
            value = argv[++(*idx)];
        }
        ctx->output_path = value;
        zu_trace_option(ctx, "--output-file=%s", value);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "out") == 0) {
        if (!value) {
            if (*idx + 1 >= argc) {
                fprintf(stderr, "zip: --out requires an argument\n");
                return ZU_STATUS_USAGE;
            }
            value = argv[++(*idx)];
        }
        ctx->output_path = value;
        zu_trace_option(ctx, "--out=%s", value);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "copy") == 0) {
        ctx->copy_mode = true;
        zu_trace_option(ctx, "--copy mode enabled");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "la") == 0 || strcmp(name, "log-append") == 0) {
        ctx->log_append = true;
        zu_trace_option(ctx, "--log-append");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "lf") == 0 || strcmp(name, "logfile-path") == 0) {
        if (!value) {
            if (*idx + 1 >= argc) {
                fprintf(stderr, "zip: --logfile-path requires an argument\n");
                return ZU_STATUS_USAGE;
            }
            value = argv[++(*idx)];
        }
        free(ctx->log_path);
        ctx->log_path = strdup(value);
        return ctx->log_path ? ZU_STATUS_OK : ZU_STATUS_OOM;
    }
    if (strcmp(name, "li") == 0 || strcmp(name, "log-info") == 0) {
        ctx->log_info = true;
        zu_trace_option(ctx, "--log-info");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "tt") == 0) {
        if (!value) {
            if (*idx + 1 >= argc) {
                fprintf(stderr, "zip: -tt requires a date\n");
                return ZU_STATUS_USAGE;
            }
            value = argv[++(*idx)];
        }
        ctx->filter_before = parse_date(value);
        if (ctx->filter_before == (time_t)-1) {
            fprintf(stderr, "zip: invalid date format for -tt: %s\n", value);
            return ZU_STATUS_USAGE;
        }
        ctx->has_filter_before = true;
        zu_trace_option(ctx, "--tt before %s", value);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "filesync") == 0 || strcmp(name, "FS") == 0) {
        ctx->filesync = true;
        ctx->update = true;
        zu_trace_option(ctx, "--filesync");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "split-size") == 0 || strcmp(name, "pause") == 0 || strcmp(name, "sp") == 0) {
        fprintf(stderr, "zip: split archives are not supported in this build\n");
        return ZU_STATUS_NOT_IMPLEMENTED;
    }
    if (strcmp(name, "fix") == 0) {
        ctx->fix_archive = true;
        zu_trace_option(ctx, "--fix");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "FF") == 0 || strcmp(name, "fixfix") == 0) {
        ctx->fix_fix_archive = true;
        zu_trace_option(ctx, "--fixfix");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "ll") == 0) {
        ctx->line_mode = ZU_LINE_CRLF_TO_LF;
        zu_trace_option(ctx, "--ll translate CRLF->LF");
        return ZU_STATUS_OK;
    }

    print_usage(stderr, argv[0]);
    return ZU_STATUS_USAGE;
}

static int parse_zip_args(int argc, char** argv, ZContext* ctx, bool is_zipnote) {
    bool endopts = false;
    int i = 1;

    while (i < argc) {
        const char* tok = argv[i];

        if (!endopts && strcmp(tok, "--") == 0) {
            endopts = true;
            ++i;
            continue;
        }

        if (!endopts && tok[0] == '-' && tok[1] != '\0') {
            zu_trace_option(ctx, "option %s", tok);
            if (strcmp(tok, "-xi") == 0 || strcmp(tok, "-ix") == 0) {
                fprintf(stderr, "zip: use -x <patterns> ... -i <patterns> instead of %s\n", tok);
                return ZU_STATUS_USAGE;
            }
            if (strcmp(tok, "-sp") == 0) {
                fprintf(stderr, "zip: split archives are not supported in this build\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }
            if (strcmp(tok, "-R") == 0) {
                ctx->recursive = true;
                ctx->recurse_from_cwd = true;
                zu_trace_option(ctx, "-R recurse with patterns");
                ++i;
                continue;
            }
            if (strcmp(tok, "-U") == 0) {
                ctx->copy_mode = true;
                zu_trace_option(ctx, "-U copy mode");
                ++i;
                continue;
            }
            if (strcmp(tok, "-TT") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "zip: -TT requires a command\n");
                    return ZU_STATUS_USAGE;
                }
                free(ctx->test_command);
                ctx->test_command = strdup(argv[++i]);
                if (!ctx->test_command) {
                    return ZU_STATUS_OOM;
                }
                zu_trace_option(ctx, "-TT custom test command");
                ++i;
                continue;
            }
            if (strcmp(tok, "-FF") == 0) {
                ctx->fix_fix_archive = true;
                zu_trace_option(ctx, "-FF fixfix");
                ++i;
                continue;
            }
            if (strcmp(tok, "-ll") == 0) {
                ctx->line_mode = ZU_LINE_CRLF_TO_LF;
                zu_trace_option(ctx, "-ll translate CRLF->LF");
                ++i;
                continue;
            }
            if (strcmp(tok, "-w") == 0) {
                if (!is_zipnote) {
                    print_usage(stderr, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                ctx->zipnote_write = true;
                zu_trace_option(ctx, "-w write zipnote comments");
                ++i;
                continue;
            }
            if (tok[1] == '-') {
                int rc = parse_long_option(tok, argc, argv, &i, ctx);
                if (rc != ZU_STATUS_OK) {
                    return rc;
                }
                ++i;
                continue;
            }

            char first = tok[1];
            const char* rest = tok + 2;

            if (strncmp(tok, "-tt", 3) == 0) {
                const char* arg = tok[3] ? tok + 3 : NULL;
                if (!arg) {
                    if (i + 1 >= argc) {
                        fprintf(stderr, "zip: -tt requires a date\n");
                        return ZU_STATUS_USAGE;
                    }
                    arg = argv[++i];
                }
                ctx->filter_before = parse_date(arg);
                if (ctx->filter_before == (time_t)-1) {
                    fprintf(stderr, "zip: invalid date format for -tt: %s\n", arg);
                    return ZU_STATUS_USAGE;
                }
                ctx->has_filter_before = true;
                zu_trace_option(ctx, "-tt before %s", arg);
                ++i;
                continue;
            }

            if (strcmp(tok, "-lf") == 0) {
                const char* arg = NULL;
                if (i + 1 >= argc) {
                    fprintf(stderr, "zip: -lf requires a path\n");
                    return ZU_STATUS_USAGE;
                }
                arg = argv[++i];
                free(ctx->log_path);
                ctx->log_path = strdup(arg);
                if (!ctx->log_path)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-lf %s", arg);
                ++i;
                continue;
            }
            if (strcmp(tok, "-la") == 0) {
                ctx->log_append = true;
                zu_trace_option(ctx, "-la");
                ++i;
                continue;
            }
            if (strcmp(tok, "-li") == 0) {
                ctx->log_info = true;
                zu_trace_option(ctx, "-li");
                ++i;
                continue;
            }
            if (strcmp(tok, "-FS") == 0) {
                ctx->filesync = true;
                ctx->update = true;
                zu_trace_option(ctx, "-FS filesync");
                ++i;
                continue;
            }

            if (first == 's') {
                fprintf(stderr, "zip: split archives are not supported in this build\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }

            if (first == 'b' || first == 't' || first == 'P' || first == 'O' || first == 'Z') {
                const char* arg = rest[0] ? rest : NULL;
                if (!arg) {
                    if (i + 1 >= argc) {
                        print_usage(stderr, argv[0]);
                        return ZU_STATUS_USAGE;
                    }
                    arg = argv[++i];
                }

                switch (first) {
                    case 'b':
                        free(ctx->temp_dir);
                        ctx->temp_dir = strdup(arg);
                        if (!ctx->temp_dir)
                            return ZU_STATUS_OOM;
                        zu_trace_option(ctx, "-b %s", arg);
                        break;
                    case 't':
                        ctx->filter_after = parse_date(arg);
                        if (ctx->filter_after == (time_t)-1) {
                            fprintf(stderr, "zip: invalid date format for -t: %s\n", arg);
                            return ZU_STATUS_USAGE;
                        }
                        ctx->has_filter_after = true;
                        zu_trace_option(ctx, "-t after %s", arg);
                        break;
                    case 'P':
                        free(ctx->password);
                        ctx->password = strdup(arg);
                        if (!ctx->password)
                            return ZU_STATUS_OOM;
                        zu_trace_option(ctx, "-P (password provided)");
                        break;
                    case 'O':
                        ctx->output_path = arg;
                        zu_trace_option(ctx, "-O %s", arg);
                        break;
                    case 'Z':
                        if (strcasecmp(arg, "deflate") == 0) {
                            ctx->compression_method = 8;
                            zu_trace_option(ctx, "-Z deflate");
                        }
                        else if (strcasecmp(arg, "store") == 0) {
                            ctx->compression_method = 0;
                            zu_trace_option(ctx, "-Z store");
                        }
                        else if (strcasecmp(arg, "bzip2") == 0) {
                            ctx->compression_method = 12;
                            zu_trace_option(ctx, "-Z bzip2");
                        }
                        else {
                            fprintf(stderr, "zip: unknown compression method '%s'\n", arg);
                            return ZU_STATUS_USAGE;
                        }
                        break;
                }
                ++i;
                continue;
            }

            if ((first == '@' || first == 'F' || first == 'z' || first == 'c' || first == 'o' || first == 'A' || first == 'J') && rest[0] != '\0') {
                fprintf(stderr, "zip: option '%c' must be separate (not clustered)\n", first);
                return ZU_STATUS_USAGE;
            }

            if (strcmp(tok, "-x") == 0) {
                int rc = parse_pattern_list(ctx, argc, argv, &i, &endopts, false);
                if (rc != ZU_STATUS_OK)
                    return rc;
                zu_trace_option(ctx, "-x patterns");
                ++i;
                continue;
            }
            if (first == 'x' && rest[0] != '\0') {
                int rc = parse_pattern_list_with_first(ctx, rest, argc, argv, &i, &endopts, false);
                if (rc != ZU_STATUS_OK)
                    return rc;
                zu_trace_option(ctx, "-x patterns");
                ++i;
                continue;
            }
            if (strcmp(tok, "-i") == 0) {
                int rc = parse_pattern_list(ctx, argc, argv, &i, &endopts, true);
                if (rc != ZU_STATUS_OK)
                    return rc;
                zu_trace_option(ctx, "-i patterns");
                ++i;
                continue;
            }
            if (first == 'i' && rest[0] != '\0') {
                int rc = parse_pattern_list_with_first(ctx, rest, argc, argv, &i, &endopts, true);
                if (rc != ZU_STATUS_OK)
                    return rc;
                zu_trace_option(ctx, "-i patterns");
                ++i;
                continue;
            }
            if (strcmp(tok, "-n") == 0) {
                int rc = parse_suffix_list(ctx, NULL, argc, argv, &i, &endopts);
                if (rc != ZU_STATUS_OK)
                    return rc;
                zu_trace_option(ctx, "-n suffix list");
                ++i;
                continue;
            }
            if (first == 'n' && rest[0] != '\0') {
                int rc = parse_suffix_list(ctx, rest, argc, argv, &i, &endopts);
                if (rc != ZU_STATUS_OK)
                    return rc;
                zu_trace_option(ctx, "-n suffix list");
                ++i;
                continue;
            }
            if (strcmp(tok, "-@") == 0) {
                int rc = read_stdin_names(ctx);
                if (rc != ZU_STATUS_OK) {
                    return rc;
                }
                zu_trace_option(ctx, "-@ names from stdin");
                ++i;
                continue;
            }
            if (strcmp(tok, "-F") == 0) {
                ctx->fix_archive = true;
                zu_trace_option(ctx, "-F fix");
                ++i;
                continue;
            }
            if (strcmp(tok, "-z") == 0) {
                ctx->zip_comment_specified = true;
                zu_trace_option(ctx, "-z zipfile comment from stdin");
                ++i;
                continue;
            }
            if (strcmp(tok, "-c") == 0) {
                fprintf(stderr, "zip: -c (add one-line comments) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }
            if (strcmp(tok, "-o") == 0) {
                ctx->set_archive_mtime = true;
                zu_trace_option(ctx, "-o set archive mtime");
                ++i;
                continue;
            }
            if (strcmp(tok, "-A") == 0) {
                fprintf(stderr, "zip: -A (adjust self-extracting exe) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }
            if (strcmp(tok, "-J") == 0) {
                fprintf(stderr, "zip: -J (junk zipfile prefix) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }

            bool handled_cluster = true;
            for (const char* p = tok + 1; *p; ++p) {
                if (!is_cluster_flag(*p)) {
                    handled_cluster = false;
                    break;
                }
                int rc = apply_cluster_flag(*p, ctx);
                if (rc != ZU_STATUS_OK)
                    return rc;
            }
            if (!handled_cluster) {
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
            }
            ++i;
            continue;
        }

        if (!ctx->archive_path) {
            ctx->archive_path = tok;
            if (strcmp(ctx->archive_path, "-") == 0) {
                ctx->output_to_stdout = true;
            }
            zu_trace_option(ctx, "archive path set to %s", ctx->archive_path);
            ++i;
            continue;
        }

        if (zu_strlist_push(&ctx->include, tok) != 0)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "include path %s", tok);
        ++i;
    }

    if (!ctx->archive_path) {
        bool interactive = isatty(STDIN_FILENO) != 0;
        if (ctx->include.len == 0) {
            if (interactive) {
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
            }
            ctx->archive_path = "-";
            ctx->output_to_stdout = true;
            if (zu_strlist_push(&ctx->include, "-") != 0)
                return ZU_STATUS_OOM;
        }
        else {
            print_usage(stderr, argv[0]);
            return ZU_STATUS_USAGE;
        }
    }

    return ZU_STATUS_OK;
}

int main(int argc, char** argv) {
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "zip: failed to allocate context\n");
        return ZU_STATUS_OOM;
    }

    // Default behavior for 'zip' is to modify/update existing archives
    ctx->modify_archive = true;

    bool invoked_as_zipcloak = is_alias(argv[0], "zipcloak");
    if (invoked_as_zipcloak) {
        ctx->encrypt = true;
        ctx->modify_archive = true;
        // zipcloak implies no recursion/store flags, usually just modifying
    }
    else if (is_alias(argv[0], "zipsplit")) {
        fprintf(stderr, "%s: split archives are not supported in this build\n", argv[0]);
        zu_context_free(ctx);
        return map_exit_code(ZU_STATUS_NOT_IMPLEMENTED);
    }
    bool is_zipnote = is_alias(argv[0], "zipnote");
    if (is_zipnote) {
        ctx->zipnote_mode = true;
    }
    const char* tool_name = invoked_as_zipcloak ? "zipcloak" : (is_zipnote ? "zipnote" : "zip");

    int parse_rc = parse_zip_args(argc, argv, ctx, is_zipnote);
    if (parse_rc == ZU_STATUS_USAGE) {
        zu_context_free(ctx);
        return map_exit_code(ZU_STATUS_USAGE);
    }
    if (parse_rc != ZU_STATUS_OK) {
        fprintf(stderr, "zip: argument parsing failed (%s)\n", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return map_exit_code(parse_rc);
    }

    if (ctx->dry_run) {
        ctx->quiet = false;
        ctx->verbose = true;
    }

    emit_zip_stub_warnings(ctx, invoked_as_zipcloak);
    trace_effective_zip_defaults(ctx);
    emit_option_trace(tool_name, ctx);

    if (is_zipnote && ctx->zip_comment_specified) {
        fprintf(stderr, "zipnote: -z is not supported with zipnote (use zip -z instead)\n");
        zu_context_free(ctx);
        return map_exit_code(ZU_STATUS_USAGE);
    }

    if (ctx->zip_comment_specified) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            if (strcmp(ctx->include.items[i], "-") == 0) {
                fprintf(stderr, "zip: -z cannot be used when reading file data from stdin\n");
                zu_context_free(ctx);
                return ZU_STATUS_USAGE;
            }
        }
        int zrc = read_zip_comment(ctx);
        if (zrc != ZU_STATUS_OK) {
            if (zrc == ZU_STATUS_USAGE) {
                fprintf(stderr, "zip: archive comment too large (max 65535 bytes)\n");
            }
            else if (zrc == ZU_STATUS_IO) {
                fprintf(stderr, "zip: failed to read archive comment from stdin\n");
            }
            zu_context_free(ctx);
            return map_exit_code(zrc);
        }
    }

    if (ctx->log_path) {
        ctx->log_file = fopen(ctx->log_path, ctx->log_append ? "ab" : "wb");
        if (!ctx->log_file) {
            fprintf(stderr, "zip: could not open log file '%s'\n", ctx->log_path);
            zu_context_free(ctx);
            return ZU_STATUS_IO;
        }
    }

    if (ctx->encrypt && !ctx->password) {
        char* pass = getpass("Enter password: ");
        if (!pass) {
            fprintf(stderr, "zip: password required\n");
            zu_context_free(ctx);
            return 1;
        }
        char* verify = getpass("Verify password: ");
        if (!verify || strcmp(pass, verify) != 0) {
            fprintf(stderr, "zip: password verification failed\n");
            zu_context_free(ctx);
            return 1;
        }
        ctx->password = strdup(pass);
        if (!ctx->password) {
            zu_context_free(ctx);
            return ZU_STATUS_OOM;
        }
    }

    int exec_rc;
    if (is_zipnote) {
        exec_rc = ctx->zipnote_write ? zipnote_apply(ctx) : zipnote_list(ctx);
    }
    else {
        exec_rc = zu_zip_run(ctx);
    }

    if (exec_rc != ZU_STATUS_OK && ctx->error_msg[0] != '\0') {
        fprintf(stderr, "zip: %s\n", ctx->error_msg);
    }

    zu_context_free(ctx);
    return map_exit_code(exec_rc);
}
