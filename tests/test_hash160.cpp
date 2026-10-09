// Cross-checks the SIMD hash160 kernels (8 way AVX2, 16 way AVX-512) against
// the scalar SHA256/RIPEMD160, and the Secp256K1 wrappers against the scalar
// hash160 of real points.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <random>

#include "../hash/sha256.h"
#include "../hash/ripemd160.h"
#include "../hash/hash160_simd.h"
#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Point.h"
#include "../secp256k1/Int.h"

// Pre-pad `len` bytes into big endian SHA256 words (16 words per block).
static int pad_sha256(const uint8_t *msg, int len, uint32_t *words) {
	uint8_t buf[128] = {0};
	memcpy(buf, msg, len);
	buf[len] = 0x80;
	int blocks = (len + 9 + 63) / 64;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) buf[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
	for (int i = 0; i < blocks * 16; i++)
		words[i] = (uint32_t)buf[4*i] << 24 | (uint32_t)buf[4*i+1] << 16 | (uint32_t)buf[4*i+2] << 8 | buf[4*i+3];
	return blocks;
}

static int failures = 0;

static uint64_t digest_prefix(const uint8_t *digest) {
	uint64_t prefix = 0;
	for (int i = 0; i < 8; i++) prefix |= (uint64_t)digest[i] << (8 * i);
	return prefix;
}

static void check_prefix_result(int lanes, uint64_t prefix, uint32_t mask,
                                const uint8_t expect[16][20], const uint8_t got[16][20]) {
	uint32_t expected_mask = 0;
	uint8_t untouched[20];
	memset(untouched, 0xA5, sizeof(untouched));
	for (int l = 0; l < lanes; l++) {
		bool match = digest_prefix(expect[l]) == prefix;
		if (match) expected_mask |= 1u << l;
		if (memcmp(got[l], match ? expect[l] : untouched, 20) != 0) {
			if (++failures < 5) printf("[test_hash160] prefix output mismatch lane=%d\n", l);
		}
	}
	if (mask != expected_mask) {
		if (++failures < 5) printf("[test_hash160] prefix mask mismatch got=%x expected=%x\n", mask, expected_mask);
	}
}

// Random messages of 33 and 65 bytes through one kernel
static void test_kernel(const char *name, int lanes,
                        void (*k1)(const uint32_t *, uint8_t *const *),
                        void (*k2)(const uint32_t *, uint8_t *const *), std::mt19937_64 &rng) {
	for (int len : {33, 65}) {
		for (int iter = 0; iter < 20000; iter++) {
			uint8_t msg[16][65];
			uint32_t words[16][32];
			uint32_t w[32 * 16];
			uint8_t digest[16][20];
			uint8_t *out[16];
			int blocks = 0;
			for (int l = 0; l < lanes; l++) {
				for (int i = 0; i < len; i++) msg[l][i] = (uint8_t)rng();
				blocks = pad_sha256(msg[l], len, words[l]);
				if (blocks != (len == 33 ? 1 : 2)) { printf("bad block count\n"); exit(1); }
				for (int j = 0; j < blocks * 16; j++) w[j * lanes + l] = words[l][j];
				out[l] = digest[l];
			}
			// Exercise masks with multiple hits, including every lane, as well as
			// the normal single-hit case. Scalar checks below remain the oracle.
			if (iter % 4 == 0) {
				for (int l = 1; l < lanes; l++) {
					if (iter % 8 == 0 || l % 2 == 0) {
						memcpy(msg[l], msg[0], len);
						for (int j = 0; j < blocks * 16; j++) w[j * lanes + l] = words[0][j];
					}
				}
			}
			if (len == 33) k1(w, out); else k2(w, out);
			for (int l = 0; l < lanes; l++) {
				uint8_t sh[32], expect[20];
				sha256(msg[l], len, sh);
				ripemd160(sh, 32, expect);
				if (memcmp(expect, digest[l], 20) != 0) {
					if (++failures < 5) printf("[test_hash160] %s MISMATCH len=%d iter=%d lane=%d\n", name, len, iter, l);
				}
			}
			uint8_t filtered[16][20];
			for (int l = 0; l < lanes; l++) out[l] = filtered[l];
			uint64_t prefix = digest_prefix(digest[iter % lanes]);
			// Flip each of the 64 prefix bits over successive batches. This also
			// verifies that BOTH words participate and no-match outputs stay intact.
			for (int mode = 0; mode < 2; mode++) {
				uint64_t target = prefix ^ (mode ? (uint64_t(1) << (iter % 64)) : 0);
				memset(filtered, 0xA5, sizeof(filtered));
				uint32_t mask = len == 33 ? hash160_simd_1B_prefix(lanes, w, target, out)
				                          : hash160_simd_2B_prefix(lanes, w, target, out);
				check_prefix_result(lanes, target, mask, digest, filtered);
			}
		}
	}
	printf("[test_hash160] %s kernel ok\n", name);
}

