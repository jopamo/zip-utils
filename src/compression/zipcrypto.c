#include "zipcrypto.h"
#include <zlib.h>

static const z_crc_t* crc_table = NULL;

static void ensure_table(void) {
    if (!crc_table) {
        crc_table = get_crc_table();
    }
}

/*
 * PKWARE ZipCrypto Keys Initialization
 */
void zu_zipcrypto_init(zu_zipcrypto_ctx* ctx, const char* password) {
    ensure_table();
    ctx->keys[0] = 305419896;
    ctx->keys[1] = 591751049;
    ctx->keys[2] = 878082192;

    while (*password) {
        zu_zipcrypto_update_keys(ctx, (uint8_t)*password++);
    }
}

/*
 * Update keys with the next byte of plaintext.
 */
void zu_zipcrypto_update_keys(zu_zipcrypto_ctx* ctx, uint8_t c) {
    /* k0 = crc32(k0, c) */
    ctx->keys[0] = crc_table[(ctx->keys[0] ^ c) & 0xff] ^ (ctx->keys[0] >> 8);

    /* k1 = (k1 + (k0 & 0xff)) * 134775813 + 1 */
    ctx->keys[1] = (ctx->keys[1] + (ctx->keys[0] & 0xff)) * 134775813 + 1;

    /* k2 = crc32(k2, k1 >> 24) */
    uint8_t b = (uint8_t)(ctx->keys[1] >> 24);
    ctx->keys[2] = crc_table[(ctx->keys[2] ^ b) & 0xff] ^ (ctx->keys[2] >> 8);
}

/*
 * Generate the next decryption byte from the current keys.
 */
uint8_t zu_zipcrypto_decrypt_byte(zu_zipcrypto_ctx* ctx) {
    uint32_t temp = ctx->keys[2] | 2;
    return (uint8_t)((temp * (temp ^ 1)) >> 8);
}

/*
 * Encryption byte is the same generator as decryption byte.
 */
uint8_t zu_zipcrypto_encrypt_byte(zu_zipcrypto_ctx* ctx) {
    return zu_zipcrypto_decrypt_byte(ctx);
}

void zu_zipcrypto_decrypt(zu_zipcrypto_ctx* ctx, uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        /* 1. Get decrypt byte */
        uint8_t val = data[i];
        uint8_t magic = zu_zipcrypto_decrypt_byte(ctx);
        /* 2. Decrypt */
        data[i] = val ^ magic;
        /* 3. Update keys with PLAINTEXT byte */
        zu_zipcrypto_update_keys(ctx, data[i]);
    }
}

void zu_zipcrypto_encrypt(zu_zipcrypto_ctx* ctx, uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        /* 1. Get encrypt byte */
        uint8_t magic = zu_zipcrypto_encrypt_byte(ctx);
        /* 2. Update keys with PLAINTEXT byte (before encryption) */
        uint8_t plain = data[i];
        zu_zipcrypto_update_keys(ctx, plain);
        /* 3. Encrypt */
        data[i] = plain ^ magic;
    }
}
