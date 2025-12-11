#ifndef ZU_ZIPUTILS_H
#define ZU_ZIPUTILS_H

/* Minimal status codes for the modern rewrite.  Keep values small for now. */
#define ZU_STATUS_OK 0
#define ZU_STATUS_IO 5
#define ZU_STATUS_OOM 12
#define ZU_STATUS_USAGE 64
#define ZU_STATUS_NOT_IMPLEMENTED 95

/* Public API surface for consumers of libziputils.  These are intentionally
 * small and focused; more will be added as functionality lands. */

typedef struct ZContext ZContext;

/* Allocate/free a reentrant context that holds options, file handles, and errors. */
ZContext *zu_context_create(void);
void zu_context_free(ZContext *ctx);
void zu_context_set_error(ZContext *ctx, int status, const char *msg);

/* Status helper. */
const char *zu_status_str(int code);

/* High-level operations. Return ZU_STATUS_* codes. */
int zu_list_archive(ZContext *ctx);
int zu_test_archive(ZContext *ctx);
int zu_extract_archive(ZContext *ctx);
int zu_write_archive(ZContext *ctx);

/* Context configuration helpers live in ctx.h; this header is the minimal
 * public surface to keep CLI and library consumers decoupled from internals. */

#endif
