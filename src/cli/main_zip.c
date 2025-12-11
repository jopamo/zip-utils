#define _GNU_SOURCE

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctx.h"
#include "ops.h"
#include "ziputils.h"

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

static void print_usage(FILE *to, const char *argv0) {
    fprintf(
        to,
        "Usage: %s [options] archive.zip [inputs...]\n"
        "\n"
        "Modern rewrite stub: captures options into a reentrant context but\n"
        "does not yet perform archiving.\n"
        "\n"
        "Common options:\n"
        "  -r, --recurse-paths   Recurse into directories\n"
        "  -j                    Junk directory paths\n"
        "  -m                    Move input files (delete after)\n"
        "  -u                    Update: add only newer files\n"
        "  -f                    Freshen: replace existing entries\n"
        "  -T                    Test archive after writing\n"
        "  -q / -v               Quiet / verbose output\n"
        "  -x pattern            Exclude pattern (can repeat)\n"
        "  -i pattern            Include pattern (can repeat)\n"
        "  -0 .. -9              Compression level\n"
        "  --help                Show this help\n",
        argv0);
}

static int parse_zip_args(int argc, char **argv, ZContext *ctx) {
    static const struct option long_opts[] = {
        {"recurse-paths", no_argument, NULL, 'r'},
        {"test", no_argument, NULL, 'T'},
        {"quiet", no_argument, NULL, 'q'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "rjTqvmdfui:x:0123456789h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'r':
                ctx->recursive = true;
                break;
            case 'j':
                ctx->store_paths = false;
                break;
            case 'm':
                ctx->remove_source = true;
                break;
            case 'd':
                ctx->difference_mode = true;
                break;
            case 'f':
                ctx->freshen = true;
                break;
            case 'u':
                ctx->update = true;
                break;
            case 'T':
                ctx->test_integrity = true;
                break;
            case 'q':
                ctx->quiet = true;
                break;
            case 'v':
                ctx->verbose = true;
                break;
            case 'i':
                if (zu_strlist_push(&ctx->include, optarg) != 0)
                    return ZU_STATUS_OOM;
                break;
            case 'x':
                if (zu_strlist_push(&ctx->exclude, optarg) != 0)
                    return ZU_STATUS_OOM;
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                ctx->compression_level = opt - '0';
                break;
            case 'h':
                print_usage(stdout, argv[0]);
                return ZU_STATUS_USAGE;
            case '?':
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
    if (strcmp(ctx->archive_path, "-") == 0) {
        ctx->output_to_stdout = true;
    }

    for (int i = optind; i < argc; ++i) {
        if (zu_strlist_push(&ctx->include, argv[i]) != 0)
            return ZU_STATUS_OOM;
    }

    return ZU_STATUS_OK;
}

int main(int argc, char **argv) {
    ZContext *ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "zip: failed to allocate context\n");
        return ZU_STATUS_OOM;
    }

    int parse_rc = parse_zip_args(argc, argv, ctx);
    if (parse_rc == ZU_STATUS_USAGE) {
        zu_context_free(ctx);
        return 0;
    }
    if (parse_rc != ZU_STATUS_OK) {
        fprintf(stderr, "zip: argument parsing failed (%s)\n", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return map_exit_code(parse_rc);
    }

    int exec_rc = zu_zip_run(ctx);
    zu_context_free(ctx);
    return map_exit_code(exec_rc);
}
