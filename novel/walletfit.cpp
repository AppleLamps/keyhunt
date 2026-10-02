/*
 * walletfit: test the puzzle creator's own description of the keys.
 *
 * The creator wrote: "There is no pattern. It is just consecutive keys from a
 * deterministic wallet (masked with leading 000...0001 to set difficulty)."
 * So key n is a 256 bit value d_n produced by some deterministic scheme from a
 * seed, reduced to n bits with the top bit forced to 1.  The 83 known keys
 * (1..70 complete) give the low n-1 bits of d_n for every n, about 2400 bits
 * in all, so a wrong seed dies at the first few keys and a right one survives
 * all of them.  This program searches the seed spaces a 2014 era script could
 * plausibly have used and the usual ways of turning a seed into a stream of
 * keys, with both byte orders and both masking directions:
 *
 *   index schemes   d_j = SHA256(seed|j) (decimal, big and little endian
 *                   binary, colon separated, either order), HMAC-SHA256(seed, j),
 *                   SHA256d(seed|j); j = n-1, n or 256-n
 *   chain schemes   d_0 = SHA256(seed), d_{j+1} = SHA256(d_j), or of its hex
 *                   text, or SHA256d; j = n-1 or n
 *   masks           low n-1 bits or top n-1 bits of the digest, digest read
 *                   as a big or a little endian number
 *
 * Seed spaces: decimal numbers, lowercase strings of 1 to 5 letters,
 * hexadecimal numbers, and a short list of puzzle and bitcoin themed words
 * with common decorations.  A hit prints the scheme, the seed and the key it
 * predicts for puzzle 71.
 *
 * Usage: walletfit [threads [scale]]   scale divides the numeric spaces
 *        walletfit selftest [threads]  plants keys and recovers them
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include "../hash/sha256.h"
#include "known_keys.h"

typedef unsigned __int128 u128;
static const int NCHK = 40;                 // early rejection depth
static const int NVER = 70;                 // verification depth for a hit
static u128 tgt[NVER + 1];

static inline void H(const uint8_t *in, size_t len, uint8_t *out) { sha256((uint8_t *)in, len, out); }

static void hmac(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t out[32]) {
  uint8_t k[64] = {0};
  if (kl > 64) H(key, kl, k); else memcpy(k, key, kl);
  uint8_t buf[64 + 64], inner[32], b2[96];
  for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;
  memcpy(buf + 64, msg, ml);
  H(buf, 64 + ml, inner);
  for (int i = 0; i < 64; i++) b2[i] = k[i] ^ 0x5c;
  memcpy(b2 + 64, inner, 32);
  H(b2, 96, out);
}

enum { S_SEED_DEC, S_DEC_SEED, S_SEED_BE32, S_SEED_LE32, S_HMAC, S_DBL, S_SEED_COLON, NIDX,
       C_SHA = NIDX, C_HEX, C_DBL, NSCH };
static const char *scheme_name[NSCH] = {
  "SHA256(seed|dec j)", "SHA256(dec j|seed)", "SHA256(seed|be32 j)", "SHA256(seed|le32 j)",
  "HMAC-SHA256(seed, dec j)", "SHA256d(seed|dec j)", "SHA256(seed|':'|dec j)",
  "chain SHA256", "chain SHA256 of hex", "chain SHA256d" };
static const char *orient_name[4] = { "low bits, big endian", "top bits, big endian",
                                      "low bits, little endian", "top bits, little endian" };
static inline int nbases(int sch) { return sch < NIDX ? 3 : 2; }

struct Gen {
  int sch, b, n;
  const uint8_t *s; size_t sl;
  uint8_t d[32];
  void init(int sch_, int b_, const std::string &seed) {
    sch = sch_; b = b_; n = 0; s = (const uint8_t *)seed.data(); sl = seed.size();
  }
  void step() {
    uint8_t t[64];
    if (sch == C_SHA) { memcpy(t, d, 32); H(t, 32, d); }
    else if (sch == C_HEX) {
      static const char hx[] = "0123456789abcdef";
      for (int i = 0; i < 32; i++) { t[2 * i] = hx[d[i] >> 4]; t[2 * i + 1] = hx[d[i] & 15]; }
      H(t, 64, d);
    } else { memcpy(t, d, 32); H(t, 32, d); memcpy(t, d, 32); H(t, 32, d); }
  }
  void skip_to(int m) { if (sch < NIDX) n = m; }          // index schemes need no history
  void next() {
    n++;
    if (sch >= NIDX) {
      if (n == 1) { H(s, sl, d); if (sch == C_DBL) { uint8_t t[32]; memcpy(t, d, 32); H(t, 32, d); }
                    for (int k = 0; k < b; k++) step(); }
      else step();
      return;
    }
    int j = (b == 2) ? 256 - n : n - 1 + b;
    char dec[16]; int dl = snprintf(dec, sizeof dec, "%d", j);
    uint8_t buf[160]; size_t len = 0;
    uint8_t be[4] = { (uint8_t)(j >> 24), (uint8_t)(j >> 16), (uint8_t)(j >> 8), (uint8_t)j };
    uint8_t le[4] = { (uint8_t)j, (uint8_t)(j >> 8), (uint8_t)(j >> 16), (uint8_t)(j >> 24) };
    switch (sch) {
      case S_SEED_DEC:   memcpy(buf, s, sl); memcpy(buf + sl, dec, dl); len = sl + dl; H(buf, len, d); break;
      case S_DEC_SEED:   memcpy(buf, dec, dl); memcpy(buf + dl, s, sl); len = dl + sl; H(buf, len, d); break;
      case S_SEED_BE32:  memcpy(buf, s, sl); memcpy(buf + sl, be, 4); H(buf, sl + 4, d); break;
      case S_SEED_LE32:  memcpy(buf, s, sl); memcpy(buf + sl, le, 4); H(buf, sl + 4, d); break;
      case S_HMAC:       hmac(s, sl, (const uint8_t *)dec, dl, d); break;
      case S_DBL:        { uint8_t t[32]; memcpy(buf, s, sl); memcpy(buf + sl, dec, dl); H(buf, sl + dl, t); H(t, 32, d); } break;
      case S_SEED_COLON: memcpy(buf, s, sl); buf[sl] = ':'; memcpy(buf + sl + 1, dec, dl); H(buf, sl + 1 + dl, d); break;
    }
  }
};

static inline u128 be128(const uint8_t *p) { u128 v = 0; for (int i = 0; i < 16; i++) v = (v << 8) | p[i]; return v; }
static inline u128 le128(const uint8_t *p) { u128 v = 0; for (int i = 15; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static inline u128 val(int o, const uint8_t *d, int n) {
  int k = n - 1; u128 top = (u128)1 << k;
  if (k == 0) return top;
  u128 m = top - 1;
  switch (o) {
    case 0:  return (be128(d + 16) & m) | top;
    case 1:  return (be128(d) >> (128 - k)) | top;
    case 2:  return (le128(d) & m) | top;
    default: return (le128(d + 16) >> (128 - k)) | top;
  }
}

// Orientations still alive after matching puzzles 1..NCHK (bit o set = orientation o fits).
static int scan(int sch, int b, const std::string &seed, int depth = NCHK) {
  Gen g; g.init(sch, b, seed);
  int alive = 15;
  if (sch < NIDX) g.skip_to(1);                         // puzzle 1 is always the key 1
  else { g.next(); }                                    // chains must walk from the start
  for (int n = 2; n <= depth; n++) {
    g.next();
    for (int o = 0; o < 4; o++)
      if ((alive >> o & 1) && val(o, g.d, n) != tgt[n]) alive &= ~(1 << o);
    if (!alive) return 0;
  }
  return alive;
}

static u128 key_of(int sch, int b, int o, const std::string &seed, int n) {
  Gen g; g.init(sch, b, seed);
  if (sch < NIDX) g.skip_to(n - 1);
  else for (int i = 1; i < n; i++) g.next();
  g.next();
  return val(o, g.d, n);
}

static std::string hex128(u128 v) {
  char buf[40]; snprintf(buf, sizeof buf, "%llx%016llx", (unsigned long long)(v >> 64), (unsigned long long)v);
  const char *p = buf; while (*p == '0' && p[1]) p++;
  if (v >> 64 == 0) { snprintf(buf, sizeof buf, "%llx", (unsigned long long)v); return buf; }
  return buf;
}

static void load_targets() {
  for (int i = 0; i < KNOWN_KEYS_N && KNOWN_KEYS[i].bits <= NVER; i++) {
    u128 v = 0;
    for (const char *p = KNOWN_KEYS[i].hex; *p; p++) {
      int c = *p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10; v = (v << 4) | c;
    }
    tgt[KNOWN_KEYS[i].bits] = v;
  }
}

// ---- seed spaces -----------------------------------------------------------
struct Space { const char *name; uint64_t count; std::string (*gen)(uint64_t); };
static std::vector<std::string> words;
static std::string gen_dec(uint64_t i) { return std::to_string(i); }
static std::string gen_hex(uint64_t i) { char b[24]; snprintf(b, sizeof b, "%llx", (unsigned long long)i); return b; }
static std::string gen_alpha(uint64_t i) {
  uint64_t cnt = 26; int len = 1;
  while (i >= cnt) { i -= cnt; cnt *= 26; len++; }
  std::string s(len, 'a');
  for (int k = len - 1; k >= 0; k--) { s[k] = 'a' + (char)(i % 26); i /= 26; }
  return s;
}
static std::string gen_word(uint64_t i) { return words[i]; }

static void build_words() {
  static const char *base[] = {
    "puzzle", "bitcoin", "btc", "satoshi", "nakamoto", "saatoshi", "saatoshi_rising", "satoshi_rising",
    "key", "keys", "private", "privatekey", "wallet", "deterministic", "seed", "secret", "password",
    "passphrase", "test", "testing", "hello", "hello world", "puzzle32", "32btc", "btc32", "bitcoinpuzzle",
    "bitcoin puzzle", "crude measuring instrument", "measuring instrument", "cracking strength",
    "collider", "lbc", "largebitcoincollider", "mask", "masked", "difficulty", "consecutive", "bruteforce",
    "brute force", "challenge", "prize", "treasure", "hunt", "lottery", "random", "entropy", "bits", "hash",
    "sha256", "ripemd160", "secp256k1", "elliptic", "curve", "genesis", "blockchain", "block", "coin",
    "coins", "mining", "miner", "hodl", "moon", "abc", "abc123", "letmein", "admin", "root", "master",
    "default", "correct horse battery staple", "trustno1", "dragon", "monkey", "shadow", "sunshine",
    "princess", "iloveyou", "football", "baseball", "welcome", "login", "starwars", "matrix", "freedom",
    "whatever", "passw0rd", "qwerty", "123456", "12345678", "123456789", "1234567890", "2014", "2015",
    "20150115", "2015-01-15", "january2015", "jan2015", "01152015", "15012015", "bitcoin2014", "bitcoin2015",
    "electrum", "armory", "multibit", "brainwallet", "bitaddress", "bitcoin-qt", "bitcoincore", "pybitcointools",
    "vitalik", "buterin", "pycoin", "bip32", "bip39", "xprv", "mnemonic", "1000btc", "1000 btc", "puzzle1000",
    "transaction", "tx", "address", "addresses", "ladder", "bits256", "2^256", "256", "keyspace", "range" };
  static const char *suf[] = { "", "1", "123", "2014", "2015", "01", "0", "12", "!", "_1" };
  for (const char *w : base) {
    std::string a = w, cap = a, up = a;
    cap[0] = (char)toupper(cap[0]);
    for (auto &c : up) c = (char)toupper(c);
    for (const char *s : suf) { words.push_back(a + s); if (cap != a) words.push_back(cap + s); if (up != a) words.push_back(up + s); }
  }
}

struct Hit { int sch, b, o; std::string seed; };
static std::mutex mu;
static std::atomic<uint64_t> total_hits{0};

static void report(const Hit &h) {
  std::lock_guard<std::mutex> lk(mu);
  int ok = 0; bool all = true;
  for (int n = 2; n <= NVER; n++) { if (key_of(h.sch, h.b, h.o, h.seed, n) == tgt[n]) ok = n; else { all = false; break; } }
  printf("[!!!] MATCH: scheme '%s', seed '%s', base %d, mask '%s', consistent with puzzles 1..%d%s\n",
         scheme_name[h.sch], h.seed.c_str(), h.b, orient_name[h.o], ok, all ? " (ALL KNOWN KEYS)" : "");
  printf("      predicted key for puzzle 71: 0x%s\n", hex128(key_of(h.sch, h.b, h.o, h.seed, 71)).c_str());
  fflush(stdout);
  total_hits++;
}

static void run_space(const Space &sp, int threads) {
  auto t0 = std::chrono::steady_clock::now();
  std::atomic<uint64_t> next{0};
  const uint64_t chunk = 2048;
  uint64_t before = total_hits;
  std::vector<std::thread> th;
  for (int t = 0; t < threads; t++) th.emplace_back([&]() {
    for (;;) {
      uint64_t lo = next.fetch_add(chunk);
      if (lo >= sp.count) return;
      uint64_t hi = lo + chunk < sp.count ? lo + chunk : sp.count;
      for (uint64_t i = lo; i < hi; i++) {
        std::string seed = sp.gen(i);
        for (int sch = 0; sch < NSCH; sch++)
          for (int b = 0; b < nbases(sch); b++) {
            int alive = scan(sch, b, seed);
            for (int o = 0; alive && o < 4; o++) if (alive >> o & 1) report({sch, b, o, seed});
          }
      }
    }
  });
  for (auto &x : th) x.join();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("[%c] %-34s %12llu seeds x %d schemes x orientations, %.0f s: %s\n",
         total_hits > before ? '!' : '-', sp.name, (unsigned long long)sp.count, NSCH, secs,
         total_hits > before ? "MATCH, see above" : "no match");
  fflush(stdout);
}

static int selftest(int threads) {
  struct Plant { int sch, b, o; const char *seed; };
  Plant plants[] = { { S_SEED_DEC, 1, 0, "zebra" }, { C_HEX, 0, 1, "4242" }, { S_HMAC, 2, 2, "abc" },
                     { C_DBL, 1, 3, "wallet" }, { S_SEED_BE32, 0, 0, "hello" }, { S_SEED_COLON, 2, 1, "x7" },
                     { C_SHA, 0, 2, "puzzle" } };
  (void)threads;
  load_targets();
  u128 real[NVER + 1]; memcpy(real, tgt, sizeof tgt);
  int fails = 0;
  for (const Plant &p : plants) {
    for (int n = 1; n <= NVER; n++) tgt[n] = key_of(p.sch, p.b, p.o, p.seed, n);
    std::vector<std::string> seeds = { "decoy", "zebra2", p.seed, "4243" };
    bool found = false; int stray = 0;
    for (const std::string &s : seeds)
      for (int sch = 0; sch < NSCH; sch++)
        for (int b = 0; b < nbases(sch); b++) {
          int alive = scan(sch, b, s);
          for (int o = 0; o < 4; o++) if (alive >> o & 1) {
            if (sch == p.sch && b == p.b && o == p.o && s == p.seed) found = true; else stray++;
          }
        }
    printf("[self test] %-26s seed '%s' base %d, %-24s %s\n", scheme_name[p.sch], p.seed, p.b, orient_name[p.o],
           found && !stray ? "recovered" : "FAILED");
    if (!(found && !stray)) fails++;
  }
  memcpy(tgt, real, sizeof tgt);
  printf(fails ? "[self test] FAILED\n" : "[self test] all plants recovered\n");
  return fails ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "selftest") == 0) return selftest(argc > 2 ? atoi(argv[2]) : 4);
  int threads = argc > 1 ? atoi(argv[1]) : 4;
  uint64_t scale = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
  if (scale < 1) scale = 1;
  load_targets();
  build_words();
  printf("[+] deterministic wallet fit over the known puzzle keys, %d threads, %d schemes\n", threads, NSCH);
  uint64_t alpha = 26 + 26ULL * 26 + 26ULL * 26 * 26 + 26ULL * 26 * 26 * 26 + 26ULL * 26 * 26 * 26 * 26;
  Space spaces[] = {
    { "word list with decorations", words.size(), gen_word },
    { "lowercase strings, 1 to 5 letters", alpha / scale, gen_alpha },
    { "decimal numbers", 50000000ULL / scale, gen_dec },
    { "hexadecimal numbers", (1ULL << 24) / scale, gen_hex },
  };
  for (const Space &sp : spaces) run_space(sp, threads);
  if (total_hits) printf("[!] %llu match(es): see above\n", (unsigned long long)total_hits.load());
  else printf("[+] no scheme and seed in the tested spaces reproduces the puzzle keys\n");
  return 0;
}
