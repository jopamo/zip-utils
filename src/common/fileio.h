#ifndef ZU_FILEIO_H
#define ZU_FILEIO_H

#include "ctx.h"

int zu_open_input(ZContext* ctx, const char* path);
int zu_open_output(ZContext* ctx, const char* path, const char* mode);
void zu_close_files(ZContext* ctx);
int zu_expand_args(ZContext* ctx);
bool zu_should_include(const ZContext* ctx, const char* name);

#endif
