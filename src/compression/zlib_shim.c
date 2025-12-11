#include "zlib_shim.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

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

int zu_deflate_buffer(const uint8_t* input, size_t input_len, int level, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len) {
        return ZU_STATUS_USAGE;
    }
    if (level < 0 || level > 9) {
        level = Z_DEFAULT_COMPRESSION;
    }

    /* Raw deflate stream (no zlib header) to match ZIP requirements. */
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)input;
    strm.avail_in = (uInt)input_len;

    int zrc = deflateInit2(&strm, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (zrc != Z_OK) {
        return ZU_STATUS_IO;
    }

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

int zu_inflate_buffer(const uint8_t* input, size_t input_len, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len) {
        return ZU_STATUS_USAGE;
    }

    /* Start with a generous guess; retry with a larger buffer on Z_BUF_ERROR. */
    size_t guess = input_len * 4 + 64;
    if (guess < 1024) {
        guess = 1024;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        int rc = alloc_out(out_buf, out_len, guess);
        if (rc != ZU_STATUS_OK) {
            return rc;
        }

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

        if (zrc != Z_BUF_ERROR) {
            return ZU_STATUS_IO;
        }

        guess *= 2;
    }

    return ZU_STATUS_IO;
}
