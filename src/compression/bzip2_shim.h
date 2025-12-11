#ifndef ZU_BZIP2_SHIM_H
#define ZU_BZIP2_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <bzlib.h>

#include "ziputils.h"

/* Buffer-to-buffer helpers matching zlib_shim style */
int zu_bzip2_compress_buffer(const uint8_t* input, size_t input_len, int level, uint8_t** out_buf, size_t* out_len);
int zu_bzip2_decompress_buffer(const uint8_t* input, size_t input_len, uint8_t** out_buf, size_t* out_len);

#endif
