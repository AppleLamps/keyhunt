// Microbenchmark: nanoseconds per secp256k1 field multiply / square for the
// scalar Int::ModMulK1 / ModSquareK1 and every batched kernel this CPU runs.
// Usage: bench_fieldmul [batch=1024] [repeat=20000]
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <random>

#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Int.h"
#include "../secp256k1/FieldMulSimd.h"

static double now_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char **argv) {
	int n = argc > 1 ? atoi(argv[1]) : 1024;
	int repeat = argc > 2 ? atoi(argv[2]) : 20000;
	if (n < 1 || n > (1 << 24) || repeat < 1) {
		fprintf(stderr, "usage: bench_fieldmul [batch 1..16777216] [repeat >= 1]\n");
		return 1;
	}
	Secp256K1 secp;
	secp.Init();
	std::mt19937_64 rng(1);
	Int *a = new Int[n], *b = new Int[n], *r = new Int[n];
	Int P; P.SetBase16("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F");
	for (int i = 0; i < n; i++) {
		for (int w = 0; w < 4; w++) { a[i].bits64[w] = rng(); b[i].bits64[w] = rng(); }
		a[i].bits64[4] = b[i].bits64[4] = 0;
		if (!a[i].IsLower(&P)) a[i].Sub(&P);
		if (!b[i].IsLower(&P)) b[i].Sub(&P);
	}
	uint64_t sink = 0;
	printf("batch %d, %d repeats, %s per operation\n", n, repeat, "ns");

	double t0 = now_ns();
	for (int rep = 0; rep < repeat; rep++) {
		for (int i = 0; i < n; i++) r[i].ModMulK1(&a[i], &b[i]);
		sink += r[rep % n].bits64[0];
	}
	double scalar_mul = (now_ns() - t0) / ((double)n * repeat);
	printf("  %-22s mul %7.2f ns", "scalar ModMulK1", scalar_mul);
	t0 = now_ns();
	for (int rep = 0; rep < repeat; rep++) {
		for (int i = 0; i < n; i++) r[i].ModSquareK1(&a[i]);
		sink += r[rep % n].bits64[0];
	}
	double scalar_sqr = (now_ns() - t0) / ((double)n * repeat);
	printf("   sqr %7.2f ns\n", scalar_sqr);

	for (int k = FIELDMUL_AVX2; k <= FIELDMUL_AVX512IFMA; k++) {
		FieldMulKernel kernel = (FieldMulKernel)k;
		if (!fieldmul_kernel_available(kernel)) continue;
		t0 = now_ns();
		for (int rep = 0; rep < repeat; rep++) {
			fieldmul_batch_with(kernel, r, a, b, n);
			sink += r[rep % n].bits64[0];
		}
		double mul = (now_ns() - t0) / ((double)n * repeat);
		t0 = now_ns();
		for (int rep = 0; rep < repeat; rep++) {
			fieldsqr_batch_with(kernel, r, a, n);
			sink += r[rep % n].bits64[0];
		}
		double sqr = (now_ns() - t0) / ((double)n * repeat);
		printf("  %-22s mul %7.2f ns (%.2fx)   sqr %7.2f ns (%.2fx)\n", fieldmul_kernel_name(kernel),
		       mul, scalar_mul / mul, sqr, scalar_sqr / sqr);
	}
	printf("default kernel: %s\n", fieldmul_kernel_name(fieldmul_kernel()));
	if (sink == 42) printf("\n");
	delete[] a; delete[] b; delete[] r;
	return 0;
}
