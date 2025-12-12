#define _GNU_SOURCE

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>

#include "cli_common.h"
#include "ctx.h"
#include "ops.h"
#include "ziputils.h"

enum { OPT_DRY_RUN = 1000 };

static const char* g_tool_name = "unzip";

static int map_exit_code(int status) {
    switch (status) {
        case ZU_STATUS_OK:
            return 0;
        case ZU_STATUS_USAGE:
            return 10;
        case ZU_STATUS_NO_FILES:
            return 11;
        case ZU_STATUS_IO:
            return 2;
        case ZU_STATUS_OOM:
            return 5;
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
        warn_unzip_stub(ctx, tool_name, "long options", "alias/negation coverage is incomplete");
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

static void print_version(FILE* to) {
    fprintf(to, "UnZip 6.00 (zip-utils rewrite; Info-ZIP compatibility work in progress)\n");
}

static void print_usage(FILE* to, const char* argv0) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "%sUsage:%s %s%s [options] archive.zip [patterns...]%s\n", c->bold, c->reset, c->green, argv0, c->reset);

    fprintf(to, "\nInfo-ZIP compliant extraction utility.\n");

    zu_cli_print_section(to, "Common Options");
    zu_cli_print_opt(to, "-l", "List contents only");
    zu_cli_print_opt(to, "-t", "Test archive integrity");
    zu_cli_print_opt(to, "-p", "Extract files to pipe (stdout)");
    zu_cli_print_opt(to, "-d <dir>", "Extract into specified directory");
    zu_cli_print_opt(to, "-o / -n", "Overwrite / Never overwrite existing files");
    zu_cli_print_opt(to, "-q / -qq", "Quiet mode (stackable)");
    zu_cli_print_opt(to, "-v", "Verbose output (or print version)");

    zu_cli_print_section(to, "Selection & Modifiers");
    zu_cli_print_opt(to, "-x <pat>", "Exclude files matching pattern");
    zu_cli_print_opt(to, "-i <pat>", "Include only files matching pattern");
    zu_cli_print_opt(to, "-C", "Case-insensitive pattern matching");
    zu_cli_print_opt(to, "-j", "Junk paths (flatten directories)");
    zu_cli_print_opt(to, "-L", "Convert filenames to lowercase (stub)");
    zu_cli_print_opt(to, "-X", "Restore UID/GID info (stub)");
    zu_cli_print_opt(to, "-P <pass>", "Provide password");

    zu_cli_print_section(to, "Zipinfo Mode (-Z)");
    zu_cli_print_opt(to, "-1", "List filenames only (one per line)");
    zu_cli_print_opt(to, "-2", "List filenames only (allow headers)");
    zu_cli_print_opt(to, "-s", "Short listing (default)");
    zu_cli_print_opt(to, "-m", "Medium listing");
    zu_cli_print_opt(to, "-h", "Force header line");
    zu_cli_print_opt(to, "-T", "Print decimal timestamps");

    zu_cli_print_section(to, "Diagnostics");
    zu_cli_print_opt(to, "--dry-run", "Show operations without writing");
    zu_cli_print_opt(to, "--help", "Show this help");

    fprintf(to, "\n");
}

/* --- Argument Parsing --- */

