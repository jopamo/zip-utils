#define _POSIX_C_SOURCE 200809L
#include "parity_common.h"
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

void create_fixture(const char* root) {
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

    snprintf(path, sizeof(path), "%s/link", root);
    symlink("a.txt", path);
}

void cleanup_fixture(const char* root) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", root);
    system(cmd);
}

int run_command(const char* cwd, const char* cmd, char** output, char** error, int* exit_code) {
    // Simple implementation using popen not sufficient for stdout+stderr separation.
    // Using system() and redirection to temp files.

    char out_path[] = "/tmp/zu_test_out_XXXXXX";
    char err_path[] = "/tmp/zu_test_err_XXXXXX";
    int fd_out = mkstemp(out_path);
    int fd_err = mkstemp(err_path);
    close(fd_out);
    close(fd_err);

    char full_cmd[4096];
    // Note: This naive command construction is vulnerable to injection if inputs were untrusted,
    // but here we control the inputs from the test definitions.
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
