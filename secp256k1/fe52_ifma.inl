/*
 * secp256k1 field arithmetic on 8 lanes in radix 2^52 (AVX-512 IFMA).
 *
 * A field element is 5 limbs of 52 bits held in 5 zmm registers, one
 * element per 64 bit lane (limb 4 holds the top 48 bits of a normalized
 * value). The multiply is the one of FieldMulSimd.cpp (vpmadd52luq/huq
 * schoolbook, K = 2^32 + 977 reduction) and is bit exact with
 * Int::ModMulK1 for any inputs below 2^256. The add and sub do what
 * mod_add4 / mod_sub4 in IntMod.cpp do on canonical inputs: exact sum then
 * minus P when the sum reaches P, exact difference then plus P when it is
 * negative. (For a non canonical operand, a value in [P, 2^256), the scalar
 * sub wraps modulo 2^256 where this one would go negative; such operands do
 * not occur in the group addition, ModMulK1 produces one with probability
 * about 2^-128.)
 *
 * Included by FieldMulSimd.cpp and GroupAdd52.cpp inside a function
 * attribute scope: the including file defines FE52_FN as the target
 * attribute of the functions.
 */

#define FE52_M52 0xFFFFFFFFFFFFFLL
#define FE52_M48 0xFFFFFFFFFFFFLL
#define FE52_K   0x1000003D1LL

// 4 words of 64 bits (one Int, 8 lanes) -> 5 limbs of 52 bits
FE52_FN static inline void fe52_from_words(__m512i L[5], const __m512i W[4]) {
  const __m512i M52 = _mm512_set1_epi64(FE52_M52);
  L[0] = _mm512_and_si512(W[0], M52);
  L[1] = _mm512_and_si512(_mm512_or_si512(_mm512_srli_epi64(W[0], 52), _mm512_slli_epi64(W[1], 12)), M52);
  L[2] = _mm512_and_si512(_mm512_or_si512(_mm512_srli_epi64(W[1], 40), _mm512_slli_epi64(W[2], 24)), M52);
  L[3] = _mm512_and_si512(_mm512_or_si512(_mm512_srli_epi64(W[2], 28), _mm512_slli_epi64(W[3], 36)), M52);
  L[4] = _mm512_srli_epi64(W[3], 16);
}

// 5 normalized limbs (limb 4 below 2^48) -> 4 words
FE52_FN static inline void fe52_to_words(__m512i W[4], const __m512i L[5]) {
  W[0] = _mm512_or_si512(L[0], _mm512_slli_epi64(L[1], 52));
  W[1] = _mm512_or_si512(_mm512_srli_epi64(L[1], 12), _mm512_slli_epi64(L[2], 40));
  W[2] = _mm512_or_si512(_mm512_srli_epi64(L[2], 24), _mm512_slli_epi64(L[3], 28));
  W[3] = _mm512_or_si512(_mm512_srli_epi64(L[3], 36), _mm512_slli_epi64(L[4], 16));
}

// Carry propagation over n limbs (unsigned: limbs below 2^64)
#define FE52_NORMALIZE(C, n) do { \
  for (int _k = 0; _k < (n); _k++) { \
    C[_k + 1] = _mm512_add_epi64(C[_k + 1], _mm512_srli_epi64(C[_k], 52)); \
    C[_k] = _mm512_and_si512(C[_k], M52); \
  } \
} while (0)

// Same with signed limbs (arithmetic shift carries a negative limb down)
#define FE52_NORMALIZE_S(C, n) do { \
  for (int _k = 0; _k < (n); _k++) { \
    C[_k + 1] = _mm512_add_epi64(C[_k + 1], _mm512_srai_epi64(C[_k], 52)); \
    C[_k] = _mm512_and_si512(C[_k], M52); \
  } \
} while (0)

