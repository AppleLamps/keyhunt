/*
 * Lane parallel secp256k1 field multiply, radix 2^29, on 64 bit lanes with a
 * 32x32->64 multiply (vpmuludq). Included once per vector extension by
 * FieldMulSimd.cpp with these macros defined:
 *
 *   VEC          vector type (LANES 64 bit lanes)
 *   LANES        4 (AVX2) or 8 (AVX-512)
 *   KFN          function attributes (target(...))
 *   SFX          suffix of the generated function: fieldmul_<SFX>
 *   VMUL32(a,b)  low 32 bits of each lane of a times low 32 bits of b -> 64 bit
 *   VADD(a,b) VAND(a,b) VOR(a,b) VSRL(x,n) VSLL(x,n)   64 bit lane ops
 *   VSET1(c) VZERO()
 *   VLOAD4(W, p)   W[0..3] = words 0..3 of the LANES Ints at p, lane parallel
 *   VSTORE4(p, W)  the inverse, also clearing the sign limb bits64[4]
 *
 * The result is bit exact with Int::ModMulK1: the 512 bit product is computed
 * exactly, then reduced exactly the way the scalar code does it
 * (lo + hi*K with K = 2^256 mod p = 0x1000003D1, once more on the overflow,
 * final carry out of bit 256 dropped).
 *
 * A 256 bit value is 9 limbs of 29 bits (the top one 24 bits). Products of
 * two limbs are < 2^58 and a column of the 9x9 schoolbook sums at most 9 of
 * them, so nothing overflows a 64 bit lane before the carries are propagated.
 */

#define FM_CAT_(a, b) a##b
#define FM_CAT(a, b) FM_CAT_(a, b)
#define FM_FN(name) FM_CAT(name, FM_CAT(_, SFX))

// 4 x 64 bit words -> 9 x 29 bit limbs
#define FM_SPLIT29(L, W0, W1, W2, W3) do { \
  L[0] = VAND(W0, M29); \
  L[1] = VAND(VSRL(W0, 29), M29); \
  L[2] = VAND(VOR(VSRL(W0, 58), VSLL(W1, 6)), M29); \
  L[3] = VAND(VSRL(W1, 23), M29); \
  L[4] = VAND(VOR(VSRL(W1, 52), VSLL(W2, 12)), M29); \
  L[5] = VAND(VSRL(W2, 17), M29); \
  L[6] = VAND(VOR(VSRL(W2, 46), VSLL(W3, 18)), M29); \
  L[7] = VAND(VSRL(W3, 11), M29); \
  L[8] = VSRL(W3, 40); \
} while (0)

// Propagate the carries of limbs [0, n) of C upwards (C[n] receives the last one)
#define FM_NORMALIZE(C, n) do { \
  for (int _k = 0; _k < (n); _k++) { \
    C[_k + 1] = VADD(C[_k + 1], VSRL(C[_k], 29)); \
    C[_k] = VAND(C[_k], M29); \
  } \
} while (0)

