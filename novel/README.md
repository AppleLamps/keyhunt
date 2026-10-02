# novel: new methods for the interval discrete log on secp256k1

A side project of this repository: self contained experiments that try to
move the state of the art for the Bitcoin puzzle transaction, aimed at
puzzle 140 (public key known, range 2^139 to 2^140). Everything here is
runnable on a plain CPU, and every method is measured on solved puzzles
whose keys are known, so a claim is always backed by a verified key and a
measured operation count.

## Ground truth first

Honesty about the problem is the starting point, or the project wastes its
time:

- With the **address only**, the key must be found by hashing candidates.
  There is no shortcut: the range of puzzle 140 has 2^139 keys.
- With the **public key**, the problem is a discrete logarithm in an
  interval. Every known method (BSGS, Pollard rho and kangaroo, Gaudry-Schost)
  needs on the order of the square root of the range, and in the generic
  group model that is a proven lower bound (Shoup 1997). For 2^139 that is
  about 2^70 group operations, which is the scale of a large GPU fleet for
  years. Nothing solvable here changes that exponent.
- What research can change is the **constant** in front of the square
  root and the **memory** needed. The negation map (counting P and -P as
  one element) is worth sqrt(2); a smarter use of distinguished points,
  better jump distributions and cheaper group operations are worth smaller
  factors each. Those gains compound, and on a 2^70 problem a factor of 2
  is years of machine time.

An "idea" is accepted into this directory only with a test that recovers a
known key and a measured constant, so that progress is real and comparable.

## Idea log

### 1. Kangaroo with distinguished points, batched arithmetic and the negation map (`kangaroo.cpp`)

**Why.** keyhunt's only public key method is BSGS, whose memory grows with
the square root of the range: it cannot reach puzzle sized intervals on a
CPU. Pollard's kangaroo has the same square root running time with memory
proportional only to the number of distinguished points, so the range size
is no longer limited by RAM, and it parallelises perfectly across threads
and machines by sharing those points.

**What is new relative to the repository.** A kangaroo implementation on
top of keyhunt's own fast field arithmetic (the 4 limb modular ops, batch
inversion and the Montgomery trick across the whole herd, one inversion per
step for all kangaroos), plus the negation map for the extra sqrt(2).

**Design.** Two herds of equal size: tame kangaroos at known positions in
the interval, wild kangaroos at the target plus a known offset. All jump
by a table of 256 pseudo random sizes with mean K*sqrt(W)/4, so the herds
travel together; a point whose x has `dp` low zero bits is distinguished
and stored, and two kangaroos that ever land on the same point reach the
same distinguished point and are detected there. With the negation map
the walk always continues from the representative of {P, -P} with even y,
tracking the sign of the target coefficient and the distance.

**What went wrong and what it taught.** Two versions were wrong before the
one that works, and both lessons are general:

1. The walk must be a pure function of the current point. A first attempt
   avoided 2-cycles by refusing to repeat the previous jump; that made the
   next step depend on history, so two kangaroos on the same point could
   leave it differently, and collisions were missed.
2. Jump sizes must have no small linear relations. Evenly spaced jumps on
   a lattice (and powers of two) looked natural, but with the negation
   map the walk adds and subtracts jumps, and signed sums of three or four
   of them cancelled exactly: 46% of cycle escapes led straight back into
   the same cycle (an identity such as J175 - J178 - J44 - J62 + esc = J19
   held exactly), the work exploded to 60 to 300 times the square root of
   the range. Pseudo random jump sizes remove the relations, and the
   constant fell back to where theory puts it.

   Fruitless cycles are handled Brent style: the minimum x of each window
   of 16 steps is remembered, the same minimum in two consecutive windows
   means a cycle, and the walker escapes from that minimum element with a
   dedicated jump, so two walkers trapped in the same cycle leave it the
   same way and the walk stays deterministic.

**Measured** (4 threads x 256 kangaroos, solved puzzles, every run checked
against the known key; ops as a multiple of sqrt(W), several seeds each):

