#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "cli_common.h"
#include "ctx.h"
#include "ops.h"
#include "ziputils.h"
#include "reader.h"

/* Tracks which alias (zip/zipnote) was used so logs match the tool name. */
static const char* g_tool_name = "zip";

/* --- Helpers --- */

static time_t parse_date(const char* str) {
    struct tm tm = {0};
    char* end;
    tm.tm_isdst = -1;

    // Try ISO 8601: yyyy-mm-dd
    end = strptime(str, "%Y-%m-%d", &tm);
    if (end && *end == '\0') {
        if (tm.tm_mon >= 0 && tm.tm_mon <= 11 && tm.tm_mday >= 1 && tm.tm_mday <= 31) {
            return mktime(&tm);
        }
    }

    // Try mmddyyyy
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;
    end = strptime(str, "%m%d%Y", &tm);
    if (end && *end == '\0') {
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

static void emit_zip_stub_warnings(ZContext* ctx) {
    if (ctx->fix_archive || ctx->fix_fix_archive) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s is stubbed (Info-ZIP 3.0 parity pending): %s", ctx->fix_fix_archive ? "-FF" : "-F", "recovery semantics are still under validation");
        zu_warn_once(ctx, buf);
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

/* --- Zipnote Helpers --- */

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
    if (rc != ZU_STATUS_OK)
        return rc;

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

/* zipnote streams come as alternating "@ name" markers and comment bodies.
 * We keep the current entry name and append every following line (with '@@' escaped)
 * until the next marker, materializing a list of edits for apply/write. */
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
                if (!e)
                    goto oom_error;

                e->name = cur_name;
                e->comment = comment_buf;
                e->comment_len = comment_len;
                e->is_archive = (strcmp(cur_name, zipnote_archive_label) == 0);

                if (zipnote_add_edit(&edits, &edits_len, &edits_cap, e) != ZU_STATUS_OK) {
                    zipnote_edit_free(e);
                    goto oom_error;
                }

                // Ownership transferred to edit struct
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

        if (!cur_name)
            continue;

        if (comment_len + data_len + 1 > comment_cap) {
            size_t new_cap = comment_cap == 0 ? 256 : comment_cap * 2;
            while (new_cap < comment_len + data_len + 1)
                new_cap *= 2;
            char* nb = realloc(comment_buf, new_cap);
            if (!nb)
                goto oom_error;
            comment_buf = nb;
            comment_cap = new_cap;
        }
        memcpy(comment_buf + comment_len, data, data_len);
        comment_len += data_len;
        comment_buf[comment_len++] = '\n';
    }

    if (cur_name) {
        zipnote_edit* e = calloc(1, sizeof(zipnote_edit));
        if (!e)
            goto oom_error;
        e->name = cur_name;
        e->comment = comment_buf;
        e->comment_len = comment_len;
        e->is_archive = (strcmp(cur_name, zipnote_archive_label) == 0);
        if (zipnote_add_edit(&edits, &edits_len, &edits_cap, e) != ZU_STATUS_OK) {
            zipnote_edit_free(e);
            goto oom_error;
        }
        cur_name = NULL;  // Prevent double free
        comment_buf = NULL;
    }

    free(line);
    *edits_out = edits;
    *edits_len_out = edits_len;
    return ZU_STATUS_OK;

oom_error:
    free(line);
    free(comment_buf);
    free(cur_name);
    for (size_t i = 0; i < edits_len; ++i)
        zipnote_edit_free(edits[i]);
    free(edits);
    return ZU_STATUS_OOM;
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
    if (rc != ZU_STATUS_OK)
        return rc;

    zipnote_edit** edits = NULL;
    size_t edits_len = 0;
    rc = zipnote_parse(ctx, &edits, &edits_len);
    if (rc != ZU_STATUS_OK)
        return rc;

    bool seen_archive = false;
    for (size_t i = 0; i < edits_len; ++i) {
        zipnote_edit* e = edits[i];
        if (e->is_archive) {
            free(ctx->zip_comment);
            ctx->zip_comment = e->comment;
            ctx->zip_comment_len = e->comment_len;
            ctx->zip_comment_specified = true;
            e->comment = NULL;  // Steal ownership
            seen_archive = true;
            continue;
        }

        zu_existing_entry* existing = zipnote_find_entry(ctx, e->name);
        if (!existing) {
            zu_cli_warn(g_tool_name, "zipnote: entry not found: %s", e->name);
            continue;
        }

        free(existing->comment);
        existing->comment = e->comment;
        existing->comment_len = (uint16_t)e->comment_len;
        existing->changed = true;
        e->comment = NULL;  // Steal ownership
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
    if (!ctx)
        return ZU_STATUS_USAGE;

    size_t cap = 1024;
    size_t len = 0;
    char* buf = malloc(cap);
    if (!buf)
        return ZU_STATUS_OOM;

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

    // Null terminate just in case, though len tracks size
    if (len == cap) {
        char* nb = realloc(buf, cap + 1);
        if (!nb) {
            free(buf);
            return ZU_STATUS_OOM;
        }
        buf = nb;
    }
    buf[len] = '\0';
    free(ctx->zip_comment);
    ctx->zip_comment = buf;
    ctx->zip_comment_len = len;
    return ZU_STATUS_OK;
}

static void print_usage(FILE* to, const char* argv0) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "%sUsage:%s %s%s [options] archive.zip [file ...]%s\n", c->bold, c->reset, c->green, argv0, c->reset);

    fprintf(to, "\nInfo-ZIP compliant compression utility (zip-utils).\n");

    zu_cli_print_section(to, "Basic Modes");
    zu_cli_print_opt(to, "(default)", "Create or modify archive");
    zu_cli_print_opt(to, "-f", "Freshen: replace existing entries only");
    zu_cli_print_opt(to, "-u", "Update: replace newer or add new entries");
    zu_cli_print_opt(to, "-d", "Delete patterns from archive");
    zu_cli_print_opt(to, "-m", "Move: delete source files after archiving");
    zu_cli_print_opt(to, "-FS", "Filesync: sync archive with filesystem content");

    zu_cli_print_section(to, "Selection & Filtering");
    zu_cli_print_opt(to, "-r", "Recurse into directories");
    zu_cli_print_opt(to, "-R", "Recurse from current dir (PKZIP style)");
    zu_cli_print_opt(to, "-j", "Junk paths (store basenames only)");
    zu_cli_print_opt(to, "-x <pats>", "Exclude patterns");
    zu_cli_print_opt(to, "-i <pats>", "Include patterns");
    zu_cli_print_opt(to, "-@", "Read file names from stdin");
    zu_cli_print_opt(to, "-t <date>", "Include files modified after mmddyyyy");
    zu_cli_print_opt(to, "-tt <date>", "Include files modified before mmddyyyy");

    zu_cli_print_section(to, "Compression & Storage");
    zu_cli_print_opt(to, "-0 ... -9", "Compression level (0=store, 9=best)");
    zu_cli_print_opt(to, "-Z <meth>", "Method: deflate, store, bzip2");
    zu_cli_print_opt(to, "-n <suf>", "Don't compress these suffixes");
    zu_cli_print_opt(to, "-y", "Store symlinks as links (not targets)");
    zu_cli_print_opt(to, "-X", "Strip extra file attributes (UID/GID)");
    zu_cli_print_opt(to, "-D", "Do not create directory entries");

    zu_cli_print_section(to, "Input / Output");
    zu_cli_print_opt(to, "-O <path>", "Write output to different file");
    zu_cli_print_opt(to, "-b <dir>", "Temporary directory");
    zu_cli_print_opt(to, "-o", "Set archive mtime to newest entry");
    zu_cli_print_opt(to, "-", "Use stdout for output or stdin for input");

    zu_cli_print_section(to, "Text Processing");
    zu_cli_print_opt(to, "-l", "Translate LF to CRLF");
    zu_cli_print_opt(to, "-ll", "Translate CRLF to LF");
    zu_cli_print_opt(to, "-z", "Read archive comment from stdin");

    zu_cli_print_section(to, "Diagnostics");
    zu_cli_print_opt(to, "-q", "Quiet mode (stackable: -qq)");
    zu_cli_print_opt(to, "-v", "Verbose / Print version info");
    zu_cli_print_opt(to, "-T", "Test archive integrity after write");
    zu_cli_print_opt(to, "--dry-run", "Show what would be done");
    zu_cli_print_opt(to, "-lf <path>", "Log file path");

    fprintf(to, "\n");
}

