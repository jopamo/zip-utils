#define _POSIX_C_SOURCE 200809L

#include "cli_common.h"

#include <stdarg.h>
#include <string.h>
#include <unistd.h>

static bool g_use_color = false;

static const ZuCliColors kCliColorsEnabled = {.reset = "\033[0m", .bold = "\033[1m", .red = "\033[31m", .green = "\033[32m", .yellow = "\033[33m", .cyan = "\033[36m"};

static const ZuCliColors kCliColorsDisabled = {.reset = "", .bold = "", .red = "", .green = "", .yellow = "", .cyan = ""};

void zu_cli_init_terminal(void) {
    g_use_color = isatty(STDOUT_FILENO);
}

const ZuCliColors* zu_cli_colors(void) {
    return g_use_color ? &kCliColorsEnabled : &kCliColorsDisabled;
}

bool zu_cli_name_matches(const char* argv0, const char* name) {
    if (!argv0 || !name)
        return false;

    const char* base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
#ifdef _WIN32
    const char* backslash = strrchr(base, '\\');
    if (backslash)
        base = backslash + 1; /* tolerate mixed separators */
#endif
    return strcmp(base, name) == 0;
}

static void vmessage(FILE* to, const char* tool, const char* label, const char* color, const char* fmt, va_list args) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "%s%s %s:%s ", color, tool, label, c->reset);
    vfprintf(to, fmt, args);
    fprintf(to, "\n");
}

void zu_cli_error(const char* tool, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vmessage(stderr, tool, "error", zu_cli_colors()->red, fmt, args);
    va_end(args);
}

void zu_cli_warn(const char* tool, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vmessage(stderr, tool, "warning", zu_cli_colors()->yellow, fmt, args);
    va_end(args);
}

void zu_cli_print_opt(FILE* to, const char* flags, const char* desc) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "  %s%-24s%s %s\n", c->green, flags, c->reset, desc);
}

void zu_cli_print_section(FILE* to, const char* title) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "\n%s%s:%s\n", c->cyan, title, c->reset);
}

void zu_cli_emit_option_trace(const char* tool, ZContext* ctx) {
    if (!(ctx->verbose || ctx->log_info || ctx->dry_run))
        return;
    if (ctx->option_events.len == 0)
        return;

    zu_log(ctx, "%s option resolution:\n", tool);
    for (size_t i = 0; i < ctx->option_events.len; ++i) {
        zu_log(ctx, "  %s\n", ctx->option_events.items[i]);
    }
}
