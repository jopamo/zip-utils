#include "fileio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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
