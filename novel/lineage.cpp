/*
 * novel/lineage.cpp: generator lineage test for the puzzle keys.
 *
 * Hypothesis under test: the puzzle keys (83 known, puzzles 1..70 complete and
 * in order) are the masked outputs of a seeded pseudo random generator. If so,
 * the small puzzles pin the seed cheaply (puzzle n reveals n-1 bits), the large
 * ones confirm it, and every unsolved puzzle's key follows. Uniform looking keys
 * do not rule this out: a seeded generator produces uniform looking keys.
 *
 * Families: Java Random (48 bit LCG), glibc random() TYPE_3, MSVC rand, BSD
 * rand (TYPE_0), minstd, xorshift32, xorshift64*, splitmix64, PCG32, MT19937 via
 * init_genrand and via init_by_array (Python's random.seed(int), numpy),
 * MT19937-64. Seed spaces: all 32 bit seeds for the cheap generators; 0..2^26
 * plus the Unix time window 2013-01-01..2017-01-01 (seconds) for the MT family.
 *
 * Derivations of an n bit key from the stream (all force the top bit, as the
 * puzzle does): one word per puzzle taking the low n-1 bits; one word per
 * puzzle taking the high n-1 bits; a continuous bit stream; rejection sampling
 * of n bit words until the top bit is set; Python's getrandbits(n-1); Python's
 * randrange(2^(n-1), 2^n). Each with 0 or 1 unused outputs between puzzles.
 *
 * A match on the first 32 puzzles is reported with the seed; it is then checked
 * against every known key. Usage: lineage [threads]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include "known_keys.h"

static const int NTEST = 32;          // puzzles used for the search (keys fit in 64 bits)
static uint64_t target[NTEST + 1];    // target[n] = key of puzzle n
static std::atomic<uint64_t> hits(0);

// ---- generators: each has reset(seed) and next32() / next64() ----------------
struct JavaRandom {
  uint64_t s;
  void reset(uint64_t seed) { s = (seed ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1); }
  uint32_t next(int bits) { s = (s * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1); return (uint32_t)(s >> (48 - bits)); }
  uint32_t next32() { return next(32); }
  uint64_t next64() { uint64_t hi = (uint64_t)(int32_t)next(32); return (hi << 32) + (uint64_t)(int32_t)next(32); }
  int bits() { return 32; }
};
struct MsvcRand {   // state*214013+2531011, 15 bit output
  uint32_t s;
  void reset(uint64_t seed) { s = (uint32_t)seed; }
  uint32_t next32() { s = s * 214013u + 2531011u; return (s >> 16) & 0x7fff; }
  uint64_t next64() { return ((uint64_t)next32() << 32) | next32(); }
  int bits() { return 15; }
};
struct BsdRand {    // TYPE_0: state*1103515245+12345, 31 bit output
  uint32_t s;
  void reset(uint64_t seed) { s = (uint32_t)seed; }
  uint32_t next32() { s = s * 1103515245u + 12345u; return s & 0x7fffffff; }
  uint64_t next64() { return ((uint64_t)next32() << 32) | next32(); }
  int bits() { return 31; }
};
struct Minstd {     // 48271 mod 2^31-1
  uint32_t s;
  void reset(uint64_t seed) { s = (uint32_t)(seed % 2147483646u) + 1; }
  uint32_t next32() { s = (uint32_t)(((uint64_t)s * 48271u) % 2147483647u); return s; }
  uint64_t next64() { return ((uint64_t)next32() << 32) | next32(); }
  int bits() { return 31; }
};
struct GlibcRandom {  // random() TYPE_3 with srandom(seed)
  int32_t r[34 + 1000]; int idx;
  void reset(uint64_t seed) {
    int32_t s = (int32_t)(uint32_t)seed; if (s == 0) s = 1;
    r[0] = s;
    for (int i = 1; i < 31; i++) { int64_t hi = r[i-1] / 127773, lo = r[i-1] % 127773; int64_t w = 16807 * lo - 2836 * hi; if (w < 0) w += 2147483647; r[i] = (int32_t)w; }
    for (int i = 31; i < 34; i++) r[i] = r[i-31];
    idx = 34;
    for (int i = 34; i < 344; i++) step();     // discard 310
  }
  int32_t step() { int32_t v = r[idx-31] + r[idx-3]; // use a ring of 34
    // keep ring small: shift when needed
    r[idx] = v; idx++;
    if (idx >= 34 + 1000) { memmove(r, r + idx - 34, 34 * sizeof(int32_t)); idx = 34; }
    return v; }
  uint32_t next32() { return ((uint32_t)step()) >> 1; }
  uint64_t next64() { return ((uint64_t)next32() << 32) | next32(); }
  int bits() { return 31; }
};
struct Xorshift32 {
  uint32_t s;
  void reset(uint64_t seed) { s = (uint32_t)seed ? (uint32_t)seed : 1; }
  uint32_t next32() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  uint64_t next64() { return ((uint64_t)next32() << 32) | next32(); }
  int bits() { return 32; }
};
struct Xorshift64s {
  uint64_t s;
  void reset(uint64_t seed) { s = seed ? seed : 1; }
  uint64_t next64() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1DULL; }
  uint32_t next32() { return (uint32_t)(next64() >> 32); }
  int bits() { return 64; }
};
struct Splitmix64 {
  uint64_t s;
  void reset(uint64_t seed) { s = seed; }
  uint64_t next64() { uint64_t z = (s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
  uint32_t next32() { return (uint32_t)(next64() >> 32); }
  int bits() { return 64; }
};
struct Pcg32 {
  uint64_t s, inc;
  void reset(uint64_t seed) { inc = (0xda3e39cb94b95bdbULL << 1) | 1; s = 0; next32(); s += seed; next32(); }
  uint32_t next32() { uint64_t old = s; s = old * 6364136223846793005ULL + inc; uint32_t x = (uint32_t)(((old >> 18) ^ old) >> 27); uint32_t rot = (uint32_t)(old >> 59); return (x >> rot) | (x << ((-rot) & 31)); }
  uint64_t next64() { return ((uint64_t)next32() << 32) | next32(); }
  int bits() { return 32; }
};
struct MT19937 {
  uint32_t mt[624]; int mti; bool by_array;
  void init_genrand(uint32_t s) { mt[0] = s; for (mti = 1; mti < 624; mti++) mt[mti] = 1812433253u * (mt[mti-1] ^ (mt[mti-1] >> 30)) + mti; }
  void reset(uint64_t seed) {
    if (!by_array) { init_genrand((uint32_t)seed); return; }
    // init_by_array with the seed's 32 bit words (Python random.seed(int), numpy)
    uint32_t key[2] = { (uint32_t)seed, (uint32_t)(seed >> 32) }; int klen = (seed >> 32) ? 2 : 1;
    init_genrand(19650218u);
    int i = 1, j = 0, k = 624 > klen ? 624 : klen;
    for (; k; k--) { mt[i] = (mt[i] ^ ((mt[i-1] ^ (mt[i-1] >> 30)) * 1664525u)) + key[j] + j; i++; j++; if (i >= 624) { mt[0] = mt[623]; i = 1; } if (j >= klen) j = 0; }
    for (k = 623; k; k--) { mt[i] = (mt[i] ^ ((mt[i-1] ^ (mt[i-1] >> 30)) * 1566083941u)) - i; i++; if (i >= 624) { mt[0] = mt[623]; i = 1; } }
    mt[0] = 0x80000000u; mti = 624;
  }
  uint32_t next32() {
    if (mti >= 624) {
      for (int k = 0; k < 624; k++) { uint32_t y = (mt[k] & 0x80000000u) | (mt[(k+1) % 624] & 0x7fffffffu); mt[k] = mt[(k+397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908b0dfu : 0); }
      mti = 0;
    }
    uint32_t y = mt[mti++];
    y ^= y >> 11; y ^= (y << 7) & 0x9d2c5680u; y ^= (y << 15) & 0xefc60000u; y ^= y >> 18;
    return y;
  }
  uint64_t next64() { uint64_t a = next32(), b = next32(); return (a << 32) | b; }
  int bits() { return 32; }
};
struct MT19937_64 {
  uint64_t mt[312]; int mti;
  void reset(uint64_t seed) { mt[0] = seed; for (mti = 1; mti < 312; mti++) mt[mti] = 6364136223846793005ULL * (mt[mti-1] ^ (mt[mti-1] >> 62)) + mti; }
  uint64_t next64() {
    if (mti >= 312) {
      for (int k = 0; k < 312; k++) { uint64_t y = (mt[k] & 0xFFFFFFFF80000000ULL) | (mt[(k+1) % 312] & 0x7FFFFFFFULL); mt[k] = mt[(k+156) % 312] ^ (y >> 1) ^ ((y & 1) ? 0xB5026F5AA96619E9ULL : 0); }
      mti = 0;
    }
    uint64_t y = mt[mti++];
    y ^= (y >> 29) & 0x5555555555555555ULL; y ^= (y << 17) & 0x71D67FFFEDA60000ULL; y ^= (y << 37) & 0xFFF7EEE000000000ULL; y ^= y >> 43;
    return y;
  }
  uint32_t next32() { return (uint32_t)(next64() >> 32); }
  int bits() { return 64; }
};

// ---- derivations --------------------------------------------------------------
enum Conv { LOW, HIGH, STREAM, REJECT, PYBITS, PYRANGE, NCONV };
static const char *conv_name[] = { "low bits of one word", "high bits of one word", "bit stream", "n bit word, retry until top bit", "python getrandbits(n-1)", "python randrange(2^(n-1),2^n)" };

// WB: word width used by the derivations. 32 takes one 32 bit output per word
// (a 64 bit generator's top half), 64 takes a 64 bit output (for 32 bit
// generators two outputs combined the way Java's nextLong does).
// The generator's outputs for a seed are produced once and cached; every
// derivation variant then reads the same cache from the start (rewind), and the
// cache grows only when a variant survives past it. Each seed costs a handful
// of generator steps instead of one run per variant.
template <class G, int WB> struct Stream {
  G g; int B; uint64_t cur; int left;
  std::vector<uint64_t> cache; size_t pos;
  void reset(uint64_t seed) { g.reset(seed); B = (WB == 64) ? 64 : (g.bits() < 32 ? g.bits() : 32); left = 0; cache.clear(); pos = 0; }
  void rewind() { pos = 0; left = 0; }
  uint64_t word() {
    if (pos < cache.size()) return cache[pos++];
    uint64_t w = WB == 64 ? g.next64() : (uint64_t)g.next32();
    cache.push_back(w); pos++;
    return w;
  }
  uint64_t bits(int k) {        // k <= 63 bits from the stream, MSB first within words
    uint64_t v = 0;
    while (k > 0) {
      if (left == 0) { cur = word(); left = B; }
      int take = k < left ? k : left;
      v = (v << take) | ((cur >> (left - take)) & ((1ULL << take) - 1));
      left -= take; k -= take;
    }
    return v;
  }
  // Python getrandbits for MT (32 bit words): k <= 32 takes the top k bits of one word
  uint64_t pybits(int k) {
    if (k <= 32) return word() >> (32 - k);
    uint64_t lo = word(); uint64_t hi = word() >> (64 - k);
    return (hi << 32) | lo;
  }
};

template <class G, int WB> static bool try_seed(Stream<G, WB> &st, Conv c, int skip) {
  st.rewind();
  for (int n = 1; n <= NTEST; n++) {
    int k = n - 1; uint64_t top = 1ULL << k, mask = top - 1, key;
    switch (c) {
      case LOW:   key = top + (st.word() & mask); break;
      case HIGH:  key = top + (k == 0 ? 0 : (st.word() >> (st.B - k)) & mask); break;
      case STREAM: key = top + (k ? st.bits(k) : 0); break;
      case REJECT: { int tries = 0; do { key = st.word() & ((top << 1) - 1); if (++tries > 64) return false; } while (key < top); break; }
      case PYBITS: key = top + (k ? st.pybits(k) : 0); break;
      case PYRANGE: { int tries = 0; do { key = st.pybits(n); if (++tries > 64) return false; } while (key >= top); key += top; break; }
      default: return false;
    }
    if (key != target[n]) return false;
    for (int s = 0; s < skip; s++) st.word();
  }
  return true;
}

template <class G, int WB> static void search_w(const char *name, uint64_t lo, uint64_t hi, int threads, bool by_array) {
  std::atomic<uint64_t> next(lo);
  std::vector<std::thread> th;
  for (int t = 0; t < threads; t++) th.emplace_back([&]() {
    Stream<G, WB> st; (void)by_array;
    if constexpr (std::is_same<G, MT19937>::value) st.g.by_array = by_array;
    for (;;) {
      uint64_t a = next.fetch_add(1 << 12); if (a >= hi) break;
      uint64_t b = a + (1 << 12) < hi ? a + (1 << 12) : hi;
      for (uint64_t s = a; s < b; s++) {
        st.reset(s);
        for (int c = 0; c < NCONV; c++) {
          if ((c == PYBITS || c == PYRANGE) && !(std::is_same<G, MT19937>::value && WB == 32)) continue;
          for (int skip = 0; skip < 2; skip++)
            if (try_seed(st, (Conv)c, skip)) {
              hits++;
              printf("\n[!!!] MATCH on the first %d puzzles: %s seed=%llu (0x%llx) words=%d bit derivation='%s' skip=%d\n", NTEST, name, (unsigned long long)s, (unsigned long long)s, WB, conv_name[c], skip);
              fflush(stdout);
            }
        }
      }
    }
  });
  for (auto &x : th) x.join();
  printf("[-] %-56s %2d bit words, seeds %llu..%llu: %s\n", name, WB, (unsigned long long)lo, (unsigned long long)hi, hits ? "see matches above" : "no match");
  fflush(stdout);
}
template <class G> static void search(const char *name, uint64_t lo, uint64_t hi, int threads, bool by_array = false) {
  search_w<G, 32>(name, lo, hi, threads, by_array);
  search_w<G, 64>(name, lo, hi, threads, by_array);
}

int main(int argc, char **argv) {
  int threads = argc > 1 ? atoi(argv[1]) : 4;
  for (int i = 0; i < KNOWN_KEYS_N && KNOWN_KEYS[i].bits <= NTEST; i++)
    target[KNOWN_KEYS[i].bits] = strtoull(KNOWN_KEYS[i].hex, NULL, 16);
  printf("[+] generator lineage test over puzzles 1..%d, %d threads\n", NTEST, threads);
  const uint64_t T0 = 1356998400ULL, T1 = 1483228800ULL;   // 2013-01-01 .. 2017-01-01
  search<JavaRandom>("java.util.Random(int)", 0, 1ULL << 32, threads);
  search<MsvcRand>("MSVC rand()", 0, 1ULL << 32, threads);
  search<BsdRand>("BSD rand() TYPE_0", 0, 1ULL << 32, threads);
  search<Minstd>("minstd_rand", 0, 1ULL << 31, threads);
  search<Xorshift32>("xorshift32", 0, 1ULL << 32, threads);
  search<Xorshift64s>("xorshift64* (32 bit seeds)", 0, 1ULL << 32, threads);
  search<Splitmix64>("splitmix64 (32 bit seeds)", 0, 1ULL << 32, threads);
  search<Pcg32>("pcg32 (32 bit seeds)", 0, 1ULL << 32, threads);
  search<GlibcRandom>("glibc random() (time window)", T0, T1, threads);
  search<GlibcRandom>("glibc random() (small seeds)", 0, 1ULL << 26, threads);
  search<MT19937>("MT19937 init_genrand (small seeds)", 0, 1ULL << 26, threads);
  search<MT19937>("MT19937 init_genrand (time window)", T0, T1, threads);
  search<MT19937>("MT19937 init_by_array = python/numpy seed(int) (small seeds)", 0, 1ULL << 26, threads, true);
  search<MT19937>("MT19937 init_by_array = python/numpy seed(int) (time window)", T0, T1, threads, true);
  search<MT19937_64>("MT19937-64 (small seeds)", 0, 1ULL << 26, threads);
  search<MT19937_64>("MT19937-64 (time window)", T0, T1, threads);
  // confirm any hit against every known key
  if (hits) printf("[!] %llu match(es): confirm against all %d known keys before believing it\n", (unsigned long long)hits.load(), KNOWN_KEYS_N);
  else printf("[+] no generator in the tested families and seed spaces reproduces the puzzle keys\n");
  return 0;
}