| method | 45 bits | 50 bits | 55 bits | mean |
| --- | --- | --- | --- | --- |
| kangaroo, negation off (`./kangaroo`) | 2.34 1.34 2.37 3.72 2.59 2.19 | 1.41 1.77 2.84 0.74 | 2.19 1.09 | **1.9** (theory 2.0) |
| kangaroo, negation on (`-e`) | 2.86 2.90 3.96 2.74 0.84 2.04 | 2.23 2.82 1.46 5.99 | 2.87 4.86 | 3.0 |
| Gaudry-Schost, negation on (`-g`) | 1.48 2.35 2.02 1.61 | 2.99 1.78 0.48 | | **1.6** (theory 1.36) |
| Gaudry-Schost, negation off (`-g -n`) | 2.94 3.18 1.17 3.48 | 1.97 0.48 0.94 | | 2.0 (theory 2.08) |

A second run with 20 seeds per method at 45 bits (mean, standard
deviation, standard error of the mean; every run recovered the key). The
Gaudry-Schost rows are from the final code after the review fixes
(parity aware distinguished point matches, two sided restart wrap, exact
operation counting, reproducible `-s` seeds); earlier measurements of the
same rows read 1.84 then 1.51, and 2.23 then 2.45, all within the standard
errors of the final values:

| method | mean | sd | se |
| --- | --- | --- | --- |
| kangaroo, negation off | 2.34 | 1.18 | 0.26 |
| kangaroo, negation on (`-e`) | 2.46 | 1.30 | 0.29 |
| Gaudry-Schost, negation on (`-g`) | **1.70** | 0.83 | 0.19 |
| Gaudry-Schost, negation off (`-g -n`) | 2.46 | 1.03 | 0.23 |

Throughput on this 4 vCPU Xeon: 14 to 19 M group operations/s. For scale,
keyhunt's BSGS on the same puzzle 50 took 494 s wall (table build included)
against 1 to 4 s for the kangaroo: BSGS pays for its table every time and
cannot grow past RAM, the kangaroo pays nothing up front.

**Conclusions.**

1. The travelling herd kangaroo lands on its textbook constant (about 2.0).
2. The negation map does **not** help the travelling herd: 2.46 against
   2.34 over 20 seeds, where theory for a working negation map would say
   1.41. (The first small sample read 3.0 and looked like a large loss; the
   larger sample says "no gain", which is the honest statement.) Negating a
   walker sends its position from the interval to its mirror image near the
   group order, so each herd splits across two distant bands and the steady
   drift the kangaroo relies on becomes a zero drift random walk in absolute
   position. The sqrt(2) only exists when the search set is symmetric under
   negation, which is the next point.
3. In Gaudry-Schost form (interval re-centred on zero, restarts at
   distinguished points) the negation map pays: 1.70 against 2.46 without
   it and against 2.34 for the plain kangaroo, towards the 1.36 that
   Galbraith and Ruprai prove and a 1.4x saving over the plain kangaroo.
   The remaining gap to theory is distinguished point overhead and the
   untuned set shapes; the GS set shapes of the paper (a tame set wider
   than the wild set) are the next tuning. The variance of a single search
   is large (0.15 to 5 times sqrt(W)), so comparisons need 20 or more
   seeds, as here.

So the project's first result is a method keyhunt did not have (square root
time with no table) and a measured, explained negative result about where
the negation map belongs.

### 2. Gaudry-Schost with equivalence classes (`-g`, in `kangaroo.cpp`)

Centre the interval (Q' = Q - c*G, k' in [-W/2, W/2]), sample tame points
uniformly from [-W/2, W/2] and wild points from k' + [-W/2, W/2], walk each
to a distinguished point and restart with a cheap precomputed offset
wrapped back into the set, so no scalar multiplication is needed per
restart. Collisions in the overlap of the two sets give k'. Negation maps
the sets onto themselves, so equivalence classes halve the effective set
size. Status: implemented, verified on 40 to 50 bits, constant 1.70 over
20 seeds at 45 bits: the best method in this directory so far.

### 3. Generator lineage test (`lineage.cpp`, `make lineage`)

**The bet.** Nobody has to beat the square root if the puzzle creator's keys
came from a weak or seeded generator. 83 keys are known, puzzles 1 to 70
complete and in order. If they are the masked outputs of a seeded
generator, the small puzzles pin the seed cheaply (puzzle n reveals n-1
bits of output), the large ones confirm it, and every unsolved puzzle's key
follows at once. The keys look uniform, but so do the outputs of any seeded
generator, so uniformity does not rule this out; only a direct search does.

