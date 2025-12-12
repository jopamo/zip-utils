#define _XOPEN_SOURCE 700

#include "fileio.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <fnmatch.h>
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#endif

static void close_file(FILE** fp) {
    if (fp && *fp) {
        fclose(*fp);
        *fp = NULL;
    }
}

static void cleanup_temp_read(ZContext* ctx) {
    if (ctx && ctx->temp_read_is_split && ctx->temp_read_path) {
        unlink(ctx->temp_read_path);
    }
    if (ctx) {
        free(ctx->temp_read_path);
        ctx->temp_read_path = NULL;
        ctx->temp_read_is_split = false;
    }
}

static bool has_zip_suffix(const char* path) {
    if (!path) {
        return false;
    }
    const char* dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".zip") == 0;
}

static void free_part_list(char** parts, size_t count) {
    if (!parts)
        return;
    for (size_t i = 0; i < count; ++i)
        free(parts[i]);
    free(parts);
}

static int collect_split_parts(const char* path, char*** parts_out, size_t* count_out) {
    if (!parts_out || !count_out) {
        return ZU_STATUS_USAGE;
    }
    *parts_out = NULL;
    *count_out = 0;

    if (!has_zip_suffix(path)) {
        return ZU_STATUS_OK;
    }

    size_t path_len = strlen(path);
    if (path_len < 4) {
        return ZU_STATUS_OK;
    }
    size_t base_len = path_len - 4; /* Strip ".zip" */

    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%.*s.z%02d", (int)base_len, path, 1);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return ZU_STATUS_IO;
    }

    struct stat st;
    if (stat(buf, &st) != 0) {
        if (errno == ENOENT) {
            return ZU_STATUS_OK; /* Not split */
        }
        return ZU_STATUS_IO;
    }

    char** parts = NULL;
    size_t count = 0;

    for (int idx = 1;; ++idx) {
        n = snprintf(buf, sizeof(buf), "%.*s.z%02d", (int)base_len, path, idx);
        if (n <= 0 || n >= (int)sizeof(buf)) {
            free_part_list(parts, count);
            return ZU_STATUS_IO;
        }
        if (stat(buf, &st) != 0) {
            if (errno == ENOENT) {
                break; /* No more parts */
            }
            free_part_list(parts, count);
            return ZU_STATUS_IO;
        }
        char* part = strdup(buf);
        if (!part) {
            free_part_list(parts, count);
            return ZU_STATUS_OOM;
        }
        char** np = realloc(parts, (count + 1) * sizeof(char*));
        if (!np) {
            free(part);
            free_part_list(parts, count);
            return ZU_STATUS_OOM;
        }
        parts = np;
        parts[count++] = part;
    }

    if (stat(path, &st) != 0) {
        free_part_list(parts, count);
        return ZU_STATUS_IO;
    }
    char* last = strdup(path);
    if (!last) {
        free_part_list(parts, count);
        return ZU_STATUS_OOM;
    }
    char** np = realloc(parts, (count + 1) * sizeof(char*));
    if (!np) {
        free(last);
        free_part_list(parts, count);
        return ZU_STATUS_OOM;
    }
    parts = np;
    parts[count++] = last;

    *parts_out = parts;
    *count_out = count;
    return ZU_STATUS_OK;
}

static int concat_split_parts(ZContext* ctx, char** parts, size_t count, char** path_out) {
    if (!ctx || !parts || count == 0 || !path_out) {
        return ZU_STATUS_USAGE;
    }

    char template[] = "/tmp/zu-split-XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "mkstemp failed for split archive");
        return ZU_STATUS_IO;
    }

    FILE* out = fdopen(fd, "wb");
    if (!out) {
        close(fd);
        unlink(template);
        zu_context_set_error(ctx, ZU_STATUS_IO, "fdopen failed for split archive");
        return ZU_STATUS_IO;
    }

    uint8_t* buf = malloc(64 * 1024);
    if (!buf) {
        fclose(out);
        unlink(template);
        return ZU_STATUS_OOM;
    }

    int rc = ZU_STATUS_OK;
    for (size_t i = 0; i < count; ++i) {
        FILE* in = fopen(parts[i], "rb");
        if (!in) {
            char msg[128];
            snprintf(msg, sizeof(msg), "open split part '%s' failed", parts[i]);
            zu_context_set_error(ctx, ZU_STATUS_IO, msg);
            rc = ZU_STATUS_IO;
            break;
        }

        size_t got;
        while ((got = fread(buf, 1, 64 * 1024, in)) > 0) {
            if (fwrite(buf, 1, got, out) != got) {
                zu_context_set_error(ctx, ZU_STATUS_IO, "write temp split archive failed");
                rc = ZU_STATUS_IO;
                break;
            }
        }
        if (ferror(in) && rc == ZU_STATUS_OK) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "read split part failed");
            rc = ZU_STATUS_IO;
        }
        fclose(in);
        if (rc != ZU_STATUS_OK)
            break;
    }

    free(buf);
    if (fclose(out) != 0 && rc == ZU_STATUS_OK) {
        zu_context_set_error(ctx, ZU_STATUS_IO, "flush split archive failed");
        rc = ZU_STATUS_IO;
    }

    if (rc != ZU_STATUS_OK) {
        unlink(template);
        return rc;
    }

    char* path = strdup(template);
    if (!path) {
        unlink(template);
        return ZU_STATUS_OOM;
    }
    *path_out = path;
    return ZU_STATUS_OK;
}

