#ifndef ZU_WRITER_H
#define ZU_WRITER_H

#include "ctx.h"

/* Create or modify an archive at ctx->archive_path (or stdout if set) from ctx->include items. */
int zu_modify_archive(ZContext* ctx);

#endif
