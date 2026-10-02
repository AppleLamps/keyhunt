/*
 * 8-way AVX2 SHA256 + RIPEMD160 ("hash160") kernels.
 *
 * Derived from the 4-way SSE kernels (hash/sha256_sse.cpp, hash/ripemd160_sse.cpp,
 * VanitySearch, Copyright (c) 2019 Jean Luc PONS, GPLv3). The SHA256 digest is
 * fed straight into RIPEMD160 in registers, saving the byte swap, the store
 * and the reload that the 4-way pair of kernels needs.
 *
 * The kernels carry a function-level target("avx2") attribute, so this file
 * builds without -mavx2 and callers pick it at runtime with
 * hash160_avx2_available().
 */

#include "hash160_avx2.h"

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>
#include <string.h>

#define AVX2_FN __attribute__((target("avx2")))

bool hash160_avx2_available() {
  return __builtin_cpu_supports("avx2");
}

namespace {

#define Maj(b,c,d) _mm256_or_si256(_mm256_and_si256(b, c), _mm256_and_si256(d, _mm256_or_si256(b, c)))
#define Ch(b,c,d)  _mm256_xor_si256(_mm256_and_si256(b, c), _mm256_andnot_si256(b, d))
#define ROR(x,n)   _mm256_or_si256(_mm256_srli_epi32(x, n), _mm256_slli_epi32(x, 32 - n))
#define SHR(x,n)   _mm256_srli_epi32(x, n)

#define S0(x) (_mm256_xor_si256(ROR((x), 2), _mm256_xor_si256(ROR((x), 13), ROR((x), 22))))
#define S1(x) (_mm256_xor_si256(ROR((x), 6), _mm256_xor_si256(ROR((x), 11), ROR((x), 25))))
#define s0(x) (_mm256_xor_si256(ROR((x), 7), _mm256_xor_si256(ROR((x), 18), SHR((x), 3))))
#define s1(x) (_mm256_xor_si256(ROR((x), 17), _mm256_xor_si256(ROR((x), 19), SHR((x), 10))))

#define add4(x0, x1, x2, x3) _mm256_add_epi32(_mm256_add_epi32(x0, x1), _mm256_add_epi32(x2, x3))
#define add3(x0, x1, x2) _mm256_add_epi32(_mm256_add_epi32(x0, x1), x2)
#define add5(x0, x1, x2, x3, x4) _mm256_add_epi32(add3(x0, x1, x2), _mm256_add_epi32(x3, x4))

#define Round(a, b, c, d, e, f, g, h, i, w)                    \
    T1 = add5(h, S1(e), Ch(e, f, g), _mm256_set1_epi32(i), w); \
    d = _mm256_add_epi32(d, T1);                               \
    T2 = _mm256_add_epi32(S0(a), Maj(a, b, c));                \
    h = _mm256_add_epi32(T1, T2);

#define WMIX() \
  w0 = add4(s1(w14), w9, s0(w1), w0); \
  w1 = add4(s1(w15), w10, s0(w2), w1); \
  w2 = add4(s1(w0), w11, s0(w3), w2); \
  w3 = add4(s1(w1), w12, s0(w4), w3); \
  w4 = add4(s1(w2), w13, s0(w5), w4); \
  w5 = add4(s1(w3), w14, s0(w6), w5); \
  w6 = add4(s1(w4), w15, s0(w7), w6); \
  w7 = add4(s1(w5), w0, s0(w8), w7); \
  w8 = add4(s1(w6), w1, s0(w9), w8); \
  w9 = add4(s1(w7), w2, s0(w10), w9); \
  w10 = add4(s1(w8), w3, s0(w11), w10); \
  w11 = add4(s1(w9), w4, s0(w12), w11); \
  w12 = add4(s1(w10), w5, s0(w13), w12); \
  w13 = add4(s1(w11), w6, s0(w14), w13); \
  w14 = add4(s1(w12), w7, s0(w15), w14); \
  w15 = add4(s1(w13), w8, s0(w0), w15);

// Transpose an 8x8 matrix of 32-bit words: row r holds the words of one lane
// on entry, row j holds word j of every lane on exit.
AVX2_FN static inline void transpose8(__m256i *r) {
  __m256i t0 = _mm256_unpacklo_epi32(r[0], r[1]);
  __m256i t1 = _mm256_unpackhi_epi32(r[0], r[1]);
  __m256i t2 = _mm256_unpacklo_epi32(r[2], r[3]);
  __m256i t3 = _mm256_unpackhi_epi32(r[2], r[3]);
  __m256i t4 = _mm256_unpacklo_epi32(r[4], r[5]);
  __m256i t5 = _mm256_unpackhi_epi32(r[4], r[5]);
  __m256i t6 = _mm256_unpacklo_epi32(r[6], r[7]);
  __m256i t7 = _mm256_unpackhi_epi32(r[6], r[7]);

  __m256i u0 = _mm256_unpacklo_epi64(t0, t2);
  __m256i u1 = _mm256_unpackhi_epi64(t0, t2);
  __m256i u2 = _mm256_unpacklo_epi64(t1, t3);
  __m256i u3 = _mm256_unpackhi_epi64(t1, t3);
  __m256i u4 = _mm256_unpacklo_epi64(t4, t6);
  __m256i u5 = _mm256_unpackhi_epi64(t4, t6);
  __m256i u6 = _mm256_unpacklo_epi64(t5, t7);
  __m256i u7 = _mm256_unpackhi_epi64(t5, t7);

  r[0] = _mm256_permute2x128_si256(u0, u4, 0x20);
  r[1] = _mm256_permute2x128_si256(u1, u5, 0x20);
  r[2] = _mm256_permute2x128_si256(u2, u6, 0x20);
  r[3] = _mm256_permute2x128_si256(u3, u7, 0x20);
  r[4] = _mm256_permute2x128_si256(u0, u4, 0x31);
  r[5] = _mm256_permute2x128_si256(u1, u5, 0x31);
  r[6] = _mm256_permute2x128_si256(u2, u6, 0x31);
  r[7] = _mm256_permute2x128_si256(u3, u7, 0x31);
}

// Load words [off, off + 16) of the 8 messages as 16 lane-parallel words.
AVX2_FN static inline void load_block(const uint32_t *const in[8], int off, __m256i *w) {
  for (int h = 0; h < 2; h++) {
    for (int l = 0; l < 8; l++)
      w[h * 8 + l] = _mm256_loadu_si256((const __m256i *)(in[l] + off + h * 8));
    transpose8(w + h * 8);
  }
}

// One SHA256 block over the state s[0..7] (lane-parallel), message w[0..15].
AVX2_FN static inline void sha256_block(__m256i *s, __m256i *w) {
  __m256i a = s[0], b = s[1], c = s[2], d = s[3];
  __m256i e = s[4], f = s[5], g = s[6], h = s[7];
  __m256i w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3], w4 = w[4], w5 = w[5], w6 = w[6], w7 = w[7];
  __m256i w8 = w[8], w9 = w[9], w10 = w[10], w11 = w[11], w12 = w[12], w13 = w[13], w14 = w[14], w15 = w[15];
  __m256i T1, T2;