int zu_open_input(ZContext* ctx, const char* path) {
    if (!ctx || !path) {
        return ZU_STATUS_USAGE;
    }
    close_file(&ctx->in_file);
    cleanup_temp_read(ctx);

    char** parts = NULL;
    size_t part_count = 0;
    int rc = collect_split_parts(path, &parts, &part_count);
    if (rc != ZU_STATUS_OK) {
        free_part_list(parts, part_count);
        zu_context_set_error(ctx, rc, "split detection failed");
        return rc;
    }

    const char* open_path = path;
    if (part_count > 0) {
        char* temp_path = NULL;
        rc = concat_split_parts(ctx, parts, part_count, &temp_path);
        free_part_list(parts, part_count);
        if (rc != ZU_STATUS_OK) {
            return rc;
        }
        ctx->temp_read_path = temp_path;
        ctx->temp_read_is_split = true;
        open_path = temp_path;
    }
    else {
        free_part_list(parts, part_count);
    }

    ctx->in_file = fopen(open_path, "rb");
    if (!ctx->in_file) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open input '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, buf);
        cleanup_temp_read(ctx);
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

int zu_open_output(ZContext* ctx, const char* path, const char* mode) {
    if (!ctx || !path) {
        return ZU_STATUS_USAGE;
    }
    close_file(&ctx->out_file);
    ctx->out_file = fopen(path, mode ? mode : "wb");
    if (!ctx->out_file) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open output '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, buf);
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

void zu_close_files(ZContext* ctx) {
    if (!ctx) {
        return;
    }
    close_file(&ctx->in_file);
    close_file(&ctx->out_file);
    cleanup_temp_read(ctx);
}

static int walk_dir(ZContext* ctx, const char* root, ZU_StrList* list) {
    DIR* d = opendir(root);
    if (!d) {
        /* If we can't open a directory, warn but continue? */
        zu_log(ctx, "warning: could not open directory %s: %s\n", root, strerror(errno));
        return ZU_STATUS_OK;
    }

    /* Add directory entry itself (Info-ZIP adds directory entries with trailing '/') */
    char dir_entry[4096];
    int dir_len = snprintf(dir_entry, sizeof(dir_entry), "%s/", root);
    if (dir_len >= (int)sizeof(dir_entry)) {
        zu_log(ctx, "warning: path too long %s/\n", root);
    }
    else {
        zu_strlist_push(list, dir_entry);
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[4096];
        int len = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (len >= (int)sizeof(path)) {
            zu_log(ctx, "warning: path too long %s/%s\n", root, entry->d_name);
            continue;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            zu_log(ctx, "warning: could not stat %s: %s\n", path, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Recurse */
            /* Add directory itself? Info-ZIP adds directory entries (size 0, end with /) */
            /* But for now, let's just recurse. */
            walk_dir(ctx, path, list);
        }
        else if (S_ISREG(st.st_mode) || (ctx->allow_symlinks && S_ISLNK(st.st_mode))) {
            if (zu_strlist_push(list, path) != 0) {
                closedir(d);
                return ZU_STATUS_OOM;
            }
        }
    }
    closedir(d);
    return ZU_STATUS_OK;
}

bool zu_should_include(const ZContext* ctx, const char* name) {
    int flags = ctx->match_case ? 0 : FNM_CASEFOLD;

    /* Exclude wins immediately. */
    for (size_t i = 0; i < ctx->exclude.len; ++i) {
        if (fnmatch(ctx->exclude.items[i], name, flags) == 0) {
            return false;
        }
    }

    /* If include list is empty, everything is included. */
    if (ctx->include.len == 0) {
        return true;
    }

    for (size_t i = 0; i < ctx->include.len; ++i) {
        if (fnmatch(ctx->include.items[i], name, flags) == 0) {
            return true;
        }
    }
    return false;
}

int zu_expand_args(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    if (!ctx->recursive) {
        return ZU_STATUS_OK;
    }

    ZU_StrList new_list;
    zu_strlist_init(&new_list);

    for (size_t i = 0; i < ctx->include.len; ++i) {
        const char* path = ctx->include.items[i];
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* If it's a directory and recursive is on, walk it */
            /* Note: should we include the directory itself in the archive?
               Standard zip does. Let's stick to files for now unless requested. */
            walk_dir(ctx, path, &new_list);
        }
        else {
            /* Just copy */
            zu_strlist_push(&new_list, path);
        }
    }

    /* Filter new_list by exclude/include patterns */
    ZU_StrList filtered;
    zu_strlist_init(&filtered);
    for (size_t i = 0; i < new_list.len; ++i) {
        const char* path = new_list.items[i];
        if (zu_should_include(ctx, path)) {
            if (zu_strlist_push(&filtered, path) != 0) {
                zu_strlist_free(&new_list);
                zu_strlist_free(&filtered);
                return ZU_STATUS_OOM;
            }
        }
    }
    zu_strlist_free(&new_list);

    /* Swap lists */
    zu_strlist_free(&ctx->include);
    ctx->include = filtered;

    return ZU_STATUS_OK;
}
