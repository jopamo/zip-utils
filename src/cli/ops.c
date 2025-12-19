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

/*
 * Execution dispatcher for zip and unzip front-ends
 *
 * This file owns the high-level operation selection for both commands
 * - zip path: fix, test-only, comment-only validation, modify/create, optional post-write test
 * - unzip path: list, test, extract
 *
 * It intentionally does not implement archive format details
 * - ZIP parsing and validation live in reader/test helpers
 * - Writing and mutation live in writer/modify helpers
 *
 * Error reporting model
 * - Operations return ZU_STATUS_* codes
 * - CLI layers map those to exit codes and decide how to format messages
 * - This file prints only user-facing text that is intrinsic to the operation flow
 */

/* --- Helpers --- */

/*
 * Build a shell command for archive testing based on a template and a target path
 *
 * Template rules
 * - If the template contains the first occurrence of "{}", that substring is replaced by target
 * - If the template does not contain "{}", target is appended as a separate argument
 *
 * Memory and ownership
 * - Returns a malloc-allocated string owned by the caller
 * - Returns NULL on OOM or if templ/target are NULL
 *
 * Safety notes
 * - This uses a shell command string, so quoting is the caller's responsibility
 * - The template is treated as a trusted configuration value, not user input
 */
static char* build_test_command(const char* templ, const char* target) {
    if (!templ || !target)
        return NULL;

    const char* placeholder = strstr(templ, "{}");
    char* out = NULL;
    size_t out_size = 0;

    if (!placeholder) {
        // No placeholder present, append target with a separating space
        out_size = strlen(templ) + 1 + strlen(target) + 1;
        out = malloc(out_size);
        if (!out)
            return NULL;

        snprintf(out, out_size, "%s %s", templ, target);
        return out;
    }

    // Replace only the first "{}" occurrence
    size_t prefix_len = (size_t)(placeholder - templ);
    size_t suffix_len = strlen(placeholder + 2);
    size_t target_len = strlen(target);

    out_size = prefix_len + target_len + suffix_len + 1;
    out = malloc(out_size);
    if (!out)
        return NULL;

    memcpy(out, templ, prefix_len);
    memcpy(out + prefix_len, target, target_len);
    memcpy(out + prefix_len + target_len, placeholder + 2, suffix_len + 1);

    return out;
}

/*
 * Run the configured test command against a target archive path
 *
 * Behavior
 * - Builds the shell command from ctx->test_command and the target path
 * - Prints progress messages unless quiet
 * - Uses system() for compatibility with complex user command templates
 *
 * Return codes
 * - ZU_STATUS_OK if the command exits successfully (exit code 0)
 * - ZU_STATUS_OOM if command string allocation fails
 * - ZU_STATUS_IO for system() failures or non-zero command outcomes
 *
 * Output expectations
 * - This uses "zip:" prefixes to match the invoking tool's UX expectations
 * - The caller chooses when this is invoked (pure test mode vs post-write test)
 */
static int run_test_command(ZContext* ctx, const char* target) {
    char* cmd = build_test_command(ctx->test_command, target);
    if (!cmd)
        return ZU_STATUS_OOM;

    if (!ctx->quiet) {
        printf("Testing archive: %s\n", target);
        if (ctx->verbose)
            printf("Executing: %s\n", cmd);
    }

    int status = system(cmd);
    free(cmd);

    // system() failed to launch a shell or fork/exec internally
    if (status == -1) {
        perror("zip: system() failed");
        return ZU_STATUS_IO;
    }

    // Success is defined as a normal exit with status 0
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (!ctx->quiet)
            printf("Test of %s OK\n", target);
        return ZU_STATUS_OK;
    }

    // Emit a concise failure reason for easier debugging
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

    /*
     * zip operation dispatch order
     *
     * 1) Fix modes (-F / -FF)
     *    - Force archive recovery logic in the writer path
     *    - Treated as a specialized modification run
     *
     * 2) Test-only mode (-T with no file operands)
     *    - Validates an existing archive without writing anything
     *    - Uses an external test command if configured, otherwise internal verifier
     *
     * 3) Input validation for modification modes
     *    - Without operands, zip has nothing to add/remove unless comment input is active
     *    - zip -z archive.zip is handled by the CLI layer by setting zip_comment_specified
     *
     * 4) Modify or create archive
     *    - Central directory update and entry writing handled by zu_modify_archive
     *
     * 5) Optional post-write test (-T with operands)
     *    - Verifies the resulting archive (output path if specified, else archive path)
     */

    // 1) Archive recovery / fix mode
    if (ctx->fix_archive || ctx->fix_fix_archive) {
        return zu_modify_archive(ctx);
    }

    // 2) Pure test mode: -T with no file operands means "verify only"
    if (ctx->test_integrity && ctx->include.len == 0) {
        if (ctx->test_command) {
            return run_test_command(ctx, ctx->archive_path);
        }

        int rc = zu_test_archive(ctx);
        if (rc == ZU_STATUS_OK && !ctx->quiet) {
            printf("No errors detected in compressed data of %s.\n", ctx->archive_path);
        }
        return rc;
    }

    // 3) Modification requires either file operands or an explicit comment read
    if (ctx->include.len == 0 && !ctx->zip_comment_specified) {
        if (ctx->stdin_names_read) {
            fprintf(stderr, "zip: error: no input files specified\n");
            return ZU_STATUS_USAGE;
        }
        printf("zip error: Nothing to do! (%s)\n", ctx->archive_path ? ctx->archive_path : "");
        return ZU_STATUS_NO_FILES;
    }

    // 4) Create/modify archive
    int rc = zu_modify_archive(ctx);

    // 5) Post-write testing when -T was requested for a write run
    if (rc == ZU_STATUS_OK && ctx->test_integrity) {
        const char* target = ctx->output_path ? ctx->output_path : ctx->archive_path;

        if (ctx->test_command) {
            rc = run_test_command(ctx, target);
        }
        else {
            /*
             * Internal verifier reads ctx->archive_path, so temporarily repoint it
             * The writer already produced target, this keeps verifier logic unchanged
             */
            const char* saved_path = ctx->archive_path;
            ctx->archive_path = target;

            rc = zu_test_archive(ctx);

            ctx->archive_path = saved_path;

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

    /*
     * unzip operation dispatch
     *
     * The CLI layer sets one of these modes via ctx flags
     * - list_only: list archive contents
     * - test_integrity: verify compressed data without extracting
     * - default: extract matching entries
     */

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
