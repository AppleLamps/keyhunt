/*
 * Lane parallel SHA256 + RIPEMD160 ("hash160") kernel, instantiated by
 * hash160_simd.cpp once per vector extension. The including file defines:
 *
 *   VEC          vector type (__m256i, __m512i)
 *   LANES        32 bit lanes per vector
 *   KFN          function attributes (target(...))
 *   SFX          name suffix: the kernels are hash160_<SFX>_1B / _2B
 *   VADD VXOR VAND VOR VANDN(a,b)=~a&b  VSRL(x,n) VSET1(i) VZERO() VLOAD(p) VSTORE(p,v)
 *   VROR(x,n) VROL(x,n)
 *   VCH(e,f,g)=(e&f)^(~e&g)  VMAJ(a,b,c)  VXOR3(a,b,c)
 *   VF3(x,y,z)=(x|~y)^z  VF4(x,y,z)=(x&z)|(y&~z)  VF5(x,y,z)=x^(y|~z)
 *   VBSWAP(x)    byte swap every 32 bit lane
 *
 * Derived from the 4-way SSE kernels (hash/sha256_sse.cpp, hash/ripemd160_sse.cpp,
 * VanitySearch, Copyright (c) 2019 Jean Luc PONS, GPLv3). The SHA256 digest is
 * fed straight into RIPEMD160 in registers.
 */

#define KCAT_(a, b, c) a##b##c
#define KCAT(a, b, c) KCAT_(a, b, c)
#define KNAME(n) KCAT(hash160_, SFX, n)