    Round(a, b, c, d, e, f, g, h, 0x428A2F98, w0);
    Round(h, a, b, c, d, e, f, g, 0x71374491, w1);
    Round(g, h, a, b, c, d, e, f, 0xB5C0FBCF, w2);
    Round(f, g, h, a, b, c, d, e, 0xE9B5DBA5, w3);
    Round(e, f, g, h, a, b, c, d, 0x3956C25B, w4);
    Round(d, e, f, g, h, a, b, c, 0x59F111F1, w5);
    Round(c, d, e, f, g, h, a, b, 0x923F82A4, w6);
    Round(b, c, d, e, f, g, h, a, 0xAB1C5ED5, w7);
    Round(a, b, c, d, e, f, g, h, 0xD807AA98, w8);
    Round(h, a, b, c, d, e, f, g, 0x12835B01, w9);
    Round(g, h, a, b, c, d, e, f, 0x243185BE, w10);
    Round(f, g, h, a, b, c, d, e, 0x550C7DC3, w11);
    Round(e, f, g, h, a, b, c, d, 0x72BE5D74, w12);
    Round(d, e, f, g, h, a, b, c, 0x80DEB1FE, w13);
    Round(c, d, e, f, g, h, a, b, 0x9BDC06A7, w14);
    Round(b, c, d, e, f, g, h, a, 0xC19BF174, w15);

    WMIX()

