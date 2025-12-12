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

static bool has_zip_suffix(const char* path) {
    if (!path) {
        return false;
    }
    const char* dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".zip") == 0;
}

static int check_for_split_archive(const char* path) {
    if (!has_zip_suffix(path)) {
        return ZU_STATUS_OK;
    }
    size_t path_len = strlen(path);
    if (path_len < 4) {
        return ZU_STATUS_OK;
    }
    size_t base_len = path_len - 4; /* Strip ".zip" */

    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%.*s.z01", (int)base_len, path);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return ZU_STATUS_IO;
    }

    struct stat st;
    if (stat(buf, &st) == 0) {
        return ZU_STATUS_NOT_IMPLEMENTED;
    }
    if (errno != ENOENT) {
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

int zu_open_input(ZContext* ctx, const char* path) {
    if (!ctx || !path) {
        return ZU_STATUS_USAGE;
    }
    close_file(&ctx->in_file);

    int rc = check_for_split_archive(path);
    if (rc == ZU_STATUS_NOT_IMPLEMENTED) {
        zu_context_set_error(ctx, rc, "split archives are not supported");
        return rc;
    }
    if (rc != ZU_STATUS_OK) {
        zu_context_set_error(ctx, rc, "split detection failed");
        return rc;
    }

    ctx->in_file = fopen(path, "rb");
    if (!ctx->in_file) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open input '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, buf);
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
}

static int walk_dir(ZContext* ctx, const char* root, ZU_StrList* list) {
    DIR* d = opendir(root);
    if (!d) {
        /* If we can't open a directory, warn but continue? */
        zu_log(ctx, "warning: could not open directory %s: %s\n", root, strerror(errno));
        return ZU_STATUS_OK;
    }

    /* Add directory entry itself (Info-ZIP adds directory entries with trailing '/') */
    if (!ctx->no_dir_entries && ctx->store_paths) {
        char dir_entry[4096];
        int dir_len = snprintf(dir_entry, sizeof(dir_entry), "%s/", root);
        if (dir_len >= (int)sizeof(dir_entry)) {
            zu_log(ctx, "warning: path too long %s/\n", root);
        }
        else {
            zu_strlist_push(list, dir_entry);
        }
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
    if (ctx->include_patterns.len == 0) {
        return true;
    }

    for (size_t i = 0; i < ctx->include_patterns.len; ++i) {
        if (fnmatch(ctx->include_patterns.items[i], name, flags) == 0) {
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
