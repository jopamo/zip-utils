#include "ctx.h"
#include "fileio.h"

#include <stdlib.h>
#include <string.h>

ZContext* zu_context_create(void) {
    ZContext* ctx = calloc(1, sizeof(ZContext));
    if (!ctx) {
        return NULL;
    }

    ctx->compression_level = 6;
    ctx->store_paths = true;
    ctx->match_case = true;
    ctx->last_error = ZU_STATUS_OK;
    ctx->io_buffer_size = 0;
    ctx->io_buffer = NULL;
    ctx->zipinfo_mode = false;
    ctx->zi_format = ZU_ZI_FMT_SHORT;
    ctx->zi_header = true;
    ctx->zi_footer = true;
    ctx->zi_list_entries = true;
    ctx->zi_decimal_time = false;
    ctx->zi_format_specified = false;
    ctx->zi_header_explicit = false;
    ctx->zi_footer_explicit = false;
    ctx->zi_allow_pager = false;
    ctx->zi_show_comments = false;
    ctx->zip_comment = NULL;
    ctx->zip_comment_len = 0;

    zu_strlist_init(&ctx->include);
    zu_strlist_init(&ctx->exclude);
    zu_strlist_init(&ctx->existing_entries);

    return ctx;
}

static void zu_existing_entry_free(void* data) {
    zu_existing_entry* entry = (zu_existing_entry*)data;
    if (entry) {
        free(entry->name);
        free(entry->extra);
        free(entry->comment);
    }
}

void zu_context_free(ZContext* ctx) {
    if (!ctx) {
        return;
    }

    zu_close_files(ctx);
    zu_strlist_free(&ctx->include);
    zu_strlist_free(&ctx->exclude);
    zu_strlist_free_with_dtor(&ctx->existing_entries, zu_existing_entry_free);
    free(ctx->io_buffer);
    free(ctx->zip_comment);
    free(ctx->password);
    free(ctx);
}

void zu_context_set_error(ZContext* ctx, int status, const char* msg) {
    if (!ctx) {
        return;
    }
    ctx->last_error = status;
    if (msg) {
        strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
        ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
    }
    else {
        ctx->error_msg[0] = '\0';
    }
}