    Round(a, b, c, d, e, f, g, h, 0xE49B69C1, w0);
    Round(h, a, b, c, d, e, f, g, 0xEFBE4786, w1);
    Round(g, h, a, b, c, d, e, f, 0x0FC19DC6, w2);
    Round(f, g, h, a, b, c, d, e, 0x240CA1CC, w3);
    Round(e, f, g, h, a, b, c, d, 0x2DE92C6F, w4);
    Round(d, e, f, g, h, a, b, c, 0x4A7484AA, w5);
    Round(c, d, e, f, g, h, a, b, 0x5CB0A9DC, w6);
    Round(b, c, d, e, f, g, h, a, 0x76F988DA, w7);
    Round(a, b, c, d, e, f, g, h, 0x983E5152, w8);
    Round(h, a, b, c, d, e, f, g, 0xA831C66D, w9);
    Round(g, h, a, b, c, d, e, f, 0xB00327C8, w10);
    Round(f, g, h, a, b, c, d, e, 0xBF597FC7, w11);
    Round(e, f, g, h, a, b, c, d, 0xC6E00BF3, w12);
    Round(d, e, f, g, h, a, b, c, 0xD5A79147, w13);
    Round(c, d, e, f, g, h, a, b, 0x06CA6351, w14);
    Round(b, c, d, e, f, g, h, a, 0x14292967, w15);

    WMIX()

    Round(a, b, c, d, e, f, g, h, 0x27B70A85, w0);
    Round(h, a, b, c, d, e, f, g, 0x2E1B2138, w1);
    Round(g, h, a, b, c, d, e, f, 0x4D2C6DFC, w2);
    Round(f, g, h, a, b, c, d, e, 0x53380D13, w3);
    Round(e, f, g, h, a, b, c, d, 0x650A7354, w4);
    Round(d, e, f, g, h, a, b, c, 0x766A0ABB, w5);
    Round(c, d, e, f, g, h, a, b, 0x81C2C92E, w6);
    Round(b, c, d, e, f, g, h, a, 0x92722C85, w7);
    Round(a, b, c, d, e, f, g, h, 0xA2BFE8A1, w8);
    Round(h, a, b, c, d, e, f, g, 0xA81A664B, w9);
    Round(g, h, a, b, c, d, e, f, 0xC24B8B70, w10);
    Round(f, g, h, a, b, c, d, e, 0xC76C51A3, w11);
    Round(e, f, g, h, a, b, c, d, 0xD192E819, w12);
    Round(d, e, f, g, h, a, b, c, 0xD6990624, w13);
    Round(c, d, e, f, g, h, a, b, 0xF40E3585, w14);
    Round(b, c, d, e, f, g, h, a, 0x106AA070, w15);

    WMIX()

    Round(a, b, c, d, e, f, g, h, 0x19A4C116, w0);
    Round(h, a, b, c, d, e, f, g, 0x1E376C08, w1);
    Round(g, h, a, b, c, d, e, f, 0x2748774C, w2);
    Round(f, g, h, a, b, c, d, e, 0x34B0BCB5, w3);
    Round(e, f, g, h, a, b, c, d, 0x391C0CB3, w4);
    Round(d, e, f, g, h, a, b, c, 0x4ED8AA4A, w5);
    Round(c, d, e, f, g, h, a, b, 0x5B9CCA4F, w6);
    Round(b, c, d, e, f, g, h, a, 0x682E6FF3, w7);
    Round(a, b, c, d, e, f, g, h, 0x748F82EE, w8);
    Round(h, a, b, c, d, e, f, g, 0x78A5636F, w9);
    Round(g, h, a, b, c, d, e, f, 0x84C87814, w10);
    Round(f, g, h, a, b, c, d, e, 0x8CC70208, w11);
    Round(e, f, g, h, a, b, c, d, 0x90BEFFFA, w12);
    Round(d, e, f, g, h, a, b, c, 0xA4506CEB, w13);
    Round(c, d, e, f, g, h, a, b, 0xBEF9A3F7, w14);
    Round(b, c, d, e, f, g, h, a, 0xC67178F2, w15);

  s[0] = _mm256_add_epi32(a, s[0]);
  s[1] = _mm256_add_epi32(b, s[1]);
  s[2] = _mm256_add_epi32(c, s[2]);
  s[3] = _mm256_add_epi32(d, s[3]);
  s[4] = _mm256_add_epi32(e, s[4]);
  s[5] = _mm256_add_epi32(f, s[5]);
  s[6] = _mm256_add_epi32(g, s[6]);
  s[7] = _mm256_add_epi32(h, s[7]);
}

