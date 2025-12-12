#define _GNU_SOURCE

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctx.h"
#include "ops.h"
#include "ziputils.h"

enum { OPT_DRY_RUN = 1000 };

static bool argv0_is_zipinfo(const char* argv0) {
    const char* base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    return strcmp(base, "zipinfo") == 0 || strcmp(base, "ii") == 0;
}

static void print_usage(FILE* to, const char* argv0) {
    fprintf(to,
            "Usage: %s [options] archive.zip [patterns...]\n"
            "       %s -Z [zipinfo-options] archive.zip [patterns...]\n"
            "\n"
            "Modern rewrite: parses legacy-compatible options into a reentrant context.\n"
            "\n"
            "Common options:\n"
            "  -l                    List contents only\n"
            "  -t                    Test archive integrity (zipinfo: show totals footer)\n"
            "  -d DIR                Extract into DIR\n"
            "  -o / -n               Overwrite / never overwrite\n"
            "  -q / -qq / -v         Quiet / really quiet / verbose output\n"
            "  -x pattern            Exclude pattern (can repeat)\n"
            "  -i pattern            Include only matching pattern\n"
            "  -C                    Case-insensitive pattern matching\n"
            "  --dry-run             Show what would be listed/extracted without writing\n"
            "  --help, -?            Show this help\n"
            "\n"
            "Zipinfo options (-Z or run as \"zipinfo\"):\n"
            "  -1 / -2               Filenames only (quiet / allow header+totals)\n"
            "  -s / -m / -l / -v     Short (default) / medium / long listing / verbose\n"
            "  -h / -t               Force header / totals footer (alone suppress entries)\n"
            "  -T                    Decimal timestamps (yymmdd.hhmmss)\n"
            "  -M                    Enable pager prompt (noop placeholder)\n"
            "  -z                    Include archive comment (ignored)\n",
            argv0, argv0);
}

