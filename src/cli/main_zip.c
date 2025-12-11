#define _GNU_SOURCE

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ctx.h"
#include "ops.h"
#include "ziputils.h"
#include "reader.h"

static time_t parse_date(const char* str) {
    struct tm tm = {0};
    // Initialize tm to avoid garbage
    tm.tm_isdst = -1;

    // Try ISO 8601: yyyy-mm-dd
    if (strptime(str, "%Y-%m-%d", &tm)) {
        return mktime(&tm);
    }

    // Try mmddyyyy
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;
    if (strptime(str, "%m%d%Y", &tm)) {
        return mktime(&tm);
    }

    return (time_t)-1;
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

static void print_usage(FILE* to, const char* argv0) {
    fprintf(to,
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
            "  -FS                   File Sync: update and delete missing\n"
            "  -T                    Test archive after writing\n"
            "  -q / -v               Quiet / verbose output\n"
            "  -x pattern            Exclude pattern (can repeat)\n"
            "  -i pattern            Include pattern (can repeat)\n"
            "  -0 .. -9              Compression level\n"
            "  -Z method             Compression method (deflate, bzip2)\n"
            "  --help                Show this help\n",
            argv0);
}

static int parse_zip_args(int argc, char** argv, ZContext* ctx) {
    static const struct option long_opts[] = {
        {"recurse-paths", no_argument, NULL, 'r'}, {"test", no_argument, NULL, 'T'},
        {"quiet", no_argument, NULL, 'q'},         {"verbose", no_argument, NULL, 'v'},
        {"encrypt", no_argument, NULL, 'e'},       {"password", required_argument, NULL, 'P'},
        {"help", no_argument, NULL, 'h'},          {"output-file", required_argument, NULL, 'O'},
        {"la", no_argument, NULL, 1001},           {"log-append", no_argument, NULL, 1001},
        {"lf", required_argument, NULL, 1002},     {"logfile-path", required_argument, NULL, 1002},
        {"li", no_argument, NULL, 1003},           {"log-info", no_argument, NULL, 1003},
        {"tt", required_argument, NULL, 1004},     {"filesync", no_argument, NULL, 1005},
        {"FS", no_argument, NULL, 1005},           {NULL, 0, NULL, 0},
    };

    int opt;
    // Added O, t to short opts
    while ((opt = getopt_long_only(argc, argv, "rjTqvmdfui:x:0123456789heP:O:t:Z:", long_opts, NULL)) != -1) {
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
            case 1005:  // -FS
                ctx->filesync = true;
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
            case 'e':
                ctx->encrypt = true;
                break;
            case 'P':
                free(ctx->password);
                ctx->password = strdup(optarg);
                if (!ctx->password)
                    return ZU_STATUS_OOM;
                break;
            case 'O':
                ctx->output_path = optarg;
                break;
            case 't':  // -t mmddyyyy (after)
                ctx->filter_after = parse_date(optarg);
                if (ctx->filter_after == (time_t)-1) {
                    fprintf(stderr, "zip: invalid date format for -t: %s\n", optarg);
                    return ZU_STATUS_USAGE;
                }
                ctx->has_filter_after = true;
                break;
            case 1004:  // -tt mmddyyyy (before)
                ctx->filter_before = parse_date(optarg);
                if (ctx->filter_before == (time_t)-1) {
                    fprintf(stderr, "zip: invalid date format for -tt: %s\n", optarg);
                    return ZU_STATUS_USAGE;
                }
                ctx->has_filter_before = true;
                break;
            case 1001:  // -la
                ctx->log_append = true;
                break;
            case 1002:  // -lf
                free(ctx->log_path);
                ctx->log_path = strdup(optarg);
                if (!ctx->log_path)
                    return ZU_STATUS_OOM;
                break;
            case 1003:  // -li
                ctx->log_info = true;
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
            case 'Z':
                if (strcasecmp(optarg, "deflate") == 0) {
                    ctx->compression_method = 8;
                }
                else if (strcasecmp(optarg, "store") == 0) {
                    ctx->compression_method = 0;
                }
                else if (strcasecmp(optarg, "bzip2") == 0) {
                    ctx->compression_method = 12;
                }
                else {
                    fprintf(stderr, "zip: unknown compression method '%s'\n", optarg);
                    return ZU_STATUS_USAGE;
                }
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

int main(int argc, char** argv) {
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "zip: failed to allocate context\n");
        return ZU_STATUS_OOM;
    }

    // Default behavior for 'zip' is to modify/update existing archives
    ctx->modify_archive = true;

    if (is_alias(argv[0], "zipcloak")) {
        ctx->encrypt = true;
        ctx->modify_archive = true;
        // zipcloak implies no recursion/store flags, usually just modifying
    }
    else if (is_alias(argv[0], "zipsplit")) {
        fprintf(stderr, "%s: functionality not yet implemented\n", argv[0]);
        zu_context_free(ctx);
        return 1;
    }
    bool is_zipnote = is_alias(argv[0], "zipnote");

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
        ctx->zipinfo_mode = true;
        ctx->zi_show_comments = true;
        ctx->list_only = true;
        // zipnote typically outputs "Name=..." lines if used for editing,
        // but just dumping comments (like unzip -z but per file) is a reasonable default
        // for "minizip functionality" when -w is not supported.
        exec_rc = zu_list_archive(ctx);
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
