#ifndef ZU_CTX_H
#define ZU_CTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "strlist.h"
#include "ziputils.h"
#include "zip_headers.h"

enum {
    ZU_ZI_FMT_SHORT = 0,
    ZU_ZI_FMT_MEDIUM,
    ZU_ZI_FMT_LONG,
    ZU_ZI_FMT_VERBOSE,
    ZU_ZI_FMT_NAMES,
};

// Represents an entry found in an existing archive's central directory
typedef struct {
    zu_central_header hdr;
    char* name;
    unsigned char* extra;
    uint16_t extra_len;
    char* comment;
    uint16_t comment_len;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t lho_offset;
    bool delete;  /* Marked for deletion */
    bool changed; /* Is new or modified version */
} zu_existing_entry;

struct ZContext {
    /* I/O */
    FILE* in_file;
    FILE* out_file;
    uint64_t current_offset;
    uint8_t* io_buffer;
    size_t io_buffer_size;
    char* temp_read_path;    /* Concatenated split archive path */
    bool temp_read_is_split; /* Whether temp_read_path should be unlinked */

    /* Configuration */
    int compression_level;  /* 0-9 */
    int compression_method; /* 8=deflate, 12=bzip2 */
    bool recursive;
    bool store_paths;
    bool remove_source;
    bool test_integrity;
    bool quiet;
    int quiet_level;
    bool verbose;
    bool zipnote_mode;
    bool zipnote_write;
    bool existing_loaded;
    bool difference_mode;
    bool freshen;
    bool update;
    bool filesync; /* -FS */
    bool output_to_stdout;
    bool list_only;
    bool overwrite;
    bool match_case;
    bool allow_symlinks;
    bool allow_fifo;
    bool zipinfo_mode;
    bool zi_header;
    bool zi_footer;
    bool zi_list_entries;
    bool zi_decimal_time;
    bool zi_format_specified;
    bool zi_header_explicit;
    bool zi_footer_explicit;
    bool zi_allow_pager;
    bool zi_show_comments;
    int zi_format; /* enum-like selector for zipinfo output style */
    char* zip_comment;
    size_t zip_comment_len;
    bool zip_comment_specified;
    const char* archive_path;
    const char* target_dir;
    ZU_StrList include;
    ZU_StrList include_patterns;
    ZU_StrList exclude;

    // Modification specific flags
    bool modify_archive;
    ZU_StrList existing_entries;  // List of zu_existing_entry
    bool sort_entries;            // Whether to sort entries in the central directory

    /* Output/Logging */
    char* temp_dir;          /* -b */
    const char* output_path; /* -O */
    char* log_path;          /* -lf */
    bool log_append;         /* -la */
    bool log_info;           /* -li */
    FILE* log_file;          /* Handle for log file */

    /* Filtering */
    time_t filter_after; /* -t */
    bool has_filter_after;
    time_t filter_before; /* -tt */
    bool has_filter_before;
    enum {
        ZU_LINE_NONE = 0,
        ZU_LINE_LF_TO_CRLF,
        ZU_LINE_CRLF_TO_LF,
    } line_mode;                     /* -l / -ll */
    ZU_StrList no_compress_suffixes; /* -n */

    /* Encryption */
    bool encrypt;
    char* password;

    /* Split Archives */
    uint64_t split_size; /* 0 = disabled */
    bool split_pause;
    uint32_t split_disk_index;
    uint64_t split_written;
    char* temp_write_path;

    /* Archive Fixing */
    bool fix_archive;     /* -F */
    bool fix_fix_archive; /* -FF */

    /* Error reporting */
    int last_error;
    char error_msg[256];
};

ZContext* zu_context_create(void);
void zu_context_free(ZContext* ctx);
void zu_context_set_error(ZContext* ctx, int status, const char* msg);
void zu_log(ZContext* ctx, const char* fmt, ...);

#endif
