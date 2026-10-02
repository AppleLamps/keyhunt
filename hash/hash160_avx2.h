#ifndef HASH160_AVX2_H
#define HASH160_AVX2_H

#include <stdint.h>

// 8-way AVX2 hash160 = RIPEMD160(SHA256(message)).
//
// `in[l]` is the pre-padded message of lane l as big endian SHA256 words:
// 16 words for the 1 block variant (messages up to 55 bytes, e.g. a 33 byte
// compressed public key) and 32 words for the 2 block variant (e.g. a 65 byte
// uncompressed public key). `out[l]` receives the 20 byte digest.
//
// Only call these when hash160_avx2_available() returns true.
bool hash160_avx2_available();
void hash160_avx2_1B(const uint32_t *const in[8], uint8_t *const out[8]);
void hash160_avx2_2B(const uint32_t *const in[8], uint8_t *const out[8]);

#endif