**What is searched.** Generator families: Java `Random` (48 bit LCG), glibc
`random()`, MSVC and BSD `rand()`, minstd, xorshift32, xorshift64*,
splitmix64, PCG32, MT19937 through `init_genrand` and through
`init_by_array` (which is what Python's `random.seed(int)` and numpy use),
MT19937-64. Derivations of an n bit key from the stream, all forcing the top bit as the
puzzle does: low n-1 bits of one word, high n-1 bits of one word, a
continuous bit stream, rejection of n bit words until the top bit is set,
Python's `getrandbits(n-1)`, Python's `randrange(2^(n-1), 2^n)`; each with
32 or 64 bit words and with 0 or 1 unused outputs between puzzles. Seed
spaces: every 32 bit seed for every family except the Mersenne Twisters
(a 624 word initialisation per seed), which get 0..2^26 plus every second
of 2013 to 2016. A match on puzzles 1 to 32 is reported with the seed.
`./lineage selftest` (part of `make test`) plants keys from six known
seed and derivation combinations and must recover them all. The full run
is `./lineage <threads> [from [to]]`; the optional family index range
(0..14, in the order printed) lets a long run be split or resumed.

**Result.** Pending: the run is in progress and this line is replaced by
its outcome.

### 4. Unknown parameter recurrence fit (`lcgfit.cpp`, `make lcgfit`)

**The bet.** The seed search above only covers generators whose parameters
are known. This one asks whether the keys are an affine recurrence modulo
2^64 with *any* multiplier, increment and seed (`s' = a*s + c`, order 1, or
`s' = a*s + b*s'' + c`, order 2), with the key being the low bits of an
output and up to three unused outputs between puzzles. Puzzles 1 to 65 are
used: puzzle 65 contributes the low 64 bits of its output, which is what
pins the top bit of a modulo 2^64 candidate. There is no seed
space to enumerate: modulo 2^j the recurrence involves only the low j bits
of its parameters, and puzzle n reveals the low n-1 bits of its output, so
the parameters are lifted one bit at a time from the bottom, discarding
every extension that contradicts a known key bit. A false hypothesis dies
within a few levels; a true one survives to 64 bits. The harness is
validated by planting keys from a random LCG and recovering its parameters
(`./lcgfit s`, part of `make test`).

**Result: negative, and decisively so.** Every candidate dies at the first
level for every order and stride. The reason is the oldest known weakness
of power of two LCGs: their lowest bit has period at most 2 (order 1) or 3
(order 2), and the keys' lowest bits run 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, ...
from puzzle 2 on. So whatever produced the keys, it was not an affine
recurrence modulo a power of two read from the low bits.

### Candidates not started

- Lineage, wider: string seeds (`random.seed("...")` hashes the string),
  Java's default seed (`System.nanoTime()` xor a uniquifier, a 64 bit space
  that needs the lattice attack on truncated LCG outputs rather than brute
  force), OpenSSL `RAND_bytes` with a known broken seeding (the 2008 Debian
  bug: 32767 possible streams per architecture), and "generated on a
  specific wallet software" hypotheses (Bitcoin Core's `GetRandBytes` of the
  time was OpenSSL based).
- Herd steering: let the wild herd start at the target minus the interval
  midpoint and run tame and wild in lock step (van Oorschot-Wiener) and
  compare the constant with the symmetric start used now.
- Distinguished point storage on disk with compact 64 bit records, so a
  run can be stopped, resumed and shared between machines.
- Using keyhunt's AVX-512 lanes for the field multiplication inside the
  herd step (the herd is naturally lane parallel).
- Gaudry-Schost set shapes from Galbraith-Ruprai (tame set wider than the
  wild set), and the 3 and 4 kangaroo variants of Galbraith-Pollard-Ruprai
  (1.72*sqrt(W) without equivalence classes).

## Running

```
make kangaroo
./kangaroo -p <public key hex> -r <start hex>:<end hex> [-t threads] [-k kangaroos per thread] [-d dp bits] [-n] [-s seed] [-q]
```

`-g` selects Gaudry-Schost; `-e` / `-n` force the negation map on / off
(default: off for kangaroo, on for Gaudry-Schost). The program prints the
number of group operations as a multiple of sqrt(W), which is the number
to compare between methods.
