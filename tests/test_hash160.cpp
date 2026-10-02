// Cross-checks the SIMD hash160 kernels against the scalar SHA256/RIPEMD160.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <random>

#include "../hash/sha256.h"
#include "../hash/ripemd160.h"
#include "../hash/hash160_avx2.h"
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

int main() {
	if (!hash160_avx2_available()) {
		printf("[test_hash160] AVX2 not available on this CPU, skipping\n");
		return 0;
	}
	std::mt19937_64 rng(12345);
	int failures = 0;
	for (int len : {33, 65}) {
		for (int iter = 0; iter < 20000; iter++) {
			uint8_t msg[8][65];
			uint32_t words[8][32];
			const uint32_t *in[8];
			uint8_t digest[8][20];
			uint8_t *out[8];
			for (int l = 0; l < 8; l++) {
				for (int i = 0; i < len; i++) msg[l][i] = (uint8_t)rng();
				int blocks = pad_sha256(msg[l], len, words[l]);
				if (blocks != (len == 33 ? 1 : 2)) { printf("bad block count\n"); return 1; }
				in[l] = words[l];
				out[l] = digest[l];
			}
			if (len == 33) hash160_avx2_1B(in, out); else hash160_avx2_2B(in, out);
			for (int l = 0; l < 8; l++) {
				uint8_t sh[32], expect[20];
				sha256(msg[l], len, sh);
				ripemd160(sh, 32, expect);
				if (memcmp(expect, digest[l], 20) != 0) {
					if (++failures < 5) printf("[test_hash160] MISMATCH len=%d iter=%d lane=%d\n", len, iter, l);
				}
			}
		}
	}
	// Secp256K1 wrappers: the 8 way hash of real points against the scalar hash160
	Secp256K1 secp;
	secp.Init();
	for (int iter = 0; iter < 2000; iter++) {
		Point pts[8];
		for (int l = 0; l < 8; l++) {
			Int key;
			key.SetInt32(1 + (uint32_t)(rng() & 0xFFFFFF));
			key.ShiftL(32); key.Add((uint64_t)rng());
			pts[l] = secp.ComputePublicKey(&key);
		}
		uint8_t got[8][20];
		uint8_t *out[8];
		for (int l = 0; l < 8; l++) out[l] = got[l];
		for (int mode = 0; mode < 4; mode++) {
			// 0: compressed, 1: uncompressed, 2/3: compressed from x only with prefix 02/03
			if (mode == 0) secp.GetHash160_8(true, pts, out);
			else if (mode == 1) secp.GetHash160_8(false, pts, out);
			else secp.GetHash160_fromX_8(mode == 2 ? 0x02 : 0x03, pts, out);
			for (int l = 0; l < 8; l++) {
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
					if (++failures < 5) printf("[test_hash160] Secp256K1 MISMATCH mode=%d iter=%d lane=%d\n", mode, iter, l);
				}
			}
		}
	}
	if (failures) { printf("[test_hash160] FAILED (%d mismatches)\n", failures); return 1; }
	printf("[test_hash160] OK\n");
	return 0;
}
