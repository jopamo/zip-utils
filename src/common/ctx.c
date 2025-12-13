#include "ctx.h"
#include "fileio.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * ZContext lifecycle and shared state management
 *
 * ZContext is the central state carrier for the zip/unzip toolchain
 * - CLI layers populate it from argv
 * - Reader/writer/execution layers consume it to perform work
 * - It owns most dynamically allocated configuration strings and transient buffers
 *
 * Ownership rules
 * - Any heap pointer stored in ctx is owned by ctx and released by zu_context_free
 * - Strlists embedded in ctx own their element storage unless freed with a custom dtor
 * - Error and warning helpers in this file are best-effort and avoid failing the run
 *
 * Threading
 * - Not thread-safe, intended for single-process CLI execution
 */

ZContext* zu_context_create(void) {
    /*
     * Allocate a zeroed context so fields not explicitly initialized default to 0/NULL/false
     * This reduces the risk of leaving pointers uninitialized and simplifies cleanup paths
     */
    ZContext* ctx = calloc(1, sizeof(ZContext));
    if (!ctx)
        return NULL;

    /*
     * Default behavior settings
     * - Compression defaults mirror common zip behavior (deflate, mid-level)
     * - Path handling defaults to preserving directory structure
     * - Pattern matching defaults to case-sensitive
     */
    ctx->compression_level = 6;
    ctx->compression_method = 8;
    ctx->store_paths = true;
    ctx->recurse_from_cwd = false;
    ctx->match_case = true;

    /*
     * Error and I/O defaults
     * - last_error starts OK, callers update via zu_context_set_error or direct assignment
     * - io_buffer is optional and can be lazily allocated by I/O paths
     */
    ctx->last_error = ZU_STATUS_OK;
    ctx->io_buffer_size = 0;
    ctx->io_buffer = NULL;

    /*
     * Output and verbosity defaults
     * - quiet_level is stackable in CLI parsing
     * - log_* settings are configured by CLI options (-lf, -la, -li)
     */
    ctx->quiet_level = 0;

    /*
     * Mode toggles
     * - zipnote_mode enables zipnote-style list/apply flows
     * - zipinfo_mode enables zipinfo-style listing formatting in unzip entrypoints
     */
    ctx->zipnote_mode = false;
    ctx->zipnote_write = false;
    ctx->zipinfo_mode = false;

    /*
     * Symlink policy
     * - allow_symlinks controls whether symlinks are permitted as inputs
     * - store_symlinks controls whether symlinks are stored as links or followed
     *   Defaults are chosen to be safe and predictable for typical use
     */
    ctx->allow_symlinks = true;

    /*
     * Existing archive bookkeeping
     * - existing_loaded indicates whether central directory / entries were loaded
     */
    ctx->existing_loaded = false;

    /*
     * Zipinfo formatting defaults
     * - Short format with header/footer enabled by default
     * - *_explicit flags track whether the user forced header/footer behaviors
     */
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

    /*
     * Archive comment state
     * - zip_comment holds the active archive comment buffer
     * - zip_comment_specified indicates the user asked to set or replace it
     */
    ctx->zip_comment = NULL;
    ctx->zip_comment_len = 0;
    ctx->zip_comment_specified = false;

    /*
     * Misc option state used by zip execution
     */
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

    ctx->copy_mode = false;
    ctx->test_command = NULL;

    /*
     * Initialize embedded dynamic arrays
     * - include holds file operands and stdin markers
     * - include_patterns/exclude hold pattern filters
     * - existing_entries owns heap-allocated zu_existing_entry objects (freed via dtor)
     * - no_compress_suffixes stores suffix filters from -n
     * - warnings tracks emitted one-time warnings to deduplicate output
     * - option_events stores trace strings for diagnostics/logging
     */
    zu_strlist_init(&ctx->include);
    zu_strlist_init(&ctx->include_patterns);
    zu_strlist_init(&ctx->exclude);
    zu_strlist_init(&ctx->existing_entries);
    zu_strlist_init(&ctx->no_compress_suffixes);
    zu_strlist_init(&ctx->warnings);
    zu_strlist_init(&ctx->option_events);

    return ctx;
}

