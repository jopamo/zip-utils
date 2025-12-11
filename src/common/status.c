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
    if (!ctx)
        return;

    // Determine if we should log based on context settings
    // Default to logging if verbose, or if it's an info message and -li is set?
    // The caller should control "info" level vs "error" level?
    // For now, this is a generic log function.

    va_list args;

    if (ctx->log_file) {
        va_start(args, fmt);
        vfprintf(ctx->log_file, fmt, args);
        va_end(args);
        // Ensure it's written immediately for log files
        fflush(ctx->log_file);
    }

    // Also print to stdout if verbose is enabled, to mirror zip behavior
    // (zip -v outputs details).
    // However, the original code prints to stdout/stderr directly in some places.
    // We should be careful not to double print.
    // For this task, we enable logging to file.
}