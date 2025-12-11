#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib_shim.h"
#include "crc32.h"

int main(void) {
    const char *msg = "hello world";
    size_t msg_len = strlen(msg);

    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    int rc = zu_deflate_buffer((const uint8_t *)msg, msg_len, 6, &compressed, &compressed_len);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "deflate failed: rc=%d\n", rc);
        return 1;
    }

    uint8_t *decompressed = NULL;
    size_t decompressed_len = 0;
    rc = zu_inflate_buffer(compressed, compressed_len, &decompressed, &decompressed_len);
    if (rc != ZU_STATUS_OK) {
        fprintf(stderr, "inflate failed: rc=%d\n", rc);
        free(compressed);
        return 1;
    }

    if (decompressed_len != msg_len || memcmp(decompressed, msg, msg_len) != 0) {
        fprintf(stderr, "round-trip mismatch\n");
        free(compressed);
        free(decompressed);
        return 1;
    }

    uint32_t crc = zu_crc32((const uint8_t *)msg, msg_len, 0);
    if (crc != 0x0d4a1185U) {
        fprintf(stderr, "crc mismatch: got %08x\n", crc);
        free(compressed);
        free(decompressed);
        return 1;
    }

    free(compressed);
    free(decompressed);
    return 0;
}
