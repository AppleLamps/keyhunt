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
 * Gaudry-Schost mode (-g): the interval is re-centred on zero (Q' = Q - c*G
 * with c the midpoint, so k' lies in [-W/2, W/2]) and the negation map then
 * maps the search set onto itself. Tame walkers sample [-W/2, W/2], wild
 * walkers sample k' + [-W/2, W/2]; each walk runs to a distinguished point
 * and then restarts at a fresh pseudo random position (a precomputed offset,
 * wrapped back into the set), so the two herds are uniform samples of their
 * sets and a tame/wild match in the overlap gives k'. With equivalence classes
 * this is the Galbraith-Ruprai setting (about 1.36*sqrt(W) expected), and it
 * is where the negation map belongs: in the travelling herd kangaroo a
 * negation moves a walker to the mirror image of the interval, which kills
 * the herd's drift and costs more than the sqrt(2) it was meant to save.
 *
 * Usage: kangaroo -p <pubkey hex> -r <start>:<end> [-t threads] [-k kangaroos/thread]
 *                 [-d dp bits] [-g] (Gaudry-Schost) [-e | -n] (negation map on | off) [-s seed] [-q]
 *                 [-w work file] [-x max ops]
 * The negation map defaults to off in kangaroo mode and on in Gaudry-Schost mode
 * (measured: it only pays in the latter, see novel/README.md).
 *
 * Work file (-w): every distinguished point is appended to it, and the points
 * already in it are loaded at start, so a run can be stopped (-x, or killed: the
 * file is flushed every second) and resumed, and the files of several machines
 * can be merged with cat. The header pins the target, the range, the mode and
 * the walk parameters (dp bits, jump size), which a resumed run takes from the
 * file so that its walks use the same jump table. In Gaudry-Schost mode the
 * distinguished points are the whole state of the search and nothing is lost;
 * in kangaroo mode the herd positions are not saved, a resumed herd starts
 * again from its seeds and the stored points only add the old trails.
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
#include <string>
#include <vector>
#include <chrono>

#include "../secp256k1/SECP256k1.h"
#include "../secp256k1/Int.h"
#include "../secp256k1/IntGroup.h"
#include "../secp256k1/Point.h"
#include "../secp256k1/Random.h"
#include "../secp256k1/FieldMulSimd.h"

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
static int negation_opt = -1;        // -1 default per mode, 0 forced off (-n), 1 forced on (-e)
static bool gs_mode = false;         // Gaudry-Schost: centred interval, restart at distinguished points
static Int centre;                   // c = a + W/2 (gs_mode): k = c + k'
static Point Qc;                     // the target used by the walk (Q, or Q - c*G in gs_mode)
static Int half_width;               // W/2
#define RESTARTS 64
static Point restartP[RESTARTS];     // restart offsets r_i*G, r_i pseudo random in [0, W)
static Int restartD[RESTARTS];
static Point widthP;                 // W*G, to wrap a position back into [-W/2, W/2]
static bool quiet = false;
static int nthreads = 4, per_thread = 512;
static std::atomic<bool> found(false);
static std::atomic<bool> stop(false);            // -x budget reached
static uint64_t max_ops = 0;
static std::atomic<uint64_t> total_steps(0);     // point additions performed by the walks
static std::atomic<uint64_t> total_seeds(0);     // scalar multiplications (seeds and re-seeds)
static std::atomic<uint64_t> dp_count(0);
static std::atomic<uint64_t> fruitless(0), reseeds(0), cycles(0), solve_fail(0), stuck(0);
static Int key_found;

// Work file: a 128 byte header, then one 64 byte record per distinguished point.
// Files can be concatenated: a header found between records is checked and skipped.
static const char DP_MAGIC[8] = {'K','G','D','P','0','0','0','1'};
struct DPFileHeader {
  char magic[8];
  uint8_t gs, negation, dp_bits, qodd;
  uint16_t mean_bits;
  uint8_t qx[32], start[32], end[32];
  uint8_t pad[128 - 8 - 6 - 96];
};
struct DPRecord { uint64_t x1, x2; int8_t sign; uint8_t yodd; uint8_t pad[6]; uint64_t dist[5]; };
static_assert(sizeof(DPFileHeader) == 128, "DP file header size");
static_assert(sizeof(DPRecord) == 64, "DP record size");
static FILE *dpfile = NULL;

// Distinguished point table: x (two limbs) -> (sign, distance)
struct DPEntry { uint64_t x2; int8_t sign; uint8_t yodd; Int dist; };
static std::unordered_multimap<uint64_t, DPEntry> dptable;
static pthread_mutex_t dpmutex = PTHREAD_MUTEX_INITIALIZER;

static void reduce_mod_order(Int &k) {
  // Int is two's complement 320 bit; bring k into [0, n)
  while (k.IsNegative()) k.Add(&secp->order);
  while (k.IsGreaterOrEqual(&secp->order)) k.Sub(&secp->order);
}

// k is the coefficient of G for Qc; in gs_mode the real key is k + centre
static bool verify(Int &k) {
  Int t(k);
  if (gs_mode) t.Add(&centre);
  reduce_mod_order(t);
  if (t.IsZero()) return false;
  Point P = secp->ComputePublicKey(&t);
  if (!(P.x.IsEqual(&Q.x) && P.y.IsEqual(&Q.y))) return false;
  k.Set(&t);
  return true;
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

static void write_dp(uint64_t k1, uint64_t k2, int8_t sign, uint8_t yodd, Int &dist) {
  DPRecord r; memset(&r, 0, sizeof(r));
  r.x1 = k1; r.x2 = k2; r.sign = sign; r.yodd = yodd;
  for (int l = 0; l < 5; l++) r.dist[l] = dist.bits64[l];
  if (fwrite(&r, sizeof(r), 1, dpfile) != 1) { perror("work file"); exit(1); }
}

// Distinguished point (x limbs 1 and 2, y parity) reached by a kangaroo with
// (sign, dist). Returns true if a solution was found through it, or the same point
// was already known (the caller re-seeds the walker). New points go to the work file.
static bool report_dp_raw(uint64_t k1, uint64_t k2, uint8_t yodd, int8_t sign, Int &dist, bool save) {
  bool hit = false;
  pthread_mutex_lock(&dpmutex);
  auto range = dptable.equal_range(k1);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second.x2 != k2) continue;
    DPEntry e = it->second;
    pthread_mutex_unlock(&dpmutex);
    // Same x but opposite y (only possible with the negation map off): the new
    // point is -(stored point), i.e. the stored relation holds for (-sign, -dist).
    int8_t s2 = sign; Int d2(dist);
    if (e.yodd != yodd) { s2 = -s2; d2.Neg(); }
    if (solve(e.sign, e.dist, s2, d2)) return true;
    hit = true;     // same point, same herd: nothing to learn, the walker is re-seeded
    pthread_mutex_lock(&dpmutex);
    break;
  }
  if (!hit) {
    DPEntry e; e.x2 = k2; e.sign = sign; e.yodd = yodd; e.dist.Set(&dist); dptable.emplace(k1, e); dp_count++;
    if (save && dpfile) write_dp(k1, k2, sign, yodd, dist);
  }
  pthread_mutex_unlock(&dpmutex);
  return hit;      // caller re-seeds on a fruitless meeting
}

