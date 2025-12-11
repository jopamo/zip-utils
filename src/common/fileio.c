#define _XOPEN_SOURCE 700

#include "fileio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>

static void close_file(FILE** fp) {
    if (fp && *fp) {
        fclose(*fp);
        *fp = NULL;
    }
}

int zu_open_input(ZContext* ctx, const char* path) {
    if (!ctx || !path) {
        return ZU_STATUS_USAGE;
    }
    close_file(&ctx->in_file);
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

    /* Swap lists */
    zu_strlist_free(&ctx->include);
    ctx->include = new_list;

    return ZU_STATUS_OK;
}
