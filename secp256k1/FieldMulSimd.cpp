/*
 * Lane parallel secp256k1 field multiply: 4 way AVX2 and 8 way AVX-512 radix
 * 2^29 kernels (fieldmul_kernel.inl, vpmuludq) and an 8 way AVX-512 IFMA
 * radix 2^52 kernel (vpmadd52luq/vpmadd52huq). Every kernel carries its own
 * target attribute, so this file compiles without -mavx2/-mavx512* and the
 * kernel is chosen at runtime. See FieldMulSimd.h.
 */

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>   // before Int.h, which redefines the adc/sbb intrinsics
#endif

#include "FieldMulSimd.h"

#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)

// ---- AVX2, 4 lanes ---------------------------------------------------------

#define VEC    __m256i
#define LANES  4
#define KFN    __attribute__((target("avx2")))
#define SFX    avx2
#define VMUL32(a, b)  _mm256_mul_epu32(a, b)
#define VADD(a, b)    _mm256_add_epi64(a, b)
#define VAND(a, b)    _mm256_and_si256(a, b)
#define VOR(a, b)     _mm256_or_si256(a, b)
#define VSRL(x, n)    _mm256_srli_epi64(x, n)
#define VSLL(x, n)    _mm256_slli_epi64(x, n)
#define VSET1(c)      _mm256_set1_epi64x((long long)(c))
#define VZERO()       _mm256_setzero_si256()
// 4 Ints (4 words each) <-> 4 word vectors: a 4x4 transpose of 64 bit words
// (unpack within the 128 bit halves, then swap the halves)
#define VLOAD4(W, p) do { \
  __m256i z0 = _mm256_loadu_si256((const __m256i *)(p)[0].bits64); \
  __m256i z1 = _mm256_loadu_si256((const __m256i *)(p)[1].bits64); \
  __m256i z2 = _mm256_loadu_si256((const __m256i *)(p)[2].bits64); \
  __m256i z3 = _mm256_loadu_si256((const __m256i *)(p)[3].bits64); \
  __m256i u0 = _mm256_unpacklo_epi64(z0, z1), u1 = _mm256_unpackhi_epi64(z0, z1); \
  __m256i u2 = _mm256_unpacklo_epi64(z2, z3), u3 = _mm256_unpackhi_epi64(z2, z3); \
  W[0] = _mm256_permute2x128_si256(u0, u2, 0x20); \
  W[1] = _mm256_permute2x128_si256(u1, u3, 0x20); \
  W[2] = _mm256_permute2x128_si256(u0, u2, 0x31); \
  W[3] = _mm256_permute2x128_si256(u1, u3, 0x31); \
} while (0)
#define VSTORE4(p, W) do { \
  __m256i u0 = _mm256_permute2x128_si256(W[0], W[2], 0x20); \
  __m256i u1 = _mm256_permute2x128_si256(W[1], W[3], 0x20); \
  __m256i u2 = _mm256_permute2x128_si256(W[0], W[2], 0x31); \
  __m256i u3 = _mm256_permute2x128_si256(W[1], W[3], 0x31); \
  _mm256_storeu_si256((__m256i *)(p)[0].bits64, _mm256_unpacklo_epi64(u0, u1)); \
  _mm256_storeu_si256((__m256i *)(p)[1].bits64, _mm256_unpackhi_epi64(u0, u1)); \
  _mm256_storeu_si256((__m256i *)(p)[2].bits64, _mm256_unpacklo_epi64(u2, u3)); \
  _mm256_storeu_si256((__m256i *)(p)[3].bits64, _mm256_unpackhi_epi64(u2, u3)); \
  for (int _l = 0; _l < 4; _l++) (p)[_l].bits64[4] = 0; \
} while (0)

#include "fieldmul_kernel.inl"

#undef VEC
#undef LANES
#undef KFN
#undef SFX
#undef VMUL32
#undef VADD
#undef VAND
#undef VOR
#undef VSRL
#undef VSLL
#undef VSET1
#undef VZERO
#undef VLOAD4
#undef VSTORE4

// ---- AVX-512 transposes (F only), shared by the radix 2^29 and IFMA kernels --
// 8 Ints (4 words each) <-> 4 word vectors of 8 lanes: pairs of Ints are
// loaded into one zmm, unpacked within the 128 bit lanes and the 128 bit
// lanes regrouped with vpermt2q.
#define FM512_IDX_LO _mm512_setr_epi64(0, 1, 8, 9, 4, 5, 12, 13)
#define FM512_IDX_HI _mm512_setr_epi64(2, 3, 10, 11, 6, 7, 14, 15)
#define FM512_LOAD2(p, i) _mm512_inserti64x4(_mm512_castsi256_si512( \
  _mm256_loadu_si256((const __m256i *)(p)[i].bits64)), \
  _mm256_loadu_si256((const __m256i *)(p)[(i) + 4].bits64), 1)