static bool report_dp(Point &P, int8_t sign, Int &dist) {
  return report_dp_raw(P.x.bits64[1], P.x.bits64[2], P.y.IsOdd(), sign, dist, true);
}

static void fill_header(DPFileHeader &h, int mean_bits) {
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, DP_MAGIC, 8);
  h.gs = gs_mode; h.negation = use_negation; h.dp_bits = (uint8_t)dp_bits; h.qodd = Q.y.IsOdd();
  h.mean_bits = (uint16_t)mean_bits;
  Q.x.Get32Bytes(h.qx); range_start.Get32Bytes(h.start); range_end.Get32Bytes(h.end);
}

// Header of an existing work file: the search it belongs to must be this one.
// Returns its mean_bits and sets dp_bits, or -1 when the file is empty or missing.
static int read_work_header(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  DPFileHeader h;
  size_t n = fread(&h, sizeof(h), 1, f);
  fclose(f);
  if (n != 1) return -1;
  DPFileHeader want; fill_header(want, 0);
  if (memcmp(h.magic, DP_MAGIC, 8) != 0) { fprintf(stderr, "%s is not a kangaroo work file\n", path); exit(1); }
  if (memcmp(h.qx, want.qx, 32) || h.qodd != want.qodd || memcmp(h.start, want.start, 32) || memcmp(h.end, want.end, 32)) {
    fprintf(stderr, "%s belongs to another public key or range\n", path); exit(1);
  }
  if (h.gs != want.gs || h.negation != want.negation) {
    fprintf(stderr, "%s was made with %s, negation map %s: use the same options\n", path,
            h.gs ? "-g" : "kangaroo mode", h.negation ? "on" : "off");
    exit(1);
  }
  dp_bits = h.dp_bits;
  return h.mean_bits;
}

