#include "ziputils.h"
#include "ctx.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/*
 * Small shared utilities used across zip/unzip components
 *
 * This translation unit provides two primitives:
 * - zu_status_str: stable string names for internal ZU_STATUS_* codes
 * - zu_log: lightweight logging helper that can mirror output to an optional log file
 *
 * Design goals
 * - Keep status strings stable because they can appear in user-facing diagnostics
 * - Keep logging usable in error paths by avoiding allocations and complex dependencies
 * - Allow callers to direct logs to a file while still keeping interactive output visible
 */

/*
 * Convert an internal status code into a short human-readable label
 *
 * Contract
 * - Strings are static and must not be freed
 * - Unknown codes map to "unknown" rather than NULL to keep formatting safe
 *
 * Intended usage
 * - CLI layers may include these strings in error paths
 * - Logs and traces can record these values for debugging
 */
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

/*
 * Log a formatted message to the active sinks
 *
 * Sinks
 * - If ctx->log_file is set, also write the message to that file and flush immediately
 * - Always write the message to stdout via vprintf for interactive visibility
 *
 * Behavior notes
 * - This does not add prefixes, timestamps, or newlines
 *   Callers decide the exact formatting for both human output and log files
 * - Flushing the log file is intentional so crashes still leave useful traces
 * - The stdout stream is not force-flushed here, leaving buffering policy to the runtime
 */
void zu_log(ZContext* ctx, const char* fmt, ...) {
    if (!ctx || !fmt)
        return;

    va_list args;

    if (ctx->log_file) {
        va_start(args, fmt);
        vfprintf(ctx->log_file, fmt, args);
        va_end(args);
        fflush(ctx->log_file);
    }

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
