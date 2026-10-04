# keyhunt

A command line tool that searches key ranges on the secp256k1 curve (Bitcoin, Ethereum) for private keys that match a list of targets. It was written for the [Bitcoin puzzle transaction](https://privatekeys.pw/puzzles/bitcoin-puzzle-tx), where the private key is known to lie in a small, fixed range, and it is best used for that: searching a range that is small enough to actually finish.

Original author: AlbertoBSD ([albertobsd/keyhunt](https://github.com/albertobsd/keyhunt), [BitcoinTalk thread](https://bitcointalk.org/index.php?topic=5322040.0)). Released under the MIT license.

> **Scope.** The whole 256-bit key space cannot be brute forced, and nothing here changes that. Use this for puzzles and for ranges you have a real reason to believe contain the key.

## Contents

- [What it can search for](#what-it-can-search-for)
- [Quick start](#quick-start)
- [Building](#building)
- [Command line reference](#command-line-reference)
- [Modes](#modes)
  - [address](#address-mode) and [vanity](#vanity-search)
  - [rmd160](#rmd160-mode)
  - [xpoint](#xpoint-mode)
  - [bsgs](#bsgs-mode-baby-step-giant-step)
  - [minikeys](#minikeys-mode)
  - [Ethereum](#ethereum)
- [Endomorphism](#endomorphism)
- [Reading the speed counter](#reading-the-speed-counter)
- [Output files](#output-files)
- [Testing](#testing)
- [FAQ](#faq)
- [Credits](#credits)

## What it can search for

| Mode | Input file (`-f`) | Targets | Typical use |
| --- | --- | --- | --- |
| `address` (default) | Bitcoin or Ethereum addresses | compressed and/or uncompressed addresses | puzzle ranges |
| `rmd160` | RIPEMD-160 hashes (40 hex chars) | the hash160 inside an address | same as `address`, skips Base58 |
| `xpoint` | public keys (compressed or uncompressed) | the X coordinate of a public key | many targets derived from one key |
| `bsgs` | public keys (compressed or uncompressed) | a known public key | much larger ranges, trades RAM for speed |
| `vanity` | `-v prefix` or a file of prefixes | addresses that start with a prefix | vanity addresses |
| `minikeys` | addresses or RIPEMD-160 hashes | Casascius style 22 character minikeys | experimental |

The `pub2rmd` mode of older versions has been removed. To check the puzzles that have no public key yet, use `rmd160` with `tests/puzzleswopublickey.txt`.

## Quick start

```
git clone https://github.com/AppleLamps/keyhunt.git
cd keyhunt
make -j$(nproc)
```

Find the solved puzzles whose keys are below `0xFFFFF` (finishes in about a second):

```
./keyhunt -m address -f tests/1to32.txt -r 1:FFFFF -n 0x100000 -t 4 -s 0
```

Try your luck on puzzle 66 (random search, add `-t` with your core count):

```
./keyhunt -m address -f tests/66.txt -b 66 -l compress -R -q -s 10 -t 8
```

Try puzzle 125 with BSGS (add `-t` and `-k`, see [BSGS](#bsgs-mode-baby-step-giant-step)):

```
./keyhunt -m bsgs -f tests/125.txt -b 125 -q -s 10 -R -t 8
```

Anything found is printed and appended to `KEYFOUNDKEYFOUND.txt` in the current directory.

## Building

Keyhunt is developed on Linux. On Windows use [WSL](https://learn.microsoft.com/windows/wsl/); native ports exist (see the [FAQ](#faq)).

### Requirements

- a C and C++ compiler with C++17 support (GCC or Clang) and `make`
- an x86-64 CPU with SSSE3 for the main build; with AVX2 or AVX-512, address and rmd160 mode (without `-e`, Bitcoin) use an 8-way or 16-way hash path, chosen at run time, so the same binary still runs on older CPUs. `KEYHUNT_SIMD=avx2` (or `none`) in the environment forces a narrower path, for CPUs where AVX-512 lowers the clock too much
- with AVX-512 IFMA (Ice Lake, Zen 4 and later), AVX-512 or AVX2, the secp256k1 field multiplications of the point additions run 8 or 4 at a time in a lane parallel kernel, also chosen at run time. `KEYHUNT_FIELD_SIMD=ifma|avx512|avx2|none` forces one (the AVX2 one is about break even with the scalar code)
- `libssl-dev` and `libgmp-dev` only for the `legacy` build

On Debian or Ubuntu:

```
sudo apt update
sudo apt install build-essential git libssl-dev libgmp-dev
```

### Targets

| Command | Result |
| --- | --- |
| `make -j$(nproc)` | builds `./keyhunt` |
| `make bsgsd` | builds `./bsgsd`, the BSGS server (Linux only, see [BSGSD.md](BSGSD.md)) |
| `make legacy` | builds `./keyhunt` from `keyhunt_legacy.cpp` with libgmp and OpenSSL, for CPUs or systems without SSSE3 (for example ARM) |
| `make test` | builds and runs the [test suite](#testing) |
| `make clean` | removes `build/` and the binaries |

Build options, set on the command line (`make ARCH=x86-64-v3`):

| Option | Effect |
| --- | --- |
| `ARCH=<cpu>` | target a specific CPU instead of the build machine (default `native`) |
| `LTO=1` | link time optimisation |
| `DEBUG=1` | `-O0 -g` |
| `SANITIZE=1` | build with AddressSanitizer and UBSan |
| `V=1` | print the full compiler command lines |

Objects live in `build/`, builds are incremental, and changing any option rebuilds what it affects. `make legacy` and `make` both produce `./keyhunt`; whichever you ran last is the one you have.

## Command line reference

```
./keyhunt -h
```

| Option | Meaning |
| --- | --- |
| `-m mode` | `address` (default), `rmd160`, `xpoint`, `bsgs`, `vanity`, `minikeys` |
| `-f file` | input file with the targets, one per line |
| `-r start:end` | range in hex. `end` can be omitted to search up to the curve order |
| `-b bits` | search the N bit range, for example `-b 66` is `0x20000000000000000` to `0x3ffffffffffffffff`. 1 to 256 |
| `-R` | random search: pick random bases inside the range (default, BSGS: same as `-B random`) |
| `-L` | sequential search: scan the range from its start upwards (BSGS: same as `-B sequential`) |
| `-n number` | address, rmd160, xpoint, vanity, minikeys: keys scanned per block (default `0x100000000`). BSGS: the size of the baby step table, see [BSGS](#bsgs-mode-baby-step-giant-step). Hex needs a `0x` prefix, otherwise decimal |
| `-t number` | threads (default 1) |
| `-l look` | `compress`, `uncompress` or `both` (default). address and rmd160 |
| `-c crypto` | `btc` (default) or `eth`. address mode |
| `-e` | enable [endomorphism](#endomorphism) (address, rmd160, xpoint, vanity). Not compatible with BSGS |
| `-I stride` | step between consecutive keys. Not for BSGS |
| `-B mode` | BSGS order: `sequential`, `backward`, `both`, `random` (default), `dance` |
| `-k factor` | BSGS: multiplies the table size, more RAM for more speed |
| `-S` | save the generated bloom filters and tables to disk and load them next time |
| `-6` | skip the SHA-256 checksum when loading saved files |
| `-z value` | bloom filter size multiplier, 1 or more (address, rmd160, xpoint, vanity) |
| `-v prefix` | vanity prefix, can be repeated (vanity mode) |
| `-C minikey` | base minikey to start from (22 characters) |
| `-8 alphabet` | Base58 alphabet used for minikeys |
| `-q` | quiet: do not print the current base key from every thread |
| `-s seconds` | seconds between speed reports (default 30), `0` turns the report off |
| `-M` | "matrix" screen (prints every thread's base key, slower) |

Input files may contain one target per line followed by a space and any comment, which is ignored. In `address` mode the lines are Bitcoin addresses or, with `-c eth`, Ethereum addresses.

> **Tip.** When stdout is a pipe, output is block buffered, so a hit may not show up until later. The result is always written to `KEYFOUNDKEYFOUND.txt` right away.

## Modes

### address mode

The default. The input file is a list of Bitcoin addresses, for example `tests/1to32.txt` (the solved puzzles 1 to 32):

```
1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH
1CUNEBjYrCn2y1SdiUMohaKUi4wpP326Lb
...
```

```
./keyhunt -m address -f tests/1to32.txt -r 1:FFFFF -n 0x100000 -t 2 -s 0
```

```
[+] Version 0.2.230519 Satoshi Quest, developed by AlbertoBSD
[+] Mode address
[+] Threads : 2
[+] Turn off stats output
[+] Setting search for btc adddress
[+] N = 0x100000
[+] Range
[+] -- from : 0x1
[+] -- to   : 0xfffff
[+] Allocating memory for 32 elements: 0.00 MB
[+] Bloom filter for 32 elements.
[+] Loading data to the bloomfilter total: 0.03 MB
[+] Sorting data ... done! 32 values were loaded and sorted
Hit! Private Key: 1
pubkey: 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
Address 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH
rmd160 751e76e8199196d454941c45d1b3a323f1433bd6
...
```

Use `-l compress` or `-l uncompress` to hash only one public key form, which makes it faster. Puzzle addresses are compressed, so for puzzles always use `-l compress`.

Targets are loaded into a bloom filter plus a sorted table, so a list with millions of addresses costs only memory, not speed. Use `-S` to cache that structure on disk.

#### Vanity search

Search for addresses that start with a prefix:

```
./keyhunt -m vanity -l compress -R -b 256 -v 1Good1 -v 1MyKey
```

or load the prefixes from a file (`tests/vanitytargets.txt`):

```
./keyhunt -m vanity -f tests/vanitytargets.txt -l compress -R -b 256 -e -s 10 -q
```

Matches are saved in `VANITYKEYFOUND.txt`. `-S` does not work for vanity mode.

### rmd160 mode

Same as address mode, but the file holds the RIPEMD-160 hash (the 20 byte hash160) instead of the Base58 address. It skips decoding the addresses, and it is the way to search targets for which you only have the hash. `tests/1to32.rmd`:

```
751e76e8199196d454941c45d1b3a323f1433bd6
7dd65592d0ab2fe0d0257d571abf032cd9db93dc
5dedfbf9ea599dd4e3ca6a80b333c472fd0b3f69
...
```

```
./keyhunt -m rmd160 -f tests/1to32.rmd -r 1:FFFFF -n 0x100000 -l compress -s 0
```

The hash of the *uncompressed* public key of private key 1:

```
echo 91b24bf9f5288532960ac687abb035127b1d28a5 > u.rmd
./keyhunt -m rmd160 -f u.rmd -r 1:FFFF -n 0x10000 -l uncompress -s 0
```

```
Hit! Private Key: 1
pubkey: 0479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8
Address 1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm
rmd160 91b24bf9f5288532960ac687abb035127b1d28a5
```

Ready made lists: `tests/66.rmd` is the hash of puzzle 66, `tests/unsolvedpuzzles.rmd` holds the unsolved puzzles and `tests/puzzleswopublickey.txt` the 29 puzzles whose public key has not been revealed. For example, a random search of puzzle 66:

```
./keyhunt -m rmd160 -f tests/66.rmd -b 66 -l compress -R -q -t 8
```

### xpoint mode

Searches for the X coordinate of a public key instead of an address. It is faster than address or rmd160, because no hashing is needed, and it finds a key if either the point or its negation matches.

The input is one public key per line, compressed (66 hex characters) or uncompressed (130). The mode is useful when you subtract a known offset from the target many times and look for any of the results: a hit on one of the shifted keys gives you the target after undoing the offset.

Example with a few values subtracted from and added to the puzzle 40 key (`tests/substracted40.txt`):

```
034eee474fe724cb631d19f24934e88016e4ef2aee80d086621d87d7f6066ff860 # - 453856235784
0274241b684e7c31e7933510b510aa14de9ac88ec3635bdd35a3bcf1d16da210be # + 453856235784
...
03a2efa402fd5268400c77c20e574ba86409ededee7c4020e4b9f0edbee53de0d4 # target
```

```
./keyhunt -m xpoint -f tests/substracted40.txt -n 65536 -t 4 -b 40
```

The hit `800258a2ce` was found for the line marked `+ 453856235784`, so the real key is `0x800258a2ce + 453856235784 = 0xE9AE4933D6`.

### bsgs mode (baby step giant step)

BSGS looks for the private key of a known public key. It precomputes a table of baby steps, which lives in RAM, and then takes giant steps in which every step covers a whole block of `n` keys. This trades memory for speed: the reported speed is the size of the range covered per second, so it can reach peta or exa keys per second.

Input: one public key per line, compressed or uncompressed, mixed freely. Anything after a space is ignored:

```
043ffa1cc011a8d23dec502c7656fb3f93dbe4c61f91fd443ba444b4ec2dd8e6f0406c36edf3d8a0dfaa7b8f309b8f1276a5c04131762c23594f130a023742bdde # 0000000000000000000000000000000000800000000000000000100000000000
046534b9e9d56624f5850198f6ac462f482fec8a60262728ee79a91cac1d60f8d6a92d5131a20f78e26726a63d212158b20b14c3025ebb9968c890c4bab90bfc69 # 0000000000000000000000000000000000800000000000000000200000000000
```

Do not load more than 100 to 1000 public keys at once, the speed drops with every key.

```
./keyhunt -m bsgs -f tests/1to63_65.txt -r 1:FFFFFFFF -n 0x1000000 -t 2 -q -s 0
```

```
[+] Mode BSGS sequential
[+] Added 64 points from file
[+] -- from : 0x1
[+] -- to   : 0xFFFFFFFF
[+] N = 0x1000000
[+] Bloom filter for 4096 elements : 0.88 MB
...
[+] Thread Key found privkey 3
[+] Publickey 02f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9
[+] Thread Key found privkey 7
...
```

(BSGS never reports private key 1, the point G itself. This is a long standing quirk of the algorithm in this code.)

#### Search order

| `-B` | Order |
| --- | --- |
| `sequential` | from the start of the range upwards (the same as `-L`) |
| `backward` | from the end downwards |
| `both` | from both ends towards the middle |
| `random` | random positions (default, the same as `-R`) |
| `dance` | each cycle randomly takes the next block from the bottom of the remaining range, from the top, or from a random position |

#### The values of `-n` and `-k`

- `-n` is the number of keys per cycle. It must be a perfect square, its square root must be divisible by 1024 and the range must be larger than `n`. The minimum is 2^20 (`0x100000`), the default is `0x100000000000` (2^44).
- `-k` multiplies the baby step table. Doubling it roughly doubles the RAM use and the speed. Use powers of two.

Each `n` has a maximum `k` that works well. Going over it can cost speed, hide hits and show a wrong speed:

| bits | `n` | max `k` | | bits | `n` | max `k` |
| --- | --- | --- | --- | --- | --- | --- |
| 20 | `0x100000` | 1 | | 44 | `0x100000000000` | 4096 |
| 22 | `0x400000` | 2 | | 46 | `0x400000000000` | 8192 |
| 24 | `0x1000000` | 4 | | 48 | `0x1000000000000` | 16384 |
| 26 | `0x4000000` | 8 | | 50 | `0x4000000000000` | 32768 |
| 28 | `0x10000000` | 16 | | 52 | `0x10000000000000` | 65536 |
| 30 | `0x40000000` | 32 | | 54 | `0x40000000000000` | 131072 |
| 32 | `0x100000000` | 64 | | 56 | `0x100000000000000` | 262144 |
| 34 | `0x400000000` | 128 | | 58 | `0x400000000000000` | 524288 |
| 36 | `0x1000000000` | 256 | | 60 | `0x1000000000000000` | 1048576 |
| 38 | `0x4000000000` | 512 | | 62 | `0x4000000000000000` | 2097152 |
| 40 | `0x10000000000` | 1024 | | 64 | `0x10000000000000000` | 4194304 |
| 42 | `0x40000000000` | 2048 | | | | |

#### What to use for the RAM you have

| RAM | options |
| --- | --- |
| 2 GB | `-k 128` |
| 4 GB | `-k 256` |
| 8 GB | `-k 512` |
| 16 GB | `-k 1024` |
| 32 GB | `-k 2048` |
| 64 GB | `-n 0x100000000000 -k 4096` |
| 128 GB | `-n 0x400000000000 -k 4096` |
| 256 GB | `-n 0x400000000000 -k 8192` |
| 512 GB | `-n 0x1000000000000 -k 8192` |
| 1 TB | `-n 0x1000000000000 -k 16384` |
| 2 TB | `-n 0x4000000000000 -k 16384` |
| 4 TB | `-n 0x4000000000000 -k 32768` |
| 8 TB | `-n 0x10000000000000 -k 32768` |

The tables must fit in RAM. Swap does not work: the access pattern is made of small random reads and it is far too slow.

`-F` makes the first bloom filter, which is most of that RAM, about half the size (false positive rate 1/1000 instead of 1/1000000). Double the `-k` of the table above with it: `-k 256 -F` uses the RAM of `-k 128`. Measured on 4 cores, `-k 8 -F` ran 1.76x faster than `-k 4` with the same filter size. At the same `-k`, `-F` is about 7% slower. The filter saved by `-S` with `-F` has its own file name (`keyhunt_bsgs_4c_*.blm`).

Example for the 63 bit puzzle (the original author measured about four minutes on 8 threads with `-k 512`, which needs around 8 GB of RAM):

```
./keyhunt -m bsgs -t 8 -f tests/63.pub -k 512 -s 0 -S -b 63
```

#### Saving the tables

Building the tables is the slow part of the start up. `-S` writes them (three bloom filters and the baby step table) to the current directory the first time and reads them on later runs. The files are the same size as the memory they use, and their names depend on `-n` and `-k`, for example `keyhunt_bsgs_4_4194304.blm` and `keyhunt_bsgs_2_4096.tbl`, so stick to one combination or you will collect many files. `-6` skips the SHA-256 check of the files on load.

For the address, rmd160, minikeys and xpoint modes `-S` writes a single `data_<id>.dat` file with the bloom filter and sorted table of the input file.

#### Is my speed real?

If you doubt the speed counter, these private keys are placed so that a given speed finds them in about two minutes after the tables are loaded. Save one of the public keys in a file and run it with the range `-b 120` (their keys are 120 bits):

| speed | private key | public key |
| --- | --- | --- |
| 1 Pkeys/s | `8000000000000001aa535d3d0c0000` | `02af4535880d694d660031a161c53a6889c45d2de513454858e94739f9c790768b` |
| 10 Pkeys/s | `8000000000000010a741a462780000` | `025deee1657cd5d363cff23ec1b14781e504cbb6292c273e515d73f98065131d40` |
| 50 Pkeys/s | `8000000000000053444835ec580000` | `03c13e9c6e5cbe2ac06817e4d8fd0a3e836f1a121aab91bb67ef44747b25c7d791` |
| 1 Ekeys/s | `800000000000068155a43676e00000` | `022b6a74badcc4c3d8fab7d01ddc1854b9d8f262172789b2aa1bb7fd42cc1b2817` |
| 5 Ekeys/s | `8000000000002086ac351052600000` | `024cf9e44f808e7b0bbb12a57ff63e3a8407cba1816f5e31e815d33d70e4a95a7f` |
| 10 Ekeys/s | `800000000000410d586a20a4c00000` | `02ee0cf78d13b4aae9c8777a0f93dff7f5be3855bd2c0f85370f861c69bb5b533a` |

```
./keyhunt -m bsgs -f testpublickey.txt -b 120 -q
```

#### bsgsd, the BSGS server

`bsgsd` keeps the BSGS tables in RAM and answers range queries over a socket, so you do not pay the load time per target. See [BSGSD.md](BSGSD.md).

### minikeys mode

Experimental. Searches 22 character minikeys. They are generated from a 16 byte buffer encoded with the Bitcoin Base58 alphabet (`123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz`, change it with `-8`). The input file is a list of addresses or RIPEMD-160 hashes.

```
./keyhunt -m minikeys -f tests/minikeys.txt -C SG64GZqySYwBm9KxE1wJ28 -n 0x10000
```

```
HIT!! Private Key: d1a4fc1f83b2f3b31dcd999acd8288ff346f7df46401596d53964e0c69d5b4d
pubkey: 048722093a2b5dd05a84c28a18b2a6601320c9eaab9db99e76b850f9574cd3d5c987bf0c9c9ed3bd0f52124a57d9ef292b529536b225b90f8760d9c67cc3aa1c32
minikey: SG64GZqySYwBm9KxE3wJ29
address: 15azScMmHvFPAQfQafrKr48E9MqRRXSnVv
```

Random minikeys: `./keyhunt -m minikeys -f tests/minikeys.txt -n 0x10000 -q -R`

### Ethereum

Use `-c eth` with address mode. The file holds Ethereum addresses (`tests/1to32.eth`):

```
./keyhunt -c eth -f tests/1to32.eth -r 1:FFFF -n 0x10000 -s 0 -q
```

```
 Hit!!!! Private Key: 1
address: 0x7e5f4552091a69125d5dfcb7b8c2659029395bdf
```

If you have the public key of an Ethereum account use `xpoint` or `bsgs` instead.

## Endomorphism

Enable it with `-e` in address, rmd160, xpoint and vanity mode. It does not work with BSGS.

secp256k1 has a cheap map on its points: for a point `Q = (x, y)`, `Q * lambda = (x * beta mod p, y)`, where

```
lambda = 0x5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72
beta   = 0x7ae96a2b657c07106e64479eac3434e99cf0497512f58995c1396c28719501ee
p      = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
```

Multiplying a point by `lambda` costs one field multiplication (`x * beta`) instead of a scalar multiplication. Together with negation (`-Q = (x, -y)`) each computed point stands for six: `Q`, `-Q`, `lambda*Q`, `-lambda*Q`, `lambda^2*Q` and `-lambda^2*Q`. The matching private keys are `k`, `-k`, `k*lambda`, `-k*lambda`, `k*lambda^2` and `-k*lambda^2` (mod the curve order).

Example, puzzle 64:

```
0x000000000000000000000000000000000000000000000000f7051f27b09112d4  03100611c54dfef604163b8358f7b7fac13ce478e02cb224ae16d45526b25d9d4d
0x2924e3e5ac18fd894504878d4fd1820e71bd63cd9b15d69511926e5f05d99d3a  03792bfa55bf659967951b21060c05c250cd261ec3ea02704815bfb1c5ccc800fd
0xd6db1c1a53e70276bafb7872b02e7df048f179191432c9a5b73ad10619cb9133  0376cdf3e4f29b709454a95ba0fc4242edf5f5685be94b6b09d36bf91280da5de5
```

Multiplying the last key by `lambda` once more gives back the first one.

**Endomorphism does not help for puzzles**: five of the six keys it checks fall outside the puzzle's range. It pays off when searching a whole key space, for example for vanity addresses.

## Reading the speed counter

For address, rmd160, xpoint and vanity mode each thread works in groups of 1024 keys and counts one step per group, so one step is 1024 keys.

- With `-e` the count is multiplied by 6 (address, rmd160, vanity) because six keys are checked per point, and by 3 in xpoint mode, where the negated points are not needed.
- In address and rmd160 mode without `-e` every key is hashed exactly once, so the shown speed is the real one. (Older versions hashed each X with both the `02` and the `03` prefix, which checks `k` and `n-k`; `n-k` is never in a puzzle range, so the shown speed was double the useful one. The Y coordinate is now computed instead, which is far cheaper than a second hash.)
- In vanity mode with `-l compress` the shown speed is still doubled: there every hash is a usable key, so both prefixes are checked.

In BSGS mode the speed is the size of the range covered per second, not the number of points computed. If it shows `0 keys/s` see the [FAQ](#faq).

## Output files

| File | Contents |
| --- | --- |
| `KEYFOUNDKEYFOUND.txt` | every private key found (address, rmd160, xpoint, bsgs, minikeys) |
| `VANITYKEYFOUND.txt` | vanity matches |
| `*.blm`, `*.tbl` | BSGS bloom filters and baby step table written with `-S` |
| `data_<id>.dat` | cached bloom filter and table of an input file, written with `-S` in the other modes |

All are written to the current directory.

## Testing

```
make test
```

builds the project and runs

- `tests/test_hash160.cpp`: cross-checks the 8-way AVX2 and 16-way AVX-512 hash kernels, and the `Secp256K1` wrappers that call them, against the scalar SHA-256 and RIPEMD-160 (each kernel is tested where the CPU supports it)
- `tests/test_int.cpp`: checks the secp256k1 field arithmetic fast paths (modular add, sub, neg, squaring, batch inversion) against reference computations
- `tests/run_tests.sh [path/to/keyhunt]`: end to end runs over the first puzzle keys (address, rmd160 in compressed, uncompressed and both modes, endomorphism, one thread, BSGS), checking the number of hits and that the program exits cleanly

The script works with the `legacy` binary too. CI builds `keyhunt`, `bsgsd` and the legacy variant, runs `make test`, and repeats the end to end tests under the sanitizers (`make SANITIZE=1`) on every push.

Sample inputs in `tests/`:

| File | Contents |
| --- | --- |
| `1to32.txt`, `1to32.rmd`, `1to32.eth` | the solved puzzles 1 to 32 as addresses, hashes and Ethereum addresses |
| `1to63_65.txt` | public keys of the solved puzzles |
| `63.pub`, `120.txt`, `125.txt`, `130.txt`, `test120.txt` | public keys for BSGS tests |
| `64.txt`, `64.rmd`, `66.txt`, `66.rmd` | puzzle 64 and 66 targets |
| `unsolvedpuzzles.txt`, `unsolvedpuzzles.rmd`, `puzzleswopublickey.txt` | unsolved puzzles |
| `substracted40.txt` | the xpoint example |
| `in.txt` | 16 public keys of a 64 bit range, the sample of JLP's BSGS tool |
| `minikeys.txt`, `vanitytargets.txt` | minikeys and vanity examples |

## FAQ

**Where are the private keys saved?**
In `KEYFOUNDKEYFOUND.txt` (and `VANITYKEYFOUND.txt` for vanity) in the current directory.

**Can I keep the bloom filters and tables between runs?**
Yes, with `-S`. It works for bsgs, address, rmd160, minikeys and xpoint, not for vanity.

**Why does the BSGS speed show `0 keys/s`?**
It was asked in [issue 69](https://github.com/albertobsd/keyhunt/issues/69) and [issue 108](https://github.com/albertobsd/keyhunt/issues/108) of the original project; the author's answer is in [this video](https://youtu.be/MVby8mYNxbI).

**Does it run on Windows?**
Use WSL with Ubuntu. Native ports made by others: [kanhavishva/keyhunt](https://github.com/kanhavishva/keyhunt), [WanderingPhilosopher/keyhunt](https://github.com/WanderingPhilosopher/keyhunt), [XopMC/keyhunt-win](https://github.com/XopMC/keyhunt-win).

**Does it run on ARM?**
The main build uses x86 SIMD. `make legacy` builds the portable version (libgmp and OpenSSL), which is slower.

**Does it use the GPU?**
No, CPU only.

## Credits

Written by AlbertoBSD, with thanks to IceLand, kanhavishva, XopMC, WanderingPhilosopher, Malboro Man, NetSec, Jean Luc Pons (the elliptic curve and SIMD hashing code derives from his VanitySearch and BSGS projects) and everyone in the group of CryptoHunters who tested it, reported bugs and shared ideas. See [CHANGELOG.md](CHANGELOG.md) for the version history.

If you want to support the original author, the donation addresses are in the [upstream README](https://github.com/albertobsd/keyhunt#donations).
