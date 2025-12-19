#include "recovery.h"
#include "zip_headers.h"
#include "ctx.h"
#include "ziputils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEMP_ARCHIVE "test_recovery.zip"

#include <errno.h>

static void cleanup(void) {
    if (unlink(TEMP_ARCHIVE) != 0 && errno != ENOENT) {
        perror("cleanup failed");
    }
}

// Helper to write a mocked Zip64 entry
static void create_mock_zip64_file(const char* path, uint64_t real_size) {
    FILE* fp = fopen(path, "wb");
    assert(fp);

    // 1. Local Header
    zu_local_header lho = {0};
    lho.signature = ZU_SIG_LOCAL;
    lho.version_needed = 45;
    lho.flags = 0;
    lho.method = 0;  // Stored
    lho.comp_size = 0xFFFFFFFF;
    lho.uncomp_size = 0xFFFFFFFF;
    lho.name_len = 4;  // "test"
    // Extra len: 2 (tag) + 2 (size) + 8 (uncomp) + 8 (comp) = 20
    lho.extra_len = 20;

    fwrite(&lho, 1, sizeof(lho), fp);
    fwrite("test", 1, 4, fp);

    // 2. Zip64 Extra Field
    uint16_t tag = 0x0001;
    uint16_t size = 16;
    fwrite(&tag, 1, 2, fp);
    fwrite(&size, 1, 2, fp);
    fwrite(&real_size, 1, 8, fp);  // Uncomp
    fwrite(&real_size, 1, 8, fp);  // Comp

    // 3. Payload
    for (uint64_t i = 0; i < real_size; i++) {
        fputc('A', fp);
    }

    fclose(fp);
}

static void test_zip64_recovery() {
    printf("Test: Zip64 Recovery Parsing\n");

    uint64_t real_size = 100;
    create_mock_zip64_file(TEMP_ARCHIVE, real_size);

    ZContext* ctx = zu_context_create();
    assert(ctx);

    ctx->in_file = fopen(TEMP_ARCHIVE, "rb");
    assert(ctx->in_file);

    // Run recovery
    int rc = zu_recover_central_directory(ctx, true);
    assert(rc == ZU_STATUS_OK);
    assert(ctx->existing_entries.len == 1);

    zu_existing_entry* entry = (zu_existing_entry*)ctx->existing_entries.items[0];
    printf("Recovered size: %lu\n", (unsigned long)entry->comp_size);

    assert(entry->comp_size == real_size);
    assert(entry->uncomp_size == real_size);

    zu_context_free(ctx);
    printf("PASS\n");
}

int main() {
    atexit(cleanup);
    test_zip64_recovery();
    return 0;
}
