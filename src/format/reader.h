#ifndef ZU_FORMAT_READER_H
#define ZU_FORMAT_READER_H

#include "ctx.h"

int zu_list_archive(ZContext* ctx);
int zu_test_archive(ZContext* ctx);
int zu_extract_archive(ZContext* ctx);

/* Loads the central directory of the archive specified in ctx->archive_path
 * and populates ctx->existing_entries. */
int zu_load_central_directory(ZContext* ctx);

#endif