static int parse_unzip_args(int argc, char** argv, ZContext* ctx) {
    if (zu_cli_name_matches(argv[0], "zipinfo") || zu_cli_name_matches(argv[0], "ii")) {
        g_tool_name = "zipinfo";
        ctx->zipinfo_mode = true;
        ctx->list_only = true;
        zu_trace_option(ctx, "zipinfo mode enabled via argv0");
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
    while ((opt = getopt_long(argc, argv, "lptd:ovnqvjLi:Xx:hCZ12smMvTz?P:", long_opts, NULL)) != -1) {
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
                zu_trace_option(ctx, "-o overwrite always");
                break;
            case 'n':
                ctx->overwrite = false;
                zu_trace_option(ctx, "-n never overwrite");
                break;
            case 'j':
                ctx->store_paths = false;
                zu_trace_option(ctx, "-j junk paths");
                break;
            case 'q':
                ctx->quiet_level++;
                ctx->quiet = true;
                ctx->verbose = false;
                zu_trace_option(ctx, "-q quiet level %d", ctx->quiet_level);
                break;
            case 'L':
                zu_trace_option(ctx, "-L lowercase (stub)");
                break;
            case 'X':
                zu_trace_option(ctx, "-X restore attrs (stub)");
                break;
            case 'v':
                ctx->verbose = true;
                ctx->list_only = true;
                /* unzip -v is effectively zipinfo -v or long format */
                ctx->zipinfo_mode = true;
                g_tool_name = "zipinfo";
                ctx->zi_format = ZU_ZI_FMT_VERBOSE;
                ctx->zi_format_specified = true;
                ctx->zi_show_comments = true;
                zu_trace_option(ctx, "-v verbose");
                break;
            case 'C':
                ctx->match_case = false;
                zu_trace_option(ctx, "-C case-insensitive");
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

            /* Zipinfo Specifics */
            case 'Z':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                g_tool_name = "zipinfo";
                zu_trace_option(ctx, "-Z zipinfo mode");
                break;
            case '1':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                g_tool_name = "zipinfo";
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
                g_tool_name = "zipinfo";
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;
                ctx->zi_list_entries = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-2 names only");
                break;
            case 's':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                g_tool_name = "zipinfo";
                ctx->zi_format = ZU_ZI_FMT_SHORT;
                ctx->zi_format_specified = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-s short listing");
                break;
            case 'm':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                g_tool_name = "zipinfo";
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
                    g_tool_name = "zipinfo";
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
                g_tool_name = "zipinfo";
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-M pager (noop)");
                break;
            case 'T':
                ctx->zipinfo_mode = true;
                ctx->zi_decimal_time = true;
                ctx->list_only = true;
                g_tool_name = "zipinfo";
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-T decimal time");
                break;
            case 'z':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                g_tool_name = "zipinfo";
                ctx->zi_show_comments = true;
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-z show comments");
                break;
            case OPT_DRY_RUN:
                ctx->dry_run = true;
                ctx->verbose = true;
                ctx->quiet = false;
                ctx->list_only = ctx->list_only || ctx->zipinfo_mode;
                zu_trace_option(ctx, "--dry-run");
                break;
            case '?':
                print_usage(stdout, argv[0]);
                return ZU_STATUS_USAGE;
            default:
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
        }
    }

    /* Positional Arguments */
    if (optind >= argc) {
        // "unzip -v" or "zipinfo -v" with no args prints version
        if (ctx->zipinfo_mode && ctx->verbose) {
            ctx->archive_path = NULL;
            return ZU_STATUS_OK;
        }
        print_usage(stderr, argv[0]);
        return ZU_STATUS_USAGE;
    }

    ctx->archive_path = argv[optind++];
    if (strcmp(ctx->archive_path, "-") == 0) {
        zu_cli_error(g_tool_name, "reading archive from stdin is not fully supported in this context version");
        return ZU_STATUS_NOT_IMPLEMENTED;
    }

    zu_trace_option(ctx, "archive path set to %s", ctx->archive_path);

    /* Remaining args are patterns */
    for (int i = optind; i < argc; ++i) {
        if (zu_strlist_push(&ctx->include, argv[i]) != 0)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "include pattern %s", argv[i]);
    }

    /* Re-add literal "--" if it was consumed by getopt but meant as a pattern marker for lib */
    /* Note: getopt handles "--" by stopping, but it doesn't pass it to us easily.
       Info-ZIP logic usually implies anything after archive is a pattern. */

    /* Final Zipinfo State Logic */
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
    zu_cli_init_terminal();
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        zu_cli_error(g_tool_name, "failed to allocate context");
        return ZU_STATUS_OOM;
    }

    int parse_rc = parse_unzip_args(argc, argv, ctx);

    if (parse_rc == ZU_STATUS_USAGE) {
        zu_context_free(ctx);
        return map_exit_code(parse_rc);
    }
    if (parse_rc != ZU_STATUS_OK) {
        if (parse_rc != ZU_STATUS_USAGE)
            zu_cli_error(g_tool_name, "argument parsing failed: %s", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return map_exit_code(parse_rc);
    }

    /* Version check mode */
    if (!ctx->archive_path && ctx->zipinfo_mode && ctx->verbose) {
        print_version(stdout);
        zu_context_free(ctx);
        return 0;
    }

    /* Dry Run Logic overrides */
    if (ctx->dry_run && !ctx->list_only && !ctx->test_integrity) {
        ctx->list_only = true;
    }
    if (ctx->dry_run) {
        ctx->quiet = false;
        ctx->verbose = true;
    }

    const char* tool_name = ctx->zipinfo_mode ? "zipinfo" : "unzip";
    g_tool_name = tool_name;
    emit_unzip_stub_warnings(ctx, tool_name);
    trace_effective_unzip_defaults(ctx);
    zu_cli_emit_option_trace(tool_name, ctx);

    int exec_rc = zu_unzip_run(ctx);

    if (exec_rc != ZU_STATUS_OK && ctx->error_msg[0] != '\0') {
        zu_cli_error(g_tool_name, "%s", ctx->error_msg);
    }

    zu_context_free(ctx);
    return map_exit_code(exec_rc);
}
