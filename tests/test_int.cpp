// Checks the secp256k1 field arithmetic fast paths (4 limb modular add/sub/neg,
// squaring, batch inversion) against straightforward reference computations.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <random>

#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Int.h"
#include "../secp256k1/IntGroup.h"
#include "../secp256k1/FieldMulSimd.h"

static Int P;
static std::mt19937_64 rng(777);
static int failures = 0;

static void random_elem(Int &r, int iter) {
	r.SetInt32(0);
	switch (iter % 8) {   // edge cases first, then random values
		case 0: break;
		case 1: r.SetInt32(1); break;
		case 2: r.Set(&P); r.SubOne(); break;
		case 3: r.Set(&P); r.SubOne(); r.SubOne(); break;
		default:
			for (int i = 0; i < 4; i++) r.bits64[i] = rng();
			if (!r.IsLower(&P)) r.Sub(&P);
	}
}

static void fail(const char *what, Int &a, Int &b, Int &got, Int &want) {
	if (++failures > 5) return;
	char *sa = a.GetBase16(), *sb = b.GetBase16(), *sg = got.GetBase16(), *sw = want.GetBase16();
	printf("[test_int] %s MISMATCH\n  a=%s\n  b=%s\n  got=%s\n  want=%s\n", what, sa, sb, sg, sw);
	free(sa); free(sb); free(sg); free(sw);
}

int main() {
	Secp256K1 secp;
	secp.Init();
	P.SetBase16("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F");

	for (int iter = 0; iter < 200000; iter++) {
		Int a, b, got, want;
		random_elem(a, iter);
		random_elem(b, iter / 8);

		// add: a + b, minus P when >= P
		want.Add(&a, &b);
		if (!want.IsLower(&P)) want.Sub(&P);
		got.ModAdd(&a, &b);
		if (!got.IsEqual(&want)) fail("ModAdd(a,b)", a, b, got, want);
		got.Set(&a); got.ModAdd(&b);
		if (!got.IsEqual(&want)) fail("ModAdd(b)", a, b, got, want);
		want.Add(&a, &a); if (!want.IsLower(&P)) want.Sub(&P);
		got.Set(&a); got.ModDouble();
		if (!got.IsEqual(&want)) fail("ModDouble", a, a, got, want);

		// sub: a - b, plus P when negative
		want.Sub(&a, &b);
		if (want.IsNegative()) want.Add(&P);
		got.ModSub(&a, &b);
		if (!got.IsEqual(&want)) fail("ModSub(a,b)", a, b, got, want);
		got.Set(&a); got.ModSub(&b);
		if (!got.IsEqual(&want)) fail("ModSub(b)", a, b, got, want);

		// neg: P - a
		want.Sub(&P, &a);
		got.Set(&a); got.ModNeg();
		if (!got.IsEqual(&want)) fail("ModNeg", a, a, got, want);

		// small operand variants
		uint64_t u = rng() & (iter % 2 ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFF);
		Int ui; ui.SetInt64(u);
		want.Add(&a, &ui); if (!want.IsLower(&P)) want.Sub(&P);
		got.Set(&a); got.ModAdd(u);
		if (!got.IsEqual(&want)) fail("ModAdd(u64)", a, ui, got, want);
		want.Sub(&a, &ui); if (want.IsNegative()) want.Add(&P);
		got.Set(&a); got.ModSub(u);
		if (!got.IsEqual(&want)) fail("ModSub(u64)", a, ui, got, want);

		// square against multiply
		want.ModMulK1(&a, &a);
		got.ModSquareK1(&a);
		if (!got.IsEqual(&want)) fail("ModSquareK1", a, a, got, want);
	}

	// batch inversion against the single inversion, several group sizes and
	// every field multiply kernel this CPU has (lane parallel chains)
	for (int kk = 0; kk <= 3; kk++) {
	FieldMulKernel kern = (FieldMulKernel)kk;
	if (!fieldmul_kernel_available(kern)) continue;
	for (int size : {1, 2, 7, 8, 9, 31, 32, 33, 100, 513, 1024}) {
		Int *v = new Int[size];
		Int *ref = new Int[size];
		for (int i = 0; i < size; i++) {
			// edge values rotate across the lane parallel chains (element i sits in
			// chain i % lanes): a chain made only of p-1 or p-2 would feed ModMulK1
			// non canonical inputs (values in [P, 2^256)) step after step, for which
			// it drops a carry; real groups never do that (random field elements)
			random_elem(v[i], 4 + i + i / 8); if (v[i].IsZero()) v[i].SetInt32(2);
			ref[i].Set(&v[i]);
			ref[i].ModInv();
		}
		IntGroup g(size);
		g.Set(v);
		g.ModInvWith(kern);
		for (int i = 0; i < size; i++) {
			// ModMulK1 may leave a value in [P, 2^256): compare the residues
			if (!v[i].IsLower(&P)) v[i].Sub(&P);
			if (!ref[i].IsLower(&P)) ref[i].Sub(&P);
			if (!v[i].IsEqual(&ref[i])) { if (++failures < 5) printf("[test_int] IntGroup::ModInv %s size=%d MISMATCH at %d\n", fieldmul_kernel_name(kern), size, i); }
		}
		delete[] v;
		delete[] ref;
	}
	}

	if (failures) { printf("[test_int] FAILED (%d mismatches)\n", failures); return 1; }
	printf("[test_int] OK\n");
	return 0;
}
