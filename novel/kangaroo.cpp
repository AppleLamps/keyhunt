/*
 * novel/kangaroo.cpp: Pollard kangaroo (interval discrete log) for secp256k1
 * with distinguished points, batched affine arithmetic and the negation map.
 *
 * Given a public key Q = k*G and an interval [a, b], finds k. Expected work is
 * about 2*sqrt(W) group operations without the negation map and about
 * 1.41*sqrt(W) with it (W = b - a), using memory proportional only to the
 * number of distinguished points, so unlike BSGS the range size is not limited
 * by RAM.
 *
 * Herds: half the kangaroos are tame (position t known, point = t*G), half are
 * wild (point = s*Q + d*G, s = +-1). All jump by the same x dependent table of
 * powers of two whose mean is K*sqrt(W)/4 (K = total kangaroos), so the herds
 * travel together and a tame/wild collision reveals k. Points whose x has `dp`
 * low zero bits are distinguished and stored; two kangaroos that landed on the
 * same point reach the same distinguished point and are detected there.
 *
 * Negation map: P and -P are treated as one element by always continuing from
 * the representative with even y. The walk then lives on a set of half the
 * size, which is where the sqrt(2) saving comes from. Negating the point flips
 * s and the sign of d. The price is fruitless cycles (P -> -(P+J) -> P and
 * longer ones): the walk must stay a pure function of the current point for
 * collisions to merge, so they are handled Brent style: the minimum x of each
 * window of WINDOW steps is remembered, the same minimum in two consecutive
 * windows means a cycle of length <= WINDOW, and the walker escapes from that
 * minimum point with a dedicated jump. Both walkers trapped in the same cycle
 * leave it the same way. A large jump table with linearly spread sizes keeps
 * cycles rare (2-cycles about 1 in 2*JUMPS steps, 4-cycles 1 in 4*JUMPS^2).
 *
 * Usage: kangaroo -p <pubkey hex> -r <start>:<end> [-t threads] [-k kangaroos/thread]
 *                 [-d dp bits] [-n] (no negation map) [-s seed] [-q]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <chrono>

#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Int.h"
#include "../secp256k1/IntGroup.h"
#include "../secp256k1/Point.h"
#include "../secp256k1/Random.h"

#define JUMPS 256
#define WINDOW 16

static Secp256K1 *secp;
static Point Q;                      // target
static Int range_start, range_end, range_width;
static Point jumpP[JUMPS];           // jump points J_i = jumpD[i] * G
static Int jumpD[JUMPS];             // their scalars, spread linearly in [mean/2, 3*mean/2)
static Point escP; static Int escD;  // cycle escape jump
static int dp_bits = -1;
static uint64_t dp_mask;
static bool use_negation = true;
static bool quiet = false;
static int nthreads = 4, per_thread = 512;
static std::atomic<bool> found(false);
static std::atomic<uint64_t> total_steps(0);
static std::atomic<uint64_t> dp_count(0);
static std::atomic<uint64_t> fruitless(0), reseeds(0), cycles(0), solve_fail(0), stuck(0);
static Int key_found;

// Distinguished point table: x (two limbs) -> (sign, distance)
struct DPEntry { uint64_t x2; int8_t sign; Int dist; };
static std::unordered_multimap<uint64_t, DPEntry> dptable;
static pthread_mutex_t dpmutex = PTHREAD_MUTEX_INITIALIZER;

static void reduce_mod_order(Int &k) {
  // Int is two's complement 320 bit; bring k into [0, n)
  while (k.IsNegative()) k.Add(&secp->order);
  while (k.IsGreaterOrEqual(&secp->order)) k.Sub(&secp->order);
}

static bool verify(Int &k) {
  Int t(k); reduce_mod_order(t);
  if (t.IsZero()) return false;
  Point P = secp->ComputePublicKey(&t);
  return P.x.IsEqual(&Q.x) && P.y.IsEqual(&Q.y);
}

// A kangaroo at point P with (sign, dist) means P = sign*Q + dist*G (sign 0: tame).
// Two of them meeting at the same point gives k.
static bool solve(int8_t s1, Int &d1, int8_t s2, Int &d2) {
  Int k;
  if (s1 == s2) { fruitless++; return false; }       // tame/tame or same-sign wild/wild
  if (s1 == 0 || s2 == 0) {
    // t*G = s*Q + d*G  =>  k = s*(t - d)
    Int t = (s1 == 0) ? d1 : d2, d = (s1 == 0) ? d2 : d1;
    int8_t s = (s1 == 0) ? s2 : s1;
    k.Sub(&t, &d);
    if (s < 0) k.Neg();
  } else {
    // s*Q + d1*G = -s*Q + d2*G  =>  2*s*k = d2 - d1
    k.Sub(&d2, &d1);
    if (s1 < 0) k.Neg();
    reduce_mod_order(k);
    if (k.IsOdd()) k.Add(&secp->order);
    k.ShiftR(1);
  }
  reduce_mod_order(k);
  if (!verify(k)) { solve_fail++; return false; }
  pthread_mutex_lock(&dpmutex);
  if (!found) { key_found.Set(&k); found = true; }
  pthread_mutex_unlock(&dpmutex);
  return true;
}

// Returns true if a solution was found through this distinguished point
static bool report_dp(Point &P, int8_t sign, Int &dist) {
  uint64_t k1 = P.x.bits64[1], k2 = P.x.bits64[2];
  bool hit = false;
  pthread_mutex_lock(&dpmutex);
  auto range = dptable.equal_range(k1);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second.x2 != k2) continue;
    DPEntry e = it->second;
    pthread_mutex_unlock(&dpmutex);
    if (solve(e.sign, e.dist, sign, dist)) return true;
    hit = true;     // same point, same herd: nothing to learn, the walker is re-seeded
    pthread_mutex_lock(&dpmutex);
    break;
  }
  if (!hit) { DPEntry e; e.x2 = k2; e.sign = sign; e.dist.Set(&dist); dptable.emplace(k1, e); dp_count++; }
  pthread_mutex_unlock(&dpmutex);
  return hit;      // caller re-seeds on a fruitless meeting
}

struct Walker {
  Point P; Int dist; int8_t sign; uint64_t since_dp;
  // cycle detection: minimum of the current and the previous window
  uint64_t min_x, prev_min_x; Point min_P; Int min_dist; int8_t min_sign; int in_window;
};

static void reset_cycle_state(Walker &w) {
  w.min_x = ~0ULL; w.prev_min_x = ~0ULL; w.in_window = 0;
}

static void canonicalize(Walker &w) {
  if (use_negation && w.P.y.IsOdd()) {
    w.P.y.ModNeg();
    w.sign = -w.sign;
    w.dist.Neg();
  }
}

// tame: random position inside the interval; wild: Q plus a random offset in
// [-W/2, W/2), so both herds cover the same region of the line
static void seed(Walker &w, bool tame) {
  Int r; r.Rand(&range_start, &range_end);
  if (tame) {
    w.P = secp->ComputePublicKey(&r);
    w.dist.Set(&r); w.sign = 0;
  } else {
    Int half(range_width); half.ShiftR(1);
    r.Sub(&range_start); r.Sub(&half);   // offset in [-W/2, W/2)
    Int mag(r); bool neg = mag.IsNegative(); if (neg) mag.Neg();
    Point R = secp->ComputePublicKey(&mag);
    if (neg) R = secp->Negation(R);
    w.P = secp->AddDirect(Q, R);
    w.dist.Set(&r); w.sign = 1;
  }
  w.since_dp = 0;
  reset_cycle_state(w);
  canonicalize(w);
}

// Called after every step. Returns true when the walker was moved out of a cycle.
static bool cycle_check(Walker &w) {
  uint64_t x = w.P.x.bits64[0];
  if (x < w.min_x) { w.min_x = x; w.min_P = w.P; w.min_dist.Set(&w.dist); w.min_sign = w.sign; }
  if (++w.in_window < WINDOW) return false;
  w.in_window = 0;
  if (w.min_x == w.prev_min_x) {
    // same minimum in two consecutive windows: in a cycle. Escape from its minimum element.
    w.P = secp->AddDirect(w.min_P, escP);
    w.dist.Set(&w.min_dist); w.dist.Add(&escD); w.sign = w.min_sign;
    canonicalize(w);
    reset_cycle_state(w);
    return true;
  }
  w.prev_min_x = w.min_x; w.min_x = ~0ULL;
  return false;
}

static void *worker(void *arg) {
  int id = (int)(intptr_t)arg;
  int K = per_thread;
  std::vector<Walker> w(K);
  for (int i = 0; i < K; i++) seed(w[i], (i & 1) == 0);   // half tame, half wild
  std::vector<Int> dx(K);
  IntGroup grp(K); grp.Set(dx.data());
  std::vector<uint8_t> idx(K);
  Int s, p, dy;
  uint64_t local = 0;
  const uint64_t stuck_limit = 64ULL << dp_bits;
  while (!found) {
    for (int i = 0; i < K; i++) {
      uint8_t j = (uint8_t)(w[i].P.x.bits64[1] % JUMPS);
      idx[i] = j;
      dx[i].ModSub(&jumpP[j].x, &w[i].P.x);
    }
    grp.ModInv();
    for (int i = 0; i < K; i++) {
      Walker &k = w[i];
      Point &J = jumpP[idx[i]];
      if (dx[i].IsZero()) { seed(k, k.sign == 0); reseeds++; continue; }
      dy.ModSub(&J.y, &k.P.y);
      s.ModMulK1(&dy, &dx[i]);
      p.ModSquareK1(&s);
      Int x3; x3.ModSub(&p, &k.P.x); x3.ModSub(&J.x);
      Int y3; y3.ModSub(&k.P.x, &x3); y3.ModMulK1(&s); y3.ModSub(&k.P.y);
      k.P.x.Set(&x3); k.P.y.Set(&y3);
      k.dist.Add(&jumpD[idx[i]]);
      canonicalize(k);
      if (use_negation && cycle_check(k)) cycles++;
      k.since_dp++;
      if ((k.P.x.bits64[0] & dp_mask) == 0) {
        k.since_dp = 0;
        if (report_dp(k.P, k.sign, k.dist)) {
          if (found) break;
          seed(k, k.sign == 0); reseeds++;
        }
      } else if (k.since_dp > stuck_limit) { seed(k, k.sign == 0); reseeds++; stuck++; }
    }
    local += K;
    if ((local & 0xFFFF) == 0) { total_steps += local; local = 0; }
  }
  total_steps += local;
  (void)id;
  return NULL;
}

static void usage() {
  fprintf(stderr, "usage: kangaroo -p <pubkey hex> -r <start>:<end> [-t threads] [-k kangaroos/thread] [-d dp bits] [-n] [-s seed] [-q]\n");
  exit(1);
}

int main(int argc, char **argv) {
  const char *pub = NULL, *range = NULL;
  uint64_t seedv = 0; bool have_seed = false;
  int c;
  while ((c = getopt(argc, argv, "p:r:t:k:d:ns:q")) != -1) {
    switch (c) {
      case 'p': pub = optarg; break;
      case 'r': range = optarg; break;
      case 't': nthreads = atoi(optarg); break;
      case 'k': per_thread = atoi(optarg); break;
      case 'd': dp_bits = atoi(optarg); break;
      case 'n': use_negation = false; break;
      case 's': seedv = strtoull(optarg, NULL, 10); have_seed = true; break;
      case 'q': quiet = true; break;
      default: usage();
    }
  }
  if (!pub || !range) usage();
  secp = new Secp256K1(); secp->Init();
  if (have_seed) rseed(seedv); else rseed((unsigned long)time(NULL) ^ (unsigned long)getpid());

  bool comp;
  char pubbuf[200]; strncpy(pubbuf, pub, 199); pubbuf[199] = 0;
  if (!secp->ParsePublicKeyHex(pubbuf, Q, comp)) { fprintf(stderr, "bad public key\n"); return 1; }
  char rbuf[200]; strncpy(rbuf, range, 199); rbuf[199] = 0;
  char *colon = strchr(rbuf, ':'); if (!colon) usage(); *colon = 0;
  range_start.SetBase16(rbuf); range_end.SetBase16(colon + 1);
  if (!range_start.IsLower(&range_end)) { fprintf(stderr, "empty range\n"); return 1; }
  range_width.Sub(&range_end, &range_start);
  int wbits = range_width.GetBitLength();
  if (per_thread < 2) per_thread = 2;
  per_thread &= ~1;
  int Ktotal = nthreads * per_thread;
  double sqrtW = pow(2.0, wbits / 2.0);
  // mean jump = K*sqrt(W)/4 as an Int: 2^(wbits/2 - 2 + log2 K). The jump sizes are
  // pseudo random values in [mean/2, 3*mean/2) from a fixed generator: with the
  // negation map the walk adds and subtracts jumps, so sizes with small linear
  // relations (an arithmetic progression, powers of two) make signed sums of a
  // few jumps cancel exactly and the walk falls into fruitless cycles constantly.
  int klog = (int)round(log2((double)Ktotal));
  int mean_bits = wbits / 2 - 2 + klog; if (mean_bits < 8) mean_bits = 8;
  Int mean; mean.SetInt32(1); mean.ShiftL(mean_bits);
  uint64_t rs = 0x9E3779B97F4A7C15ULL;          // splitmix64, fixed seed: same table in every run
  auto nextrand = [&rs]() { uint64_t z = (rs += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); };
  auto random_below = [&](Int &bound) { Int r; r.SetInt32(0); for (int l = 0; l < 4; l++) { r.ShiftL(64); r.Add(nextrand()); } r.Mod(&bound); return r; };
  for (int i = 0; i < JUMPS; i++) {
    jumpD[i].Set(&mean); jumpD[i].ShiftR(1);
    Int r = random_below(mean); jumpD[i].Add(&r);   // [mean/2, 3*mean/2)
    if (jumpD[i].IsEven()) jumpD[i].AddOne();
    jumpP[i] = secp->ComputePublicKey(&jumpD[i]);
  }
  // escape jump: random in [2*mean, 3*mean)
  escD.Set(&mean); escD.ShiftL(1); { Int r = random_below(mean); escD.Add(&r); }
  escP = secp->ComputePublicKey(&escD);
  int e = mean_bits;
  if (dp_bits < 0) {
    // keep the distinguished point overhead (K * 2^dp) near sqrt(W)/16
    dp_bits = (int)floor(wbits / 2.0 - log2((double)Ktotal) - 4);
    if (dp_bits < 0) dp_bits = 0;
    if (dp_bits > 40) dp_bits = 40;
  }
  dp_mask = (dp_bits >= 64) ? ~0ULL : ((1ULL << dp_bits) - 1);

  if (!quiet) {
    char *hs = range_start.GetBase16(), *he = range_end.GetBase16();
    printf("[+] range %s:%s (%d bits)\n", hs, he, wbits); free(hs); free(he);
    printf("[+] %d threads x %d kangaroos, %d jumps of mean 2^%d, dp %d bits, negation map %s\n",
           nthreads, per_thread, JUMPS, e, dp_bits, use_negation ? "on" : "off");
    printf("[+] expected ~%.3g ops (%.2f*sqrt(W))\n", (use_negation ? 1.414 : 2.0) * sqrtW, use_negation ? 1.414 : 2.0);
    fflush(stdout);
  }
  auto t0 = std::chrono::steady_clock::now();
  std::vector<pthread_t> th(nthreads);
  for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
  if (!quiet) {
    while (!found) {
      usleep(1000000);
      double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      uint64_t st = total_steps;
      printf("\r[+] %.3g ops, %.1f Mops/s, %llu dp, %.0fs   ", (double)st, st / sec / 1e6, (unsigned long long)dp_count.load(), sec);
      fflush(stdout);
    }
    printf("\n");
  }
  for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  char *kh = key_found.GetBase16();
  uint64_t st = total_steps;
  printf("[+] found privkey %s\n", kh);
  printf("[+] %llu ops = %.3f*sqrt(W), %.1fs, %.1f Mops/s, %llu dp stored, %llu fruitless, %llu cycles escaped, %llu reseeds\n",
         (unsigned long long)st, st / sqrtW, sec, st / sec / 1e6,
         (unsigned long long)dp_count.load(), (unsigned long long)fruitless.load(),
         (unsigned long long)cycles.load(), (unsigned long long)reseeds.load());
  if (solve_fail || stuck)
    printf("[+] %llu tame/wild meetings failed verification, %llu walkers re-seeded for lack of distinguished points\n",
           (unsigned long long)solve_fail.load(), (unsigned long long)stuck.load());
  free(kh);
  return 0;
}
