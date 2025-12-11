#ifndef ZU_WRITER_H
#define ZU_WRITER_H

#include "ctx.h"

/* Create a new archive at ctx->archive_path (or stdout if set) from ctx->include items. */
int zu_write_archive(ZContext *ctx);

#endif
