/*
 * novel/lcgfit.cpp: fit the puzzle keys to a linear congruential recurrence
 * with UNKNOWN parameters (no seed space to enumerate).
 *
 * Hypothesis: output_n = s_{i(n)} with s_{i+1} = a*s_i + c (mod 2^64)
 * (order 1), or s_{i+1} = a*s_i + b*s_{i-1} + c (order 2), and key_n =
 * 2^(n-1) + (output_n mod 2^(n-1)). i(n) = 1 + (n-1)*stride allows unused
 * outputs between puzzles.
 *
 * Why this is solvable: modulo 2^j the recurrence only involves the low j bits
 * of the parameters and the seed, and puzzle n reveals the low n-1 bits of
 * output n. So the unknowns are lifted one bit at a time: every candidate
 * (a, c, s_1) mod 2^j is extended to mod 2^(j+1) in 8 ways (32 for order 2),
 * and the extensions that contradict any known key bit are dropped. False
 * hypotheses die within a few levels (about 70-j fresh bits of constraint per
 * level against 3 or 5 new unknown bits); a true one survives to the top.
 *
 * Validated by planting keys from a random LCG and recovering (a, c, s_1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include "known_keys.h"

static const int NMAX = 64;             // consecutive known puzzles 1..64 (keys fit in 64 bits)
static uint64_t out[NMAX + 1];           // low n-1 bits of output n (key_n - 2^(n-1))
static int known_bits[NMAX + 1];         // n-1

struct Cand1 { uint64_t a, c, s; };
struct Cand2 { uint64_t a, b, c, s1, s2; };

static inline bool check1(const Cand1 &x, int bits, int stride) {
  uint64_t mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
  uint64_t s = x.s; int idx = 1;
  for (int n = 1; n <= NMAX; n++) {
    int need = known_bits[n] < bits ? known_bits[n] : bits;
    if (need > 0) { uint64_t m = (1ULL << need) - 1; if (((s - out[n]) & m) != 0) return false; }
    if (n == NMAX) break;
    int target = 1 + n * stride;
    while (idx < target) { s = (x.a * s + x.c) & mask; idx++; }
  }
  return true;
}
static inline bool check2(const Cand2 &x, int bits, int stride) {
  uint64_t mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
  uint64_t p = x.s1, q = x.s2; int idx = 1;   // p = s_idx, q = s_{idx+1}
  for (int n = 1; n <= NMAX; n++) {
    int need = known_bits[n] < bits ? known_bits[n] : bits;
    if (need > 0) { uint64_t m = (1ULL << need) - 1; if (((p - out[n]) & m) != 0) return false; }
    if (n == NMAX) break;
    int target = 1 + n * stride;
    while (idx < target) { uint64_t r = (x.a * q + x.b * p + x.c) & mask; p = q; q = r; idx++; }
  }
  return true;
}

static int fit1(int stride, bool verbose) {
  std::vector<Cand1> cur; cur.push_back({0, 0, 0});
  for (int j = 0; j < 64; j++) {
    std::vector<Cand1> nxt;
    for (auto &x : cur)
      for (int e = 0; e < 8; e++) {
        Cand1 y = { x.a | ((uint64_t)(e & 1) << j), x.c | ((uint64_t)((e >> 1) & 1) << j), x.s | ((uint64_t)((e >> 2) & 1) << j) };
        if (check1(y, j + 1, stride)) nxt.push_back(y);
      }
    cur.swap(nxt);
    if (verbose) printf("    order 1 stride %d: %2d bits lifted, %zu candidates\n", stride, j + 1, cur.size());
    if (cur.empty()) return 0;
    if (cur.size() > 2000000) { printf("    (too many candidates, constraints exhausted at %d bits)\n", j + 1); return -1; }
  }
  for (auto &x : cur) printf("  [!!!] order 1 LCG fits all keys: a=0x%llx c=0x%llx s1=0x%llx (stride %d)\n", (unsigned long long)x.a, (unsigned long long)x.c, (unsigned long long)x.s, stride);
  return (int)cur.size();
}
static int fit2(int stride, bool verbose) {
  std::vector<Cand2> cur; cur.push_back({0, 0, 0, 0, 0});
  for (int j = 0; j < 60; j++) {
    std::vector<Cand2> nxt;
    for (auto &x : cur)
      for (int e = 0; e < 32; e++) {
        Cand2 y = { x.a | ((uint64_t)(e & 1) << j), x.b | ((uint64_t)((e >> 1) & 1) << j), x.c | ((uint64_t)((e >> 2) & 1) << j),
                    x.s1 | ((uint64_t)((e >> 3) & 1) << j), x.s2 | ((uint64_t)((e >> 4) & 1) << j) };
        if (check2(y, j + 1, stride)) nxt.push_back(y);
      }
    cur.swap(nxt);
    if (verbose) printf("    order 2 stride %d: %2d bits lifted, %zu candidates\n", stride, j + 1, cur.size());
    if (cur.empty()) return 0;
    if (cur.size() > 2000000) { printf("    (too many candidates, constraints exhausted at %d bits)\n", j + 1); return -1; }
  }
  for (auto &x : cur) printf("  [!!!] order 2 recurrence fits all keys: a=0x%llx b=0x%llx c=0x%llx (stride %d)\n", (unsigned long long)x.a, (unsigned long long)x.b, (unsigned long long)x.c, stride);
  return (int)cur.size();
}

static void load_known() {
  for (int i = 0; i < KNOWN_KEYS_N; i++) {
    int n = KNOWN_KEYS[i].bits; if (n > NMAX) continue;
    uint64_t k = strtoull(KNOWN_KEYS[i].hex, NULL, 16);
    out[n] = k - (1ULL << (n - 1)); known_bits[n] = n - 1;
  }
}

int main(int argc, char **argv) {
  bool selftest = argc > 1 && argv[1][0] == 's';
  bool verbose = argc > 1 && argv[1][0] == 'v';
  if (selftest) {
    // plant: random odd a, random c, random seed, stride 2, order 1
    uint64_t a = 0x5851F42D4C957F2DULL | 1, c = 0x14057B7EF767814FULL, s = 0xDEADBEEFCAFEF00DULL;
    uint64_t st = s; int idx = 1;
    for (int n = 1; n <= NMAX; n++) { out[n] = st & ((1ULL << (n - 1)) - 1); known_bits[n] = n - 1; int t = 1 + n * 2; while (idx < t) { st = a * st + c; idx++; } }
    printf("[self test] planted a=0x%llx c=0x%llx s1=0x%llx stride 2\n", (unsigned long long)a, (unsigned long long)c, (unsigned long long)s);
    int r = fit1(2, false);
    printf("[self test] %s\n", r > 0 ? "recovered" : "FAILED");
    return r > 0 ? 0 : 1;
  }
  load_known();
  printf("[+] unknown parameter LCG fit over puzzles 1..%d (low bits derivation)\n", NMAX);
  int any = 0;
  for (int stride = 1; stride <= 4; stride++) {
    int r1 = fit1(stride, verbose);
    printf("[-] order 1, stride %d: %s\n", stride, r1 > 0 ? "FIT FOUND" : r1 == 0 ? "no LCG fits (candidates died out)" : "undecided");
    int r2 = fit2(stride, verbose);
    printf("[-] order 2, stride %d: %s\n", stride, r2 > 0 ? "FIT FOUND" : r2 == 0 ? "no recurrence fits (candidates died out)" : "undecided");
    any |= (r1 > 0) | (r2 > 0);
  }
  printf("[+] %s\n", any ? "SOMETHING FITS: verify against the keys above 70" : "no linear congruential recurrence of order 1 or 2 (mod 2^64, strides 1..4) generates the puzzle keys");
  return 0;
}