// Normalize and reduce an exact product in 18 radix 2^29 limbs, then store it.
// Keeping this common tail separate lets the squarer avoid the duplicate half
// of the schoolbook product without duplicating the fairly subtle reduction.
KFN static inline void FM_FN(reduce_store)(Int *r, VEC C[18]) {
  const VEC M29 = VSET1(0x1FFFFFFFULL);
  const VEC M24 = VSET1(0x00FFFFFFULL);
  VEC S[11], H[9], W[4];

  // Exact 512 bit product in 18 normalized limbs (C[17] < 2^19)
  FM_NORMALIZE(C, 17);

  // hi = product >> 256: bit 256 is bit 24 of limb 8
  for (int j = 0; j < 9; j++)
    H[j] = VAND(VOR(VSRL(C[8 + j], 24), VSLL(C[9 + j], 5)), M29);
  // S = lo + hi*K, K = 2^32 + 977 = (977) + (2^3 << 29)
  const VEC K977 = VSET1(977);
  for (int k = 0; k < 8; k++) S[k] = C[k];
  S[8] = VAND(C[8], M24);
  S[9] = VZERO();
  for (int k = 0; k < 9; k++) {
    S[k] = VADD(S[k], VMUL32(H[k], K977));
    S[k + 1] = VADD(S[k + 1], VSLL(H[k], 3));
  }
  S[10] = VZERO();
  FM_NORMALIZE(S, 9);      // S < 2^290 fits 10 limbs; S[9] < 2^29

  // Second round: shi = S >> 256 (< 2^34), lo + shi*K, mod 2^256
  // shi*977 = (S[8] >> 24)*977 + (S[9]*977) << 5, both factors < 2^32
  VEC s8h = VSRL(S[8], 24);
  VEC shi = VOR(s8h, VSLL(S[9], 5));
  S[8] = VAND(S[8], M24);
  S[0] = VADD(S[0], VADD(VMUL32(s8h, K977), VSLL(VMUL32(S[9], K977), 5)));
  S[1] = VADD(S[1], VSLL(shi, 3));
  FM_NORMALIZE(S, 8);
  S[8] = VAND(S[8], M24);  // drop the carry out of bit 256, like the scalar code

  // 9 x 29 bit limbs -> 4 x 64 bit words, transpose back to the lanes' Ints
  W[0] = VOR(VOR(S[0], VSLL(S[1], 29)), VSLL(S[2], 58));
  W[1] = VOR(VOR(VSRL(S[2], 6), VSLL(S[3], 23)), VSLL(S[4], 52));
  W[2] = VOR(VOR(VSRL(S[4], 12), VSLL(S[5], 17)), VSLL(S[6], 46));
  W[3] = VOR(VOR(VSRL(S[6], 18), VSLL(S[7], 11)), VSLL(S[8], 40));
  VSTORE4(r, W);
}

// r[0..LANES) = a[0..LANES) * b[0..LANES) mod p, as Int::ModMulK1 computes it
KFN static void FM_FN(fieldmul)(Int *r, const Int *a, const Int *b) {
  const VEC M29 = VSET1(0x1FFFFFFFULL);
  VEC A[9], B[9], C[18], W[4];

  // Transpose the operands to lane parallel layout
  VLOAD4(W, a);
  FM_SPLIT29(A, W[0], W[1], W[2], W[3]);
  VLOAD4(W, b);
  FM_SPLIT29(B, W[0], W[1], W[2], W[3]);

  // 9x9 schoolbook, column by column (one live accumulator: fewer spills on
  // the 16 register AVX2 target): C[k] = sum A[i]*B[k-i]
#pragma GCC unroll 17
  for (int k = 0; k < 17; k++) {
    VEC acc = VZERO();
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++)
      if (k - i >= 0 && k - i < 9) acc = VADD(acc, VMUL32(A[i], B[k - i]));
    C[k] = acc;
  }
  C[17] = VZERO();
  FM_FN(reduce_store)(r, C);
}

// r[0..LANES) = a[0..LANES)^2 mod p. Cross products occur twice, so compute
// each only once and double it: 45 vector multiplies instead of the generic
// multiply's 81. Accumulator bounds are unchanged from the full schoolbook.
KFN static void FM_FN(fieldsqr)(Int *r, const Int *a) {
  const VEC M29 = VSET1(0x1FFFFFFFULL);
  VEC A[9], C[18], W[4];

  VLOAD4(W, a);
  FM_SPLIT29(A, W[0], W[1], W[2], W[3]);

#pragma GCC unroll 17
  for (int k = 0; k < 17; k++) {
    VEC acc = VZERO();
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) {
      int j = k - i;
      if (j >= i && j < 9) {
        VEC p = VMUL32(A[i], A[j]);
        acc = VADD(acc, i == j ? p : VADD(p, p));
      }
    }
    C[k] = acc;
  }
  C[17] = VZERO();
  FM_FN(reduce_store)(r, C);
}

#undef FM_SPLIT29
#undef FM_NORMALIZE
#undef FM_FN
#undef FM_CAT
#undef FM_CAT_
