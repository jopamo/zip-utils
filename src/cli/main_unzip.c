#define _GNU_SOURCE

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctx.h"
#include "ops.h"
#include "ziputils.h"

static bool argv0_is_zipinfo(const char *argv0) {
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    return strcmp(base, "zipinfo") == 0 || strcmp(base, "ii") == 0;
}

static void print_usage(FILE *to, const char *argv0) {
    fprintf(
        to,
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
        "  -q / -v               Quiet / verbose output\n"
        "  -x pattern            Exclude pattern (can repeat)\n"
        "  -i pattern            Include only matching pattern\n"
        "  -C                    Case-insensitive pattern matching\n"
        "  --help, -?            Show this help\n"
        "\n"
        "Zipinfo options (-Z or run as \"zipinfo\"):\n"
        "  -1 / -2               Filenames only (quiet / allow header+totals)\n"
        "  -s / -m / -l / -v     Short (default) / medium / long listing / verbose\n"
        "  -h / -t               Force header / totals footer (alone suppress entries)\n"
        "  -T                    Decimal timestamps (yymmdd.hhmmss)\n"
        "  -M                    Enable pager prompt (noop placeholder)\n"
        "  -z                    Include archive comment (ignored)\n",
        argv0,
        argv0);
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
        case ZU_STATUS_NOT_IMPLEMENTED:
            return 3;
        default:
            return 3;
    }
}

static int parse_unzip_args(int argc, char **argv, ZContext *ctx) {
    if (argv0_is_zipinfo(argv[0])) {
        ctx->zipinfo_mode = true;
        ctx->list_only = true;
    }

    static const struct option long_opts[] = {
        {"help", no_argument, NULL, '?'},
        {"list", no_argument, NULL, 'l'},
        {"test", no_argument, NULL, 't'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "ltd:ovnqvi:x:hCZ12smMvTz?", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'l':
                if (ctx->zipinfo_mode) {
                    ctx->zi_format = ZU_ZI_FMT_LONG;
                    ctx->zi_format_specified = true;
                    ctx->list_only = true;
                } else {
                    ctx->list_only = true;
                }
                break;
            case 't':
                if (ctx->zipinfo_mode) {
                    ctx->zi_footer = true;
                    ctx->zi_footer_explicit = true;
                    ctx->list_only = true;
                } else {
                    ctx->test_integrity = true;
                }
                break;
            case 'd':
                ctx->target_dir = optarg;
                break;
            case 'o':
                ctx->overwrite = true;
                break;
            case 'n':
                ctx->overwrite = false;
                break;
            case 'q':
                ctx->quiet = true;
                break;
            case 'v':
                ctx->verbose = true;
                if (ctx->zipinfo_mode) {
                    ctx->zi_format = ZU_ZI_FMT_VERBOSE;
                    ctx->zi_format_specified = true;
                    ctx->list_only = true;
                    ctx->zi_show_comments = true;
                }
                break;
            case 'C':
                ctx->match_case = false;
                break;
            case 'i':
                if (zu_strlist_push(&ctx->include, optarg) != 0)
                    return ZU_STATUS_OOM;
                break;
            case 'x':
                if (zu_strlist_push(&ctx->exclude, optarg) != 0)
                    return ZU_STATUS_OOM;
                break;
            case 'Z':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
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
                break;
            case '2':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;
                ctx->zi_list_entries = true;
                break;
            case 's':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_SHORT;
                ctx->zi_format_specified = true;
                break;
            case 'm':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_format = ZU_ZI_FMT_MEDIUM;
                ctx->zi_format_specified = true;
                break;
            case 'h':
                if (ctx->zipinfo_mode) {
                    ctx->zi_header = true;
                    ctx->zi_header_explicit = true;
                    ctx->list_only = true;
                } else {
                    print_usage(stdout, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                break;
            case 'M':
                ctx->zipinfo_mode = true;
                ctx->zi_allow_pager = true;
                ctx->list_only = true;
                break;
            case 'T':
                ctx->zipinfo_mode = true;
                ctx->zi_decimal_time = true;
                ctx->list_only = true;
                break;
            case 'z':
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                ctx->zi_show_comments = true;
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
    for (int i = optind; i < argc; ++i) {
        if (zu_strlist_push(&ctx->include, argv[i]) != 0)
            return ZU_STATUS_OOM;
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

int main(int argc, char **argv) {
    ZContext *ctx = zu_context_create();
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

    int exec_rc = zu_unzip_run(ctx);
    if (exec_rc != ZU_STATUS_OK && ctx->error_msg[0] != '\0') {
        fprintf(stderr, "unzip: %s\n", ctx->error_msg);
    }
    zu_context_free(ctx);
    return map_exit_code(exec_rc);
}