// R = A * B mod p, the exact value Int::ModMulK1 gives (inputs below 2^256,
// normalized limbs; output normalized, limb 4 below 2^48). R may alias A or B.
FE52_FN static inline void fe52_mul(__m512i R[5], const __m512i A[5], const __m512i B[5]) {
  const __m512i M52 = _mm512_set1_epi64(FE52_M52);
  const __m512i M48 = _mm512_set1_epi64(FE52_M48);
  const __m512i K = _mm512_set1_epi64(FE52_K);
  __m512i C[11], H[5], S[7];

  for (int k = 0; k < 11; k++) C[k] = _mm512_setzero_si512();
#pragma GCC unroll 5
  for (int i = 0; i < 5; i++) {
#pragma GCC unroll 5
    for (int j = 0; j < 5; j++) {
      C[i + j] = _mm512_madd52lo_epu64(C[i + j], A[i], B[j]);
      C[i + j + 1] = _mm512_madd52hi_epu64(C[i + j + 1], A[i], B[j]);
    }
  }
  // Exact 512 bit product in 10 normalized limbs (C[9] < 2^44)
  FE52_NORMALIZE(C, 9);

  // hi = product >> 256: bit 256 is bit 48 of limb 4
  for (int j = 0; j < 5; j++)
    H[j] = _mm512_and_si512(_mm512_or_si512(_mm512_srli_epi64(C[4 + j], 48), _mm512_slli_epi64(C[5 + j], 4)), M52);
  // S = lo + hi*K  (< 2^290: 6 limbs)
  for (int k = 0; k < 4; k++) S[k] = C[k];
  S[4] = _mm512_and_si512(C[4], M48);
  S[5] = _mm512_setzero_si512();
  S[6] = _mm512_setzero_si512();
  for (int k = 0; k < 5; k++) {
    S[k] = _mm512_madd52lo_epu64(S[k], H[k], K);
    S[k + 1] = _mm512_madd52hi_epu64(S[k + 1], H[k], K);
  }
  FE52_NORMALIZE(S, 5);

  // Second round: shi = S >> 256 (< 2^34), lo + shi*K, mod 2^256
  __m512i shi = _mm512_or_si512(_mm512_srli_epi64(S[4], 48), _mm512_slli_epi64(S[5], 4));
  S[4] = _mm512_and_si512(S[4], M48);
  S[0] = _mm512_madd52lo_epu64(S[0], shi, K);
  S[1] = _mm512_madd52hi_epu64(S[1], shi, K);
  FE52_NORMALIZE(S, 4);
  S[4] = _mm512_and_si512(S[4], M48);   // drop the carry out of bit 256, like the scalar code

  for (int k = 0; k < 5; k++) R[k] = S[k];
}

// The field characteristic in 52 bit limbs
FE52_FN static inline void fe52_set_p(__m512i P[5]) {
  P[0] = _mm512_set1_epi64((long long)(0x10000000000000LL - FE52_K));   // 2^52 - K
  P[1] = _mm512_set1_epi64(FE52_M52);
  P[2] = _mm512_set1_epi64(FE52_M52);
  P[3] = _mm512_set1_epi64(FE52_M52);
  P[4] = _mm512_set1_epi64(FE52_M48);
}

// R = A + B, minus P when the sum reaches P (mod_add4 on canonical inputs)
FE52_FN static inline void fe52_add(__m512i R[5], const __m512i A[5], const __m512i B[5], const __m512i P[5]) {
  const __m512i M52 = _mm512_set1_epi64(FE52_M52);
  __m512i T[5], U[5];
  for (int k = 0; k < 5; k++) T[k] = _mm512_add_epi64(A[k], B[k]);
  FE52_NORMALIZE(T, 4);
  for (int k = 0; k < 5; k++) U[k] = _mm512_sub_epi64(T[k], P[k]);
  FE52_NORMALIZE_S(U, 4);
  // U >= 0 (top limb not negative): the sum reached P, take sum - P
  __mmask8 ge = _mm512_cmpge_epi64_mask(U[4], _mm512_setzero_si512());
  for (int k = 0; k < 5; k++) R[k] = _mm512_mask_blend_epi64(ge, T[k], U[k]);
}

