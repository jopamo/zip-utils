#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

#include "ctx.h"
#include "fileio.h"
#include "writer.h"
#include "reader.h"
#include "ziputils.h"

static int write_file(const char* path, const char* payload) {
    FILE* fp = fopen(path, "wb");
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

static int read_file_content(const char* path, char* buf, size_t bufsz) {
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return -1;
    size_t n = fread(buf, 1, bufsz - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return 0;
}

static void cleanup_temp_dir(const char* dirpath) {
    // Remove files we created
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/a.txt", dirpath);
    unlink(path);
    snprintf(path, sizeof(path), "%s/b.bin", dirpath);
    unlink(path);
    snprintf(path, sizeof(path), "%s/test.zip", dirpath);
    unlink(path);
    snprintf(path, sizeof(path), "%s/extract/a.txt", dirpath);
    unlink(path);
    snprintf(path, sizeof(path), "%s/extract/b.bin", dirpath);
    unlink(path);
    snprintf(path, sizeof(path), "%s/extract", dirpath);
    rmdir(path);
    rmdir(dirpath);
}

static int test_basic_extract(void) {
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "ctx alloc failed\n");
        return 1;
    }

    // Create temporary directory
    char template[] = "/tmp/zu_reader_testXXXXXX";
    if (!mkdtemp(template)) {
        perror("mkdtemp");
        zu_context_free(ctx);
        return 1;
    }

    // Save original working directory
    char orig_cwd[PATH_MAX];
    if (!getcwd(orig_cwd, sizeof(orig_cwd))) {
        perror("getcwd");
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Change to temp directory
    if (chdir(template) != 0) {
        perror("chdir");
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Create test files with relative paths
    if (write_file("a.txt", "hello") != 0 || write_file("b.bin", "world") != 0) {
        fprintf(stderr, "failed to create test files\n");
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Create archive using writer
    ctx->archive_path = "test.zip";
    ctx->store_paths = true;
    ctx->quiet = true;
    if (zu_strlist_push(&ctx->include, "a.txt") != 0 || zu_strlist_push(&ctx->include, "b.bin") != 0) {
        fprintf(stderr, "failed to push to include list\n");
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }
    int rc = zu_modify_archive(ctx);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "zu_modify_archive failed: %d\n", rc);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Clear context for extraction (reuse same context but reset some fields)
    zu_strlist_free(&ctx->include);
    zu_strlist_free(&ctx->include_patterns);
    zu_strlist_free(&ctx->exclude);
    // Keep archive_path
    ctx->target_dir = "extract";                  // subdirectory to extract into
    ctx->overwrite_policy = ZU_OVERWRITE_ALWAYS;  // allow overwrite
    ctx->quiet = true;

    // Create extraction directory
    mkdir("extract", 0755);

    // Extract archive
    rc = zu_extract_archive(ctx);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "zu_extract_archive failed: %d\n", rc);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Verify extracted files exist and contain correct content
    char content[16];
    if (read_file_content("extract/a.txt", content, sizeof(content)) != 0) {
        fprintf(stderr, "failed to read extracted a.txt\n");
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }
    if (strcmp(content, "hello") != 0) {
        fprintf(stderr, "extracted a.txt content mismatch: %s\n", content);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }
    if (read_file_content("extract/b.bin", content, sizeof(content)) != 0) {
        fprintf(stderr, "failed to read extracted b.bin\n");
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }
    if (strcmp(content, "world") != 0) {
        fprintf(stderr, "extracted b.bin content mismatch: %s\n", content);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Change back to original directory
    if (chdir(orig_cwd) != 0) {
        perror("chdir back");
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Cleanup
    cleanup_temp_dir(template);
    zu_context_free(ctx);
    return 0;
}

int main(void) {
    if (test_basic_extract() != 0) {
        return 1;
    }
    printf("All reader tests passed\n");
    return 0;
}