namespace {

#define S0(x) VXOR3(VROR((x), 2), VROR((x), 13), VROR((x), 22))
#define S1(x) VXOR3(VROR((x), 6), VROR((x), 11), VROR((x), 25))
#define s0(x) VXOR3(VROR((x), 7), VROR((x), 18), VSRL((x), 3))
#define s1(x) VXOR3(VROR((x), 17), VROR((x), 19), VSRL((x), 10))

#define add3(x0, x1, x2) VADD(VADD(x0, x1), x2)
#define add4(x0, x1, x2, x3) VADD(VADD(x0, x1), VADD(x2, x3))
#define add5(x0, x1, x2, x3, x4) VADD(add3(x0, x1, x2), VADD(x3, x4))

#define Round(a, b, c, d, e, f, g, h, i, w)          \
    T1 = add5(h, S1(e), VCH(e, f, g), VSET1(i), w);  \
    d = VADD(d, T1);                                 \
    T2 = VADD(S0(a), VMAJ(a, b, c));                 \
    h = VADD(T1, T2);

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

// One SHA256 block over the lane parallel state s[0..7], message words
// in[0..15] in lane parallel layout.
KFN static inline void KNAME(_sha256_block)(VEC *s, const uint32_t *in) {
  VEC a = s[0], b = s[1], c = s[2], d = s[3];
  VEC e = s[4], f = s[5], g = s[6], h = s[7];
  VEC w0 = VLOAD(in + 0 * LANES), w1 = VLOAD(in + 1 * LANES), w2 = VLOAD(in + 2 * LANES), w3 = VLOAD(in + 3 * LANES);
  VEC w4 = VLOAD(in + 4 * LANES), w5 = VLOAD(in + 5 * LANES), w6 = VLOAD(in + 6 * LANES), w7 = VLOAD(in + 7 * LANES);
  VEC w8 = VLOAD(in + 8 * LANES), w9 = VLOAD(in + 9 * LANES), w10 = VLOAD(in + 10 * LANES), w11 = VLOAD(in + 11 * LANES);
  VEC w12 = VLOAD(in + 12 * LANES), w13 = VLOAD(in + 13 * LANES), w14 = VLOAD(in + 14 * LANES), w15 = VLOAD(in + 15 * LANES);
  VEC T1, T2;

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

  s[0] = VADD(a, s[0]);
  s[1] = VADD(b, s[1]);
  s[2] = VADD(c, s[2]);
  s[3] = VADD(d, s[3]);
  s[4] = VADD(e, s[4]);
  s[5] = VADD(f, s[5]);
  s[6] = VADD(g, s[6]);
  s[7] = VADD(h, s[7]);
}

KFN static inline void KNAME(_sha256_init)(VEC *s) {
  s[0] = VSET1(0x6a09e667);
  s[1] = VSET1(0xbb67ae85);
  s[2] = VSET1(0x3c6ef372);
  s[3] = VSET1(0xa54ff53a);
  s[4] = VSET1(0x510e527f);
  s[5] = VSET1(0x9b05688c);
  s[6] = VSET1(0x1f83d9ab);
  s[7] = VSET1(0x5be0cd19);
}

#undef S0
#undef S1
#undef s0
#undef s1
#undef Round
#undef WMIX

// RIPEMD160 ------------------------------------------------------------------

#define RRound(a,b,c,d,e,f,x,k,r) \
  u = add4(a,f,x,VSET1(k)); \
  a = VADD(VROL(u, r),e); \
  c = VROL(c, 10);

#define R11(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VXOR3(b, c, d), x, 0, r)
#define R21(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VCH(b, c, d), x, 0x5A827999ul, r)
#define R31(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VF3(b, c, d), x, 0x6ED9EBA1ul, r)
#define R41(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VF4(b, c, d), x, 0x8F1BBCDCul, r)
#define R51(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VF5(b, c, d), x, 0xA953FD4Eul, r)
#define R12(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VF5(b, c, d), x, 0x50A28BE6ul, r)
#define R22(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VF4(b, c, d), x, 0x5C4DD124ul, r)
#define R32(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VF3(b, c, d), x, 0x6D703EF3ul, r)
#define R42(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VCH(b, c, d), x, 0x7A6D76E9ul, r)
#define R52(a,b,c,d,e,x,r) RRound(a, b, c, d, e, VXOR3(b, c, d), x, 0, r)

// RIPEMD160 of the 32 byte SHA256 digest held in the lane parallel state
// sha[0..7] (big endian words). Result in out[0..4]: the little endian words
// of the 20 byte digest.
KFN static inline void KNAME(_ripemd160_of_sha)(const VEC *sha, VEC *out) {
  VEC w[16];
  for (int i = 0; i < 8; i++)
    w[i] = VBSWAP(sha[i]);
  w[8] = VSET1(0x80);   // padding: 0x80, then zeros
  w[9] = w[10] = w[11] = w[12] = w[13] = VZERO();
  w[14] = VSET1(32 << 3);   // message length in bits
  w[15] = VZERO();

  VEC s0 = VSET1(0x67452301);
  VEC s1 = VSET1(0xEFCDAB89);
  VEC s2 = VSET1(0x98BADCFE);
  VEC s3 = VSET1(0x10325476);
  VEC s4 = VSET1(0xC3D2E1F0);
  VEC a1 = s0, b1 = s1, c1 = s2, d1 = s3, e1 = s4;
  VEC a2 = s0, b2 = s1, c2 = s2, d2 = s3, e2 = s4;
  VEC u;

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

#undef RRound
#undef R11
#undef R21
#undef R31
#undef R41
#undef R51
#undef R12
#undef R22
#undef R32
#undef R42
#undef R52
#undef add3
#undef add4
#undef add5

// Write the 5 lane parallel result words as LANES separate 20 byte digests.
KFN static inline void KNAME(_store_digests)(const VEC *h, uint8_t *const *out) {
  uint32_t t[5][LANES];
  for (int i = 0; i < 5; i++) VSTORE(t[i], h[i]);
  for (int l = 0; l < LANES; l++) {
    uint32_t d[5] = { t[0][l], t[1][l], t[2][l], t[3][l], t[4][l] };
    memcpy(out[l], d, 20);
  }
}

} // namespace

KFN void KNAME(_1B)(const uint32_t *w, uint8_t *const out[LANES]) {
  VEC s[8], h[5];
  KNAME(_sha256_init)(s);
  KNAME(_sha256_block)(s, w);
  KNAME(_ripemd160_of_sha)(s, h);
  KNAME(_store_digests)(h, out);
}

KFN void KNAME(_2B)(const uint32_t *w, uint8_t *const out[LANES]) {
  VEC s[8], h[5];
  KNAME(_sha256_init)(s);
  KNAME(_sha256_block)(s, w);
  KNAME(_sha256_block)(s, w + 16 * LANES);
  KNAME(_ripemd160_of_sha)(s, h);
  KNAME(_store_digests)(h, out);
}

#undef KCAT_
#undef KCAT
#undef KNAME
