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

**Measured.** Results table pending: the 45 to 55 bit evaluation (both
modes, several seeds, BSGS comparison) is being run and will be added here.
Every run checks the found key against the known key.

### Candidates not started

- Herd steering: let the wild herd start at the target minus the interval
  midpoint and run tame and wild in lock step (van Oorschot-Wiener) and
  compare the constant with the symmetric start used now.
- Distinguished point storage on disk with compact 64 bit records, so a
  run can be stopped, resumed and shared between machines.
- Using keyhunt's AVX-512 lanes for the field multiplication inside the
  herd step (the herd is naturally lane parallel).
- Gaudry-Schost with the negation map, which some analyses put a few
  percent ahead of kangaroo for the same memory.

## Running

```
make kangaroo
./kangaroo -p <public key hex> -r <start hex>:<end hex> [-t threads] [-k kangaroos per thread] [-d dp bits] [-n] [-s seed] [-q]
```

`-n` disables the negation map (for A/B measurements). The program prints
the number of group operations as a multiple of sqrt(W), which is the
number to compare between methods.
