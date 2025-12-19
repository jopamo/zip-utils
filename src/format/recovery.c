#define _XOPEN_SOURCE 700

#include "recovery.h"
#include "zip_headers.h"
#include "ziputils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define ZU_IO_CHUNK (64 * 1024)

/*
 * Scan forward for a data descriptor signature (0x08074b50)
 *
 * The scan starts at 'start_offset'.
 * If found, it reads the descriptor and checks if the compressed size
 * roughly matches the distance from 'data_start'.
 *
 * Returns true if a plausible descriptor is found, populating 'dd'.
 */
static bool find_data_descriptor(ZContext* ctx, off_t start_offset, off_t data_start, zu_data_descriptor* dd, off_t* found_pos) {
    if (fseeko(ctx->in_file, start_offset, SEEK_SET) != 0)
        return false;

    uint8_t buf[ZU_IO_CHUNK];
    size_t got;
    off_t current = start_offset;

    int loop_guard = 0;
    while ((got = fread(buf, 1, sizeof(buf), ctx->in_file)) > 0) {
        if (got < 4)
            break;
        if (++loop_guard > 1000000)
            break;  // Safety break

        for (size_t i = 0; i < got - 3; ++i) {
            uint32_t sig;
            memcpy(&sig, buf + i, 4);
            if (sig == ZU_SIG_DESCRIPTOR) {
                // Potential descriptor found
                off_t desc_pos = current + (off_t)i;

                // Read the full descriptor
                if (fseeko(ctx->in_file, desc_pos, SEEK_SET) != 0)
                    continue;

                zu_data_descriptor temp_dd;
                if (fread(&temp_dd, 1, sizeof(temp_dd), ctx->in_file) != sizeof(temp_dd))
                    continue;

                // Heuristic check:
                // If the compressed size in the descriptor matches the distance
                // from data_start to desc_pos, it's very likely the correct one.
                if ((off_t)temp_dd.comp_size == (desc_pos - data_start)) {
                    *dd = temp_dd;
                    *found_pos = desc_pos;
                    return true;
                }
            }
        }

        // Advance, keeping overlap for 4-byte signature
        current += (off_t)got - 3;
        if (fseeko(ctx->in_file, current, SEEK_SET) != 0)
            break;
    }

    return false;
}

