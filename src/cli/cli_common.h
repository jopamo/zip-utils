#ifndef ZU_CLI_COMMON_H
#define ZU_CLI_COMMON_H

#include <stdio.h>
#include <stdbool.h>

#include "ctx.h"

typedef struct {
    const char* reset;
    const char* bold;
    const char* red;
    const char* green;
    const char* yellow;
    const char* cyan;
} ZuCliColors;

void zu_cli_init_terminal(void);
const ZuCliColors* zu_cli_colors(void);
bool zu_cli_name_matches(const char* argv0, const char* name);

void zu_cli_error(const char* tool, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void zu_cli_warn(const char* tool, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void zu_cli_print_opt(FILE* to, const char* flags, const char* desc);
void zu_cli_print_section(FILE* to, const char* title);
void zu_cli_emit_option_trace(const char* tool, ZContext* ctx);

#endif
