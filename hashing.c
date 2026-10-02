// Hashing helpers for the legacy build, on top of the OpenSSL EVP API
// (the SHA256_* / RIPEMD160_* functions are deprecated since OpenSSL 3.0).
#include <openssl/evp.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "hashing.h"
#include "sha3/sha3.h"

static int evp_digest(const EVP_MD *md, const unsigned char *data, size_t length, unsigned char *digest) {
	unsigned int len = 0;
	if (md == NULL || EVP_Digest(data, length, digest, &len, md, NULL) != 1) {
		printf("Failed to compute digest\n");
		return 1;
	}
	return 0;
}

int sha256(const unsigned char *data, size_t length, unsigned char *digest) {
	return evp_digest(EVP_sha256(), data, length, digest);
}

int rmd160(const unsigned char *data, size_t length, unsigned char *digest) {
	return evp_digest(EVP_ripemd160(), data, length, digest);
}

int sha256_4(size_t length, const unsigned char *data0, const unsigned char *data1,
             const unsigned char *data2, const unsigned char *data3,
             unsigned char *digest0, unsigned char *digest1,
             unsigned char *digest2, unsigned char *digest3) {
	return sha256(data0, length, digest0) || sha256(data1, length, digest1) ||
	       sha256(data2, length, digest2) || sha256(data3, length, digest3);
}

int rmd160_4(size_t length, const unsigned char *data0, const unsigned char *data1,
             const unsigned char *data2, const unsigned char *data3,
             unsigned char *digest0, unsigned char *digest1,
             unsigned char *digest2, unsigned char *digest3) {
	return rmd160(data0, length, digest0) || rmd160(data1, length, digest1) ||
	       rmd160(data2, length, digest2) || rmd160(data3, length, digest3);
}

int keccak(const unsigned char *data, size_t length, unsigned char *digest) {
	SHA3_256_CTX ctx;
	SHA3_256_Init(&ctx);
	SHA3_256_Update(&ctx,data,length);
	KECCAK_256_Final(digest,&ctx);
	return 0;
}

bool sha256_file(const char* file_name, uint8_t* digest) {
	FILE* file = fopen(file_name, "rb");
	if (file == NULL) {
		printf("Failed to open file: %s\n", file_name);
		return false;
	}

	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		printf("Failed to initialize SHA256 context\n");
		EVP_MD_CTX_free(ctx);
		fclose(file);
		return false;
	}

	uint8_t buffer[8192];
	size_t bytes_read;
	bool ok = true;
	while (ok && (bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		if (EVP_DigestUpdate(ctx, buffer, bytes_read) != 1) {
			printf("Failed to update digest\n");
			ok = false;
		}
	}
	unsigned int len = 0;
	if (ok && EVP_DigestFinal_ex(ctx, digest, &len) != 1) {
		printf("Failed to finalize digest\n");
		ok = false;
	}
	EVP_MD_CTX_free(ctx);
	fclose(file);
	return ok;
}
