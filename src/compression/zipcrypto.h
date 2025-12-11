#ifndef ZU_ZIPCRYPTO_H
#define ZU_ZIPCRYPTO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t keys[3];
} zu_zipcrypto_ctx;

void zu_zipcrypto_init(zu_zipcrypto_ctx* ctx, const char* password);
void zu_zipcrypto_update_keys(zu_zipcrypto_ctx* ctx, uint8_t c);
uint8_t zu_zipcrypto_decrypt_byte(zu_zipcrypto_ctx* ctx);
uint8_t zu_zipcrypto_encrypt_byte(zu_zipcrypto_ctx* ctx);

/*
 * Decrypts a buffer in place.
 * This updates the internal keys state.
 */
void zu_zipcrypto_decrypt(zu_zipcrypto_ctx* ctx, uint8_t* data, size_t len);

/*
 * Encrypts a buffer in place.
 * This updates the internal keys state.
 */
void zu_zipcrypto_encrypt(zu_zipcrypto_ctx* ctx, uint8_t* data, size_t len);

#endif
