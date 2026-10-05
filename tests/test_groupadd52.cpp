// Checks the radix 2^52 group addition (secp256k1/GroupAdd52.cpp) bit for bit
// against the scalar formulas of keyhunt's group loop, and its field
// primitives (secp256k1/fe52_ifma.inl) against Int::ModAdd / ModSub / ModMulK1
// on canonical operands and the edge values 0, 1, p-1. Skipped (reported as
// ok) on a CPU without AVX-512 IFMA.
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <random>
#include <vector>

#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Int.h"
#include "../secp256k1/IntGroup.h"
#include "../secp256k1/FieldMulSimd.h"
#include "../secp256k1/GroupAdd52.h"

static std::mt19937_64 rng(7171);
static int failures = 0;
static Int P;

static bool same(const Int &x, const Int &y) {
	return memcmp(x.bits64, y.bits64, 5 * sizeof(uint64_t)) == 0;
}

static void canonical(Int &r, int sel) {
	r.SetInt32(0);
	switch (sel % 8) {
		case 0: break;
		case 1: r.SetInt32(1); break;
		case 2: r.Set(&P); r.SubOne(); break;
		case 3: r.bits64[3] = 0xFFFFFFFFFFFFFFFFULL; break;
		default: for (int i = 0; i < 4; i++) r.bits64[i] = rng(); if (!r.IsLower(&P)) r.Sub(&P);
	}
	r.bits64[4] = 0;
}

static void fail(const char *what, int i, Int &a, Int &b, Int &got, Int &want) {
	if (++failures > 10) return;
	char *sa = a.GetBase16(), *sb = b.GetBase16(), *sg = got.GetBase16(), *sw = want.GetBase16();
	printf("[test_groupadd52] %s i=%d MISMATCH\n  a=%s\n  b=%s\n  got=%s\n  want=%s\n", what, i, sa, sb, sg, sw);
	free(sa); free(sb); free(sg); free(sw);
}

#if defined(__x86_64__) || defined(__i386__)
#define FE52_FN __attribute__((target("avx512f,avx512ifma")))
#include "../secp256k1/fe52_ifma.inl"

FE52_FN static void prim_round(int round) {
	Int a[8], b[8], r[8], want;
	for (int l = 0; l < 8; l++) { canonical(a[l], round < 64 ? round : rng()); canonical(b[l], round < 64 ? round / 8 : rng()); }
	const uint64_t *qa[8], *qb[8]; uint64_t *qr[8];
	for (int l = 0; l < 8; l++) { qa[l] = a[l].bits64; qb[l] = b[l].bits64; qr[l] = r[l].bits64; }
	__m512i A[5], B[5], R[5], Pl[5];
	fe52_set_p(Pl);
	fe52_load8(A, qa); fe52_load8(B, qb);
	fe52_add(R, A, B, Pl); fe52_store8(qr, R);
	for (int l = 0; l < 8; l++) { want.ModAdd(&a[l], &b[l]); if (!same(r[l], want)) fail("add", l, a[l], b[l], r[l], want); }
	fe52_sub(R, A, B, Pl); fe52_store8(qr, R);
	for (int l = 0; l < 8; l++) { want.ModSub(&a[l], &b[l]); if (!same(r[l], want)) fail("sub", l, a[l], b[l], r[l], want); }
	fe52_mul(R, A, B); fe52_store8(qr, R);
	for (int l = 0; l < 8; l++) { want.ModMulK1(&a[l], &b[l]); if (!same(r[l], want)) fail("mul", l, a[l], b[l], r[l], want); }
	// round trip through the broadcast load
	fe52_set1(R, a[3].bits64); fe52_store8(qr, R);
	for (int l = 0; l < 8; l++) if (!same(r[l], a[3])) fail("set1", l, a[3], a[3], r[l], a[3]);
}
#endif

// Scalar reference: the formulas of keyhunt's group loop
static void reference(Point &startP, Point *G, Int *dx, Point *plus, Point *minus, int n, bool calculate_y) {
	Int dy, dyn, _s, _p;
	for (int i = 0; i < n; i++) {
		dy.ModSub(&G[i].y, &startP.y);
		dyn.ModAdd(&G[i].y, &startP.y);
		_s.ModMulK1(&dy, &dx[i]);
		_p.ModSquareK1(&_s);
		Point &pp = plus[i];
		pp.x.ModSub(&_p, &startP.x);
		pp.x.ModSub(&G[i].x);
		if (calculate_y) { pp.y.ModSub(&G[i].x, &pp.x); pp.y.ModMulK1(&_s); pp.y.ModSub(&G[i].y); }
		_s.ModMulK1(&dyn, &dx[i]);
		_p.ModSquareK1(&_s);
		Point &pn = minus[-i];
		pn.x.ModSub(&_p, &startP.x);
		pn.x.ModSub(&G[i].x);
		if (calculate_y) { pn.y.ModSub(&pn.x, &G[i].x); pn.y.ModMulK1(&_s); pn.y.ModAdd(&G[i].y); }
	}
}

