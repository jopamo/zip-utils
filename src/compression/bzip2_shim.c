#include "bzip2_shim.h"

#include <stdlib.h>
#include <string.h>

static int alloc_out(uint8_t** out_buf, size_t* out_len, size_t hint) {
    if (!out_buf || !out_len) {
        return ZU_STATUS_USAGE;
    }
    *out_buf = malloc(hint);
    if (!*out_buf) {
        return ZU_STATUS_OOM;
    }
    *out_len = hint;
    return ZU_STATUS_OK;
}

int zu_bzip2_compress_buffer(const uint8_t* input, size_t input_len, int level, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len) {
        return ZU_STATUS_USAGE;
    }
    if (level < 1 || level > 9) {
        level = 9; /* Default Bzip2 level */
    }

    /* Bzip2 often compresses well, but can expand. */
    size_t guess = input_len + (input_len / 100) + 600;
    int rc = alloc_out(out_buf, out_len, guess);
    if (rc != ZU_STATUS_OK) {
        return rc;
    }

    unsigned int destLen = (unsigned int)*out_len;
    int bzrc = BZ2_bzBuffToBuffCompress((char*)*out_buf, &destLen, (char*)input, (unsigned int)input_len, level, 0, 30);

    if (bzrc == BZ_OUTBUFF_FULL) {
        /* Should not happen with the recommended buffer size, but if so, retry with larger */
        free(*out_buf);
        guess = guess * 2;
        rc = alloc_out(out_buf, out_len, guess);
        if (rc != ZU_STATUS_OK)
            return rc;

        destLen = (unsigned int)*out_len;
        bzrc = BZ2_bzBuffToBuffCompress((char*)*out_buf, &destLen, (char*)input, (unsigned int)input_len, level, 0, 30);
    }

    if (bzrc != BZ_OK) {
        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;
        return ZU_STATUS_IO;
    }

    *out_len = destLen;
    return ZU_STATUS_OK;
}

int zu_bzip2_decompress_buffer(const uint8_t* input, size_t input_len, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len) {
        return ZU_STATUS_USAGE;
    }

    /* Guess uncompressed size */
    size_t guess = input_len * 4 + 1024;

    for (int attempt = 0; attempt < 3; ++attempt) {
        int rc = alloc_out(out_buf, out_len, guess);
        if (rc != ZU_STATUS_OK)
            return rc;

        unsigned int destLen = (unsigned int)*out_len;
        int bzrc = BZ2_bzBuffToBuffDecompress((char*)*out_buf, &destLen, (char*)input, (unsigned int)input_len, 0, 0);

        if (bzrc == BZ_OK) {
            *out_len = destLen;
            return ZU_STATUS_OK;
        }

        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;

        if (bzrc != BZ_OUTBUFF_FULL) {
            return ZU_STATUS_IO;
        }
        guess *= 2;
    }

    return ZU_STATUS_IO;
}
