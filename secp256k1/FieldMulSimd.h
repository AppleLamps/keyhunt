#ifndef FIELDMULSIMD_H
#define FIELDMULSIMD_H

#include <stdint.h>
#include "Int.h"

// Lane parallel secp256k1 field multiplication.
//
//   fieldmul_batch(r, a, b, n):  r[i] = a[i] * b[i]  (mod p)   for i in [0, n)
//   fieldsqr_batch(r, a, n):     r[i] = a[i] * a[i]  (mod p)
//
// Every lane produces exactly the 256 bit value Int::ModMulK1 / ModSquareK1
// would (the same partial reduction: a value in [0, 2^256) congruent to the
// product, final carry dropped), so the batched routines are drop in
// replacements for loops over the scalar ones. r may alias a or b.
//
// Kernels, picked at runtime from the CPU flags (the file builds without
// -mavx2/-mavx512*: every kernel carries its own target attribute):
//   AVX-512 IFMA   8 lanes, radix 2^52 (vpmadd52luq/huq)
//   AVX-512 F      8 lanes, radix 2^29 (vpmuludq)
//   AVX2           4 lanes, radix 2^29 (vpmuludq)
//   scalar         Int::ModMulK1 per element (also used for the tail of a batch)
//
// KEYHUNT_FIELD_SIMD=ifma|avx512|avx2|none forces a kernel (tests, benchmarks).

enum FieldMulKernel {
  FIELDMUL_SCALAR = 0,
  FIELDMUL_AVX2 = 1,
  FIELDMUL_AVX512F = 2,
  FIELDMUL_AVX512IFMA = 3
};

// Kernel selected for this CPU (and KEYHUNT_FIELD_SIMD), and its lane count
// (0 for scalar). The selection is made once, on the first call.
FieldMulKernel fieldmul_kernel();
int fieldmul_lanes();
// Lane count of kernel k (0 for scalar).
int fieldmul_kernel_lanes(FieldMulKernel k);
const char *fieldmul_kernel_name(FieldMulKernel k);
// True when this CPU can run kernel k.
bool fieldmul_kernel_available(FieldMulKernel k);

void fieldmul_batch(Int *r, const Int *a, const Int *b, int n);
void fieldsqr_batch(Int *r, const Int *a, int n);
// Same, with an explicit kernel (must be available): tests and benchmarks.
void fieldmul_batch_with(FieldMulKernel k, Int *r, const Int *a, const Int *b, int n);
void fieldsqr_batch_with(FieldMulKernel k, Int *r, const Int *a, int n);

#endif
