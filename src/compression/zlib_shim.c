#include "zlib_shim.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/*
 * zlib shim for ZIP-style raw deflate streams
 *
 * This module provides small helpers that compress or decompress an in-memory buffer
 * using zlib, but configured for the ZIP file format requirements
 *
 * ZIP uses raw DEFLATE streams
 * - No zlib header
 * - No gzip header
 * - The ZIP container stores metadata and CRC separately
 *
 * Ownership model
 * - On success, *out_buf points to a heap buffer owned by the caller
 * - The caller must free(*out_buf)
 * - On failure, functions leave *out_buf = NULL and *out_len = 0
 *
 * Error model
 * - Returns ZU_STATUS_USAGE for invalid pointers
 * - Returns ZU_STATUS_OOM for allocation failures
 * - Returns ZU_STATUS_IO for zlib failures or unexpected stream conditions
 */

/*
 * Allocate an output buffer and set length
 *
 * This is a tiny helper to keep allocation policy consistent across deflate/inflate
 * - out_len is set to the allocated capacity, not the produced byte count
 * - Callers must update *out_len to the actual produced size on success
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
 * Compress an input buffer using raw DEFLATE suitable for ZIP entries
 *
 * Parameters
 * - input/input_len: source data
 * - level: compression level 0..9, invalid values fall back to zlib default
 * - out_buf/out_len: output buffer and produced length
 *
 * Implementation notes
 * - Uses deflateInit2 with windowBits = -MAX_WBITS to request raw deflate
 * - Uses deflateBound to size the output buffer so we can do a single pass
 * - Uses Z_FINISH to produce the complete stream in one call
 */
int zu_deflate_buffer(const uint8_t* input, size_t input_len, int level, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = NULL;
    *out_len = 0;

    if (level < 0 || level > 9)
        level = Z_DEFAULT_COMPRESSION;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)input;
    strm.avail_in = (uInt)input_len;

    int zrc = deflateInit2(&strm, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (zrc != Z_OK)
        return ZU_STATUS_IO;

    size_t guess = deflateBound(&strm, (uLong)input_len);
    int rc = alloc_out(out_buf, out_len, guess);
    if (rc != ZU_STATUS_OK) {
        deflateEnd(&strm);
        return rc;
    }

    strm.next_out = *out_buf;
    strm.avail_out = (uInt)*out_len;

    zrc = deflate(&strm, Z_FINISH);
    if (zrc != Z_STREAM_END) {
        deflateEnd(&strm);
        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;
        return ZU_STATUS_IO;
    }

    *out_len = strm.total_out;
    deflateEnd(&strm);
    return ZU_STATUS_OK;
}

/*
 * Decompress a raw DEFLATE stream into an output buffer
 *
 * Parameters
 * - input/input_len: compressed DEFLATE data
 * - out_buf/out_len: output buffer pointer and produced length
 *
 * Allocation strategy
 * - ZIP does not always carry the uncompressed size in a convenient place here,
 *   so we start with a heuristic guess and retry if the buffer is too small
 * - Retries only on Z_BUF_ERROR, doubling the buffer each attempt
 * - Limits retries to avoid unbounded memory growth on corrupt inputs
 *
 * Implementation notes
 * - Uses inflateInit2 with windowBits = -MAX_WBITS to accept raw deflate
 * - Uses a single inflate(..., Z_FINISH) call and expects Z_STREAM_END
 */
int zu_inflate_buffer(const uint8_t* input, size_t input_len, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = NULL;
    *out_len = 0;

    size_t guess = input_len * 4 + 64;
    if (guess < 1024)
        guess = 1024;

    for (int attempt = 0; attempt < 3; ++attempt) {
        int rc = alloc_out(out_buf, out_len, guess);
        if (rc != ZU_STATUS_OK)
            return rc;

        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        strm.next_in = (Bytef*)input;
        strm.avail_in = (uInt)input_len;
        strm.next_out = *out_buf;
        strm.avail_out = (uInt)*out_len;

        int zrc = inflateInit2(&strm, -MAX_WBITS);
        if (zrc != Z_OK) {
            free(*out_buf);
            *out_buf = NULL;
            *out_len = 0;
            return ZU_STATUS_IO;
        }

        zrc = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);

        if (zrc == Z_STREAM_END) {
            *out_len = strm.total_out;
            return ZU_STATUS_OK;
        }

        free(*out_buf);
        *out_buf = NULL;
        *out_len = 0;

        if (zrc != Z_BUF_ERROR)
            return ZU_STATUS_IO;

        guess *= 2;
    }

    return ZU_STATUS_IO;
}
