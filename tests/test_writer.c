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

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
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
    rmdir(dirpath);
}

static int test_basic_create(void) {
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        fprintf(stderr, "ctx alloc failed\n");
        return 1;
    }

    // Create temporary directory
    char template[] = "/tmp/zu_writer_testXXXXXX";
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

    // Set up context for archive creation
    ctx->archive_path = "test.zip";
    ctx->store_paths = true;  // default
    ctx->quiet = true;        // suppress output

    // Add files to include list (relative paths)
    if (zu_strlist_push(&ctx->include, "a.txt") != 0 || zu_strlist_push(&ctx->include, "b.bin") != 0) {
        fprintf(stderr, "failed to push to include list\n");
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Call modify archive
    int rc = zu_modify_archive(ctx);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "zu_modify_archive failed: %d\n", rc);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Verify archive exists
    if (!file_exists("test.zip")) {
        fprintf(stderr, "archive not created\n");
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Load central directory to verify entries
    rc = zu_load_central_directory(ctx);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "zu_load_central_directory failed: %d\n", rc);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Check entry count
    if (ctx->existing_entries.len != 2) {
        fprintf(stderr, "expected 2 entries, got %zu\n", ctx->existing_entries.len);
        chdir(orig_cwd);
        cleanup_temp_dir(template);
        zu_context_free(ctx);
        return 1;
    }

    // Check entry names (order may be preserved)
    bool found_a = false, found_b = false;
    for (size_t i = 0; i < ctx->existing_entries.len; i++) {
        zu_existing_entry* entry = (zu_existing_entry*)ctx->existing_entries.items[i];
        if (strcmp(entry->name, "a.txt") == 0) {
            found_a = true;
        }
        else if (strcmp(entry->name, "b.bin") == 0) {
            found_b = true;
        }
        else {
            fprintf(stderr, "unexpected entry name: %s\n", entry->name);
            chdir(orig_cwd);
            cleanup_temp_dir(template);
            zu_context_free(ctx);
            return 1;
        }
    }
    if (!found_a || !found_b) {
        fprintf(stderr, "missing expected entries\n");
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
    if (test_basic_create() != 0) {
        return 1;
    }
    printf("All writer tests passed\n");
    return 0;
}