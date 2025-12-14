#ifndef PARITY_COMMON_H
#define PARITY_COMMON_H

#include <stdbool.h>

void create_fixture(const char* root, const char* zip_bin);
void cleanup_fixture(const char* root);
void cleanup_files_keeping_zip(const char* root);
int run_command(const char* cwd, const char* cmd, char** output, char** error, int* exit_code);

#endif
