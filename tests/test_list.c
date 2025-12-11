#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctx.h"
#include "reader.h"
#include "ziputils.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s archive.zip\n", argv[0]);
        return 1;
    }

    ZContext* ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    ctx->archive_path = argv[1];
    ctx->list_only = true;
    ctx->quiet = false;

    int rc = zu_list_archive(ctx);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "list failed: %s\n", ctx->error_msg);
        zu_context_free(ctx);
        return 1;
    }

    zu_context_free(ctx);
    return 0;
}
