#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static void mkdir_p(const char* path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
    else {
        perror(path);
    }
}

void create_fixture(const char* root, const char* zip_bin) {
    char path[512];

    snprintf(path, sizeof(path), "%s/dir/sub", root);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/dir/deep", root);
    mkdir_p(path);

    snprintf(path, sizeof(path), "%s/a.txt", root);
    write_file(path, "hello\nworld\n", 12);

    // b.bin: using deterministic pattern instead of random
    snprintf(path, sizeof(path), "%s/b.bin", root);
    unsigned char buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = (unsigned char)i;
    write_file(path, buf, 256);

    snprintf(path, sizeof(path), "%s/crlf.txt", root);
    write_file(path, "one\r\ntwo\r\n", 10);

    snprintf(path, sizeof(path), "%s/data.dat", root);
    write_file(path, "database data", 13);

    snprintf(path, sizeof(path), "%s/script.log", root);
    write_file(path, "log data", 8);

    snprintf(path, sizeof(path), "%s/dir/c.txt", root);
    write_file(path, "inside\n", 7);

    snprintf(path, sizeof(path), "%s/dir/sub/d.txt", root);
    write_file(path, "nested\n", 7);

    snprintf(path, sizeof(path), "%s/dir/deep/e.txt", root);
    write_file(path, "deep nested\n", 12);

    snprintf(path, sizeof(path), "%s/-dash.txt", root);
    write_file(path, "file starting with dash\n", 24);

    snprintf(path, sizeof(path), "%s/pat_a1.txt", root);
    write_file(path, "match", 5);

    snprintf(path, sizeof(path), "%s/pat_b1.txt", root);
    write_file(path, "no match", 8);

    snprintf(path, sizeof(path), "%s/spaced name.txt", root);
    write_file(path, "filename with spaces", 20);

    snprintf(path, sizeof(path), "%s/--looks-like-opt", root);
    write_file(path, "confusing filename", 18);

    snprintf(path, sizeof(path), "%s/link", root);
    symlink("a.txt", path);

    // Create a dummy zip file for zipinfo tests if zip_bin is provided
    if (zip_bin) {
        char cmd[4096];
        // Create test.zip with a.txt and dir/c.txt to match basic expectations
        // We mute output.
        // We use absolute paths for zip_bin if possible, but we expect caller to handle it.
        // We create 'test.zip' because some tests might expect it (though output.py uses various names).
        // Actually, document_zipinfo_output.py probably uses a specific name 'test.zip' or similar?
        // document_zipinfo_output.py creates 'test.zip' in make_fixture logic (via zip command).
        // So we should create 'test.zip'.

        // Note: zip-utils 'zip' supports -q (quiet) and -r (recurse).
        snprintf(cmd, sizeof(cmd), "cd \"%s\" && \"%s\" -q -r test.zip . -x test.zip", root, zip_bin);
        // We ignore failure here as zip might not be available or fail,
        // but it's best effort for the fixture.
        system(cmd);

        // Add archive comment
        snprintf(cmd, sizeof(cmd), "echo \"This is the archive comment\" | \"%s\" -z \"%s/test.zip\"", zip_bin, root);
        system(cmd);

        // Also create 'out.zip' as copy of test.zip because some tests might use it?
        // document_zip_output.py uses 'out.zip'.
        // We can just cp.
        snprintf(cmd, sizeof(cmd), "cp \"%s/test.zip\" \"%s/out.zip\"", root, root);
        system(cmd);

        snprintf(cmd, sizeof(cmd), "cp \"%s/test.zip\" \"%s/source.zip\"", root, root);
        system(cmd);
    }
}

void cleanup_fixture(const char* root) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", root);
    system(cmd);
}

void cleanup_files_keeping_zip(const char* root) {
    const char* files[] = {"a.txt", "b.bin", "crlf.txt", "data.dat", "script.log", "-dash.txt", "pat_a1.txt", "pat_b1.txt", "spaced name.txt", "--looks-like-opt", "link", NULL};
    char path[512];

    for (int i = 0; files[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i]);
        unlink(path);
    }

    snprintf(path, sizeof(path), "%s/dir", root);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
    system(cmd);
}

int run_command(const char* cwd, const char* cmd, char** output, char** error, int* exit_code) {
    char out_path[] = "/tmp/zu_test_out_XXXXXX";
    char err_path[] = "/tmp/zu_test_err_XXXXXX";
    int fd_out = mkstemp(out_path);
    int fd_err = mkstemp(err_path);
    close(fd_out);
    close(fd_err);

    char full_cmd[4096];
    snprintf(full_cmd, sizeof(full_cmd), "cd \"%s\" && %s >%s 2>%s", cwd, cmd, out_path, err_path);

    int rc = system(full_cmd);
    *exit_code = WEXITSTATUS(rc);

    // Read back
    FILE* f = fopen(out_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        *output = malloc(fsize + 1);
        if (*output) {
            fread(*output, 1, fsize, f);
            (*output)[fsize] = 0;
        }
        fclose(f);
    }
    else {
        *output = strdup("");
    }

    f = fopen(err_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        *error = malloc(fsize + 1);
        if (*error) {
            fread(*error, 1, fsize, f);
            (*error)[fsize] = 0;
        }
        fclose(f);
    }
    else {
        *error = strdup("");
    }

    unlink(out_path);
    unlink(err_path);

    return 0;
}