/*
 * 8-way AVX2 and 16-way AVX-512 hash160 kernels. The kernel body lives in
 * hash160_kernel.inl and is instantiated once per extension with the vector
 * primitives defined below. Each instantiation carries its own target
 * attribute, so this file compiles without -mavx2/-mavx512f and the caller
 * picks a kernel at runtime (hash160_simd_lanes()).
 */

#include "hash160_simd.h"

#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>
#include <string.h>

// ---- AVX2, 8 lanes ---------------------------------------------------------

#define VEC    __m256i
#define LANES  8
#define KFN    __attribute__((target("avx2")))
#define SFX    avx2
#define VADD(a, b)     _mm256_add_epi32(a, b)
#define VXOR(a, b)     _mm256_xor_si256(a, b)
#define VAND(a, b)     _mm256_and_si256(a, b)
#define VOR(a, b)      _mm256_or_si256(a, b)
#define VANDN(a, b)    _mm256_andnot_si256(a, b)
#define VSRL(x, n)     _mm256_srli_epi32(x, n)
#define VSET1(i)       _mm256_set1_epi32((int)(i))
#define VZERO()        _mm256_setzero_si256()
#define VLOAD(p)       _mm256_loadu_si256((const __m256i *)(p))
#define VSTORE(p, v)   _mm256_storeu_si256((__m256i *)(p), v)
#define VROR(x, n)     VOR(_mm256_srli_epi32(x, n), _mm256_slli_epi32(x, 32 - (n)))
#define VROL(x, n)     VOR(_mm256_slli_epi32(x, n), _mm256_srli_epi32(x, 32 - (n)))
#define VNOT(x)        VXOR(x, VSET1(-1))
#define VCH(e, f, g)   VXOR(VAND(e, f), VANDN(e, g))
#define VMAJ(a, b, c)  VOR(VAND(a, b), VAND(c, VOR(a, b)))
#define VXOR3(a, b, c) VXOR(a, VXOR(b, c))
#define VF3(x, y, z)   VXOR(VOR(x, VNOT(y)), z)
#define VF4(x, y, z)   VOR(VAND(x, z), VANDN(z, y))
#define VF5(x, y, z)   VXOR(x, VOR(y, VNOT(z)))
#define VBSWAP(x)      _mm256_shuffle_epi8(x, _mm256_setr_epi8( \
    3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, \
    3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12))

#include "hash160_kernel.inl"

#undef VEC
#undef LANES
#undef KFN
#undef SFX
#undef VADD
#undef VXOR
#undef VAND
#undef VOR
#undef VANDN
#undef VSRL
#undef VSET1
#undef VZERO
#undef VLOAD
#undef VSTORE
#undef VROR
#undef VROL
#undef VNOT
#undef VCH
#undef VMAJ
#undef VXOR3
#undef VF3
#undef VF4
#undef VF5
#undef VBSWAP

// ---- AVX-512 (F + BW), 16 lanes --------------------------------------------
// Rotates are single instructions and every 3 input boolean function is one
// vpternlogd, so a round is about a third of the AVX2 instruction count, on
// twice the lanes, with 32 registers (the AVX2 kernel spills).

#define VEC    __m512i
#define LANES  16
#define KFN    __attribute__((target("avx512f,avx512bw")))
#define SFX    avx512
#define VADD(a, b)     _mm512_add_epi32(a, b)
#define VXOR(a, b)     _mm512_xor_si512(a, b)
#define VAND(a, b)     _mm512_and_si512(a, b)
#define VOR(a, b)      _mm512_or_si512(a, b)
#define VANDN(a, b)    _mm512_andnot_si512(a, b)
#define VSRL(x, n)     _mm512_srli_epi32(x, n)
#define VSET1(i)       _mm512_set1_epi32((int)(i))
#define VZERO()        _mm512_setzero_si512()
#define VLOAD(p)       _mm512_loadu_si512((const void *)(p))
#define VSTORE(p, v)   _mm512_storeu_si512((void *)(p), v)
#define VROR(x, n)     _mm512_ror_epi32(x, n)
#define VROL(x, n)     _mm512_rol_epi32(x, n)
// vpternlogd immediates: bit (a<<2 | b<<1 | c) of the immediate is f(a,b,c)
#define VCH(e, f, g)   _mm512_ternarylogic_epi32(e, f, g, 0xCA)   // (e&f)^(~e&g)
#define VMAJ(a, b, c)  _mm512_ternarylogic_epi32(a, b, c, 0xE8)   // majority
#define VXOR3(a, b, c) _mm512_ternarylogic_epi32(a, b, c, 0x96)   // a^b^c
#define VF3(x, y, z)   _mm512_ternarylogic_epi32(x, y, z, 0x59)   // (x|~y)^z
#define VF4(x, y, z)   _mm512_ternarylogic_epi32(x, y, z, 0xE4)   // (x&z)|(y&~z)
#define VF5(x, y, z)   _mm512_ternarylogic_epi32(x, y, z, 0x2D)   // x^(y|~z)
#define VBSWAP(x)      _mm512_shuffle_epi8(x, _mm512_set4_epi32(0x0c0d0e0f, 0x08090a0b, 0x04050607, 0x00010203))

#include "hash160_kernel.inl"

bool hash160_avx2_available() {
  return __builtin_cpu_supports("avx2");
}

bool hash160_avx512_available() {
  return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
}

#else  // not x86

bool hash160_avx2_available() { return false; }
void hash160_avx2_1B(const uint32_t *, uint8_t *const[8]) {}
void hash160_avx2_2B(const uint32_t *, uint8_t *const[8]) {}
bool hash160_avx512_available() { return false; }
void hash160_avx512_1B(const uint32_t *, uint8_t *const[16]) {}
void hash160_avx512_2B(const uint32_t *, uint8_t *const[16]) {}

#endif

int hash160_simd_lanes() {
  static int lanes = -1;
  if (lanes < 0) {
    const char *env = getenv("KEYHUNT_SIMD");   // "avx2", "avx512" or "none" to force a kernel
    if (env && strcmp(env, "none") == 0) lanes = 0;
    else if (env && strcmp(env, "avx2") == 0) lanes = hash160_avx2_available() ? 8 : 0;
    else if (hash160_avx512_available() && !(env && strcmp(env, "avx512") != 0)) lanes = 16;
    else lanes = hash160_avx2_available() ? 8 : 0;
  }
  return lanes;
}

void hash160_simd_1B(int lanes, const uint32_t *w, uint8_t *const *out) {
  if (lanes == 16) hash160_avx512_1B(w, out);
  else hash160_avx2_1B(w, out);
}

void hash160_simd_2B(int lanes, const uint32_t *w, uint8_t *const *out) {
  if (lanes == 16) hash160_avx512_2B(w, out);
  else hash160_avx2_2B(w, out);
}
