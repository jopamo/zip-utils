#include "ziputils.h"
#include "ctx.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

const char* zu_status_str(int code) {
    switch (code) {
        case ZU_STATUS_OK:
            return "ok";
        case ZU_STATUS_IO:
            return "i/o error";
        case ZU_STATUS_OOM:
            return "out of memory";
        case ZU_STATUS_USAGE:
            return "invalid usage";
        case ZU_STATUS_NOT_IMPLEMENTED:
            return "not implemented";
        case ZU_STATUS_PASSWORD_REQUIRED:
            return "password required";
        case ZU_STATUS_BAD_PASSWORD:
            return "bad password";
        default:
            return "unknown";
    }
}

void zu_log(ZContext* ctx, const char* fmt, ...) {
    if (!ctx || !fmt)
        return;

    va_list args;

    if (ctx->log_file) {
        va_start(args, fmt);
        vfprintf(ctx->log_file, fmt, args);
        va_end(args);
        // Ensure it's written immediately for log files
        fflush(ctx->log_file);
    }

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
