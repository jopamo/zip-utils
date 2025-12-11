#include "ops.h"

#include <stdio.h>

#include "reader.h"
#include "writer.h"
#include "ziputils.h"

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
        return zu_test_archive(ctx);
    }

    // Allow comment-only updates via -z even when no inputs are provided.
    if (ctx->include.len == 0 && !ctx->zip_comment_specified) {
        fprintf(stderr, "zip: nothing to do\n");
        return ZU_STATUS_USAGE;
    }

    int rc = zu_modify_archive(ctx);
    if (rc == ZU_STATUS_OK && ctx->test_integrity) {
        // Test after write
        // We need to test the OUTPUT file.
        // If output_path was set, use that. Else archive_path.
        const char* target = ctx->output_path ? ctx->output_path : ctx->archive_path;

        // We need a new context or reuse current?
        // Reuse current but swap path?
        // It's cleaner to create a new context or just point a temp context.
        // For simplicity, let's just call zu_test_archive with modified path in ctx?
        // But ctx has other state.
        // Let's rely on user verifying manually for now, or just print "Verification not implemented in this flow".
        // Or better, let's verify.

        // HACK: swap archive_path
        const char* old_path = ctx->archive_path;
        ctx->archive_path = target;
        rc = zu_test_archive(ctx);
        ctx->archive_path = old_path;
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
