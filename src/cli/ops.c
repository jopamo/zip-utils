/* Ensure POSIX macros (WIFEXITED, etc.) are available */
#define _POSIX_C_SOURCE 200809L

#include "ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

#include "reader.h"
#include "writer.h"
#include "ziputils.h"

/* --- Helpers --- */

/**
 * Constructs a command string by replacing the first occurrence of "{}"
 * in `templ` with `target`. If "{}" is not found, appends `target` to `templ`.
 * * Returns a malloc'd string that must be freed by caller, or NULL on OOM.
 */
static char* build_test_command(const char* templ, const char* target) {
    if (!templ || !target)
        return NULL;

    const char* placeholder = strstr(templ, "{}");
    char* out = NULL;
    size_t out_size = 0;

    if (!placeholder) {
        /* Fallback: append target to command with a space */
        /* Size: templ + space + target + null terminator */
        out_size = strlen(templ) + 1 + strlen(target) + 1;
        out = malloc(out_size);
        if (!out)
            return NULL;

        snprintf(out, out_size, "%s %s", templ, target);
    }
    else {
        /* Replace "{}" with target */
        size_t prefix_len = (size_t)(placeholder - templ);
        size_t suffix_len = strlen(placeholder + 2);
        size_t target_len = strlen(target);

        out_size = prefix_len + target_len + suffix_len + 1;
        out = malloc(out_size);
        if (!out)
            return NULL;

        /* Construct string parts */
        memcpy(out, templ, prefix_len);
        memcpy(out + prefix_len, target, target_len);
        /* Copy suffix including null terminator */
        memcpy(out + prefix_len + target_len, placeholder + 2, suffix_len + 1);
    }

    return out;
}

static int run_test_command(ZContext* ctx, const char* target) {
    char* cmd = build_test_command(ctx->test_command, target);
    if (!cmd)
        return ZU_STATUS_OOM;

    if (!ctx->quiet) {
        printf("Testing archive: %s\n", target);
        if (ctx->verbose) {
            printf("Executing: %s\n", cmd);
        }
    }

    int status = system(cmd);
    free(cmd);

    if (status == -1) {
        perror("zip: system() failed");
        return ZU_STATUS_IO;
    }

    /* Check shell exit code */
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (!ctx->quiet)
            printf("Test of %s OK\n", target);
        return ZU_STATUS_OK;
    }

    /* Report failure details */
    if (WIFEXITED(status)) {
        fprintf(stderr, "zip: test command failed (exit code %d)\n", WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status)) {
        fprintf(stderr, "zip: test command terminated by signal %d\n", WTERMSIG(status));
    }
    else {
        fprintf(stderr, "zip: test command failed abnormally\n");
    }

    return ZU_STATUS_IO;
}

/* --- Public Operations --- */

int zu_zip_run(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    /* 1. Archive Recovery / Fix Mode */
    if (ctx->fix_archive || ctx->fix_fix_archive) {
        return zu_modify_archive(ctx);
    }

    /* 2. Pure Test Mode (no modifications, just verification) */
    if (ctx->test_integrity && ctx->include.len == 0) {
        if (ctx->test_command) {
            return run_test_command(ctx, ctx->archive_path);
        }
        else {
            int rc = zu_test_archive(ctx);
            if (rc == ZU_STATUS_OK && !ctx->quiet) {
                printf("No errors detected in compressed data of %s.\n", ctx->archive_path);
            }
            return rc;
        }
    }

    /* 3. Comment-only Update */
    /* zip -z archive.zip (with no file args) reads comment from stdin */
    if (ctx->include.len == 0 && !ctx->zip_comment_specified) {
        fprintf(stderr, "zip: error: no input files specified\n");
        return ZU_STATUS_USAGE;
    }

    /* 4. Modification / Creation */
    int rc = zu_modify_archive(ctx);

    /* 5. Post-Write Testing (-T) */
    if (rc == ZU_STATUS_OK && ctx->test_integrity) {
        const char* target = ctx->output_path ? ctx->output_path : ctx->archive_path;

        if (ctx->test_command) {
            rc = run_test_command(ctx, target);
        }
        else {
            /* * Temporary context swap:
             * Point context to the NEW file (target) for reading/testing.
             */
            const char* saved_path = ctx->archive_path;
            ctx->archive_path = target;

            rc = zu_test_archive(ctx);

            ctx->archive_path = saved_path;  // Restore

            if (rc == ZU_STATUS_OK && !ctx->quiet) {
                printf("test of %s OK\n", target);
            }
        }
    }

    return rc;
}

int zu_unzip_run(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    if (ctx->list_only) {
        return zu_list_archive(ctx);
    }

    if (ctx->test_integrity) {
        int rc = zu_test_archive(ctx);
        if (rc == ZU_STATUS_OK && !ctx->quiet) {
            printf("No errors detected in compressed data of %s.\n", ctx->archive_path);
        }
        return rc;
    }

    return zu_extract_archive(ctx);
}