AVX2_FN static inline void sha256_init(__m256i *s) {
  s[0] = _mm256_set1_epi32(0x6a09e667);
  s[1] = _mm256_set1_epi32(0xbb67ae85);
  s[2] = _mm256_set1_epi32(0x3c6ef372);
  s[3] = _mm256_set1_epi32(0xa54ff53a);
  s[4] = _mm256_set1_epi32(0x510e527f);
  s[5] = _mm256_set1_epi32(0x9b05688c);
  s[6] = _mm256_set1_epi32(0x1f83d9ab);
  s[7] = _mm256_set1_epi32(0x5be0cd19);
}

#undef Maj
#undef Ch
#undef ROR
#undef SHR
#undef S0
#undef S1
#undef s0
#undef s1
#undef Round

// RIPEMD160 ------------------------------------------------------------------

#define ROL(x,n) _mm256_or_si256(_mm256_slli_epi32(x, n), _mm256_srli_epi32(x, 32 - n))
#define NOT(x) _mm256_xor_si256(x, _mm256_set1_epi32(-1))

#define f1(x,y,z) _mm256_xor_si256(x, _mm256_xor_si256(y, z))
#define f2(x,y,z) _mm256_or_si256(_mm256_and_si256(x,y),_mm256_andnot_si256(x,z))
#define f3(x,y,z) _mm256_xor_si256(_mm256_or_si256(x,NOT(y)),z)
#define f4(x,y,z) _mm256_or_si256(_mm256_and_si256(x,z),_mm256_andnot_si256(z,y))
#define f5(x,y,z) _mm256_xor_si256(x,_mm256_or_si256(y,NOT(z)))

#define Round(a,b,c,d,e,f,x,k,r) \
  u = add4(a,f,x,_mm256_set1_epi32(k)); \
  a = _mm256_add_epi32(ROL(u, r),e); \
  c = ROL(c, 10);

#define R11(a,b,c,d,e,x,r) Round(a, b, c, d, e, f1(b, c, d), x, 0, r)
#define R21(a,b,c,d,e,x,r) Round(a, b, c, d, e, f2(b, c, d), x, 0x5A827999ul, r)
#define R31(a,b,c,d,e,x,r) Round(a, b, c, d, e, f3(b, c, d), x, 0x6ED9EBA1ul, r)
#define R41(a,b,c,d,e,x,r) Round(a, b, c, d, e, f4(b, c, d), x, 0x8F1BBCDCul, r)
#define R51(a,b,c,d,e,x,r) Round(a, b, c, d, e, f5(b, c, d), x, 0xA953FD4Eul, r)
#define R12(a,b,c,d,e,x,r) Round(a, b, c, d, e, f5(b, c, d), x, 0x50A28BE6ul, r)
#define R22(a,b,c,d,e,x,r) Round(a, b, c, d, e, f4(b, c, d), x, 0x5C4DD124ul, r)
#define R32(a,b,c,d,e,x,r) Round(a, b, c, d, e, f3(b, c, d), x, 0x6D703EF3ul, r)
#define R42(a,b,c,d,e,x,r) Round(a, b, c, d, e, f2(b, c, d), x, 0x7A6D76E9ul, r)
#define R52(a,b,c,d,e,x,r) Round(a, b, c, d, e, f1(b, c, d), x, 0, r)

