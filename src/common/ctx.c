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
    ctx->compression_method = 8;
    ctx->store_paths = true;
    ctx->match_case = true;
    ctx->last_error = ZU_STATUS_OK;
    ctx->io_buffer_size = 0;
    ctx->io_buffer = NULL;
    ctx->quiet_level = 0;
    ctx->zipnote_mode = false;
    ctx->zipnote_write = false;
    ctx->existing_loaded = false;
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
    ctx->zip_comment_specified = false;

    ctx->output_path = NULL;
    ctx->log_path = NULL;
    ctx->log_file = NULL;
    ctx->log_append = false;
    ctx->log_info = false;
    ctx->has_filter_after = false;
    ctx->has_filter_before = false;
    ctx->line_mode = ZU_LINE_NONE;
    ctx->no_dir_entries = false;
    ctx->exclude_extra_attrs = false;
    ctx->store_symlinks = false;
    ctx->set_archive_mtime = false;
    ctx->newest_mtime_valid = false;

    zu_strlist_init(&ctx->include);
    zu_strlist_init(&ctx->include_patterns);
    zu_strlist_init(&ctx->exclude);
    zu_strlist_init(&ctx->existing_entries);
    zu_strlist_init(&ctx->no_compress_suffixes);

    return ctx;
}

static void zu_existing_entry_free(void* data) {
    zu_existing_entry* entry = (zu_existing_entry*)data;
    if (entry) {
        free(entry->name);
        free(entry->extra);
        free(entry->comment);
        free(entry);
    }
}

void zu_context_free(ZContext* ctx) {
    if (!ctx) {
        return;
    }

    zu_close_files(ctx);
    if (ctx->log_file) {
        fclose(ctx->log_file);
        ctx->log_file = NULL;
    }
    free(ctx->log_path);

    zu_strlist_free(&ctx->include);
    zu_strlist_free(&ctx->include_patterns);
    zu_strlist_free(&ctx->exclude);
    zu_strlist_free(&ctx->no_compress_suffixes);
    zu_strlist_free_with_dtor(&ctx->existing_entries, zu_existing_entry_free);
    free(ctx->io_buffer);
    free(ctx->zip_comment);
    free(ctx->temp_read_path);
    free(ctx->temp_dir);
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
