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

/*
 * File and path utilities used by zip/unzip execution
 *
 * Responsibilities
 * - Open/close archive input and output streams attached to ZContext
 * - Detect split archive fragments and reject them when unsupported
 * - Expand CLI operands when recursive traversal is enabled
 * - Apply include/exclude pattern rules to produce a final list of inputs
 *
 * Conventions
 * - Paths stored in ctx lists are normalized by stripping leading "./" segments
 * - Directory recursion uses lstat so symlink policy can be enforced by the caller
 * - Pattern matching uses fnmatch with optional case folding when available
 *
 * Notes
 * - This file is intentionally conservative about continuing on filesystem errors
 *   traversal warnings are logged and the run proceeds where possible
 */

/*
 * Normalize a path string by removing repeated leading "./" segments
 *
 * Examples
 * - "./a/b" -> "a/b"
 * - "././a" -> "a"
 * - "."     -> "" (treated as empty so callers can avoid adding it as an entry name)
 *
 * This is a presentation/archiving normalization
 * - It does not resolve ".." or symlinks
 * - It does not attempt canonicalization
 */
static const char* strip_leading_dot_slash(const char* path) {
    const char* p = path ? path : "";
    while (p[0] == '.' && p[1] == '/') {
        p += 2;
    }
    if (p[0] == '.' && p[1] == '\0') {
        return p + 1;
    }
    return p;
}

/*
 * Close a FILE* and clear the caller's pointer
 *
 * This is used to manage ctx->in_file and ctx->out_file safely
 */
static void close_file(FILE** fp) {
    if (fp && *fp) {
        fclose(*fp);
        *fp = NULL;
    }
}

/*
 * Check whether a path ends with ".zip" using case-insensitive comparison
 *
 * This is used only as a heuristic for split archive detection
 */
static bool has_zip_suffix(const char* path) {
    if (!path)
        return false;

    const char* dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".zip") == 0;
}

/*
 * Detect a split archive companion segment and reject if found
 *
 * Policy
 * - If the user provides "foo.zip", check for "foo.z01"
 * - If "foo.z01" exists, report NOT_IMPLEMENTED because split archives are unsupported
 *
 * Return values
 * - ZU_STATUS_OK when no split segment is detected
 * - ZU_STATUS_NOT_IMPLEMENTED when split segment exists
 * - ZU_STATUS_IO when a filesystem error occurs other than ENOENT
 */
static int check_for_split_archive(const char* path) {
    if (!has_zip_suffix(path))
        return ZU_STATUS_OK;

    size_t path_len = strlen(path);
    if (path_len < 4)
        return ZU_STATUS_OK;

    size_t base_len = path_len - 4;

    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%.*s.z01", (int)base_len, path);
    if (n <= 0 || n >= (int)sizeof(buf))
        return ZU_STATUS_IO;

    struct stat st;
    if (stat(buf, &st) == 0)
        return ZU_STATUS_NOT_IMPLEMENTED;

    if (errno != ENOENT)
        return ZU_STATUS_IO;

    return ZU_STATUS_OK;
}

/*
 * Open an archive for reading and attach it to ctx->in_file
 *
 * Behavior
 * - Closes any previously open input stream in ctx
 * - Rejects split archives early (foo.zip + foo.z01)
 * - Opens the file in binary mode
 *
 * Error reporting
 * - On failure, ctx->last_error and ctx->error_msg are updated via zu_context_set_error
 */
int zu_open_input(ZContext* ctx, const char* path) {
    if (!ctx || !path)
        return ZU_STATUS_USAGE;

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
    setvbuf(ctx->in_file, NULL, _IOFBF, 64 * 1024);

    return ZU_STATUS_OK;
}

/*
 * Open an archive output stream and attach it to ctx->out_file
 *
 * Behavior
 * - Closes any previously open output stream in ctx
 * - Opens the file using the supplied mode, defaulting to "wb"
 *
 * Error reporting
 * - On failure, ctx->last_error and ctx->error_msg are updated via zu_context_set_error
 */