static int map_exit_code(int status) {
    switch (status) {
        case ZU_STATUS_OK:
            return 0;
        case ZU_STATUS_USAGE:
            return 10;
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

static void warn_unzip_stub(ZContext* ctx, const char* tool, const char* opt, const char* detail) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s is stubbed (Info-ZIP 3.0 parity pending): %s", tool, opt, detail ? detail : "behavior incomplete");
    zu_warn_once(ctx, buf);
}

static void emit_unzip_stub_warnings(ZContext* ctx, const char* tool_name) {
    if (ctx->zipinfo_stub_used || (ctx->zipinfo_mode && (ctx->zi_format_specified || ctx->zi_header_explicit || ctx->zi_footer_explicit))) {
        warn_unzip_stub(ctx, tool_name, "zipinfo formatting flags", "output layout and timestamps may differ from Info-ZIP 3.0/6.0");
    }
    if (ctx->used_long_option) {
        warn_unzip_stub(ctx, tool_name, "long options", "alias/negation coverage is incomplete; only implemented flags are wired");
    }
}

static void trace_effective_unzip_defaults(ZContext* ctx) {
    const char* mode = ctx->list_only ? "list" : (ctx->test_integrity ? "test" : "extract");
    zu_trace_option(ctx, "mode: %s%s", mode, ctx->dry_run ? " +dry-run" : "");
    const char* target = ctx->target_dir ? ctx->target_dir : "(cwd)";
    zu_trace_option(ctx, "target dir: %s", target);
    zu_trace_option(ctx, "overwrite: %s", ctx->overwrite ? "always" : "never");
    zu_trace_option(ctx, "pattern match: include=%zu exclude=%zu case %s", ctx->include.len, ctx->exclude.len, ctx->match_case ? "sensitive" : "insensitive");
    zu_trace_option(ctx, "zipinfo mode: %s (format %d)", ctx->zipinfo_mode ? "on" : "off", ctx->zi_format);
}

static void emit_option_trace(const char* tool_name, ZContext* ctx) {
    if (!(ctx->verbose || ctx->log_info || ctx->dry_run)) {
        return;
    }
    if (ctx->option_events.len == 0) {
        return;
    }
    zu_log(ctx, "%s option resolution:\n", tool_name);
    for (size_t i = 0; i < ctx->option_events.len; ++i) {
        zu_log(ctx, "  %s\n", ctx->option_events.items[i]);
    }
}

static int parse_unzip_args(int argc, char** argv, ZContext* ctx) {
    if (argv0_is_zipinfo(argv[0])) {
        ctx->zipinfo_mode = true;
        ctx->list_only = true;
        zu_trace_option(ctx, "zipinfo mode via argv0");
    }

    static const struct option long_opts[] = {
        {"help", no_argument, NULL, '?'},
        {"list", no_argument, NULL, 'l'},
        {"pipe", no_argument, NULL, 'p'},
        {"test", no_argument, NULL, 't'},
        {"password", required_argument, NULL, 'P'},
        {"dry-run", no_argument, NULL, OPT_DRY_RUN},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "lptd:ovnqvi:x:hCZ12smMvTz?P:", long_opts, NULL)) != -1) {
        if (optind > 0 && optind <= argc && strncmp(argv[optind - 1], "--", 2) == 0) {
            ctx->used_long_option = true;
        }
        switch (opt) {
            case 'l':
                if (ctx->zipinfo_mode) {
                    ctx->zi_format = ZU_ZI_FMT_LONG;
                    ctx->zi_format_specified = true;
                    ctx->list_only = true;
                }
                else {
                    ctx->list_only = true;
                }
                zu_trace_option(ctx, "-l list");
                break;
            case 'p':
                ctx->output_to_stdout = true;
                zu_trace_option(ctx, "-p output to stdout");
                break;
            case 'P':
                free(ctx->password);
                ctx->password = strdup(optarg);
                if (!ctx->password)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-P (password provided)");
                break;
            case 't':
                if (ctx->zipinfo_mode) {
                    ctx->zi_footer = true;
                    ctx->zi_footer_explicit = true;
                    ctx->list_only = true;
                }
                else {
                    ctx->test_integrity = true;
                }
                zu_trace_option(ctx, "-t test");
                break;
            case 'd':
                ctx->target_dir = optarg;
                zu_trace_option(ctx, "-d %s", optarg);
                break;
            case 'o':
                ctx->overwrite = true;
                zu_trace_option(ctx, "-o overwrite existing files");
                break;
            case 'n':
                ctx->overwrite = false;
                zu_trace_option(ctx, "-n never overwrite");
                break;
            case 'q':
                ctx->quiet_level++;
                ctx->quiet = ctx->quiet_level > 0;
                ctx->verbose = false;
                zu_trace_option(ctx, "-q quiet level %d", ctx->quiet_level);
                break;
            case 'v':
                ctx->verbose = true;
                ctx->list_only = true;
                /* unzip -v is effectively zipinfo -v or long format */
                ctx->zipinfo_mode = true;
                ctx->zi_format = ZU_ZI_FMT_VERBOSE;
                ctx->zi_format_specified = true;
                ctx->zi_show_comments = true;
                zu_trace_option(ctx, "-v verbose listing");
                break;
            case 'C':
                ctx->match_case = false;
                zu_trace_option(ctx, "-C case-insensitive patterns");
                break;
            case 'i':
                if (zu_strlist_push(&ctx->include, optarg) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-i pattern %s", optarg);
                break;
            case 'x':
                if (zu_strlist_push(&ctx->exclude, optarg) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-x pattern %s", optarg);
                break;
            case 'Z':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                zu_trace_option(ctx, "-Z zipinfo mode");
                break;
            case '1':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;
                ctx->zi_header = false;
                ctx->zi_footer = false;
                ctx->zi_header_explicit = true;
                ctx->zi_footer_explicit = true;
                ctx->zi_list_entries = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-1 names only (quiet)");
                break;
            case '2':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;
                ctx->zi_list_entries = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-2 names only");
                break;
            case 's':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_SHORT;
                ctx->zi_format_specified = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-s short listing");
                break;
            case 'm':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_MEDIUM;
                ctx->zi_format_specified = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-m medium listing");
                break;
            case 'h':
                if (ctx->zipinfo_mode) {
                    ctx->zi_header = true;
                    ctx->zi_header_explicit = true;
                    ctx->list_only = true;
                    ctx->zipinfo_stub_used = true;
                    zu_trace_option(ctx, "-h show header");
                }
                else {
                    print_usage(stdout, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                break;
            case 'M':
                ctx->zipinfo_mode = true;
                ctx->zi_allow_pager = true;
                ctx->list_only = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-M pager prompt (noop)");
                break;
            case 'T':
                ctx->zipinfo_mode = true;
                ctx->zi_decimal_time = true;
                ctx->list_only = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-T decimal time");
                break;
            case 'z':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_show_comments = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-z include comments");
                break;
            case OPT_DRY_RUN:
                ctx->dry_run = true;
                ctx->verbose = true;
                ctx->quiet = false;
                ctx->list_only = ctx->list_only || ctx->zipinfo_mode;
                zu_trace_option(ctx, "--dry-run enable plan-only mode");
                break;
            case '?':
                print_usage(stdout, argv[0]);
                return ZU_STATUS_USAGE;
            default:
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
        }
    }

    if (optind >= argc) {
        print_usage(stderr, argv[0]);
        return ZU_STATUS_USAGE;
    }

    ctx->archive_path = argv[optind++];
    zu_trace_option(ctx, "archive path set to %s", ctx->archive_path);
    for (int i = optind; i < argc; ++i) {
        if (zu_strlist_push(&ctx->include, argv[i]) != 0)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "include pattern %s", argv[i]);
    }

    if (ctx->zipinfo_mode) {
        if (ctx->include.len > 0) {
            if (!ctx->zi_header_explicit)
                ctx->zi_header = false;
            if (!ctx->zi_footer_explicit)
                ctx->zi_footer = false;
        }
        if (!ctx->zi_format_specified && (ctx->zi_header_explicit || ctx->zi_footer_explicit)) {
            ctx->zi_list_entries = false;
        }
        if (!ctx->zi_format_specified && ctx->zi_footer_explicit && !ctx->zi_header_explicit) {
            ctx->zi_header = false;
        }
        if (!ctx->zi_list_entries && !ctx->zi_footer_explicit) {
            ctx->zi_footer = false;
        }
        ctx->list_only = true;
    }

    return ZU_STATUS_OK;
}

int main(int argc, char** argv) {
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "unzip: failed to allocate context\n");
        return ZU_STATUS_OOM;
    }

    int parse_rc = parse_unzip_args(argc, argv, ctx);
    if (parse_rc == ZU_STATUS_USAGE) {
        zu_context_free(ctx);
        return 0;
    }
    if (parse_rc != ZU_STATUS_OK) {
        fprintf(stderr, "unzip: argument parsing failed (%s)\n", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return map_exit_code(parse_rc);
    }

    if (ctx->dry_run && !ctx->list_only && !ctx->test_integrity) {
        ctx->list_only = true;
    }
    if (ctx->dry_run) {
        ctx->quiet = false;
        ctx->verbose = true;
    }

    const char* tool_name = ctx->zipinfo_mode ? "zipinfo" : "unzip";
    emit_unzip_stub_warnings(ctx, tool_name);
    trace_effective_unzip_defaults(ctx);
    emit_option_trace(tool_name, ctx);

    int exec_rc = zu_unzip_run(ctx);
    if (exec_rc != ZU_STATUS_OK && ctx->error_msg[0] != '\0') {
        fprintf(stderr, "unzip: %s\n", ctx->error_msg);
    }
    zu_context_free(ctx);
    return map_exit_code(exec_rc);
}
