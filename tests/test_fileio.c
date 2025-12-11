#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctx.h"
#include "fileio.h"
#include "ziputils.h"

static int write_file(const char *path, const char *payload) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t len = strlen(payload);
    if (fwrite(payload, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static bool read_match(FILE *fp, const char *expected) {
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    return n == strlen(expected) && memcmp(buf, expected, n) == 0;
}

int main(void) {
    ZContext *ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "ctx alloc failed\n");
        return 1;
    }

    /* Nonexistent input should set an IO error and leave files NULL. */
    const char *missing = "/tmp/zu_fileio_missing_does_not_exist";
    int rc = zu_open_input(ctx, missing);
    if (rc == ZU_STATUS_OK || ctx->in_file != NULL || ctx->last_error != ZU_STATUS_IO ||
        ctx->error_msg[0] == '\0') {
        fprintf(stderr, "expected failure opening missing file\n");
        zu_context_free(ctx);
        return 1;
    }

    /* Create a real file and read it back through the helper. */
    char read_template[] = "/tmp/zu_fileio_readXXXXXX";
    int read_fd = mkstemp(read_template);
    if (read_fd < 0) {
        perror("mkstemp");
        zu_context_free(ctx);
        return 1;
    }
    close(read_fd);

    const char *payload = "abc123";
    if (write_file(read_template, payload) != 0) {
        fprintf(stderr, "failed writing payload\n");
        unlink(read_template);
        zu_context_free(ctx);
        return 1;
    }

    rc = zu_open_input(ctx, read_template);
    if (rc != ZU_STATUS_OK || ctx->in_file == NULL) {
        fprintf(stderr, "failed to open input file via helper\n");
        unlink(read_template);
        zu_context_free(ctx);
        return 1;
    }
    if (!read_match(ctx->in_file, payload)) {
        fprintf(stderr, "read payload mismatch\n");
        unlink(read_template);
        zu_context_free(ctx);
        return 1;
    }

    /* Opening output should create/truncate and be writable. */
    char write_template[] = "/tmp/zu_fileio_writeXXXXXX";
    int write_fd = mkstemp(write_template);
    if (write_fd < 0) {
        perror("mkstemp");
        unlink(read_template);
        zu_context_free(ctx);
        return 1;
    }
    close(write_fd);

    rc = zu_open_output(ctx, write_template, "wb");
    if (rc != ZU_STATUS_OK || ctx->out_file == NULL) {
        fprintf(stderr, "failed to open output file via helper\n");
        unlink(read_template);
        unlink(write_template);
        zu_context_free(ctx);
        return 1;
    }
    if (fwrite(payload, 1, strlen(payload), ctx->out_file) != strlen(payload)) {
        fprintf(stderr, "write via helper failed\n");
        unlink(read_template);
        unlink(write_template);
        zu_context_free(ctx);
        return 1;
    }
    zu_close_files(ctx);

    FILE *verify = fopen(write_template, "rb");
    if (!verify || !read_match(verify, payload)) {
        fprintf(stderr, "verify read failed\n");
        if (verify) {
            fclose(verify);
        }
        unlink(read_template);
        unlink(write_template);
        zu_context_free(ctx);
        return 1;
    }
    fclose(verify);

    unlink(read_template);
    unlink(write_template);
    zu_context_free(ctx);
    return 0;
}
