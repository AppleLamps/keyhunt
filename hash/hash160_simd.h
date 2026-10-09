#ifndef HASH160_SIMD_H
#define HASH160_SIMD_H

#include <stdint.h>

// Lane parallel hash160 = RIPEMD160(SHA256(message)) kernels: 8 way AVX2 and
// 16 way AVX-512.
//
// The messages are passed pre-padded as big endian SHA256 words in lane
// parallel ("transposed") layout: w[j * LANES + l] is word j of lane l.
// 16 words per lane for the 1 block variant (messages up to 55 bytes, e.g. a
// 33 byte compressed public key), 32 words for the 2 block variant (e.g. a
// 65 byte uncompressed public key). out[l] receives the 20 byte digest of
// lane l.
//
// Only call a kernel when its *_available() function returns true. The
// kernels carry function level target attributes, so the file builds and the
// program runs on CPUs without these extensions.
#define HASH160_AVX2_LANES   8
#define HASH160_AVX512_LANES 16

bool hash160_avx2_available();
void hash160_avx2_1B(const uint32_t *w, uint8_t *const out[8]);
void hash160_avx2_2B(const uint32_t *w, uint8_t *const out[8]);
uint32_t hash160_avx2_1B_prefix(const uint32_t *w, uint64_t prefix, uint8_t *const out[8]);
uint32_t hash160_avx2_2B_prefix(const uint32_t *w, uint64_t prefix, uint8_t *const out[8]);

bool hash160_avx512_available();
void hash160_avx512_1B(const uint32_t *w, uint8_t *const out[16]);
void hash160_avx512_2B(const uint32_t *w, uint8_t *const out[16]);
uint32_t hash160_avx512_1B_prefix(const uint32_t *w, uint64_t prefix, uint8_t *const out[16]);
uint32_t hash160_avx512_2B_prefix(const uint32_t *w, uint64_t prefix, uint8_t *const out[16]);

// Widest kernel this CPU supports: 16, 8 or 0 (none).
int hash160_simd_lanes();
// Dispatch on the lane count returned by hash160_simd_lanes().
void hash160_simd_1B(int lanes, const uint32_t *w, uint8_t *const *out);
void hash160_simd_2B(int lanes, const uint32_t *w, uint8_t *const *out);

// Prefix-filtered variants: prefix is the first 8 digest bytes interpreted as
// a little endian integer. Bit l of the returned mask means lane l matches.
// Only matching lanes have their full digest written to out[l]; all other
// outputs are untouched. Callers MUST confirm candidates against all 20 bytes.
uint32_t hash160_simd_1B_prefix(int lanes, const uint32_t *w, uint64_t prefix, uint8_t *const *out);
uint32_t hash160_simd_2B_prefix(int lanes, const uint32_t *w, uint64_t prefix, uint8_t *const *out);

#endif
