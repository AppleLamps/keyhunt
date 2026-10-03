// Checks every lane parallel field multiply kernel this CPU can run
// (secp256k1/FieldMulSimd.cpp) bit for bit against Int::ModMulK1 and
// Int::ModSquareK1, on edge cases, reduced and unreduced random operands,
// odd batch lengths and aliased arguments.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <random>

#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Int.h"
#include "../secp256k1/FieldMulSimd.h"

static Int P;
static std::mt19937_64 rng(4242);
static int failures = 0;

// Operands cover the whole 256 bit input domain of ModMulK1 (which does not
// require reduced inputs), with a bias to the values that stress the carries.
static void random_elem(Int &r, int sel) {
	r.SetInt32(0);
	switch (sel % 16) {
		case 0: break;
		case 1: r.SetInt32(1); break;
		case 2: r.Set(&P); r.SubOne(); break;                       // p - 1
		case 3: r.Set(&P); break;                                   // p (unreduced)
		case 4: for (int i = 0; i < 4; i++) r.bits64[i] = ~0ULL; break;    // 2^256 - 1
		case 5: r.bits64[3] = 0xFFFFFFFFFFFFFFFFULL; break;         // high limb only
		case 6: r.bits64[0] = 0x1000003D1ULL; break;                // K
		case 7: for (int i = 0; i < 4; i++) r.bits64[i] = rng() & 0x1FFFFFFFULL; break; // small limbs
		case 8: for (int i = 0; i < 4; i++) r.bits64[i] = ~0ULL; r.bits64[rng() % 4] = rng(); break;
		case 9: r.bits64[rng() % 4] = 1ULL << (rng() % 64); break;  // single bit
		case 10: case 11: case 12:
			for (int i = 0; i < 4; i++) r.bits64[i] = rng();          // any 256 bit value
			break;
		default:
			for (int i = 0; i < 4; i++) r.bits64[i] = rng();          // reduced
			if (!r.IsLower(&P)) r.Sub(&P);
	}
	r.bits64[4] = 0;
}

static void fail(const char *what, const char *kernel, int n, int i, Int &a, Int &b, Int &got, Int &want) {
	if (++failures > 10) return;
	char *sa = a.GetBase16(), *sb = b.GetBase16(), *sg = got.GetBase16(), *sw = want.GetBase16();
	printf("[test_fieldmul] %s %s n=%d i=%d MISMATCH\n  a=%s\n  b=%s\n  got=%s\n  want=%s\n", kernel, what, n, i, sa, sb, sg, sw);
	free(sa); free(sb); free(sg); free(sw);
}

static bool same(const Int &x, const Int &y) {
	return memcmp(x.bits64, y.bits64, 5 * sizeof(uint64_t)) == 0;
}

#define MAXN 67

static void check_kernel(FieldMulKernel k, int rounds) {
	const char *name = fieldmul_kernel_name(k);
	Int a[MAXN], b[MAXN], r[MAXN], want[MAXN], wsq[MAXN], s[MAXN];
	for (int round = 0; round < rounds; round++) {
		int n = (round < 2 * MAXN) ? round % MAXN : 1 + rng() % (MAXN - 1);
		for (int i = 0; i < n; i++) {
			random_elem(a[i], round < 256 ? round : rng());
			random_elem(b[i], round < 256 ? round / 16 : rng());
			want[i].ModMulK1(&a[i], &b[i]);
			wsq[i].ModSquareK1(&a[i]);
			// ModSquareK1 and ModMulK1(a,a) are specified to agree
			Int m; m.ModMulK1(&a[i], &a[i]);
			if (!same(m, wsq[i])) fail("ModSquareK1 vs ModMulK1(a,a)", "scalar", n, i, a[i], a[i], wsq[i], m);
		}
		// r = a*b
		for (int i = 0; i < MAXN; i++) for (int w = 0; w < 5; w++) r[i].bits64[w] = 0xABABABABABABABABULL;
		fieldmul_batch_with(k, r, a, b, n);
		for (int i = 0; i < n; i++) if (!same(r[i], want[i])) fail("mul", name, n, i, a[i], b[i], r[i], want[i]);
		// r = a^2
		fieldsqr_batch_with(k, s, a, n);
		for (int i = 0; i < n; i++) if (!same(s[i], wsq[i])) fail("sqr", name, n, i, a[i], a[i], s[i], wsq[i]);
		// in place: a = a*b, then b = b*b
		for (int i = 0; i < n; i++) r[i].Set(&a[i]);
		fieldmul_batch_with(k, r, r, b, n);
		for (int i = 0; i < n; i++) if (!same(r[i], want[i])) fail("mul in place", name, n, i, a[i], b[i], r[i], want[i]);
		for (int i = 0; i < n; i++) { r[i].Set(&b[i]); wsq[i].ModSquareK1(&b[i]); }
		fieldsqr_batch_with(k, r, r, n);
		for (int i = 0; i < n; i++) if (!same(r[i], wsq[i])) fail("sqr in place", name, n, i, b[i], b[i], r[i], wsq[i]);
	}
}

int main(int argc, char **argv) {
	int rounds = argc > 1 ? atoi(argv[1]) : 20000;
	Secp256K1 secp;
	secp.Init();
	P.SetBase16("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F");

	int tested = 0;
	for (int k = FIELDMUL_SCALAR; k <= FIELDMUL_AVX512IFMA; k++) {
		FieldMulKernel kernel = (FieldMulKernel)k;
		if (!fieldmul_kernel_available(kernel)) {
			printf("[skip] field multiply kernel %s: not supported by this CPU\n", fieldmul_kernel_name(kernel));
			continue;
		}
		int before = failures;
		check_kernel(kernel, rounds);
		if (failures == before) printf("[ok]   field multiply kernel %s bit exact (%d batches)\n", fieldmul_kernel_name(kernel), rounds);
		tested++;
	}
	printf("[%s] field multiply: %d kernels tested, default %s (%d lanes)\n",
	       failures ? "FAIL" : "ok", tested, fieldmul_kernel_name(fieldmul_kernel()), fieldmul_lanes());
	return failures ? 1 : 0;
}