#define VLOAD4(W, p) do { \
  __m512i z0 = FM512_LOAD2(p, 0), z1 = FM512_LOAD2(p, 1); \
  __m512i z2 = FM512_LOAD2(p, 2), z3 = FM512_LOAD2(p, 3); \
  __m512i u0 = _mm512_unpacklo_epi64(z0, z1), u1 = _mm512_unpackhi_epi64(z0, z1); \
  __m512i u2 = _mm512_unpacklo_epi64(z2, z3), u3 = _mm512_unpackhi_epi64(z2, z3); \
  W[0] = _mm512_permutex2var_epi64(u0, FM512_IDX_LO, u2); \
  W[1] = _mm512_permutex2var_epi64(u1, FM512_IDX_LO, u3); \
  W[2] = _mm512_permutex2var_epi64(u0, FM512_IDX_HI, u2); \
  W[3] = _mm512_permutex2var_epi64(u1, FM512_IDX_HI, u3); \
} while (0)
#define FM512_STORE2(p, i, z) do { \
  _mm256_storeu_si256((__m256i *)(p)[i].bits64, _mm512_castsi512_si256(z)); \
  _mm256_storeu_si256((__m256i *)(p)[(i) + 4].bits64, _mm512_extracti64x4_epi64(z, 1)); \
} while (0)
#define VSTORE4(p, W) do { \
  __m512i u0 = _mm512_permutex2var_epi64(W[0], FM512_IDX_LO, W[2]); \
  __m512i u1 = _mm512_permutex2var_epi64(W[1], FM512_IDX_LO, W[3]); \
  __m512i u2 = _mm512_permutex2var_epi64(W[0], FM512_IDX_HI, W[2]); \
  __m512i u3 = _mm512_permutex2var_epi64(W[1], FM512_IDX_HI, W[3]); \
  FM512_STORE2(p, 0, _mm512_unpacklo_epi64(u0, u1)); \
  FM512_STORE2(p, 1, _mm512_unpackhi_epi64(u0, u1)); \
  FM512_STORE2(p, 2, _mm512_unpacklo_epi64(u2, u3)); \
  FM512_STORE2(p, 3, _mm512_unpackhi_epi64(u2, u3)); \
  for (int _l = 0; _l < 8; _l++) (p)[_l].bits64[4] = 0; \
} while (0)

// ---- AVX-512 F, 8 lanes ------------------------------------------------------

#define VEC    __m512i
#define LANES  8
#define KFN    __attribute__((target("avx512f")))
#define SFX    avx512f
#define VMUL32(a, b)  _mm512_mul_epu32(a, b)
#define VADD(a, b)    _mm512_add_epi64(a, b)
#define VAND(a, b)    _mm512_and_si512(a, b)
#define VOR(a, b)     _mm512_or_si512(a, b)
#define VSRL(x, n)    _mm512_srli_epi64(x, n)
#define VSLL(x, n)    _mm512_slli_epi64(x, n)
#define VSET1(c)      _mm512_set1_epi64((long long)(c))
#define VZERO()       _mm512_setzero_si512()

#include "fieldmul_kernel.inl"

#undef VEC
#undef LANES
#undef KFN
#undef SFX
#undef VMUL32
#undef VADD
#undef VAND
#undef VOR
#undef VSRL
#undef VSLL
#undef VSET1
#undef VZERO

// ---- AVX-512 IFMA, 8 lanes ---------------------------------------------------
//
// Radix 2^52: a 256 bit value is 5 limbs (the top one 48 bits), the 5x5
// schoolbook is 25 vpmadd52luq + 25 vpmadd52huq (each adds the low/high 52
// bits of a 52x52 product to its accumulator), a column holds at most 10
// such terms (< 2^56). Same exact reduction as the radix 2^29 kernel, with
// K = 0x1000003D1 (33 bits) as a single multiplier. The limb arithmetic
// lives in fe52_ifma.inl, shared with the group addition (GroupAdd52.cpp).

#define FE52_FN __attribute__((target("avx512f,avx512ifma")))
#include "fe52_ifma.inl"

FE52_FN static void fieldmul_ifma(Int *r, const Int *a, const Int *b) {
  __m512i A[5], B[5], W[4];
  VLOAD4(W, a);
  fe52_from_words(A, W);
  VLOAD4(W, b);
  fe52_from_words(B, W);
  fe52_mul(A, A, B);
  fe52_to_words(W, A);
  VSTORE4(r, W);
}

