#include "crc32.h"

#include <zlib.h>

uint32_t zu_crc32(const uint8_t *data, size_t len, uint32_t seed) {
    return crc32(seed, data, (uInt)len);
}