int zu_recover_central_directory(ZContext* ctx, bool full_scan) {
    if (ctx->verbose || !ctx->quiet)
        zu_log(ctx, "Scanning archive for local headers (%s)...\n", full_scan ? "-FF" : "-F");

    if (fseeko(ctx->in_file, 0, SEEK_SET) != 0)
        return ZU_STATUS_IO;

    ctx->existing_entries.len = 0;
    int entries_found = 0;

    // We scan the file in chunks. 'current' tracks the absolute file offset.
    // If we find a header, we process it and then seek past the payload.
    // If we don't find a header in the buffer, we advance by (buffer_size - overlap).

    off_t current = 0;
    uint8_t buf[ZU_IO_CHUNK];
    size_t got;
    int loop_guard = 0;

    while (true) {
        if (++loop_guard > 10000000) {  // 10M chunks is > 600GB
            zu_context_set_error(ctx, ZU_STATUS_IO, "recovery scan limit reached");
            return ZU_STATUS_IO;
        }

        if (fseeko(ctx->in_file, current, SEEK_SET) != 0)
            break;
        got = fread(buf, 1, sizeof(buf), ctx->in_file);
        if (got < 4)
            break;

        bool found_in_chunk = false;
        size_t found_offset = 0;

        for (size_t i = 0; i < got - 3; ++i) {
            uint32_t s;
            memcpy(&s, buf + i, 4);
            if (s == ZU_SIG_LOCAL) {
                found_in_chunk = true;
                found_offset = i;
                break;
            }
        }

        if (found_in_chunk) {
            off_t lho_offset = current + (off_t)found_offset;

            // Process header
            if (fseeko(ctx->in_file, lho_offset, SEEK_SET) != 0)
                break;

            zu_local_header lho;
            if (fread(&lho, 1, sizeof(lho), ctx->in_file) != sizeof(lho))
                break;

            char* name = malloc(lho.name_len + 1);
            if (!name)
                return ZU_STATUS_OOM;

            if (fread(name, 1, lho.name_len, ctx->in_file) != lho.name_len) {
                free(name);
                // Header corrupted? Skip 4 bytes and continue scan
                // Header corrupted? Skip 1 byte and continue scan
                current = lho_offset + 1;
                continue;
            }
            name[lho.name_len] = '\0';

            if (fseeko(ctx->in_file, lho.extra_len, SEEK_CUR) != 0) {
                free(name);
                // Header corrupted? Skip 1 byte and continue scan
                current = lho_offset + 1;
                continue;
            }

            off_t data_start = ftello(ctx->in_file);
            uint64_t comp_size = lho.comp_size;
            uint64_t uncomp_size = lho.uncomp_size;
            uint32_t crc32 = lho.crc32;

            // Check for Zip64 extra field if sizes are masked
            if (lho.comp_size == 0xffffffffu || lho.uncomp_size == 0xffffffffu) {
                // Rewind to read extra field
                if (fseeko(ctx->in_file, data_start - lho.extra_len, SEEK_SET) == 0) {
                    uint8_t* extra = malloc(lho.extra_len);
                    if (extra) {
                        if (fread(extra, 1, lho.extra_len, ctx->in_file) == lho.extra_len) {
                            size_t pos = 0;
                            while (pos + 4 <= lho.extra_len) {
                                uint16_t tag, sz;
                                memcpy(&tag, extra + pos, 2);
                                memcpy(&sz, extra + pos + 2, 2);
                                if (pos + 4 + sz > lho.extra_len)
                                    break;

                                if (tag == 0x0001) {  // Zip64 tag
                                    size_t zpos = pos + 4;
                                    // Local header Zip64 extra layout:
                                    // - uncomp size (8 bytes)
                                    // - comp size (8 bytes)
                                    // But ONLY if the LFH fields were 0xFFFFFFFF

                                    if (lho.uncomp_size == 0xffffffffu && zpos + 8 <= pos + 4 + sz) {
                                        memcpy(&uncomp_size, extra + zpos, 8);
                                        zpos += 8;
                                    }
                                    if (lho.comp_size == 0xffffffffu && zpos + 8 <= pos + 4 + sz) {
                                        memcpy(&comp_size, extra + zpos, 8);
                                        zpos += 8;
                                    }
                                    break;
                                }
                                pos += 4 + sz;
                            }
                        }
                        free(extra);
                    }
                    // Restore position
                    fseeko(ctx->in_file, data_start, SEEK_SET);
                }
            }

            if (lho.flags & 8) {
                zu_data_descriptor dd;
                off_t dd_pos = 0;
                if (find_data_descriptor(ctx, data_start, data_start, &dd, &dd_pos)) {
                    comp_size = dd.comp_size;
                    uncomp_size = dd.uncomp_size;
                    crc32 = dd.crc32;
                }
                else {
                    comp_size = 0;
                }
            }

            zu_existing_entry* entry = calloc(1, sizeof(zu_existing_entry));
            if (!entry) {
                free(name);
                return ZU_STATUS_OOM;
            }

            entry->hdr.signature = ZU_SIG_CENTRAL;
            entry->hdr.version_made = 20;
            entry->hdr.version_needed = lho.version_needed;
            entry->hdr.flags = lho.flags;
            entry->hdr.method = lho.method;
            entry->hdr.mod_time = lho.mod_time;
            entry->hdr.mod_date = lho.mod_date;
            entry->hdr.crc32 = crc32;
            entry->hdr.comp_size = (uint32_t)comp_size;
            entry->hdr.uncomp_size = (uint32_t)uncomp_size;
            entry->hdr.name_len = lho.name_len;
            entry->hdr.extra_len = lho.extra_len;
            entry->hdr.lho_offset = (uint32_t)lho_offset;
            entry->name = name;
            entry->comp_size = comp_size;
            entry->uncomp_size = uncomp_size;
            entry->lho_offset = (uint64_t)lho_offset;

            if (ctx->existing_entries.len == ctx->existing_entries.cap) {
                size_t new_cap = ctx->existing_entries.cap == 0 ? 16 : ctx->existing_entries.cap * 2;
                char** new_items = realloc(ctx->existing_entries.items, new_cap * sizeof(char*));
                if (!new_items) {
                    free(name);
                    free(entry);
                    return ZU_STATUS_OOM;
                }
                ctx->existing_entries.items = new_items;
                ctx->existing_entries.cap = new_cap;
            }
            ctx->existing_entries.items[ctx->existing_entries.len++] = (char*)entry;
            entries_found++;

            if (comp_size > 0) {
                // Known size: Skip payload
                current = data_start + (off_t)comp_size;
                if (lho.flags & 8)
                    current += 16;  // Skip descriptor
            }
            else {
                // Unknown size: Resume scan at data start
                current = data_start;
            }
        }
        else {
            // No header found in this chunk, advance
            current += (off_t)got - 3;
        }
    }

    // Post-scan size fixup
    for (size_t k = 0; k < ctx->existing_entries.len; ++k) {
        zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[k];
        if ((e->hdr.flags & 8) && e->comp_size == 0) {
            uint64_t next_offset = 0;
            if (k + 1 < ctx->existing_entries.len) {
                next_offset = ((zu_existing_entry*)ctx->existing_entries.items[k + 1])->lho_offset;
            }
            else {
                fseeko(ctx->in_file, 0, SEEK_END);
                next_offset = (uint64_t)ftello(ctx->in_file);
            }

            uint64_t start_data = e->lho_offset + 30 + e->hdr.name_len + e->hdr.extra_len;
            if (next_offset > start_data) {
                uint64_t gap = next_offset - start_data;
                if (gap > 16)
                    e->comp_size = gap - 16;
                else
                    e->comp_size = gap;
                e->hdr.comp_size = (uint32_t)e->comp_size;
            }
        }
    }

    return entries_found > 0 ? ZU_STATUS_OK : ZU_STATUS_IO;
}
/* End of file */
