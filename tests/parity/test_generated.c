#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    const char* name;
    const char* tool_env;
    const char* args;
    int expected_rc;
    const char* expected_stdout;
    const char* expected_stderr;
} TestCase;

static TestCase tests[] = {
    {
        .name = "01-version-check_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-v",
        .expected_rc = 0,
        .expected_stdout = "Zip 3.0 (zip-utils rewrite; Info-ZIP compatibility work in progress)\n",
        .expected_stderr = "",
    },
    {
        .name = "02-bare-invocation_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "",
        .expected_rc = 0,
        .expected_stdout = "<binary output 231 bytes>",
        .expected_stderr = "  adding: - (stored 0%)\n",
    },
    {
        .name = "03-stdin-names_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-@ out.zip",
        .expected_rc = 16,
        .expected_stdout = "",
        .expected_stderr = "zip: error: no input files specified\n",
    },
    {
        .name = "04-stream-stdin-to-file_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "streamed.zip -",
        .expected_rc = 0,
        .expected_stdout = "  adding: - (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "05-stream-stdin-to-stdout_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "- -",
        .expected_rc = 0,
        .expected_stdout = "<binary output 231 bytes>",
        .expected_stderr = "  adding: - (stored 0%)\n",
    },
    {
        .name = "06-stdin-conflict_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-@ - -",
        .expected_rc = 0,
        .expected_stdout = "PK\003\004-\000\000\000\000\000\177775&\177775[\000\000\000\000\177775\177775\177775\177775\177775\177775\177775\177775\001\000\024\000-"
                           "\001\000\020\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000PK\001\002\036\003-\000\000\000\000\000\177775&\177775["
                           "\000\000\000\000\177775\177775\177775\177775\177775\177775\177775\177775\001\000\034\000\000\000\000\000\001\000\000\000\177775\021\177775\177775\177775\177775-"
                           "\001\000\030\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000PK\006\006,\000\000\000\000\000\000\000\036\003-"
                           "\000\000\000\000\000\000\000\000\000\001\000\000\000\000\000\000\000\001\000\000\000\000\000\000\000K\000\000\000\000\000\000\0003\000\000\000\000\000\000\000PK\006\007"
                           "\000\000\000\000~\000\000\000\000\000\000\000\001\000\000\000PK\005\006\000\000\000\000\001\000\001\000K\000\000\0003\000\000\000\000\000",
        .expected_stderr = "  adding: - (stored 0%)\n",
    },
    {
        .name = "07-syntax-variations_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-rq -b. -n.txt out.zip .",
        .expected_rc = 0,
        .expected_stdout = "",
        .expected_stderr = "",
    },
    {
        .name = "08-arg-separator_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip -- -dash.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: -dash.txt (deflated -7%)\n",
        .expected_stderr = "",
    },
    {
        .name = "09-basic-modes_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "10-update-newer_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "10-update-newer_cmd1",
        .tool_env = "ZIP_BIN",
        .args = "-u out.zip a.txt",
        .expected_rc = 12,
        .expected_stdout = "",
        .expected_stderr = "",
    },
    {
        .name = "11-freshen_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "11-freshen_cmd1",
        .tool_env = "ZIP_BIN",
        .args = "-f out.zip a.txt b.bin",
        .expected_rc = 12,
        .expected_stdout = "",
        .expected_stderr = "",
    },
    {
        .name = "12-filesync_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "13-delete-entry_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt dir/c.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n  adding: dir/c.txt (deflated -28%)\n",
        .expected_stderr = "",
    },
    {
        .name = "13-delete-entry_cmd1",
        .tool_env = "ZIP_BIN",
        .args = "-d out.zip 'dir/*'",
        .expected_rc = 0,
        .expected_stdout = "deleting: dir/c.txt\n",
        .expected_stderr = "",
    },
    {
        .name = "14-delete-filtered_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "15-copy-out_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "source.zip a.txt b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "16-recurse-standard_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-r out.zip dir",
        .expected_rc = 0,
        .expected_stdout = "  adding: dir/ (stored 0%)\n  adding: dir/sub/ (stored 0%)\n  adding: dir/sub/d.txt (deflated -28%)\n  adding: dir/deep/ (stored 0%)\n  adding: dir/deep/e.txt (deflated "
                           "-16%)\n  adding: dir/c.txt (deflated -28%)\n",
        .expected_stderr = "",
    },
    {
        .name = "17-recurse-patterns_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-R out.zip '*.txt'",
        .expected_rc = 0,
        .expected_stdout = "  adding: dir/sub/d.txt (deflated -28%)\n  adding: dir/deep/e.txt (deflated -16%)\n  adding: dir/c.txt (deflated -28%)\n  adding: a.txt (deflated -16%)\n  adding: "
                           "crlf.txt (deflated -19%)\n  adding: -dash.txt (deflated -7%)\n  adding: pat_a1.txt (deflated -39%)\n  adding: pat_b1.txt (deflated -24%)\n",
        .expected_stderr = "",
    },
    {
        .name = "18-junk-paths_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-j out.zip dir/c.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: c.txt (deflated -28%)\n",
        .expected_stderr = "",
    },
    {
        .name = "19-no-dir-entries_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-r -D out.zip dir",
        .expected_rc = 0,
        .expected_stdout = "  adding: dir/sub/d.txt (deflated -28%)\n  adding: dir/deep/e.txt (deflated -16%)\n  adding: dir/c.txt (deflated -28%)\n",
        .expected_stderr = "",
    },
    {
        .name = "20-symlinks-store_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-y out.zip link",
        .expected_rc = 0,
        .expected_stdout = "  adding: link (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "21-include-filter_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-r out.zip . -i '*.txt'",
        .expected_rc = 0,
        .expected_stdout = "  adding: dir/sub/d.txt (deflated -28%)\n  adding: dir/deep/e.txt (deflated -16%)\n  adding: dir/c.txt (deflated -28%)\n  adding: a.txt (deflated -16%)\n  adding: "
                           "crlf.txt (deflated -19%)\n  adding: -dash.txt (deflated -7%)\n  adding: pat_a1.txt (deflated -39%)\n  adding: pat_b1.txt (deflated -24%)\n",
        .expected_stderr = "",
    },
    {
        .name = "22-exclude-filter_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-r out.zip . -x '*.bin' '*.dat'",
        .expected_rc = 0,
        .expected_stdout = "  adding: dir/ (stored 0%)\n  adding: dir/sub/ (stored 0%)\n  adding: dir/sub/d.txt (deflated -28%)\n  adding: dir/deep/ (stored 0%)\n  adding: dir/deep/e.txt (deflated "
                           "-16%)\n  adding: dir/c.txt (deflated -28%)\n  adding: a.txt (deflated -16%)\n  adding: crlf.txt (deflated -19%)\n  adding: script.log (deflated -24%)\n  adding: -dash.txt "
                           "(deflated -7%)\n  adding: pat_a1.txt (deflated -39%)\n  adding: pat_b1.txt (deflated -24%)\n  adding: link (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "23-pattern-brackets_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip 'pat_[a-z]1.txt'",
        .expected_rc = 12,
        .expected_stdout = "\nzip error: Nothing to do! (out.zip)\n",
        .expected_stderr = "zip error: stat 'pat_[a-z]1.txt': No such file or directory\n",
    },
    {
        .name = "24-time-filter-after_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-t 2020-01-01 out.zip a.txt b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "25-time-filter-before_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-tt 2015-01-01 out.zip a.txt b.bin",
        .expected_rc = 12,
        .expected_stdout = "\nzip error: Nothing to do! (out.zip)\n",
        .expected_stderr = "",
    },
    {
        .name = "26-compression-store_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-0 out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "27-compression-max_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-9 out.zip b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "28-compression-method_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-Z store out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "29-suffixes_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-n .txt:.dat out.zip a.txt data.dat b.bin",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (stored 0%)\n  adding: data.dat (stored 0%)\n  adding: b.bin (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "30-text-lf-crlf_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-l out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "31-text-crlf-lf_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-ll out.zip crlf.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: crlf.txt (stored 0%)\n",
        .expected_stderr = "",
    },
    {
        .name = "32-quiet-mode_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-q out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "",
        .expected_stderr = "",
    },
    {
        .name = "33-temp-path_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-b . out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "34-test-integrity_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "34-test-integrity_cmd1",
        .tool_env = "ZIP_BIN",
        .args = "-T out.zip",
        .expected_rc = 0,
        .expected_stdout = "No errors detected in compressed data of out.zip.\n",
        .expected_stderr = "",
    },
    {
        .name = "35-test-custom_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "<binary output 32 bytes>",
        .expected_stderr = "",
    },
    {
        .name = "35-test-custom_cmd1",
        .tool_env = "ZIP_BIN",
        .args = "-T -TT 'ls -l {}' out.zip",
        .expected_rc = 0,
        .expected_stdout = "<binary output 90 bytes>",
        .expected_stderr = "",
    },
    {
        .name = "36-set-mtime_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-o out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "37-strip-extra_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-X out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -16%)\n",
        .expected_stderr = "",
    },
    {
        .name = "38-move-files_cmd0",
        .tool_env = "ZIP_BIN",
        .args = "-m out.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "  adding: a.txt (deflated -21%)\n",
        .expected_stderr = "",
    },
    {
        .name = "01-version-check_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-v",
        .expected_rc = 0,
        .expected_stdout = "UnZip 6.00 (zip-utils rewrite; Info-ZIP compatibility work in progress)\n",
        .expected_stderr = "",
    },
    {
        .name = "02-mode-list_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-l test.zip",
        .expected_rc = 0,
        .expected_stdout = "a.txt\ndir/b.txt\ndir/sub/c.dat\nskip_me.log\n",
        .expected_stderr = "",
    },
    {
        .name = "03-mode-test_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-t test.zip",
        .expected_rc = 0,
        .expected_stdout = "No errors detected in compressed data of test.zip.\n",
        .expected_stderr = "",
    },
    {
        .name = "04-mode-comment_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-z test.zip",
        .expected_rc = 0,
        .expected_stdout = "zipfile comment:\nThis is the archive comment\n",
        .expected_stderr = "",
    },
    {
        .name = "05-mode-pipe_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-p test.zip a.txt",
        .expected_rc = 0,
        .expected_stdout = "<binary output 29 bytes>",
        .expected_stderr = "",
    },
    {
        .name = "06-extract-default_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip",
        .expected_rc = 2,
        .expected_stdout = "  inflating: a.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "07-extract-select_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip a.txt",
        .expected_rc = 2,
        .expected_stdout = "  inflating: a.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "08-extract-exclude_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip -x 'dir/*'",
        .expected_rc = 2,
        .expected_stdout = "  inflating: a.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "09-wildcards_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip 'dir/?.*'",
        .expected_rc = 2,
        .expected_stdout = "  inflating: dir/b.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "10-brackets_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip 'dir/[b-z].txt'",
        .expected_rc = 2,
        .expected_stdout = "  inflating: dir/b.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "11-exdir-flag_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip -d outdir",
        .expected_rc = 0,
        .expected_stdout = "  inflating: a.txt\n  inflating: dir/b.txt\n  inflating: dir/sub/c.dat\n  inflating: skip_me.log\n",
        .expected_stderr = "",
    },
    {
        .name = "12-exdir-sticky_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip -doutdir",
        .expected_rc = 0,
        .expected_stdout = "  inflating: a.txt\n  inflating: dir/b.txt\n  inflating: dir/sub/c.dat\n  inflating: skip_me.log\n",
        .expected_stderr = "",
    },
    {
        .name = "13-junk-paths_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-j test.zip",
        .expected_rc = 2,
        .expected_stdout = "  inflating: a.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "14-overwrite-never_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-n test.zip",
        .expected_rc = 0,
        .expected_stdout = "  inflating: a.txt\n  inflating: dir/b.txt\n  inflating: dir/sub/c.dat\n  inflating: skip_me.log\n",
        .expected_stderr = "",
    },
    {
        .name = "15-overwrite-always_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-o test.zip",
        .expected_rc = 0,
        .expected_stdout = "  inflating: a.txt\n  inflating: dir/b.txt\n  inflating: dir/sub/c.dat\n  inflating: skip_me.log\n",
        .expected_stderr = "",
    },
    {
        .name = "16-arg-separator_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "test.zip -- a.txt",
        .expected_rc = 2,
        .expected_stdout = "  inflating: a.txt\n",
        .expected_stderr = "unzip error: file exists (non-interactive)\n",
    },
    {
        .name = "17-stdin-stream_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-",
        .expected_rc = 2,
        .expected_stdout = "",
        .expected_stderr = "unzip error: missing end of central directory\n",
    },
    {
        .name = "18-fail-C_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-C test.zip",
        .expected_rc = 10,
        .expected_stdout = "",
        .expected_stderr = "unzip error: option -C (case-insensitive) is not supported\n",
    },
    {
        .name = "19-fail-L_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-L test.zip",
        .expected_rc = 10,
        .expected_stdout = "",
        .expected_stderr = "unzip error: option -L (lowercase names) is not supported\n",
    },
    {
        .name = "20-fail-X_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-X test.zip",
        .expected_rc = 10,
        .expected_stdout = "",
        .expected_stderr = "unzip error: option -X (restore UID/GID) is not supported\n",
    },
    {
        .name = "01-version-check_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -v",
        .expected_rc = 0,
        .expected_stdout = "UnZip 6.00 (zip-utils rewrite; Info-ZIP compatibility work in progress)\n",
        .expected_stderr = "",
    },
    {
        .name = "02-short-list_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z test.zip",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n\n-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 a.txt\n-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 "
                           "dir/b.txt\n-rw------- 2.0 unx            16 b- defl 14-Dec-25 04:55 dir/sub/c.dat\n-rw------- 2.0 unx            17 t- defl 14-Dec-25 04:55 skip_me.log\n4 files, 53 bytes "
                           "uncompressed, 61 bytes compressed:  0.0%\n",
        .expected_stderr = "",
    },
    {
        .name = "03-names-only-quiet_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -1 test.zip",
        .expected_rc = 0,
        .expected_stdout = "a.txt\ndir/b.txt\ndir/sub/c.dat\nskip_me.log\n",
        .expected_stderr = "",
    },
    {
        .name = "04-names-with-header_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -2 test.zip",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n\na.txt\ndir/b.txt\ndir/sub/c.dat\nskip_me.log\n4 files, 53 bytes uncompressed, 61 bytes compressed:  0.0%\n",
        .expected_stderr = "",
    },
    {
        .name = "05-medium-list_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -m test.zip",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n\n-rw------- 2.0 unx            10 t-     0% defl 14-Dec-25 04:55 a.txt\n-rw------- 2.0 unx            10 t-     0% defl "
                           "14-Dec-25 04:55 dir/b.txt\n-rw------- 2.0 unx            16 b-     0% defl 14-Dec-25 04:55 dir/sub/c.dat\n-rw------- 2.0 unx            17 t-     0% defl 14-Dec-25 04:55 "
                           "skip_me.log\n4 files, 53 bytes uncompressed, 61 bytes compressed:  0.0%\n",
        .expected_stderr = "",
    },
    {
        .name = "06-verbose-list_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -v test.zip",
        .expected_rc = 0,
        .expected_stdout =
            "zipinfo option resolution:\n  -Z zipinfo mode\n  -v verbose\n  archive path set to test.zip\n  mode: list\n  target dir: (cwd)\n  overwrite: prompt\n  pattern match: include=0 exclude=0 "
            "case sensitive\n  zipinfo mode: on (format 3)\nArchive:  test.zip   490 bytes   4 files\n\n-rw------- 2.0 unx            10 t-         12 defl 14-Dec-25 04:55 a.txt\n    version needed: "
            "2.0  flags: 0x0000  method: 8  offset: 0\n    sizes: comp=12  uncomp=10  crc=2c5a6e75\n    extra fields: none\n\n-rw------- 2.0 unx            10 t-         12 defl 14-Dec-25 04:55 "
            "dir/b.txt\n    version needed: 2.0  flags: 0x0000  method: 8  offset: 47\n    sizes: comp=12  uncomp=10  crc=07773db6\n    extra fields: none\n\n-rw------- 2.0 unx            16 b-      "
            "   18 defl 14-Dec-25 04:55 dir/sub/c.dat\n    version needed: 2.0  flags: 0x0000  method: 8  offset: 98\n    sizes: comp=18  uncomp=16  crc=74ebe392\n    extra fields: "
            "none\n\n-rw------- 2.0 unx            17 t-         19 defl 14-Dec-25 04:55 skip_me.log\n    version needed: 2.0  flags: ",
        .expected_stderr = "",
    },
    {
        .name = "07-decimal-time_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -T test.zip",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n\n-rw------- 2.0 unx            10 t- defl 251214.045504 a.txt\n-rw------- 2.0 unx            10 t- defl 251214.045504 "
                           "dir/b.txt\n-rw------- 2.0 unx            16 b- defl 251214.045504 dir/sub/c.dat\n-rw------- 2.0 unx            17 t- defl 251214.045504 skip_me.log\n4 files, 53 bytes "
                           "uncompressed, 61 bytes compressed:  0.0%\n",
        .expected_stderr = "",
    },
    {
        .name = "08-show-comments_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -z test.zip",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n\n-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 a.txt\n-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 "
                           "dir/b.txt\n-rw------- 2.0 unx            16 b- defl 14-Dec-25 04:55 dir/sub/c.dat\n-rw------- 2.0 unx            17 t- defl 14-Dec-25 04:55 skip_me.log\n4 files, 53 bytes "
                           "uncompressed, 61 bytes compressed:  0.0%\n\nzipfile comment:\nThis is the archive comment\n",
        .expected_stderr = "",
    },
    {
        .name = "09-pattern-header-defaults_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z test.zip 'dir/*'",
        .expected_rc = 0,
        .expected_stdout = "-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 dir/b.txt\n-rw------- 2.0 unx            16 b- defl 14-Dec-25 04:55 dir/sub/c.dat\n",
        .expected_stderr = "",
    },
    {
        .name = "10-pattern-with-header_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -h test.zip 'dir/*'",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n",
        .expected_stderr = "",
    },
    {
        .name = "11-pager-stub_cmd0",
        .tool_env = "UNZIP_BIN",
        .args = "-Z -M test.zip",
        .expected_rc = 0,
        .expected_stdout = "Archive:  test.zip   490 bytes   4 files\n\n-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 a.txt\n-rw------- 2.0 unx            10 t- defl 14-Dec-25 04:55 "
                           "dir/b.txt\n-rw------- 2.0 unx            16 b- defl 14-Dec-25 04:55 dir/sub/c.dat\n-rw------- 2.0 unx            17 t- defl 14-Dec-25 04:55 skip_me.log\n4 files, 53 bytes "
                           "uncompressed, 61 bytes compressed:  0.0%\n",
        .expected_stderr = "",
    },
};

