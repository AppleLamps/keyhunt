/*
 * Affine point addition of a group, 8 lanes in radix 2^52 (AVX-512 IFMA).
 * See GroupAdd52.h. The field primitives are in fe52_ifma.inl, shared with
 * the lane parallel multiply of FieldMulSimd.cpp.
 */

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>   // before Int.h, which redefines the adc/sbb intrinsics
#endif

#include "GroupAdd52.h"
#include "FieldMulSimd.h"

bool groupadd52_available() {
  return fieldmul_kernel() == FIELDMUL_AVX512IFMA;
}

#if defined(__x86_64__) || defined(__i386__)

#define FE52_FN __attribute__((target("avx512f,avx512ifma")))
#include "fe52_ifma.inl"

/*
 * Same formulas and the same order of operations as the scalar chunk loop
 * (keyhunt.cpp group_points_batch), per lane:
 *   s  = (G.y - P.y) * dx          s' = (G.y + P.y) * dx   (-slope of P - G)
 *   x+ = s^2 - P.x - G.x           x- = s'^2 - P.x - G.x
 *   y+ = s * (G.x - x+) - G.y      y- = s' * (x- - G.x) + G.y
 */
FE52_FN static void groupadd52_ifma(const Point &startP, const Point *G, const Int *dx, Point *plus, Point *minus, int n, bool calculate_y) {
  __m512i P[5], Px[5], Py[5];
  __m512i Gx[5], Gy[5], Dx[5], dy[5], dyn[5], sp[5], sn[5], tp[5], tn[5], rxp[5], rxn[5];
  fe52_set_p(P);
  fe52_set1(Px, startP.x.bits64);
  fe52_set1(Py, startP.y.bits64);
  for (int i0 = 0; i0 < n; i0 += 8) {
    const uint64_t *qx[8], *qy[8], *qd[8];
    uint64_t *px[8], *py[8], *nx[8], *ny[8];
    for (int l = 0; l < 8; l++) {
      qx[l] = G[i0 + l].x.bits64;
      qy[l] = G[i0 + l].y.bits64;
      qd[l] = dx[i0 + l].bits64;
      px[l] = plus[i0 + l].x.bits64;
      py[l] = plus[i0 + l].y.bits64;
      nx[l] = minus[-(i0 + l)].x.bits64;
      ny[l] = minus[-(i0 + l)].y.bits64;
    }
    fe52_load8(Gx, qx);
    fe52_load8(Gy, qy);
    fe52_load8(Dx, qd);

    fe52_sub(dy, Gy, Py, P);
    fe52_add(dyn, Gy, Py, P);
    fe52_mul(sp, dy, Dx);
    fe52_mul(sn, dyn, Dx);
    fe52_sqr(tp, sp);
    fe52_sqr(tn, sn);
    fe52_sub(rxp, tp, Px, P);
    fe52_sub(rxp, rxp, Gx, P);
    fe52_sub(rxn, tn, Px, P);
    fe52_sub(rxn, rxn, Gx, P);
    fe52_store8(px, rxp);
    fe52_store8(nx, rxn);
    if (calculate_y) {
      fe52_sub(tp, Gx, rxp, P);
      fe52_mul(tp, tp, sp);
      fe52_sub(tp, tp, Gy, P);
      fe52_store8(py, tp);
      fe52_sub(tn, rxn, Gx, P);
      fe52_mul(tn, tn, sn);
      fe52_add(tn, tn, Gy, P);
      fe52_store8(ny, tn);
    }
  }
}

void groupadd52(const Point &startP, const Point *G, const Int *dx, Point *plus, Point *minus, int n, bool calculate_y) {
  groupadd52_ifma(startP, G, dx, plus, minus, n, calculate_y);
}

#else

void groupadd52(const Point &, const Point *, const Int *, Point *, Point *, int, bool) {}

#endif