/* --- Argument Parsing --- */

static bool is_cluster_flag(char c) {
    return (strchr("rjTqvmdfuDXyel", c) != NULL) || (isdigit(c));
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
            ctx->quiet = true;
            ctx->verbose = false;
            zu_trace_option(ctx, "-q quiet level %d", ctx->quiet_level);
            return ZU_STATUS_OK;
        case 'v':
            ctx->verbose = true;
            zu_trace_option(ctx, "-v verbose");
            return ZU_STATUS_OK;
        case 'm':
            ctx->remove_source = true;
            zu_trace_option(ctx, "-m move");
            return ZU_STATUS_OK;
        case 'd':
            ctx->difference_mode = true;
            zu_trace_option(ctx, "-d delete");
            return ZU_STATUS_OK;
        case 'f':
            ctx->freshen = true;
            zu_trace_option(ctx, "-f freshen");
            return ZU_STATUS_OK;
        case 'u':
            ctx->update = true;
            zu_trace_option(ctx, "-u update");
            return ZU_STATUS_OK;
        case 'D':
            ctx->no_dir_entries = true;
            zu_trace_option(ctx, "-D no dir entries");
            return ZU_STATUS_OK;
        case 'X':
            ctx->exclude_extra_attrs = true;
            zu_trace_option(ctx, "-X drop extra attrs");
            return ZU_STATUS_OK;
        case 'y':
            ctx->store_symlinks = true;
            ctx->allow_symlinks = true;
            zu_trace_option(ctx, "-y store symlinks");
            return ZU_STATUS_OK;
        case 'e':
            zu_cli_error(g_tool_name, "encryption is not supported in this build");
            return ZU_STATUS_NOT_IMPLEMENTED;
        case 'l':
            ctx->line_mode = ZU_LINE_LF_TO_CRLF;
            zu_trace_option(ctx, "-l LF->CRLF");
            return ZU_STATUS_OK;
        default:
            break;
    }

    if (isdigit(c)) {
        ctx->compression_level = c - '0';
        zu_trace_option(ctx, "compression level %d", ctx->compression_level);
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
        if (!*endopts && tok[0] == '-' && tok[1] != '\0')
            break;

        if (zu_strlist_push(include ? &ctx->include_patterns : &ctx->exclude, tok) != 0)
            return ZU_STATUS_OOM;
        any = true;
    }

    if (!any) {
        zu_cli_error(g_tool_name, "Option requires one or more patterns");
        return ZU_STATUS_USAGE;
    }

    *idx = i - 1;
    return ZU_STATUS_OK;
}

