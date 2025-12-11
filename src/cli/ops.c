#include "ops.h"

#include <stdio.h>

#include "reader.h"
#include "writer.h"
#include "ziputils.h"

int zu_zip_run(ZContext* ctx) {
    if (!ctx) {
        return ZU_STATUS_USAGE;
    }
    return zu_modify_archive(ctx);
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