int zu_open_output(ZContext* ctx, const char* path, const char* mode) {
    if (!ctx || !path)
        return ZU_STATUS_USAGE;

    close_file(&ctx->out_file);

    ctx->out_file = fopen(path, mode ? mode : "wb");
    if (!ctx->out_file) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open output '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, buf);
        return ZU_STATUS_IO;
    }
    setvbuf(ctx->out_file, NULL, _IOFBF, 64 * 1024);

    return ZU_STATUS_OK;
}

/*
 * Close any archive input/output streams associated with a context
 *
 * Safe to call multiple times
 */
void zu_close_files(ZContext* ctx) {
    if (!ctx)
        return;

    close_file(&ctx->in_file);
    close_file(&ctx->out_file);
}

/*
 * Recursively walk a directory and add file operands into list
 *
 * Behavior
 * - Uses opendir/readdir and lstat to avoid following symlinks by accident
 * - Adds each regular file path to list
 * - When allow_symlinks is true, also adds symlink paths as operands
 *
 * Directory entries
 * - If store_paths is enabled and no_dir_entries is false, an explicit directory entry with
 *   a trailing '/' is also added for each directory visited (except "." which normalizes empty)
 *
 * Error policy
 * - Failure to open or stat subpaths is logged as a warning and traversal continues
 * - Allocation failure is treated as fatal and returns ZU_STATUS_OOM
 */
static int walk_dir(ZContext* ctx, const char* root, ZU_StrList* list) {
    DIR* d = opendir(root);
    if (!d) {
        zu_log(ctx, "warning: could not open directory %s: %s\n", root, strerror(errno));
        return ZU_STATUS_OK;
    }

    if (!ctx->no_dir_entries && ctx->store_paths) {
        char dir_entry[4096];
        const char* normalized = strip_leading_dot_slash(root);
        if (normalized[0] != '\0') {
            int dir_len = snprintf(dir_entry, sizeof(dir_entry), "%s/", normalized);
            if (dir_len >= (int)sizeof(dir_entry)) {
                zu_log(ctx, "warning: path too long %s/\n", normalized);
            }
            else {
                zu_strlist_push(list, dir_entry);
            }
        }
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

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
            walk_dir(ctx, path, list);
        }
        else if (S_ISREG(st.st_mode) || (ctx->allow_symlinks && S_ISLNK(st.st_mode))) {
            const char* normalized = strip_leading_dot_slash(path);
            if (zu_strlist_push(list, normalized) != 0) {
                closedir(d);
                return ZU_STATUS_OOM;
            }
        }
    }

    closedir(d);
    return ZU_STATUS_OK;
}

/*
 * Check whether a path matches any of the provided patterns
 *
 * Pattern semantics
 * - Uses fnmatch
 * - If match_case is false and FNM_CASEFOLD is available, enable case-folded matching
 *
 * Empty pattern list
 * - Treated as match-all to keep callers simple
 */
static bool matches_any_pattern(const ZU_StrList* patterns, const char* path, bool match_case) {
    if (!patterns || patterns->len == 0)
        return true;

    int flags = match_case ? 0 : FNM_CASEFOLD;

    for (size_t i = 0; i < patterns->len; ++i) {
        if (fnmatch(patterns->items[i], path, flags) == 0)
            return true;
    }

    return false;
}

/*
 * Walk the filesystem from root and collect files that match operand patterns
 *
 * This is used for PKZIP-style recursion from current directory (-R)
 * - patterns is the initial list of user-provided operands (ctx->include)
 * - Every discovered file is filtered by patterns and then by include/exclude rules
 *
 * Error policy
 * - Directory open failures are ignored to keep behavior closer to zip tools
 * - Allocation failures are fatal
 */