int main() {
	Secp256K1 *secp = new Secp256K1();
	secp->Init();
	P.Set(Int::GetFieldCharacteristic());
	if (!groupadd52_available()) {
		printf("[ok]   groupadd52 (no AVX-512 IFMA on this CPU, kernel not tested)\n");
		return 0;
	}
#if defined(__x86_64__) || defined(__i386__)
	for (int round = 0; round < 4000; round++) prim_round(round);
#endif
	const int N = 512;
	std::vector<Point> G(N);
	G[0] = secp->G;
	for (int i = 1; i < N; i++) G[i] = secp->AddDirect(G[i - 1], secp->G);
	std::vector<Point> want(2 * N + 1), got(2 * N + 1);
	Int dx[N];
	IntGroup grp(N);
	grp.Set(dx);
	for (int round = 0; round < 40 && failures == 0; round++) {
		int n = (round % 4 == 3) ? 8 * (1 + rng() % 64) : N;
		bool calculate_y = round % 2 == 0;
		Int k;
		for (int i = 0; i < 4; i++) k.bits64[i] = rng();
		k.bits64[4] = 0;
		k.Mod(Int::GetFieldCharacteristic());   // any scalar; the order is close to p
		if (round == 0) k.SetInt32(1000);
		Point startP = secp->ComputePublicKey(&k);
		for (int i = 0; i < n; i++) dx[i].ModSub(&G[i].x, &startP.x);
		IntGroup g(n); g.Set(dx); g.ModInv();
		for (int i = 0; i < 2 * N + 1; i++) { want[i].Clear(); got[i].Clear(); for (int w = 0; w < 5; w++) got[i].x.bits64[w] = got[i].y.bits64[w] = 0xABABABABABABABABULL; }
		reference(startP, G.data(), dx, &want[N + 1], &want[N - 1], n, calculate_y);
		groupadd52(startP, G.data(), dx, &got[N + 1], &got[N - 1], n, calculate_y);
		for (int i = 0; i < n; i++) {
			if (!same(got[N + 1 + i].x, want[N + 1 + i].x)) fail("plus.x", i, G[i].x, startP.x, got[N + 1 + i].x, want[N + 1 + i].x);
			if (!same(got[N - 1 - i].x, want[N - 1 - i].x)) fail("minus.x", i, G[i].x, startP.x, got[N - 1 - i].x, want[N - 1 - i].x);
			if (calculate_y) {
				if (!same(got[N + 1 + i].y, want[N + 1 + i].y)) fail("plus.y", i, G[i].y, startP.y, got[N + 1 + i].y, want[N + 1 + i].y);
				if (!same(got[N - 1 - i].y, want[N - 1 - i].y)) fail("minus.y", i, G[i].y, startP.y, got[N - 1 - i].y, want[N - 1 - i].y);
			}
		}
		// the kernel writes nothing beyond n and nothing to y when x only
		for (int i = n; i < N; i++) if (got[N + 1 + i].x.bits64[0] != 0xABABABABABABABABULL || got[N - 1 - i].x.bits64[0] != 0xABABABABABABABABULL) { failures++; printf("[test_groupadd52] wrote past n=%d\n", n); break; }
		if (!calculate_y) for (int i = 0; i < n; i++) if (got[N + 1 + i].y.bits64[0] != 0xABABABABABABABABULL) { failures++; printf("[test_groupadd52] wrote y in x only mode\n"); break; }
		// and the points are the real sums: check a few against AddDirect
		if (calculate_y) for (int t = 0; t < 4; t++) {
			int i = rng() % n;
			Point s = secp->AddDirect(startP, G[i]);
			if (!same(s.x, got[N + 1 + i].x) || !same(s.y, got[N + 1 + i].y)) fail("AddDirect plus", i, s.x, s.y, got[N + 1 + i].x, got[N + 1 + i].y);
			Point ng = G[i]; ng.y.ModNeg();
			Point d = secp->AddDirect(startP, ng);
			if (!same(d.x, got[N - 1 - i].x) || !same(d.y, got[N - 1 - i].y)) fail("AddDirect minus", i, d.x, d.y, got[N - 1 - i].x, got[N - 1 - i].y);
		}
	}
	if (failures) { printf("[FAIL] groupadd52: %d mismatches\n", failures); return 1; }
	printf("[ok]   groupadd52 bit exact with the scalar group loop (primitives, 40 groups)\n");
	return 0;
}