FE52_FN static void fieldsqr_ifma(Int *r, const Int *a) {
  __m512i A[5], W[4];
  VLOAD4(W, a);
  fe52_from_words(A, W);
  fe52_sqr(A, A);
  fe52_to_words(W, A);
  VSTORE4(r, W);
}

static bool cpu_has(FieldMulKernel k) {
  switch (k) {
    case FIELDMUL_AVX2:       return __builtin_cpu_supports("avx2");
    case FIELDMUL_AVX512F:    return __builtin_cpu_supports("avx512f");
    case FIELDMUL_AVX512IFMA: return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512ifma");
    default:                  return true;
  }
}

static void run_kernel(FieldMulKernel k, Int *r, const Int *a, const Int *b) {
  switch (k) {
    case FIELDMUL_AVX512IFMA: fieldmul_ifma(r, a, b); break;
    case FIELDMUL_AVX512F:    fieldmul_avx512f(r, a, b); break;
    default:                  fieldmul_avx2(r, a, b); break;
  }
}

static void run_square_kernel(FieldMulKernel k, Int *r, const Int *a) {
  switch (k) {
    case FIELDMUL_AVX512IFMA: fieldsqr_ifma(r, a); break;
    case FIELDMUL_AVX512F:    fieldsqr_avx512f(r, a); break;
    default:                  fieldsqr_avx2(r, a); break;
  }
}

#else  // not x86

static bool cpu_has(FieldMulKernel k) { return k == FIELDMUL_SCALAR; }
static void run_kernel(FieldMulKernel, Int *, const Int *, const Int *) {}
static void run_square_kernel(FieldMulKernel, Int *, const Int *) {}

#endif

static const int kernel_lanes[4] = { 0, 4, 8, 8 };

const char *fieldmul_kernel_name(FieldMulKernel k) {
  static const char *names[4] = { "scalar", "avx2", "avx512f", "avx512ifma" };
  return names[k];
}

bool fieldmul_kernel_available(FieldMulKernel k) {
  return cpu_has(k);
}

static FieldMulKernel detect_kernel() {
  const char *env = getenv("KEYHUNT_FIELD_SIMD");
  if (env) {
    if (strcmp(env, "none") == 0) return FIELDMUL_SCALAR;
    if (strcmp(env, "avx2") == 0) return cpu_has(FIELDMUL_AVX2) ? FIELDMUL_AVX2 : FIELDMUL_SCALAR;
    if (strcmp(env, "avx512") == 0) return cpu_has(FIELDMUL_AVX512F) ? FIELDMUL_AVX512F : FIELDMUL_SCALAR;
    if (strcmp(env, "ifma") == 0) return cpu_has(FIELDMUL_AVX512IFMA) ? FIELDMUL_AVX512IFMA : FIELDMUL_SCALAR;
  }
  if (cpu_has(FIELDMUL_AVX512IFMA)) return FIELDMUL_AVX512IFMA;
  if (cpu_has(FIELDMUL_AVX512F)) return FIELDMUL_AVX512F;
  if (cpu_has(FIELDMUL_AVX2)) return FIELDMUL_AVX2;
  return FIELDMUL_SCALAR;
}

FieldMulKernel fieldmul_kernel() {
  // Thread safe: the initializer runs exactly once (C++11 static initialization)
  static const FieldMulKernel k = detect_kernel();
  return k;
}

int fieldmul_lanes() {
  return kernel_lanes[fieldmul_kernel()];
}

int fieldmul_kernel_lanes(FieldMulKernel k) {
  return kernel_lanes[k];
}

void fieldmul_batch_with(FieldMulKernel k, Int *r, const Int *a, const Int *b, int n) {
  int i = 0;
  int lanes = kernel_lanes[k];
  if (lanes) {
    for (; i + lanes <= n; i += lanes) run_kernel(k, r + i, a + i, b + i);
  }
  for (; i < n; i++) r[i].ModMulK1((Int *)&a[i], (Int *)&b[i]);
}

void fieldsqr_batch_with(FieldMulKernel k, Int *r, const Int *a, int n) {
  int i = 0;
  int lanes = kernel_lanes[k];
  if (lanes) {
    for (; i + lanes <= n; i += lanes) run_square_kernel(k, r + i, a + i);
  }
  for (; i < n; i++) r[i].ModSquareK1((Int *)&a[i]);
}

void fieldmul_batch(Int *r, const Int *a, const Int *b, int n) {
  fieldmul_batch_with(fieldmul_kernel(), r, a, b, n);
}

void fieldsqr_batch(Int *r, const Int *a, int n) {
  fieldsqr_batch_with(fieldmul_kernel(), r, a, n);
}
