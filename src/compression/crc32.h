#ifndef ZU_CRC32_H
#define ZU_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t zu_crc32(const uint8_t *data, size_t len, uint32_t seed);

#endif
