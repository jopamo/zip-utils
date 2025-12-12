#include "ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "reader.h"
#include "writer.h"
#include "ziputils.h"

static char* build_test_command(const char* templ, const char* target) {
    if (!templ || !target) {
        return NULL;
    }
    const char* placeholder = strstr(templ, "{}");
    if (!placeholder) {
        size_t len = strlen(templ) + 1 + strlen(target) + 1;
        char* out = malloc(len);
        if (!out)
            return NULL;
        snprintf(out, len, "%s %s", templ, target);
        return out;
    }
    size_t prefix_len = (size_t)(placeholder - templ);
    size_t templ_len = strlen(templ);
    size_t target_len = strlen(target);
    size_t suffix_len = templ_len - prefix_len - 2;
    size_t out_len = prefix_len + target_len + suffix_len + 1;
    char* out = malloc(out_len);
    if (!out)
        return NULL;
    memcpy(out, templ, prefix_len);
    memcpy(out + prefix_len, target, target_len);
    memcpy(out + prefix_len + target_len, placeholder + 2, suffix_len);
    out[out_len - 1] = '\0';
    return out;
}

static int run_test_command(ZContext* ctx, const char* target) {
    char* cmd = build_test_command(ctx->test_command, target);
    if (!cmd) {
        return ZU_STATUS_OOM;
    }
    int rc = system(cmd);
    free(cmd);
    if (rc == -1) {
        return ZU_STATUS_IO;
    }
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) {
        if (!ctx->quiet) {
            printf("test of %s OK\n", target);
        }
        return ZU_STATUS_OK;
    }
    return ZU_STATUS_IO;
}

int zu_zip_run(ZContext* ctx) {
    if (!ctx) {
        return ZU_STATUS_USAGE;
    }

    // If fixing, proceed to modify (which will recover)
    if (ctx->fix_archive || ctx->fix_fix_archive) {
        return zu_modify_archive(ctx);
    }

    // If testing and no inputs, run test
    if (ctx->test_integrity && ctx->include.len == 0) {
        if (ctx->test_command) {
            return run_test_command(ctx, ctx->archive_path);
        }
        else {
            int test_rc = zu_test_archive(ctx);
            if (test_rc == ZU_STATUS_OK && !ctx->quiet) {
                printf("test of %s OK\n", ctx->archive_path);
            }
            return test_rc;
        }
    }

    // Allow comment-only updates via -z even when no inputs are provided.
    if (ctx->include.len == 0 && !ctx->zip_comment_specified) {
        fprintf(stderr, "zip: nothing to do\n");
        return ZU_STATUS_USAGE;
    }

    int rc = zu_modify_archive(ctx);
    if (rc == ZU_STATUS_OK && ctx->test_integrity) {
        const char* target = ctx->output_path ? ctx->output_path : ctx->archive_path;
        if (ctx->test_command) {
            rc = run_test_command(ctx, target);
        }
        else {
            const char* old_path = ctx->archive_path;
            ctx->archive_path = target;
            int test_rc = zu_test_archive(ctx);
            ctx->archive_path = old_path;
            if (test_rc == ZU_STATUS_OK && !ctx->quiet) {
                printf("test of %s OK\n", target);
            }
            rc = test_rc;
        }
    }
    return rc;
}

int zu_unzip_run(ZContext* ctx) {
    if (ctx->list_only) {
        return zu_list_archive(ctx);
    }
    if (ctx->test_integrity) {
        return zu_test_archive(ctx);
    }
    return zu_extract_archive(ctx);
}