// RIPEMD160 of the 32 byte SHA256 digest held in the lane-parallel state
// sha[0..7] (big endian words). Result is returned in out[0..4] as the
// little endian words of the 20 byte digest.
AVX2_FN static inline void ripemd160_of_sha(const __m256i *sha, __m256i *out) {
  const __m256i bswap = _mm256_setr_epi8(
    3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
    3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  __m256i w[16];
  for (int i = 0; i < 8; i++)
    w[i] = _mm256_shuffle_epi8(sha[i], bswap);
  w[8] = _mm256_set1_epi32(0x80);   // padding: 0x80, then zeros
  w[9] = w[10] = w[11] = w[12] = w[13] = _mm256_setzero_si256();
  w[14] = _mm256_set1_epi32(32 << 3);   // message length in bits
  w[15] = _mm256_setzero_si256();

  __m256i s0 = _mm256_set1_epi32(0x67452301);
  __m256i s1 = _mm256_set1_epi32(0xEFCDAB89);
  __m256i s2 = _mm256_set1_epi32(0x98BADCFE);
  __m256i s3 = _mm256_set1_epi32(0x10325476);
  __m256i s4 = _mm256_set1_epi32(0xC3D2E1F0);
  __m256i a1 = s0, b1 = s1, c1 = s2, d1 = s3, e1 = s4;
  __m256i a2 = s0, b2 = s1, c2 = s2, d2 = s3, e2 = s4;
  __m256i u;

    R11(a1, b1, c1, d1, e1, w[0], 11);
    R12(a2, b2, c2, d2, e2, w[5], 8);
    R11(e1, a1, b1, c1, d1, w[1], 14);
    R12(e2, a2, b2, c2, d2, w[14], 9);
    R11(d1, e1, a1, b1, c1, w[2], 15);
    R12(d2, e2, a2, b2, c2, w[7], 9);
    R11(c1, d1, e1, a1, b1, w[3], 12);
    R12(c2, d2, e2, a2, b2, w[0], 11);
    R11(b1, c1, d1, e1, a1, w[4], 5);
    R12(b2, c2, d2, e2, a2, w[9], 13);
    R11(a1, b1, c1, d1, e1, w[5], 8);
    R12(a2, b2, c2, d2, e2, w[2], 15);
    R11(e1, a1, b1, c1, d1, w[6], 7);
    R12(e2, a2, b2, c2, d2, w[11], 15);
    R11(d1, e1, a1, b1, c1, w[7], 9);
    R12(d2, e2, a2, b2, c2, w[4], 5);
    R11(c1, d1, e1, a1, b1, w[8], 11);
    R12(c2, d2, e2, a2, b2, w[13], 7);
    R11(b1, c1, d1, e1, a1, w[9], 13);
    R12(b2, c2, d2, e2, a2, w[6], 7);
    R11(a1, b1, c1, d1, e1, w[10], 14);
    R12(a2, b2, c2, d2, e2, w[15], 8);
    R11(e1, a1, b1, c1, d1, w[11], 15);
    R12(e2, a2, b2, c2, d2, w[8], 11);
    R11(d1, e1, a1, b1, c1, w[12], 6);
    R12(d2, e2, a2, b2, c2, w[1], 14);
    R11(c1, d1, e1, a1, b1, w[13], 7);
    R12(c2, d2, e2, a2, b2, w[10], 14);
    R11(b1, c1, d1, e1, a1, w[14], 9);
    R12(b2, c2, d2, e2, a2, w[3], 12);
    R11(a1, b1, c1, d1, e1, w[15], 8);
    R12(a2, b2, c2, d2, e2, w[12], 6);

    R21(e1, a1, b1, c1, d1, w[7], 7);
    R22(e2, a2, b2, c2, d2, w[6], 9);
    R21(d1, e1, a1, b1, c1, w[4], 6);
    R22(d2, e2, a2, b2, c2, w[11], 13);
    R21(c1, d1, e1, a1, b1, w[13], 8);
    R22(c2, d2, e2, a2, b2, w[3], 15);
    R21(b1, c1, d1, e1, a1, w[1], 13);
    R22(b2, c2, d2, e2, a2, w[7], 7);
    R21(a1, b1, c1, d1, e1, w[10], 11);
    R22(a2, b2, c2, d2, e2, w[0], 12);
    R21(e1, a1, b1, c1, d1, w[6], 9);
    R22(e2, a2, b2, c2, d2, w[13], 8);
    R21(d1, e1, a1, b1, c1, w[15], 7);
    R22(d2, e2, a2, b2, c2, w[5], 9);
    R21(c1, d1, e1, a1, b1, w[3], 15);
    R22(c2, d2, e2, a2, b2, w[10], 11);
    R21(b1, c1, d1, e1, a1, w[12], 7);
    R22(b2, c2, d2, e2, a2, w[14], 7);
    R21(a1, b1, c1, d1, e1, w[0], 12);
    R22(a2, b2, c2, d2, e2, w[15], 7);
    R21(e1, a1, b1, c1, d1, w[9], 15);
    R22(e2, a2, b2, c2, d2, w[8], 12);
    R21(d1, e1, a1, b1, c1, w[5], 9);
    R22(d2, e2, a2, b2, c2, w[12], 7);
    R21(c1, d1, e1, a1, b1, w[2], 11);
    R22(c2, d2, e2, a2, b2, w[4], 6);
    R21(b1, c1, d1, e1, a1, w[14], 7);
    R22(b2, c2, d2, e2, a2, w[9], 15);
    R21(a1, b1, c1, d1, e1, w[11], 13);
    R22(a2, b2, c2, d2, e2, w[1], 13);
    R21(e1, a1, b1, c1, d1, w[8], 12);
    R22(e2, a2, b2, c2, d2, w[2], 11);

    R31(d1, e1, a1, b1, c1, w[3], 11);
    R32(d2, e2, a2, b2, c2, w[15], 9);
    R31(c1, d1, e1, a1, b1, w[10], 13);
    R32(c2, d2, e2, a2, b2, w[5], 7);
    R31(b1, c1, d1, e1, a1, w[14], 6);
    R32(b2, c2, d2, e2, a2, w[1], 15);
    R31(a1, b1, c1, d1, e1, w[4], 7);
    R32(a2, b2, c2, d2, e2, w[3], 11);
    R31(e1, a1, b1, c1, d1, w[9], 14);
    R32(e2, a2, b2, c2, d2, w[7], 8);
    R31(d1, e1, a1, b1, c1, w[15], 9);
    R32(d2, e2, a2, b2, c2, w[14], 6);
    R31(c1, d1, e1, a1, b1, w[8], 13);
    R32(c2, d2, e2, a2, b2, w[6], 6);
    R31(b1, c1, d1, e1, a1, w[1], 15);
    R32(b2, c2, d2, e2, a2, w[9], 14);
    R31(a1, b1, c1, d1, e1, w[2], 14);
    R32(a2, b2, c2, d2, e2, w[11], 12);
    R31(e1, a1, b1, c1, d1, w[7], 8);
    R32(e2, a2, b2, c2, d2, w[8], 13);
    R31(d1, e1, a1, b1, c1, w[0], 13);
    R32(d2, e2, a2, b2, c2, w[12], 5);
    R31(c1, d1, e1, a1, b1, w[6], 6);
    R32(c2, d2, e2, a2, b2, w[2], 14);
    R31(b1, c1, d1, e1, a1, w[13], 5);
    R32(b2, c2, d2, e2, a2, w[10], 13);
    R31(a1, b1, c1, d1, e1, w[11], 12);
    R32(a2, b2, c2, d2, e2, w[0], 13);
    R31(e1, a1, b1, c1, d1, w[5], 7);
    R32(e2, a2, b2, c2, d2, w[4], 7);
    R31(d1, e1, a1, b1, c1, w[12], 5);
    R32(d2, e2, a2, b2, c2, w[13], 5);

    R41(c1, d1, e1, a1, b1, w[1], 11);
    R42(c2, d2, e2, a2, b2, w[8], 15);
    R41(b1, c1, d1, e1, a1, w[9], 12);
    R42(b2, c2, d2, e2, a2, w[6], 5);
    R41(a1, b1, c1, d1, e1, w[11], 14);
    R42(a2, b2, c2, d2, e2, w[4], 8);
    R41(e1, a1, b1, c1, d1, w[10], 15);
    R42(e2, a2, b2, c2, d2, w[1], 11);
    R41(d1, e1, a1, b1, c1, w[0], 14);
    R42(d2, e2, a2, b2, c2, w[3], 14);
    R41(c1, d1, e1, a1, b1, w[8], 15);
    R42(c2, d2, e2, a2, b2, w[11], 14);
    R41(b1, c1, d1, e1, a1, w[12], 9);
    R42(b2, c2, d2, e2, a2, w[15], 6);
    R41(a1, b1, c1, d1, e1, w[4], 8);
    R42(a2, b2, c2, d2, e2, w[0], 14);
    R41(e1, a1, b1, c1, d1, w[13], 9);
    R42(e2, a2, b2, c2, d2, w[5], 6);
    R41(d1, e1, a1, b1, c1, w[3], 14);
    R42(d2, e2, a2, b2, c2, w[12], 9);
    R41(c1, d1, e1, a1, b1, w[7], 5);
    R42(c2, d2, e2, a2, b2, w[2], 12);
    R41(b1, c1, d1, e1, a1, w[15], 6);
    R42(b2, c2, d2, e2, a2, w[13], 9);
    R41(a1, b1, c1, d1, e1, w[14], 8);
    R42(a2, b2, c2, d2, e2, w[9], 12);
    R41(e1, a1, b1, c1, d1, w[5], 6);
    R42(e2, a2, b2, c2, d2, w[7], 5);
    R41(d1, e1, a1, b1, c1, w[6], 5);
    R42(d2, e2, a2, b2, c2, w[10], 15);
    R41(c1, d1, e1, a1, b1, w[2], 12);
    R42(c2, d2, e2, a2, b2, w[14], 8);

    R51(b1, c1, d1, e1, a1, w[4], 9);
    R52(b2, c2, d2, e2, a2, w[12], 8);
    R51(a1, b1, c1, d1, e1, w[0], 15);
    R52(a2, b2, c2, d2, e2, w[15], 5);
    R51(e1, a1, b1, c1, d1, w[5], 5);
    R52(e2, a2, b2, c2, d2, w[10], 12);
    R51(d1, e1, a1, b1, c1, w[9], 11);
    R52(d2, e2, a2, b2, c2, w[4], 9);
    R51(c1, d1, e1, a1, b1, w[7], 6);
    R52(c2, d2, e2, a2, b2, w[1], 12);
    R51(b1, c1, d1, e1, a1, w[12], 8);
    R52(b2, c2, d2, e2, a2, w[5], 5);
    R51(a1, b1, c1, d1, e1, w[2], 13);
    R52(a2, b2, c2, d2, e2, w[8], 14);
    R51(e1, a1, b1, c1, d1, w[10], 12);
    R52(e2, a2, b2, c2, d2, w[7], 6);
    R51(d1, e1, a1, b1, c1, w[14], 5);
    R52(d2, e2, a2, b2, c2, w[6], 8);
    R51(c1, d1, e1, a1, b1, w[1], 12);
    R52(c2, d2, e2, a2, b2, w[2], 13);
    R51(b1, c1, d1, e1, a1, w[3], 13);
    R52(b2, c2, d2, e2, a2, w[13], 6);
    R51(a1, b1, c1, d1, e1, w[8], 14);
    R52(a2, b2, c2, d2, e2, w[14], 5);
    R51(e1, a1, b1, c1, d1, w[11], 11);
    R52(e2, a2, b2, c2, d2, w[0], 15);
    R51(d1, e1, a1, b1, c1, w[6], 8);
    R52(d2, e2, a2, b2, c2, w[3], 13);
    R51(c1, d1, e1, a1, b1, w[15], 5);
    R52(c2, d2, e2, a2, b2, w[9], 11);
    R51(b1, c1, d1, e1, a1, w[13], 6);
    R52(b2, c2, d2, e2, a2, w[11], 11);

  out[0] = add3(s1, c1, d2);
  out[1] = add3(s2, d1, e2);
  out[2] = add3(s3, e1, a2);
  out[3] = add3(s4, a1, b2);
  out[4] = add3(s0, b1, c2);
}

// Write the 5 lane-parallel result words as 8 consecutive 20 byte digests.
AVX2_FN static inline void store_digests(const __m256i *h, uint8_t *const out[8]) {
  __m256i r[8];
  for (int i = 0; i < 5; i++) r[i] = h[i];
  r[5] = r[6] = r[7] = _mm256_setzero_si256();
  transpose8(r);
  for (int l = 0; l < 8; l++) {
    _mm_storeu_si128((__m128i *)out[l], _mm256_castsi256_si128(r[l]));
    uint32_t last = (uint32_t)_mm256_extract_epi32(r[l], 4);
    memcpy(out[l] + 16, &last, 4);
  }
}

} // namespace

AVX2_FN void hash160_avx2_1B(const uint32_t *const in[8], uint8_t *const out[8]) {
  __m256i w[16], s[8], h[5];
  load_block(in, 0, w);
  sha256_init(s);
  sha256_block(s, w);
  ripemd160_of_sha(s, h);
  store_digests(h, out);
}

AVX2_FN void hash160_avx2_2B(const uint32_t *const in[8], uint8_t *const out[8]) {
  __m256i w[16], s[8], h[5];
  sha256_init(s);
  load_block(in, 0, w);
  sha256_block(s, w);
  load_block(in, 16, w);
  sha256_block(s, w);
  ripemd160_of_sha(s, h);
  store_digests(h, out);
}

#else  // not x86

bool hash160_avx2_available() { return false; }
void hash160_avx2_1B(const uint32_t *const[8], uint8_t *const[8]) {}
void hash160_avx2_2B(const uint32_t *const[8], uint8_t *const[8]) {}

#endif
