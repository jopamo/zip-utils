#define _GNU_SOURCE

#include <getopt.h>
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

static uint64_t parse_size(const char* str) {
    char* end;
    double val = strtod(str, &end);
    if (end == str || val < 0)
        return 0;

    uint64_t mult = 1;
    if (*end) {
        switch (*end) {
            case 'k':
            case 'K':
                mult = 1024;
                break;
            case 'm':
            case 'M':
                mult = 1024 * 1024;
                break;
            case 'g':
            case 'G':
                mult = 1024 * 1024 * 1024;
                break;
            case 't':
            case 'T':
                mult = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
                break;
        }
    }
    return (uint64_t)(val * mult);
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
            "Modern rewrite stub: captures options into a reentrant context but\n"
            "does not yet perform archiving.\n"
            "\n"
            "Common options:\n"
            "  -r, --recurse-paths   Recurse into directories\n"
            "  -j                    Junk directory paths\n"
            "  -m                    Move input files (delete after)\n"
            "  -d                    Delete entries in zipfile\n"
            "  -@                    Read names from stdin\n"
            "  -u                    Update: add only newer files\n"
            "  -f                    Freshen: replace existing entries\n"
            "  -T                    Test archive after writing\n"
            "  -q / -qq / -v         Quiet / really quiet / verbose output\n"
            "  -x pattern            Exclude pattern (can repeat)\n"
            "  -i pattern            Include pattern (can repeat)\n"
            "  -0 .. -9              Compression level\n"
            "  -Z method             Compression method (deflate, bzip2)\n"
            "  --help                Show this help\n",
            argv0);
}

static int parse_zip_args(int argc, char** argv, ZContext* ctx, bool is_zipnote) {
    static const struct option long_opts[] = {
        {"recurse-paths", no_argument, NULL, 'r'},
        {"test", no_argument, NULL, 'T'},
        {"quiet", no_argument, NULL, 'q'},
        {"verbose", no_argument, NULL, 'v'},
        {"encrypt", no_argument, NULL, 'e'},
        {"password", required_argument, NULL, 'P'},
        {"help", no_argument, NULL, 'h'},
        {"output-file", required_argument, NULL, 'O'},
        {"la", no_argument, NULL, 1001},
        {"log-append", no_argument, NULL, 1001},
        {"lf", required_argument, NULL, 1002},
        {"logfile-path", required_argument, NULL, 1002},
        {"li", no_argument, NULL, 1003},
        {"log-info", no_argument, NULL, 1003},
        {"tt", required_argument, NULL, 1004},
        {"filesync", no_argument, NULL, 1005},
        {"FS", no_argument, NULL, 1005},
        {"split-size", required_argument, NULL, 's'},
        {"pause", no_argument, NULL, 1006},
        {"sp", no_argument, NULL, 1006},
        {"fix", no_argument, NULL, 'F'},
        {"FF", no_argument, NULL, 1007},
        {"fixfix", no_argument, NULL, 1007},
        {"ll", no_argument, NULL, 1008},
        {NULL, 0, NULL, 0},
    };

    int opt;
    // Added O, t to short opts
    while ((opt = getopt_long_only(argc, argv, "rjTqvmdfui:x:0123456789heP:O:t:Z:s:Fzw@cob:n:DAJXyl", long_opts, NULL)) != -1) {
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
            case 's':
                ctx->split_size = parse_size(optarg);
                if (ctx->split_size == 0) {
                    fprintf(stderr, "zip: invalid split size: %s\n", optarg);
                    return ZU_STATUS_USAGE;
                }
                break;
            case 1006:  // -sp (pause) -- usually passed as -sp, but getopt_long_only might match -s if not careful.
                        // Actually, standard zip uses -s <size> and -sp for pause.
                        // Since we use getopt_long_only, "-sp" might be interpreted as short 's' with arg "p" if we aren't careful?
                        // No, getopt_long_only checks long options first.
                        // But "sp" isn't in long_opts? Wait. "pause" is 1006.
                        // The user said "-s" and "-sp".
                        // If I want to support "-sp" specifically, I should probably add it as a long option or handle it differently.
                        // However, since we are using getopt_long_only, we can just add "sp" to long_opts or handle it manually?
                        // Standard zip options are a mix.
                        // Let's assume user uses `--pause` or we need to hack it.
                        // Wait, standard `zip` treats `-sp` as a flag. But `zip -s 10m` takes an arg.
                        // If I put "sp" in long_opts, it will work with single dash too in long_only mode.
                ctx->split_pause = true;
                break;
            case 'F':
                ctx->fix_archive = true;
                break;
            case 1007:  // -FF
                ctx->fix_fix_archive = true;
                break;
            case 1008:  // -ll
                fprintf(stderr, "zip: -ll (convert CR LF to LF) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'T':
                ctx->test_integrity = true;
                break;
            case 'q':
                ctx->quiet_level++;
                ctx->quiet = ctx->quiet_level > 0;
                ctx->verbose = false;
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
                if (zu_strlist_push(&ctx->include_patterns, optarg) != 0)
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
            case 'z':
                ctx->zip_comment_specified = true;
                break;
            case '@': {
                int rc = read_stdin_names(ctx);
                if (rc != ZU_STATUS_OK) {
                    return rc;
                }
            } break;
            case 'c':
                fprintf(stderr, "zip: -c (add one-line comments) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'o':
                fprintf(stderr, "zip: -o (make zipfile as old as latest entry) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'b':
                fprintf(stderr, "zip: -b (temp file path) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'n':
                fprintf(stderr, "zip: -n (don't compress suffixes) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'D':
                fprintf(stderr, "zip: -D (do not add directory entries) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'A':
                fprintf(stderr, "zip: -A (adjust self-extracting exe) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'J':
                fprintf(stderr, "zip: -J (junk zipfile prefix) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'X':
                fprintf(stderr, "zip: -X (exclude extra file attributes) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'y':
                fprintf(stderr, "zip: -y (store symbolic links as the link) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'l':
                fprintf(stderr, "zip: -l (convert LF to CR LF) not supported in this version\n");
                return ZU_STATUS_NOT_IMPLEMENTED;
            case 'w':
                if (!is_zipnote) {
                    print_usage(stderr, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                ctx->zipnote_write = true;
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
    if (is_zipnote) {
        ctx->zipnote_mode = true;
    }

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
