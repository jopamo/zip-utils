#include "bzip2_shim.h"

#include <stdlib.h>
#include <string.h>

/*
 * bzip2 shim for ZIP entry compression methods
 *
 * This module provides in-memory compression/decompression helpers built on libbzip2
 * It is used when the archive selects the bzip2 method (ZIP method 12)
 *
 * Ownership model
 * - On success, *out_buf points to a heap buffer owned by the caller
 * - The caller must free(*out_buf)
 * - On failure, functions ensure *out_buf = NULL and *out_len = 0
 *
 * Error model
 * - Returns ZU_STATUS_USAGE for invalid pointers
 * - Returns ZU_STATUS_OOM for allocation failures
 * - Returns ZU_STATUS_IO for libbzip2 failures or unexpected conditions
 *
 * Size assumptions
 * - libbzip2 buffer-to-buffer APIs use unsigned int lengths
 *   This limits per-call sizes to UINT_MAX, which is acceptable for typical ZIP entries
 *   Larger entries should use streaming APIs if ever needed
 */

static int alloc_out(uint8_t** out_buf, size_t* out_len, size_t hint) {
    if (!out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = malloc(hint);
    if (!*out_buf)
        return ZU_STATUS_OOM;

    *out_len = hint;
    return ZU_STATUS_OK;
}

/*
 * Compress an input buffer using libbzip2 buffer-to-buffer API
 *
 * Parameters
 * - input/input_len: source data
 * - level: bzip2 block size 1..9, invalid values fall back to 9
 * - out_buf/out_len: output buffer pointer and produced length
 *
 * Buffer sizing
 * - Bzip2 can expand incompressible data slightly
 * - The guess formula is the commonly recommended "input + 1% + 600" style sizing
 * - If the output buffer is still too small, retry once with a doubled buffer
 *
 * Notes
 * - Uses workFactor 30 (default) and verbosity 0
 * - Produces a complete bzip2 stream suitable for ZIP method 12 payloads
 */
int zu_bzip2_compress_buffer(const uint8_t* input, size_t input_len, int level, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = NULL;
    *out_len = 0;

    if (level < 1 || level > 9)
        level = 9;

    size_t guess = input_len + (input_len / 100) + 600;

    int rc = alloc_out(out_buf, out_len, guess);
    if (rc != ZU_STATUS_OK)
        return rc;

    unsigned int destLen = (unsigned int)*out_len;
    int bzrc = BZ2_bzBuffToBuffCompress((char*)*out_buf, &destLen, (char*)input, (unsigned int)input_len, level, 0, 30);

    if (bzrc == BZ_OUTBUFF_FULL) {
        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;

        guess *= 2;
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

    *out_len = (size_t)destLen;
    return ZU_STATUS_OK;
}

/*
 * Decompress a bzip2-compressed buffer into an output buffer
 *
 * Parameters
 * - input/input_len: compressed data
 * - out_buf/out_len: output buffer pointer and produced length
 *
 * Allocation strategy
 * - The uncompressed size is not always known in this layer
 * - Start with a heuristic guess and retry if the output buffer is too small
 * - Retries only on BZ_OUTBUFF_FULL, doubling capacity each time
 * - Attempts are capped to avoid runaway allocations on corrupt inputs
 *
 * Notes
 * - Uses small=0 and verbosity=0
 * - If entries can be very large in practice, consider streaming decompression in the future
 */
int zu_bzip2_decompress_buffer(const uint8_t* input, size_t input_len, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = NULL;
    *out_len = 0;

    size_t guess = input_len * 4 + 1024;

    for (int attempt = 0; attempt < 3; ++attempt) {
        int rc = alloc_out(out_buf, out_len, guess);
        if (rc != ZU_STATUS_OK)
            return rc;

        unsigned int destLen = (unsigned int)*out_len;
        int bzrc = BZ2_bzBuffToBuffDecompress((char*)*out_buf, &destLen, (char*)input, (unsigned int)input_len, 0, 0);

        if (bzrc == BZ_OK) {
            *out_len = (size_t)destLen;
            return ZU_STATUS_OK;
        }

        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;

        if (bzrc != BZ_OUTBUFF_FULL)
            return ZU_STATUS_IO;

        guess *= 2;
    }

    return ZU_STATUS_IO;
}
