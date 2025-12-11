#ifndef ZU_ZLIB_SHIM_H
#define ZU_ZLIB_SHIM_H

#include <stddef.h>
#include <stdint.h>

#include "ziputils.h"

int zu_deflate_buffer(const uint8_t* input, size_t input_len, int level, uint8_t** out_buf, size_t* out_len);
int zu_inflate_buffer(const uint8_t* input, size_t input_len, uint8_t** out_buf, size_t* out_len);

#endif
