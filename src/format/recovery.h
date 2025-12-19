#ifndef ZU_RECOVERY_H
#define ZU_RECOVERY_H

#include "ctx.h"

/*
 * Attempt to recover the central directory by scanning the archive.
 *
 * - Fills ctx->existing_entries with found entries.
 * - Used by -F and -FF modes.
 */
int zu_recover_central_directory(ZContext* ctx, bool full_scan);

#endif