// Secp256K1 wrappers: the N way hash of real points against the scalar hash160
static void test_wrappers(Secp256K1 &secp, int lanes, std::mt19937_64 &rng) {
	for (int iter = 0; iter < 2000; iter++) {
		Point pts[16];
		for (int l = 0; l < lanes; l++) {
			Int key;
			key.SetInt32(1 + (uint32_t)(rng() & 0xFFFFFF));
			key.ShiftL(32); key.Add((uint64_t)rng());
			pts[l] = secp.ComputePublicKey(&key);
		}
		uint8_t got[16][20];
		uint8_t *out[16];
		for (int l = 0; l < lanes; l++) out[l] = got[l];
		for (int mode = 0; mode < 4; mode++) {
			// 0: compressed, 1: uncompressed, 2/3: compressed from x only with prefix 02/03
			if (mode == 0) secp.GetHash160_N(lanes, true, pts, out);
			else if (mode == 1) secp.GetHash160_N(lanes, false, pts, out);
			else secp.GetHash160_fromX_N(lanes, mode == 2 ? 0x02 : 0x03, pts, out);
			if (mode < 2) {
				uint8_t filtered[16][20];
				uint8_t *filtered_out[16];
				for (int l = 0; l < lanes; l++) filtered_out[l] = filtered[l];
				memset(filtered, 0xA5, sizeof(filtered));
				uint64_t prefix = digest_prefix(got[iter % lanes]);
				uint32_t mask = secp.GetHash160_N_Prefix(lanes, mode == 0, pts, prefix, filtered_out);
				check_prefix_result(lanes, prefix, mask, got, filtered);
			}
			for (int l = 0; l < lanes; l++) {
				uint8_t expect[20];
				if (mode < 2) {
					secp.GetHash160(P2PKH, mode == 0, pts[l], expect);
				} else {
					Point q = pts[l];
					// force the requested parity: flip y if needed
					bool wantOdd = (mode == 3);
					if (q.y.IsOdd() != wantOdd) q.y.ModNeg();
					secp.GetHash160(P2PKH, true, q, expect);
				}
				if (memcmp(expect, got[l], 20) != 0) {
					if (++failures < 5) printf("[test_hash160] Secp256K1 %d lanes MISMATCH mode=%d iter=%d lane=%d\n", lanes, mode, iter, l);
				}
			}
		}
	}
	printf("[test_hash160] Secp256K1 %d lane wrappers ok\n", lanes);
}

int main() {
	std::mt19937_64 rng(12345);
	Secp256K1 secp;
	secp.Init();
	int tested = 0;
	if (hash160_avx2_available()) {
		test_kernel("avx2", 8, hash160_avx2_1B, hash160_avx2_2B, rng);
		test_wrappers(secp, 8, rng);
		tested++;
	}
	if (hash160_avx512_available()) {
		test_kernel("avx512", 16, hash160_avx512_1B, hash160_avx512_2B, rng);
		test_wrappers(secp, 16, rng);
		tested++;
	}
	if (!tested) {
		printf("[test_hash160] no AVX2/AVX-512 on this CPU, skipping\n");
		return 0;
	}
	if (failures) { printf("[test_hash160] FAILED (%d mismatches)\n", failures); return 1; }
	printf("[test_hash160] OK\n");
	return 0;
}