// R = A - B, plus P when the difference is negative (mod_sub4 on canonical inputs)
FE52_FN static inline void fe52_sub(__m512i R[5], const __m512i A[5], const __m512i B[5], const __m512i P[5]) {
  const __m512i M52 = _mm512_set1_epi64(FE52_M52);
  __m512i T[5];
  for (int k = 0; k < 5; k++) T[k] = _mm512_sub_epi64(A[k], B[k]);
  FE52_NORMALIZE_S(T, 4);
  __mmask8 neg = _mm512_cmplt_epi64_mask(T[4], _mm512_setzero_si512());
  for (int k = 0; k < 5; k++) T[k] = _mm512_mask_add_epi64(T[k], neg, T[k], P[k]);
  FE52_NORMALIZE_S(T, 4);
  for (int k = 0; k < 5; k++) R[k] = T[k];
}

// 8 Ints, given by the addresses of their bits64, -> limbs (an 8x4 transpose
// of 64 bit words: pairs of Ints in one zmm, unpack within the 128 bit lanes,
// regroup the lanes with vpermt2q)
FE52_FN static inline void fe52_load8(__m512i L[5], const uint64_t *const q[8]) {
  const __m512i IDX_LO = _mm512_setr_epi64(0, 1, 8, 9, 4, 5, 12, 13);
  const __m512i IDX_HI = _mm512_setr_epi64(2, 3, 10, 11, 6, 7, 14, 15);
  __m512i z[4], W[4];
  for (int i = 0; i < 4; i++)
    z[i] = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_loadu_si256((const __m256i *)q[i])),
                              _mm256_loadu_si256((const __m256i *)q[i + 4]), 1);
  __m512i u0 = _mm512_unpacklo_epi64(z[0], z[1]), u1 = _mm512_unpackhi_epi64(z[0], z[1]);
  __m512i u2 = _mm512_unpacklo_epi64(z[2], z[3]), u3 = _mm512_unpackhi_epi64(z[2], z[3]);
  W[0] = _mm512_permutex2var_epi64(u0, IDX_LO, u2);
  W[1] = _mm512_permutex2var_epi64(u1, IDX_LO, u3);
  W[2] = _mm512_permutex2var_epi64(u0, IDX_HI, u2);
  W[3] = _mm512_permutex2var_epi64(u1, IDX_HI, u3);
  fe52_from_words(L, W);
}

// limbs -> 8 Ints (bits64[0..3]; bits64[4] cleared)
FE52_FN static inline void fe52_store8(uint64_t *const q[8], const __m512i L[5]) {
  const __m512i IDX_LO = _mm512_setr_epi64(0, 1, 8, 9, 4, 5, 12, 13);
  const __m512i IDX_HI = _mm512_setr_epi64(2, 3, 10, 11, 6, 7, 14, 15);
  __m512i W[4];
  fe52_to_words(W, L);
  __m512i u0 = _mm512_permutex2var_epi64(W[0], IDX_LO, W[2]);
  __m512i u1 = _mm512_permutex2var_epi64(W[1], IDX_LO, W[3]);
  __m512i u2 = _mm512_permutex2var_epi64(W[0], IDX_HI, W[2]);
  __m512i u3 = _mm512_permutex2var_epi64(W[1], IDX_HI, W[3]);
  __m512i z[4] = { _mm512_unpacklo_epi64(u0, u1), _mm512_unpackhi_epi64(u0, u1),
                   _mm512_unpacklo_epi64(u2, u3), _mm512_unpackhi_epi64(u2, u3) };
  for (int i = 0; i < 4; i++) {
    _mm256_storeu_si256((__m256i *)q[i], _mm512_castsi512_si256(z[i]));
    _mm256_storeu_si256((__m256i *)q[i + 4], _mm512_extracti64x4_epi64(z[i], 1));
  }
  for (int i = 0; i < 8; i++) q[i][4] = 0;
}

// One Int broadcast to the 8 lanes
FE52_FN static inline void fe52_set1(__m512i L[5], const uint64_t *w) {
  L[0] = _mm512_set1_epi64((long long)(w[0] & FE52_M52));
  L[1] = _mm512_set1_epi64((long long)(((w[0] >> 52) | (w[1] << 12)) & FE52_M52));
  L[2] = _mm512_set1_epi64((long long)(((w[1] >> 40) | (w[2] << 24)) & FE52_M52));
  L[3] = _mm512_set1_epi64((long long)(((w[2] >> 28) | (w[3] << 36)) & FE52_M52));
  L[4] = _mm512_set1_epi64((long long)(w[3] >> 16));
}