static int walk_dir_patterns(ZContext* ctx, const char* root, ZU_StrList* out, const ZU_StrList* patterns) {
    DIR* d = opendir(root);
    if (!d)
        return ZU_STATUS_OK;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[4096];
        if (strcmp(root, ".") == 0 || root[0] == '\0') {
            snprintf(path, sizeof(path), "%s", entry->d_name);
        }
        else {
            snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        }

        struct stat st;
        if (lstat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            walk_dir_patterns(ctx, path, out, patterns);
            continue;
        }

        // Accept regular files, and optionally symlinks and FIFOs depending on policy flags
        if (!(S_ISREG(st.st_mode) || (ctx->allow_symlinks && S_ISLNK(st.st_mode)) || (ctx->allow_fifo && S_ISFIFO(st.st_mode)))) {
            continue;
        }

        const char* normalized = strip_leading_dot_slash(path);

        if (!matches_any_pattern(patterns, normalized, ctx->match_case))
            continue;

        if (!zu_should_include(ctx, normalized))
            continue;

        if (zu_strlist_push(out, normalized) != 0) {
            closedir(d);
            return ZU_STATUS_OOM;
        }
    }

    closedir(d);
    return ZU_STATUS_OK;
}

/*
 * Decide whether a candidate path should be included based on ctx patterns
 *
 * Precedence rules
 * - Exclude patterns win immediately
 * - If no include_patterns are set, everything not excluded is included
 * - Otherwise the path must match at least one include_pattern
 *
 * Notes
 * - This is separate from operand matching
 *   operand matching selects candidates, include/exclude further filters them
 */
bool zu_should_include(const ZContext* ctx, const char* name) {
    int flags = ctx->match_case ? 0 : FNM_CASEFOLD;

    for (size_t i = 0; i < ctx->exclude.len; ++i) {
        if (fnmatch(ctx->exclude.items[i], name, flags) == 0) {
            return false;
        }
    }

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

/*
 * Expand ctx->include operands when recursion is enabled
 *
 * Modes
 * - recursive + recurse_from_cwd (-R)
 *   Walk from ".", collecting any files matching the operand patterns in ctx->include
 *   The resulting collected list replaces ctx->include
 *
 * - recursive (-r)
 *   For each operand in ctx->include:
 *   - If it is a directory, recursively walk it and collect file paths
 *   - Otherwise, keep it as-is
 *   After collection, apply include/exclude rules to produce the final ctx->include list
 *
 * Return values
 * - ZU_STATUS_OK on success
 * - ZU_STATUS_OOM on allocation failure
 * - ZU_STATUS_USAGE if ctx is NULL
 */
int zu_expand_args(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    if (ctx->recursive && ctx->recurse_from_cwd) {
        ZU_StrList collected;
        zu_strlist_init(&collected);

        int rc = walk_dir_patterns(ctx, ".", &collected, &ctx->include);
        if (rc != ZU_STATUS_OK) {
            zu_strlist_free(&collected);
            return rc;
        }

        zu_strlist_free(&ctx->include);
        ctx->include = collected;
        return ZU_STATUS_OK;
    }

    if (!ctx->recursive) {
        return ZU_STATUS_OK;
    }

    ZU_StrList new_list;
    zu_strlist_init(&new_list);

    for (size_t i = 0; i < ctx->include.len; ++i) {
        const char* path = ctx->include.items[i];

        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            int rc = walk_dir(ctx, path, &new_list);
            if (rc != ZU_STATUS_OK) {
                zu_strlist_free(&new_list);
                return rc;
            }
        }
        else {
            const char* normalized = strip_leading_dot_slash(path);
            if (*normalized != '\0')
                zu_strlist_push(&new_list, normalized);
        }
    }

    // Filter collected paths through include/exclude lists
    ZU_StrList filtered;
    zu_strlist_init(&filtered);

    for (size_t i = 0; i < new_list.len; ++i) {
        const char* path = new_list.items[i];
        if (!zu_should_include(ctx, path))
            continue;

        if (zu_strlist_push(&filtered, path) != 0) {
            zu_strlist_free(&new_list);
            zu_strlist_free(&filtered);
            return ZU_STATUS_OOM;
        }
    }

    zu_strlist_free(&new_list);

    // Replace ctx->include with the filtered list
    zu_strlist_free(&ctx->include);
    ctx->include = filtered;

    return ZU_STATUS_OK;
}