static const char* fallback_bin_for(const char* tool_env) {
    if (strcmp(tool_env, "UNZIP_BIN") == 0)
        return "./build/unzip";
    if (strcmp(tool_env, "ZIPINFO_BIN") == 0)
        return "./build/zipinfo";
    if (strcmp(tool_env, "ZIP_BIN") == 0)
        return "./build/zip";
    return "./build/tool";
}

int main(void) {
    int passed = 0;
    int failed = 0;
    char fixture_root[] = "/tmp/zu_parity_test_XXXXXX";
    if (!mkdtemp(fixture_root)) {
        perror("mkdtemp");
        return 1;
    }

    const char* zip_bin_for_fixture = getenv("ZIP_BIN");
    if (!zip_bin_for_fixture || zip_bin_for_fixture[0] == '\0')
        zip_bin_for_fixture = "./build/zip";

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        TestCase* t = &tests[i];
        printf("Running %s... ", t->name);
        fflush(stdout);

        create_fixture(fixture_root, zip_bin_for_fixture);

        const char* bin = getenv(t->tool_env);
        if (!bin || bin[0] == '\0')
            bin = fallback_bin_for(t->tool_env);

        char cmd[8192];
        if (t->args && t->args[0])
            snprintf(cmd, sizeof(cmd), "%s %s", bin, t->args);
        else
            snprintf(cmd, sizeof(cmd), "%s", bin);

        char* out = NULL;
        char* err = NULL;
        int rc = 0;
        run_command(fixture_root, cmd, &out, &err, &rc);

        if (!out)
            out = strdup("");
        if (!err)
            err = strdup("");

        bool ok = true;
        if (rc != t->expected_rc) {
            printf("\n  RC mismatch: expected %d, got %d\n", t->expected_rc, rc);
            ok = false;
        }

        if (t->expected_stdout[0] == '\0') {
            if (out[0] != '\0') {
                printf("\n  Stdout mismatch: expected empty, got %zu bytes\n", strlen(out));
                ok = false;
            }
        }
        else {
            if (out[0] == '\0') {
                printf("\n  Stdout mismatch: expected content, got empty\n");
                ok = false;
            }
        }

        if (t->expected_stderr[0] == '\0') {
            if (err[0] != '\0') {
                printf("\n  Stderr mismatch: expected empty, got %zu bytes\n", strlen(err));
                ok = false;
            }
        }
        else {
            if (err[0] == '\0') {
                printf("\n  Stderr mismatch: expected content, got empty\n");
                ok = false;
            }
        }

        free(out);
        free(err);
        cleanup_fixture(fixture_root);

        if (ok) {
            printf("PASS\n");
            passed++;
        }
        else {
            printf("FAIL\n");
            failed++;
        }
    }

    rmdir(fixture_root);
    printf("\nPassed: %d, Failed: %d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
