#!/usr/bin/env python3
"""Second pass of Idea 5: which deterministic-nonce convention produced the
ten creator-session nonces that are not plain RFC 6979?  Tests the HMAC-DRBG
continuation outputs (retry rule), key/message mix-ups between inputs, and
common extra-data conventions.  Any rule that reproduces all 15 known nonces
is then applied to the five unknown inputs (140..160) where possible."""
import json, hashlib, hmac
from nonce_forensics import N, mul, inv, rfc6979
rows = json.load(open("novel/nonce_table.json"))
tx = "17e4e323cfbc68d7f0071cad09364e8193eedf8fefbcbd8a21b4b65717a4b3d3"
ses = sorted([r for r in rows if r["txid"] == tx], key=lambda r: r["vin"])
for r in ses:
    for f in ("r", "s", "z", "d", "k"): r[f] = int(r[f], 16) if r[f] else None
known = [r for r in ses if r["k"]]

def drbg_outputs(d, z, count, extra=b""):
    """The first `count` in-range outputs of the RFC 6979 HMAC-DRBG for (d, z)."""
    x = d.to_bytes(32, "big") + z.to_bytes(32, "big") + extra
    V = b"\x01" * 32; K = b"\x00" * 32
    K = hmac.new(K, V + b"\x00" + x, hashlib.sha256).digest(); V = hmac.new(K, V, hashlib.sha256).digest()
    K = hmac.new(K, V + b"\x01" + x, hashlib.sha256).digest(); V = hmac.new(K, V, hashlib.sha256).digest()
    out = []
    while len(out) < count:
        V = hmac.new(K, V, hashlib.sha256).digest()
        k = int.from_bytes(V, "big")
        if 1 <= k < N: out.append(k)
        K = hmac.new(K, V + b"\x00", hashlib.sha256).digest(); V = hmac.new(K, V, hashlib.sha256).digest()
    return out

def sig_of(d, z, k):
    r = mul(k)[0] % N; s = (z + r * d) * inv(k, N) % N
    return r, s

print("[+] test 1: nonce = n-th output of the RFC 6979 DRBG (retry rule), with the raw r,s of every earlier output")
hyp_ok = True
for x in known:
    outs = drbg_outputs(x["d"], x["z"], 40)
    idx = outs.index(x["k"]) if x["k"] in outs else None
    if idx is None: hyp_ok = False; print(f"    puzzle {x['puzzle']:3d}: NOT among the first 40 outputs"); continue
    earlier = []
    for k in outs[:idx]:
        r, s = sig_of(x["d"], x["z"], k)
        earlier.append(f"{'highS' if s > N//2 else 'lowS'}/{'highR' if r >= 1<<255 else 'lowR'}")
    r, s = sig_of(x["d"], x["z"], x["k"])
    print(f"    puzzle {x['puzzle']:3d}: output #{idx+1:2d}, final raw s is {'high' if s > N//2 else 'low'}, earlier outputs: {earlier}")
print(f"    => {'every known nonce is a DRBG output' if hyp_ok else 'retry rule alone does not explain the nonces'}")

print("[+] test 2: key/message mix-ups between inputs of the same transaction")
hits = 0
for i, a in enumerate(known):
    for j, b in enumerate(ses):
        if b["d"] is None and a is b: continue
        for name, k in (("rfc6979(d_i, z_j)", rfc6979(a["d"], b["z"])),
                        ("rfc6979(d_j, z_i)", rfc6979(b["d"], a["z"]) if b["d"] else None)):
            if k is not None and k == a["k"] and a is not b: hits += 1; print(f"    HIT {name}: puzzle {a['puzzle']} nonce from puzzle {b['puzzle']}")
print(f"    => {hits} cross-input hits")

print("[+] test 3: extra-data conventions (counter encodings, input index, pubkey, sighash byte)")
convs = {}
for x in known:
    found = []
    cands = {}
    for c in range(0, 8):
        cands[f"LE32-pad32 counter {c}"] = c.to_bytes(32, "little")
        cands[f"BE32-pad32 counter {c}"] = c.to_bytes(32, "big")
        cands[f"1-byte counter {c}"] = bytes([c])
        cands[f"4-byte LE counter {c}"] = c.to_bytes(4, "little")
    cands["vin index LE32"] = x["vin"].to_bytes(4, "little")
    cands["pubkey"] = bytes.fromhex(x["pubkey"])
    cands["sighash byte 01"] = b"\x01"
    cands["sighash LE32"] = (1).to_bytes(4, "little")
    for name, extra in cands.items():
        if x["k"] in drbg_outputs(x["d"], x["z"], 3, extra): found.append(name)
    convs[x["puzzle"]] = found
    print(f"    puzzle {x['puzzle']:3d}: {found or 'none'}")

print("[+] test 4: nonce = RFC 6979 over the reversed (little-endian) message hash, or over sha256(z)")
for x in known:
    zle = int.from_bytes(x["z"].to_bytes(32, "big")[::-1], "big")
    zh = int.from_bytes(hashlib.sha256(x["z"].to_bytes(32, "big")).digest(), "big")
    m = [n for n, zz in (("reversed z", zle), ("sha256(z)", zh)) if x["k"] in drbg_outputs(x["d"], zz, 3)]
    if m: print(f"    puzzle {x['puzzle']}: {m}")
print("    (done)")