// Loads every distinguished point of the work file (all headers must match the first one).
// Returns true if two of them already solve the key.
static bool load_work_file(const char *path, const DPFileHeader &want, uint64_t &loaded) {
  FILE *f = fopen(path, "rb");
  loaded = 0;
  if (!f) return false;
  DPRecord r;
  bool solved = false;
  while (fread(&r, sizeof(r), 1, f) == 1) {
    if (memcmp(&r, DP_MAGIC, 8) == 0) {            // a header (first one, or a concatenated file)
      DPFileHeader h; memcpy(&h, &r, sizeof(r));
      if (fread((char *)&h + sizeof(r), sizeof(h) - sizeof(r), 1, f) != 1) break;
      if (memcmp(&h, &want, sizeof(h)) != 0) { fprintf(stderr, "%s: a concatenated file has other parameters\n", path); exit(1); }
      continue;
    }
    Int d; d.SetInt32(0);
    for (int l = 0; l < 5; l++) d.bits64[l] = r.dist[l];
    loaded++;
    if (report_dp_raw(r.x1, r.x2, r.yodd, r.sign, d, false) && found) { solved = true; break; }
  }
  fclose(f);
  return solved;
}

static uint64_t base_seed;

static inline uint64_t splitmix64(uint64_t &st) {
  uint64_t z = (st += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// uniform value in [0, bound) from the walker's own stream (bound < 2^256)
static Int rand_below(Int &bound, uint64_t &st) {
  Int r; r.SetInt32(0);
  for (int l = 0; l < 4; l++) { r.ShiftL(64); r.Add(splitmix64(st)); }
  r.Mod(&bound);
  return r;
}

struct Walker {
  Point P; Int dist; int8_t sign; uint64_t since_dp; uint64_t rng;
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

// Position bookkeeping: tame point = dist*G, wild point = sign*Qc + dist*G.
// Kangaroo mode: tame at a random position inside the interval, wild at Q plus a
// random offset in [-W/2, W/2), so both herds cover the same region of the line.
// GS mode: tame dist and wild offset both uniform in [-W/2, W/2).
static Point scalar_point(Int &r) {
  Int mag(r); bool neg = mag.IsNegative(); if (neg) mag.Neg();
  Point R = secp->ComputePublicKey(&mag);
  if (neg) R = secp->Negation(R);
  return R;
}

static void seed(Walker &w, bool tame) {
  Int r;
  total_seeds++;
  for (;;) {
    r = rand_below(range_width, w.rng); r.Add(&range_start);
    if (gs_mode || !tame) { r.Sub(&range_start); r.Sub(&half_width); }   // [-W/2, W/2)
    if (r.IsZero()) continue;                 // the identity has no affine form
    if (tame) break;
    Point R = scalar_point(r);
    if (R.x.IsEqual(&Qc.x)) continue;         // Qc + (+-Qc): doubling or the identity
    w.P = secp->AddDirect(Qc, R);
    break;
  }
  if (tame) {
    w.P = scalar_point(r);
    w.dist.Set(&r); w.sign = 0;
  } else {
    w.dist.Set(&r); w.sign = 1;
  }
  w.since_dp = 0;
  reset_cycle_state(w);
  canonicalize(w);
}

// GS restart after a distinguished point: add a pseudo random offset and wrap the
// position back into [-W/2, W/2) with W*G, so the walker's set is sampled uniformly
// without a scalar multiplication. The offset index comes from the point, so the
// restart is still a function of the walk.
// Int's comparisons are unsigned on the limbs; distances are signed (two's complement)
static inline bool signed_less(Int &a, Int &b) {
  bool na = a.IsNegative(), nb = b.IsNegative();
  if (na != nb) return na;
  return a.IsLower(&b);   // same sign: the unsigned order is the signed order
}

// Returns the number of point additions performed (0 when the walker was re-seeded instead).
static int gs_restart(Walker &w) {
  int adds = 0;
  int i = (int)(w.P.x.bits64[2] % RESTARTS);
  if (restartP[i].x.IsEqual(&w.P.x)) { seed(w, w.sign == 0); reseeds++; return 0; }   // no affine sum
  w.P = secp->AddDirect(w.P, restartP[i]); adds++;
  w.dist.Add(&restartD[i]);
  // the position of a walker is sign*k' + dist; only dist is known, wrap it into
  // the window [-half, W - half) of exactly W values (W may be odd), moving the
  // point by the matching multiple of W*G
  Int lo(half_width); lo.Neg();
  Int hi(range_width); hi.Sub(&half_width);
  while (!signed_less(w.dist, hi) || signed_less(w.dist, lo)) {
    bool up = signed_less(w.dist, lo);
    Point step = up ? widthP : secp->Negation(widthP);
    if (step.x.IsEqual(&w.P.x)) { seed(w, w.sign == 0); reseeds++; return adds; }
    if (up) w.dist.Add(&range_width); else w.dist.Sub(&range_width);
    w.P = secp->AddDirect(w.P, step); adds++;
  }
  reset_cycle_state(w);
  canonicalize(w);
  return adds;
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
  for (int i = 0; i < K; i++) {
    // each walker owns a stream derived from the run seed, its thread and its index
    w[i].rng = base_seed ^ ((uint64_t)(id + 1) << 40) ^ ((uint64_t)(i + 1) << 8);
    splitmix64(w[i].rng);
    seed(w[i], (i & 1) == 0);   // half tame, half wild
  }
  std::vector<Int> dx(K);
  IntGroup grp(K); grp.Set(dx.data());
  std::vector<uint8_t> idx(K), bad(K);
  // one slope, square and y term per lane, so the field multiplies of the
  // whole batch go through fieldmul_batch (AVX2/AVX-512 when available)
  std::vector<Int> s(K), p(K), t(K);
  uint64_t local = 0;
  const uint64_t stuck_limit = 64ULL << dp_bits;
  while (!found && !stop) {
    for (int i = 0; i < K; i++) {
      uint8_t j = (uint8_t)(w[i].P.x.bits64[1] % JUMPS);
      idx[i] = j;
      dx[i].ModSub(&jumpP[j].x, &w[i].P.x);
      // a zero denominator would zero every lane of the batch inversion: take it
      // out (inverse of 1) and re-seed that walker alone afterwards
      bad[i] = dx[i].IsZero();
      if (bad[i]) dx[i].SetInt32(1);
    }
    grp.ModInv();
    // P + J for every lane, in phases: s = (J.y - P.y)/dx, p = s^2,
    // x3 = p - P.x - J.x, y3 = s*(P.x - x3) - P.y. Bad lanes (dx = 1) compute a
    // meaningless point that the re-seed below throws away.
    for (int i = 0; i < K; i++) s[i].ModSub(&jumpP[idx[i]].y, &w[i].P.y);
    fieldmul_batch(s.data(), s.data(), dx.data(), K);
    fieldsqr_batch(p.data(), s.data(), K);
    for (int i = 0; i < K; i++) {
      p[i].ModSub(&w[i].P.x); p[i].ModSub(&jumpP[idx[i]].x);   // x3
      t[i].ModSub(&w[i].P.x, &p[i]);
    }
    fieldmul_batch(t.data(), t.data(), s.data(), K);
    for (int i = 0; i < K; i++) {
      Walker &k = w[i];
      if (bad[i]) { seed(k, k.sign == 0); reseeds++; continue; }
      local++;
      t[i].ModSub(&k.P.y);                                      // y3
      k.P.x.Set(&p[i]); k.P.y.Set(&t[i]);
      k.dist.Add(&jumpD[idx[i]]);
      canonicalize(k);
      if (use_negation && cycle_check(k)) { cycles++; local++; }   // the escape is one addition
      k.since_dp++;
      if ((k.P.x.bits64[0] & dp_mask) == 0) {
        k.since_dp = 0;
        if (report_dp(k.P, k.sign, k.dist)) {
          if (found) break;
          seed(k, k.sign == 0); reseeds++;
        } else if (gs_mode) local += gs_restart(k);
      } else if (k.since_dp > stuck_limit) { seed(k, k.sign == 0); reseeds++; stuck++; }
      if (gs_mode && !found) {
        // a tame walker that left [-W/2, W/2) samples nothing useful: restart it
        Int mag(k.dist); if (mag.IsNegative()) mag.Neg();
        if (k.sign == 0 && mag.IsGreater(&half_width)) local += gs_restart(k);
      }
    }
    if (local >= 65536) {
      // The main thread polls the -x budget every 1/10 s, which on a fast
      // machine is millions of ops late: enforce it here, at the flush, so
      // the overshoot is bounded by 65536 ops per thread.
      total_steps += local; local = 0;
      if (max_ops && total_steps >= max_ops) stop = true;
    }
  }
  total_steps += local;
  (void)id;
  return NULL;
}

static void usage() {
  fprintf(stderr, "usage: kangaroo -p <pubkey hex> -r <start>:<end> [-t threads] [-k kangaroos/thread] [-d dp bits] [-g] [-e|-n] [-s seed] [-q] [-w work file] [-x max ops]\n");
  exit(1);
}

int main(int argc, char **argv) {
  const char *pub = NULL, *range = NULL, *workfile = NULL;
  uint64_t seedv = 0; bool have_seed = false;
  int c;
  while ((c = getopt(argc, argv, "p:r:t:k:d:negs:qw:x:")) != -1) {
    switch (c) {
      case 'p': pub = optarg; break;
      case 'r': range = optarg; break;
      case 't': nthreads = atoi(optarg); break;
      case 'k': per_thread = atoi(optarg); break;
      case 'd': dp_bits = atoi(optarg); break;
      case 'n': negation_opt = 0; break;
      case 'e': negation_opt = 1; break;
      case 'g': gs_mode = true; break;
      case 's': seedv = strtoull(optarg, NULL, 10); have_seed = true; break;
      case 'q': quiet = true; break;
      case 'w': workfile = optarg; break;
      case 'x': max_ops = strtoull(optarg, NULL, 10); break;
      default: usage();
    }
  }
  if (!pub || !range) usage();
  if (nthreads < 1 || nthreads > 1024) { fprintf(stderr, "threads must be 1..1024\n"); return 1; }
  if (per_thread < 2 || per_thread > 65536) { fprintf(stderr, "kangaroos per thread must be 2..65536\n"); return 1; }
  if (dp_bits > 48) { fprintf(stderr, "dp bits must be 0..48\n"); return 1; }
  use_negation = negation_opt < 0 ? gs_mode : (negation_opt == 1);
  secp = new Secp256K1(); secp->Init();
  base_seed = have_seed ? seedv : ((uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)getpid() << 32);

  bool comp;
  char pubbuf[200]; strncpy(pubbuf, pub, 199); pubbuf[199] = 0;
  if (!secp->ParsePublicKeyHex(pubbuf, Q, comp)) { fprintf(stderr, "bad public key\n"); return 1; }
  char rbuf[200]; strncpy(rbuf, range, 199); rbuf[199] = 0;
  char *colon = strchr(rbuf, ':'); if (!colon) usage(); *colon = 0;
  range_start.SetBase16(rbuf); range_end.SetBase16(colon + 1);
  if (!range_start.IsLower(&range_end)) { fprintf(stderr, "empty range\n"); return 1; }
  range_width.Sub(&range_end, &range_start);
  int wbits = range_width.GetBitLength();
  if (wbits <= 6) {
    // a few dozen keys: check them directly, the walks need room to move
    Int k(range_start);
    while (k.IsLowerOrEqual(&range_end)) {
      if (!k.IsZero()) { Point P = secp->ComputePublicKey(&k); if (P.x.IsEqual(&Q.x) && P.y.IsEqual(&Q.y)) { char *kh = k.GetBase16(); printf("[+] found privkey %s (direct check, tiny range)\n", kh); free(kh); return 0; } }
      k.AddOne();
    }
    printf("[+] key not in range\n");
    return 1;
  }
  half_width.Set(&range_width); half_width.ShiftR(1);
  centre.Set(&range_start); centre.Add(&half_width);
  if (gs_mode) {
    Point C = secp->ComputePublicKey(&centre);
    if (C.x.IsEqual(&Q.x) && C.y.IsEqual(&Q.y)) {      // k is the midpoint: Q - c*G would be the identity
      char *kh = centre.GetBase16(); printf("[+] found privkey %s (interval midpoint)\n", kh); free(kh);
      return 0;
    }
    Point nC = secp->Negation(C);
    Qc = secp->AddDirect(Q, nC);                     // Q' = Q - c*G, k' in [-W/2, W/2]
    widthP = secp->ComputePublicKey(&range_width);
  } else Qc = Q;
  if (per_thread < 2) per_thread = 2;
  per_thread &= ~1;
  int Ktotal = nthreads * per_thread;
  double Wd = 0; for (int l = 3; l >= 0; l--) Wd = Wd * 18446744073709551616.0 + (double)range_width.bits64[l];
  double sqrtW = sqrt(Wd);
  // mean jump = K*sqrt(W)/4 as an Int: 2^(wbits/2 - 2 + log2 K). The jump sizes are
  // pseudo random values in [mean/2, 3*mean/2) from a fixed generator: with the
  // negation map the walk adds and subtracts jumps, so sizes with small linear
  // relations (an arithmetic progression, powers of two) make signed sums of a
  // few jumps cancel exactly and the walk falls into fruitless cycles constantly.
  int klog = (int)round(log2((double)Ktotal));
  int mean_bits = wbits / 2 - 2 + klog; if (mean_bits < 8) mean_bits = 8;
  int file_mean_bits = workfile ? read_work_header(workfile) : -1;   // also sets dp_bits
  if (dp_bits < 0) {
    dp_bits = (int)floor(wbits / 2.0 - log2((double)Ktotal) - 4);
    if (dp_bits < 0) dp_bits = 0;
    if (dp_bits > 40) dp_bits = 40;
  }
  if (gs_mode) {
    // a walk of about 2^dp steps should cover a small fraction (1/64) of the set
    mean_bits = wbits - dp_bits - 6; if (mean_bits < 8) mean_bits = 8;
  }
  if (file_mean_bits >= 0) mean_bits = file_mean_bits;   // same jump table as the runs before
  Int mean; mean.SetInt32(1); mean.ShiftL(mean_bits);
  uint64_t rs = 0x9E3779B97F4A7C15ULL;          // fixed seed: the same jump table in every run
  auto random_below = [&](Int &bound) { return rand_below(bound, rs); };
  for (int i = 0; i < JUMPS; i++) {
    jumpD[i].Set(&mean); jumpD[i].ShiftR(1);
    Int r = random_below(mean); jumpD[i].Add(&r);   // [mean/2, 3*mean/2)
    if (jumpD[i].IsEven()) jumpD[i].AddOne();
    jumpP[i] = secp->ComputePublicKey(&jumpD[i]);
  }
  // escape jump: random in [2*mean, 3*mean)
  escD.Set(&mean); escD.ShiftL(1); { Int r = random_below(mean); escD.Add(&r); }
  escP = secp->ComputePublicKey(&escD);
  if (gs_mode) {
    for (int i = 0; i < RESTARTS; i++) {
      restartD[i] = random_below(range_width);
      if (restartD[i].IsZero()) restartD[i].AddOne();        // the identity has no affine form
      restartP[i] = secp->ComputePublicKey(&restartD[i]);
    }
  }
  int e = mean_bits;
  dp_mask = (dp_bits >= 64) ? ~0ULL : ((1ULL << dp_bits) - 1);

  uint64_t loaded = 0;
  if (workfile) {
    DPFileHeader h; fill_header(h, mean_bits);
    if (load_work_file(workfile, h, loaded)) {
      char *kh = key_found.GetBase16();
      printf("[+] found privkey %s (from the %llu distinguished points of %s)\n", kh, (unsigned long long)loaded, workfile);
      free(kh);
      return 0;
    }
    bool fresh = file_mean_bits < 0;
    dpfile = fopen(workfile, fresh ? "wb" : "ab");
    if (!dpfile) { perror(workfile); return 1; }
    if (fresh && fwrite(&h, sizeof(h), 1, dpfile) != 1) { perror(workfile); return 1; }
    fflush(dpfile);
    // new walks, not a replay of the ones that produced the stored points
    if (loaded) base_seed ^= loaded * 0xD6E8FEB86659FD93ULL;
  }

  if (!quiet) {
    char *hs = range_start.GetBase16(), *he = range_end.GetBase16();
    printf("[+] range %s:%s (%d bits)\n", hs, he, wbits); free(hs); free(he);
    printf("[+] %d threads x %d kangaroos, %d jumps of mean 2^%d, dp %d bits, negation map %s\n",
           nthreads, per_thread, JUMPS, e, dp_bits, use_negation ? "on" : "off");
    double expect = gs_mode ? (use_negation ? 1.36 : 2.08) : (use_negation ? 1.414 : 2.0);
    printf("[+] mode %s, expected ~%.3g ops (%.2f*sqrt(W))\n", gs_mode ? "Gaudry-Schost" : "kangaroo", expect * sqrtW, expect);
    if (workfile) printf("[+] work file %s: %llu distinguished points loaded\n", workfile, (unsigned long long)loaded);
    fflush(stdout);
  }
  auto t0 = std::chrono::steady_clock::now();
  std::vector<pthread_t> th(nthreads);
  for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
  // the work file is flushed and the -x budget checked every 1/10 s; stats every second
  for (int tick = 1; !found && !stop; tick++) {
    usleep(100000);
    if (dpfile) { pthread_mutex_lock(&dpmutex); fflush(dpfile); pthread_mutex_unlock(&dpmutex); }
    if (max_ops && total_steps >= max_ops) stop = true;
    if (!quiet && tick % 10 == 0) {
      double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      uint64_t st = total_steps;
      printf("\r[+] %.3g ops, %.1f Mops/s, %llu dp, %.0fs   ", (double)st, st / sec / 1e6, (unsigned long long)dp_count.load(), sec);
      fflush(stdout);
    }
  }
  if (!quiet) printf("\n");
  for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
  if (dpfile) { fclose(dpfile); dpfile = NULL; }
  if (!found) {
    printf("[+] stopped after %llu ops (-x), key not found yet; %llu distinguished points in %s\n",
           (unsigned long long)total_steps.load(), (unsigned long long)dp_count.load(), workfile ? workfile : "memory (no -w: lost)");
    return 2;
  }
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  char *kh = key_found.GetBase16();
  uint64_t st = total_steps;
  printf("[+] found privkey %s\n", kh);
  printf("[+] %llu ops = %.3f*sqrt(W), %.1fs, %.1f Mops/s, %llu dp stored, %llu fruitless, %llu cycles escaped, %llu reseeds\n",
         (unsigned long long)st, st / sqrtW, sec, st / sec / 1e6,
         (unsigned long long)dp_count.load(), (unsigned long long)fruitless.load(),
         (unsigned long long)cycles.load(), (unsigned long long)reseeds.load());
  printf("[+] ops are point additions (walk steps, restarts, wraps, cycle escapes); %llu scalar multiplications for seeds are not included\n",
         (unsigned long long)total_seeds.load());
  if (solve_fail || stuck)
    printf("[+] %llu tame/wild meetings failed verification, %llu walkers re-seeded for lack of distinguished points\n",
           (unsigned long long)solve_fail.load(), (unsigned long long)stuck.load());
  free(kh);
  return 0;
}
