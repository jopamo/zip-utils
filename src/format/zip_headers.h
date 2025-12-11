#ifndef ZU_ZIP_HEADERS_H
#define ZU_ZIP_HEADERS_H

#include <stdint.h>

#define ZU_SIG_LOCAL   0x04034b50u
#define ZU_SIG_CENTRAL 0x02014b50u
#define ZU_SIG_END     0x06054b50u
#define ZU_SIG_END64   0x06064b50u
#define ZU_SIG_END64LOC 0x07064b50u
#define ZU_SIG_DESCRIPTOR 0x08074b50u

#pragma pack(push, 1)
typedef struct {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t name_len;
    uint16_t extra_len;
} zu_local_header;

typedef struct {
    uint32_t signature;
    uint16_t version_made;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t name_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint16_t disk_start;
    uint16_t int_attr;
    uint32_t ext_attr;
    uint32_t lho_offset;
} zu_central_header;

typedef struct {
    uint32_t signature;
    uint16_t disk_num;
    uint16_t disk_start;
    uint16_t entries_disk;
    uint16_t entries_total;
    uint32_t cd_size;
    uint32_t cd_offset;
    uint16_t comment_len;
} zu_end_central;

typedef struct {
    uint32_t signature;
    uint64_t size;
    uint16_t version_made;
    uint16_t version_needed;
    uint32_t disk_num;
    uint32_t disk_start;
    uint64_t entries_disk;
    uint64_t entries_total;
    uint64_t cd_size;
    uint64_t cd_offset;
} zu_end_central64;

typedef struct {
    uint32_t signature;
    uint32_t disk_num;
    uint64_t eocd64_offset;
    uint32_t total_disks;
} zu_end64_locator;

typedef struct {
    uint32_t signature;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
} zu_data_descriptor;

typedef struct {
    uint32_t signature;
    uint32_t crc32;
    uint64_t comp_size;
    uint64_t uncomp_size;
} zu_data_descriptor64;
#pragma pack(pop)

#endif