/*
 * Destructor for zu_existing_entry objects stored in ctx->existing_entries
 *
 * These are not plain strings, so freeing requires releasing each owned field first
 * This matches the allocation strategy used by central directory load and writer paths
 */
static void zu_existing_entry_free(void* data) {
    zu_existing_entry* entry = (zu_existing_entry*)data;
    if (!entry)
        return;

    free(entry->name);
    free(entry->extra);
    free(entry->comment);
    free(entry);
}

/*
 * Free a context and all owned allocations
 *
 * Order matters
 * - Close files first so buffers and paths remain valid during cleanup
 * - Close log file explicitly before freeing log_path so diagnostics remain possible
 * - Free strlists and owned buffers, then free ctx itself
 *
 * Safe to call with NULL
 */
void zu_context_free(ZContext* ctx) {
    if (!ctx)
        return;

    // Close any open archive streams and temporary files tracked by ctx
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
    zu_strlist_free(&ctx->warnings);
    zu_strlist_free(&ctx->option_events);

    zu_strlist_free_with_dtor(&ctx->existing_entries, zu_existing_entry_free);

    free(ctx->io_buffer);
    free(ctx->zip_comment);
    free(ctx->temp_dir);
    free(ctx->password);
    free(ctx->test_command);

    free(ctx);
}

/*
 * Record an error status and an optional human-readable message into the context
 *
 * Behavior
 * - Always stores the numeric status into last_error
 * - If msg is non-NULL, copies it into ctx->error_msg with truncation
 * - If msg is NULL, clears ctx->error_msg
 *
 * Notes
 * - This is intended for execution layers to leave a single final message for the CLI
 * - The caller decides whether status is fatal or recoverable
 */
void zu_context_set_error(ZContext* ctx, int status, const char* msg) {
    if (!ctx)
        return;

    ctx->last_error = status;

    if (msg) {
        strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
        ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
    }
    else {
        ctx->error_msg[0] = '\0';
    }
}

/*
 * Emit a warning at most once per process run
 *
 * Deduplication
 * - Keeps an in-memory set of exact warning strings in ctx->warnings
 * - If the same string is seen again, it is suppressed
 *
 * Output sinks
 * - Always prints to stderr
 * - If ctx->log_file is active, also writes there and flushes immediately
 *
 * Failure behavior
 * - If the warnings list cannot grow (OOM), this silently drops the warning
 *   This keeps the tool usable under memory pressure without cascading failures
 */
void zu_warn_once(ZContext* ctx, const char* msg) {
    if (!ctx || !msg)
        return;

    for (size_t i = 0; i < ctx->warnings.len; ++i) {
        if (strcmp(ctx->warnings.items[i], msg) == 0)
            return;
    }

    if (zu_strlist_push(&ctx->warnings, msg) != 0)
        return;

    fprintf(stderr, "%s\n", msg);

    if (ctx->log_file) {
        fprintf(ctx->log_file, "%s\n", msg);
        fflush(ctx->log_file);
    }
}

/*
 * Record a formatted option trace entry into ctx->option_events
 *
 * Purpose
 * - Captures parsing decisions and normalization steps as human-readable strings
 * - Used by CLI layers to emit a consolidated trace when verbose/log_info/dry_run is enabled
 *
 * Behavior
 * - Formats into a fixed-size stack buffer, truncating long strings
 * - Best-effort append to ctx->option_events
 * - If list growth fails, the trace entry is silently dropped
 */
void zu_trace_option(ZContext* ctx, const char* fmt, ...) {
    if (!ctx || !fmt)
        return;

    char buf[256];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    zu_strlist_push(&ctx->option_events, buf);
}
