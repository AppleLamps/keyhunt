#ifndef GROUPADD52_H
#define GROUPADD52_H

#include "Int.h"
#include "Point.h"

// Lane parallel affine point addition in radix 2^52 (AVX-512 IFMA).
//
// groupadd52(startP, G, dx, plus, minus, n, calculate_y) computes, for i in
// [0, n), n a multiple of 8:
//   plus[i]  = startP + G[i]
//   minus[-i] = startP - G[i]        (minus points at the last element of a
//                                     descending run: minus[0], minus[-1], ...)
// with dx[i] = 1 / (G[i].x - startP.x) already inverted, x only when
// calculate_y is false. The eight additions of a chunk stay in 52 bit limb
// form from the load of G[i] and dx[i] to the store of the results: one
// conversion each way per chunk instead of one per field multiply, and the
// modular add/sub are lane parallel too. The results are bit for bit those of
// the scalar formulas in keyhunt's group_points_batch (see fe52_ifma.inl for
// the one caveat, a non canonical operand).
//
// groupadd52_available() is true when the CPU has AVX-512 IFMA and the field
// multiply kernel selection (KEYHUNT_FIELD_SIMD) did not force another kernel.
bool groupadd52_available();
void groupadd52(const Point &startP, const Point *G, const Int *dx, Point *plus, Point *minus, int n, bool calculate_y);

#endif