static int parse_pattern_list_with_first(ZContext* ctx, const char* first, int argc, char** argv, int* idx, bool* endopts, bool include) {
    if (first) {
        if (zu_strlist_push(include ? &ctx->include_patterns : &ctx->exclude, first) != 0)
            return ZU_STATUS_OOM;
        return ZU_STATUS_OK;
    }
    return parse_pattern_list(ctx, argc, argv, idx, endopts, include);
}

static int push_suffixes(ZContext* ctx, const char* str) {
    if (!str || !*str)
        return ZU_STATUS_OK;
    char* copy = strdup(str);
    if (!copy)
        return ZU_STATUS_OOM;

    char* saveptr;
    char* token = strtok_r(copy, ":", &saveptr);
    while (token) {
        if (zu_strlist_push(&ctx->no_compress_suffixes, token) != 0) {
            free(copy);
            return ZU_STATUS_OOM;
        }
        token = strtok_r(NULL, ":", &saveptr);
    }
    free(copy);
    return ZU_STATUS_OK;
}

static int parse_suffix_list(ZContext* ctx, const char* first, int argc, char** argv, int* idx, bool* endopts) {
    (void)endopts;
    if (first)
        return push_suffixes(ctx, first);

    if (*idx + 1 >= argc) {
        zu_cli_error(g_tool_name, "-n requires suffix list");
        return ZU_STATUS_USAGE;
    }
    return push_suffixes(ctx, argv[++(*idx)]);
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

// Helper macro to check arg requirements
#define REQUIRE_ARG(optname)                                                   \
    do {                                                                       \
        if (!value) {                                                          \
            if (*idx + 1 >= argc) {                                            \
                zu_cli_error(g_tool_name, "%s requires an argument", optname); \
                return ZU_STATUS_USAGE;                                        \
            }                                                                  \
            value = argv[++(*idx)];                                            \
        }                                                                      \
    } while (0)

    if (strcmp(name, "dry-run") == 0) {
        ctx->dry_run = true;
        ctx->verbose = true;
        ctx->quiet = false;
        zu_trace_option(ctx, "--dry-run");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "recurse-paths") == 0) {
        ctx->recursive = true;
        zu_trace_option(ctx, "--recurse-paths");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "test") == 0) {
        ctx->test_integrity = true;
        zu_trace_option(ctx, "--test");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "test-command") == 0) {
        REQUIRE_ARG("--test-command");
        free(ctx->test_command);
        ctx->test_command = strdup(value);
        if (!ctx->test_command)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "--test-command");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "quiet") == 0) {
        ctx->quiet_level++;
        ctx->quiet = true;
        ctx->verbose = false;
        zu_trace_option(ctx, "--quiet");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "verbose") == 0) {
        ctx->verbose = true;
        zu_trace_option(ctx, "--verbose");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "encrypt") == 0 || strcmp(name, "password") == 0) {
        zu_cli_error(g_tool_name, "encryption is not supported in this build");
        return ZU_STATUS_NOT_IMPLEMENTED;
    }
    if (strcmp(name, "help") == 0) {
        print_usage(stdout, argv[0]);
        return ZU_STATUS_USAGE;
    }
    if (strcmp(name, "output-file") == 0 || strcmp(name, "out") == 0) {
        REQUIRE_ARG("--out");
        ctx->output_path = value;
        zu_trace_option(ctx, "--out=%s", value);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "copy") == 0) {
        ctx->copy_mode = true;
        zu_trace_option(ctx, "--copy");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "la") == 0 || strcmp(name, "log-append") == 0) {
        ctx->log_append = true;
        zu_trace_option(ctx, "--log-append");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "lf") == 0 || strcmp(name, "logfile-path") == 0) {
        REQUIRE_ARG("--logfile-path");
        free(ctx->log_path);
        ctx->log_path = strdup(value);
        return ctx->log_path ? ZU_STATUS_OK : ZU_STATUS_OOM;
    }
    if (strcmp(name, "li") == 0 || strcmp(name, "log-info") == 0) {
        ctx->log_info = true;
        zu_trace_option(ctx, "--log-info");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "filesync") == 0 || strcmp(name, "FS") == 0) {
        ctx->filesync = true;
        ctx->update = true;
        zu_trace_option(ctx, "--filesync");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "split-size") == 0 || strcmp(name, "pause") == 0 || strcmp(name, "sp") == 0) {
        zu_cli_error(g_tool_name, "split archives are not supported");
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
        zu_trace_option(ctx, "--ll");
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

            // Rejects / Stubs
            if (strcmp(tok, "-xi") == 0 || strcmp(tok, "-ix") == 0) {
                zu_cli_error(g_tool_name, "use -x <patterns> ... -i <patterns> instead of %s", tok);
                return ZU_STATUS_USAGE;
            }
            if (strcmp(tok, "-sp") == 0) {
                zu_cli_error(g_tool_name, "split archives are not supported");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }
            if (strcmp(tok, "-c") == 0 || strcmp(tok, "-A") == 0 || strcmp(tok, "-J") == 0) {
                zu_cli_error(g_tool_name, "option %s not supported in this version", tok);
                return ZU_STATUS_NOT_IMPLEMENTED;
            }

            // Standalone flags
            if (strcmp(tok, "-R") == 0) {
                ctx->recursive = true;
                ctx->recurse_from_cwd = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-U") == 0) {
                ctx->copy_mode = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-FF") == 0) {
                ctx->fix_fix_archive = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-ll") == 0) {
                ctx->line_mode = ZU_LINE_CRLF_TO_LF;
                ++i;
                continue;
            }
            if (strcmp(tok, "-w") == 0) {
                if (!is_zipnote) {
                    print_usage(stderr, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                ctx->zipnote_write = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-la") == 0) {
                ctx->log_append = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-li") == 0) {
                ctx->log_info = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-FS") == 0) {
                ctx->filesync = true;
                ctx->update = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-F") == 0) {
                ctx->fix_archive = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-z") == 0) {
                ctx->zip_comment_specified = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-o") == 0) {
                ctx->set_archive_mtime = true;
                ++i;
                continue;
            }

            // Flags with Arguments
            if (strcmp(tok, "-TT") == 0) {
                if (i + 1 >= argc) {
                    zu_cli_error(g_tool_name, "-TT requires a command");
                    return ZU_STATUS_USAGE;
                }
                free(ctx->test_command);
                ctx->test_command = strdup(argv[++i]);
                if (!ctx->test_command)
                    return ZU_STATUS_OOM;
                ++i;
                continue;
            }
            if (strcmp(tok, "-lf") == 0) {
                if (i + 1 >= argc) {
                    zu_cli_error(g_tool_name, "-lf requires a path");
                    return ZU_STATUS_USAGE;
                }
                free(ctx->log_path);
                ctx->log_path = strdup(argv[++i]);
                if (!ctx->log_path)
                    return ZU_STATUS_OOM;
                ++i;
                continue;
            }
            if (strncmp(tok, "-tt", 3) == 0) {
                const char* arg = tok[3] ? tok + 3 : NULL;
                if (!arg) {
                    if (i + 1 >= argc) {
                        zu_cli_error(g_tool_name, "-tt requires a date");
                        return ZU_STATUS_USAGE;
                    }
                    arg = argv[++i];
                }
                ctx->filter_before = parse_date(arg);
                if (ctx->filter_before == (time_t)-1) {
                    zu_cli_error(g_tool_name, "invalid date: %s", arg);
                    return ZU_STATUS_USAGE;
                }
                ctx->has_filter_before = true;
                ++i;
                continue;
            }
            if (tok[1] == '-') {
                int rc = parse_long_option(tok, argc, argv, &i, ctx);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }

            // Clusterable with possible arg
            char first = tok[1];
            const char* rest = tok + 2;

            // Handle options that take arguments (b, t, P, O, Z)
            if (strchr("btPOZ", first)) {
                const char* arg = rest[0] ? rest : NULL;
                if (!arg) {
                    if (i + 1 >= argc) {
                        zu_cli_error(g_tool_name, "-%c requires argument", first);
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
                        break;
                    case 't':
                        ctx->filter_after = parse_date(arg);
                        if (ctx->filter_after == (time_t)-1) {
                            zu_cli_error(g_tool_name, "invalid date for -t");
                            return ZU_STATUS_USAGE;
                        }
                        ctx->has_filter_after = true;
                        break;
                    case 'P':
                        zu_cli_error(g_tool_name, "encryption is not supported in this build");
                        return ZU_STATUS_NOT_IMPLEMENTED;
                    case 'O':
                        ctx->output_path = arg;
                        break;
                    case 'Z':
                        if (strcasecmp(arg, "deflate") == 0)
                            ctx->compression_method = 8;
                        else if (strcasecmp(arg, "store") == 0)
                            ctx->compression_method = 0;
                        else if (strcasecmp(arg, "bzip2") == 0)
                            ctx->compression_method = 12;
                        else {
                            zu_cli_error(g_tool_name, "unknown compression method '%s'", arg);
                            return ZU_STATUS_USAGE;
                        }
                        break;
                }
                ++i;
                continue;
            }

            // Handle patterns / lists
            if (strcmp(tok, "-x") == 0) {
                if (parse_pattern_list(ctx, argc, argv, &i, &endopts, false) != ZU_STATUS_OK)
                    return ZU_STATUS_USAGE;
                ++i;
                continue;
            }
            if (first == 'x' && rest[0]) {
                if (parse_pattern_list_with_first(ctx, rest, argc, argv, &i, &endopts, false) != ZU_STATUS_OK)
                    return ZU_STATUS_USAGE;
                ++i;
                continue;
            }
            if (strcmp(tok, "-i") == 0) {
                if (parse_pattern_list(ctx, argc, argv, &i, &endopts, true) != ZU_STATUS_OK)
                    return ZU_STATUS_USAGE;
                ++i;
                continue;
            }
            if (first == 'i' && rest[0]) {
                if (parse_pattern_list_with_first(ctx, rest, argc, argv, &i, &endopts, true) != ZU_STATUS_OK)
                    return ZU_STATUS_USAGE;
                ++i;
                continue;
            }
            if (strcmp(tok, "-n") == 0) {
                if (parse_suffix_list(ctx, NULL, argc, argv, &i, &endopts) != ZU_STATUS_OK)
                    return ZU_STATUS_USAGE;
                ++i;
                continue;
            }
            if (first == 'n' && rest[0]) {
                if (parse_suffix_list(ctx, rest, argc, argv, &i, &endopts) != ZU_STATUS_OK)
                    return ZU_STATUS_USAGE;
                ++i;
                continue;
            }
            if (strcmp(tok, "-@") == 0) {
                if (read_stdin_names(ctx) != ZU_STATUS_OK)
                    return ZU_STATUS_IO;
                ++i;
                continue;
            }

            // Check non-clusterable flags being clustered
            if (strchr("@FzcoAJ", first) && rest[0]) {
                zu_cli_error(g_tool_name, "option '%c' cannot be clustered", first);
                return ZU_STATUS_USAGE;
            }

            // Cluster flags parsing
            bool handled = true;
            for (const char* p = tok + 1; *p; ++p) {
                if (!is_cluster_flag(*p)) {
                    handled = false;
                    break;
                }
                int rc = apply_cluster_flag(*p, ctx);
                if (rc != ZU_STATUS_OK)
                    return rc;
            }
            if (!handled) {
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
            }
            ++i;
            continue;
        }

        // Positional Arguments
        if (!ctx->archive_path) {
            ctx->archive_path = tok;
            if (strcmp(ctx->archive_path, "-") == 0)
                ctx->output_to_stdout = true;
        }
        else {
            if (zu_strlist_push(&ctx->include, tok) != 0)
                return ZU_STATUS_OOM;
        }
        ++i;
    }

    if (!ctx->archive_path) {
        if (ctx->include.len == 0 && isatty(STDIN_FILENO)) {
            print_usage(stderr, argv[0]);
            return ZU_STATUS_USAGE;
        }
        // Implicit stdin-to-stdout filter mode
        ctx->archive_path = "-";
        ctx->output_to_stdout = true;
        if (zu_strlist_push(&ctx->include, "-") != 0)
            return ZU_STATUS_OOM;
    }

    return ZU_STATUS_OK;
}

int main(int argc, char** argv) {
    zu_cli_init_terminal();
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        zu_cli_error(g_tool_name, "failed to allocate context");
        return ZU_STATUS_OOM;
    }

    ctx->modify_archive = true;

    bool invoked_as_zipcloak = zu_cli_name_matches(argv[0], "zipcloak");
    bool is_zipnote = zu_cli_name_matches(argv[0], "zipnote");

    if (invoked_as_zipcloak) {
        zu_cli_error(g_tool_name, "zipcloak/encryption is not supported in this build");
        zu_context_free(ctx);
        return map_exit_code(ZU_STATUS_NOT_IMPLEMENTED);
    }
    else if (is_zipnote) {
        g_tool_name = "zipnote";
    }

    if (is_zipnote)
        ctx->zipnote_mode = true;

    int parse_rc = parse_zip_args(argc, argv, ctx, is_zipnote);
    if (parse_rc != ZU_STATUS_OK) {
        if (parse_rc != ZU_STATUS_USAGE)
            zu_cli_error(g_tool_name, "argument parsing failed: %s", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return map_exit_code(parse_rc);
    }

    if (ctx->dry_run) {
        ctx->quiet = false;
        ctx->verbose = true;
    }

    emit_zip_stub_warnings(ctx);
    trace_effective_zip_defaults(ctx);
    zu_cli_emit_option_trace(g_tool_name, ctx);

    if (is_zipnote && ctx->zip_comment_specified) {
        zu_cli_error(g_tool_name, "zipnote: -z is not supported (use zip -z instead)");
        zu_context_free(ctx);
        return map_exit_code(ZU_STATUS_USAGE);
    }

    if (ctx->zip_comment_specified) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            if (strcmp(ctx->include.items[i], "-") == 0) {
                zu_cli_error(g_tool_name, "-z cannot be used when reading file data from stdin");
                zu_context_free(ctx);
                return ZU_STATUS_USAGE;
            }
        }
        int zrc = read_zip_comment(ctx);
        if (zrc != ZU_STATUS_OK) {
            zu_cli_error(g_tool_name, "failed to read archive comment: %s", zu_status_str(zrc));
            zu_context_free(ctx);
            return map_exit_code(zrc);
        }
    }

    if (ctx->log_path) {
        ctx->log_file = fopen(ctx->log_path, ctx->log_append ? "ab" : "wb");
        if (!ctx->log_file) {
            zu_cli_error(g_tool_name, "could not open log file '%s'", ctx->log_path);
            zu_context_free(ctx);
            return ZU_STATUS_IO;
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
        zu_cli_error(g_tool_name, "%s", ctx->error_msg);
    }

    zu_context_free(ctx);
    return map_exit_code(exec_rc);
}
