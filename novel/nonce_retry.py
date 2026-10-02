#!/usr/bin/env python3
"""Third pass of Idea 5: is the split between plain-RFC 6979 nonces and the
others explained by a high-S retry, and if so what is the retry nonce?"""
import json, hashlib
from nonce_forensics import N, mul, inv, rfc6979
from nonce_variants import drbg_outputs, sig_of, known
half = N // 2
print("[+] raw (r, s) of the FIRST RFC 6979 output for every known creator input:")
hs = {}
for x in known:
    k1 = rfc6979(x["d"], x["z"]); r1, s1 = sig_of(x["d"], x["z"], k1)
    hs[x["puzzle"]] = s1 > half
    print(f"    puzzle {x['puzzle']:3d}: first output gives {'HIGH' if s1 > half else 'low '} S, {'high' if r1 >= 1<<255 else 'low '} R; final nonce {'==' if k1 == x['k'] else '!='} first output")
plain = [p for p, x in zip(hs, known) if rfc6979(x["d"], x["z"]) == x["k"]]
print(f"    plain matches all low-S on first output: {all(not hs[p] for p in plain)}")
print(f"    non-matches all HIGH-S on first output: {all(hs[x['puzzle']] for x in known if x['puzzle'] not in plain)}")
print("[+] transforms of the first output k1 that could be a 'retry' nonce:")
for x in known:
    if x["puzzle"] in plain: continue
    k1 = rfc6979(x["d"], x["z"]); kb = k1.to_bytes(32, "big")
    cands = {
        "N - k1": (N - k1) % N, "sha256(k1)": int.from_bytes(hashlib.sha256(kb).digest(), "big") % N,
        "sha256d(k1)": int.from_bytes(hashlib.sha256(hashlib.sha256(kb).digest()).digest(), "big") % N,
        "k1 + 1": k1 + 1, "k1 - 1": k1 - 1, "2*k1": 2 * k1 % N, "k1 xor d": k1 ^ x["d"], "k1 + d": (k1 + x["d"]) % N,
        "k1 * d": k1 * x["d"] % N, "k1^-1": inv(k1, N), "k1 + z": (k1 + x["z"]) % N, "k1 xor z": k1 ^ x["z"],
        "rfc6979(d, z) extra=k1": rfc6979(x["d"], x["z"], kb), "rfc6979(k1, z)": rfc6979(k1, x["z"]),
        "rfc6979(d, k1)": rfc6979(x["d"], k1), "rfc6979(d, sha256(z||01))": rfc6979(x["d"], int.from_bytes(hashlib.sha256(x["z"].to_bytes(32, "big") + b"\x01").digest(), "big")),
        "rfc6979(N-d, z)": rfc6979(N - x["d"], x["z"]), "rfc6979(d, N-z)": rfc6979(x["d"], (N - x["z"]) % N),
        "rfc6979(d, z) 2nd DRBG seed (K,V reseed with 0x01)": drbg_outputs(x["d"], x["z"], 2)[1],
    }
    hit = [n for n, k in cands.items() if k % N == x["k"]]
    print(f"    puzzle {x['puzzle']:3d}: {hit or 'no transform matches'}")
