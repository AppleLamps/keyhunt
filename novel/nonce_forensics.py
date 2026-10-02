#!/usr/bin/env python3
"""Idea 5: nonce forensics.  Rebuild the message hash of every legacy P2PKH
signature made by puzzle keys (novel/nonce_sigs.json from nonce_fetch.py),
recover the ECDSA nonce k = (z + r*d)/s mod n wherever the private key d is
known, verify it, and test whether the nonces of the creator's 2019 session
(one transaction, inputs from puzzles 65, 70, ... 160) are deterministic
(RFC 6979, with and without Bitcoin Core's low-R grinding), reused, or
algebraically related.  Any relation that predicts a nonce for inputs 140 to
160 would hand over that key: d = (s*k - z)/r mod n."""
import json, hashlib, hmac, sys
from collections import Counter

P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)
def inv(a, m=P): return pow(a, -1, m)
def add(A, B):
    if A is None: return B
    if B is None: return A
    if A[0] == B[0]:
        if (A[1] + B[1]) % P == 0: return None
        l = 3 * A[0] * A[0] * inv(2 * A[1]) % P
    else: l = (B[1] - A[1]) * inv(B[0] - A[0]) % P
    x = (l * l - A[0] - B[0]) % P
    return (x, (l * (A[0] - x) - A[1]) % P)
def mul(k, A=G):
    R = None
    while k:
        if k & 1: R = add(R, A)
        A = add(A, A); k >>= 1
    return R
def sha256d(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()

# ---- transaction parsing (legacy, no witness) ----
def varint(b, i):
    v = b[i]
    if v < 0xfd: return v, i + 1
    if v == 0xfd: return int.from_bytes(b[i+1:i+3], "little"), i + 3
    if v == 0xfe: return int.from_bytes(b[i+1:i+5], "little"), i + 5
    return int.from_bytes(b[i+1:i+9], "little"), i + 9
def enc_varint(n):
    if n < 0xfd: return bytes([n])
    if n < 0x10000: return b"\xfd" + n.to_bytes(2, "little")
    return b"\xfe" + n.to_bytes(4, "little")
def parse_tx(h):
    b = bytes.fromhex(h); i = 0
    ver = b[i:i+4]; i += 4
    segwit = b[i] == 0 and b[i+1] == 1               # marker+flag: witnesses follow the outputs and play no part in a legacy sighash
    if segwit: i += 2
    nin, i = varint(b, i); vin = []
    for _ in range(nin):
        prev = b[i:i+36]; i += 36
        sl, i = varint(b, i); script = b[i:i+sl]; i += sl
        seq = b[i:i+4]; i += 4
        vin.append((prev, script, seq))
    nout, i = varint(b, i); vout = []
    for _ in range(nout):
        val = b[i:i+8]; i += 8
        sl, i = varint(b, i); spk = b[i:i+sl]; i += sl
        vout.append(val + enc_varint(sl) + spk)
    if segwit:
        for _ in range(nin):
            items, i = varint(b, i)
            for _ in range(items):
                l, i = varint(b, i); i += l
    lock = b[i:i+4]
    assert i + 4 == len(b), "transaction not fully consumed"
    return ver, vin, vout, lock
def sighash_all(h, idx, prev_spk):
    ver, vin, vout, lock = parse_tx(h)
    s = ver + enc_varint(len(vin))
    for j, (prev, script, seq) in enumerate(vin):
        sc = prev_spk if j == idx else b""
        s += prev + enc_varint(len(sc)) + sc + seq
    s += enc_varint(len(vout)) + b"".join(vout) + lock + (1).to_bytes(4, "little")
    return int.from_bytes(sha256d(s), "big")
def parse_scriptsig(hexs):
    b = bytes.fromhex(hexs); i = 0
    l = b[i]; i += 1; sig = b[i:i+l]; i += l
    l = b[i]; i += 1; pub = b[i:i+l]
    return sig, pub
def parse_der(sig):
    assert sig[0] == 0x30
    i = 2; assert sig[i] == 2; l = sig[i+1]; r = int.from_bytes(sig[i+2:i+2+l], "big"); i += 2 + l
    assert sig[i] == 2; l = sig[i+1]; s = int.from_bytes(sig[i+2:i+2+l], "big")
    return r, s, sig[-1]

# ---- RFC 6979 (secp256k1, SHA-256), with optional extra data (libsecp256k1 nonce_function_rfc6979) ----
def rfc6979(d, z, extra=b""):
    x = d.to_bytes(32, "big") + z.to_bytes(32, "big") + extra
    V = b"\x01" * 32; K = b"\x00" * 32
    K = hmac.new(K, V + b"\x00" + x, hashlib.sha256).digest(); V = hmac.new(K, V, hashlib.sha256).digest()
    K = hmac.new(K, V + b"\x01" + x, hashlib.sha256).digest(); V = hmac.new(K, V, hashlib.sha256).digest()
    while True:
        V = hmac.new(K, V, hashlib.sha256).digest()
        k = int.from_bytes(V, "big")
        if 1 <= k < N: return k
        K = hmac.new(K, V + b"\x00", hashlib.sha256).digest(); V = hmac.new(K, V, hashlib.sha256).digest()

def main():
  sigs = json.load(open("novel/nonce_sigs.json"))
  rows = []
  for o in sigs:
      sig, pub = parse_scriptsig(o["scriptsig"])
      if pub.hex() != o["pubkey"]: print(f"[!] pubkey mismatch puzzle {o['puzzle']} {o['txid']}:{o['vin']}"); continue
      r, s, ht = parse_der(sig)
      if ht != 1: print(f"[-] skip hashtype {ht} puzzle {o['puzzle']}"); continue
      z = sighash_all(o["rawtx"], o["vin"], bytes.fromhex(o["prev_spk"]))
      d = int(o["privkey"], 16) if o["privkey"] else None
      k = None
      if d:
          k = (z + r * d) * inv(s, N) % N
          R = mul(k)
          if R[0] % N != r: print(f"[!] nonce check FAILED puzzle {o['puzzle']} {o['txid']}:{o['vin']}"); k = None
      rows.append(dict(o, r=r, s=s, z=z, d=d, k=k, lowS=s <= N // 2, lowR=r < (1 << 255), siglen=len(sig)))

  creator_tx = "17e4e323cfbc68d7f0071cad09364e8193eedf8fefbcbd8a21b4b65717a4b3d3"
  ses = sorted([x for x in rows if x["txid"] == creator_tx], key=lambda x: x["vin"])
  known = [x for x in ses if x["k"]]
  print(f"[+] {len(rows)} signatures parsed, {sum(1 for x in rows if x['k'])} nonces recovered and verified")
  print(f"[+] creator session {creator_tx[:16]}...: {len(ses)} inputs, puzzles {[x['puzzle'] for x in ses]}")
  print(f"    nonces known for {[x['puzzle'] for x in known]}, unknown for {[x['puzzle'] for x in ses if not x['k']]}")

  # 1. nonce reuse anywhere (same r across all 426 signatures)
  rc = Counter(x["r"] for x in rows)
  dup = [r for r, c in rc.items() if c > 1]
  print(f"[{'!' if dup else '-'}] repeated r values across all signatures: {len(dup)}")
  for r in dup:
      print("    r=%064x used by puzzles %s" % (r, [x['puzzle'] for x in rows if x['r'] == r]))

  # 2. deterministic nonces? RFC 6979 plain, and Bitcoin Core low-R grinding (extra = 32-byte LE counter)
  def classify(x):
      # a recovered nonce is the canonical one: a wallet that fixes high S by negating s
      # (or equivalently the nonce) leaves k' = n - k, the same signature as nonce k
      d, z, k = x["d"], x["z"], x["k"]
      k1 = rfc6979(d, z)
      if k == k1: return "rfc6979 (first output already low-S)"
      if k == N - k1: return "rfc6979 + low-S normalisation (s -> n-s)"
      for c in range(1, 64):
          if k in (rfc6979(d, z, c.to_bytes(32, "little")), N - rfc6979(d, z, c.to_bytes(32, "little"))): return f"rfc6979+core-grind(counter={c})"
      return "NOT deterministic"
  print("[+] creator session nonces:")
  for x in known:
      print(f"    puzzle {x['puzzle']:3d} vin {x['vin']:2d} lowR={int(x['lowR'])} lowS={int(x['lowS'])} siglen={x['siglen']} nonce={x['k']:064x} -> {classify(x)}")
  print(f"    all creator inputs low-R: {all(x['lowR'] for x in ses)} (Bitcoin Core >= 0.17 grinds for low R)")
  det = all(classify(x).startswith("rfc6979") for x in known)
  print(f"[{'+' if det else '!'}] creator nonces deterministic (RFC 6979): {det}" + (" -> no nonce leakage is possible; the 140..160 signatures reveal nothing beyond the public keys" if det else ""))

  # 3. relations among the known creator nonces
  ks = [x["k"] for x in known]
  small = lambda v: min(v, N - v).bit_length()
  print("[+] pairwise |k_i - k_j| bit lengths (a short one means a counter or a weak generator):",
        sorted(small((a - b) % N) for i, a in enumerate(ks) for b in ks[i+1:])[:5], "...")
  print("[+] nonce bit lengths:", [k.bit_length() for k in ks], "(256-bit random: mostly 255..256)")
  print("[+] nonce == some other puzzle key? ", any(k == x["d"] for k in ks for x in rows if x["d"]))
  # affine recurrence k_{i+1} = a*k_i + c mod N fitted on the first three, verified on the rest
  if len(ks) >= 4:
      a = (ks[2] - ks[1]) * inv((ks[1] - ks[0]) % N, N) % N
      c = (ks[1] - a * ks[0]) % N
      ok = all((a * ks[i] + c) % N == ks[i+1] for i in range(3, len(ks) - 1))
      print(f"[{'!' if ok else '-'}] affine recurrence mod n (fit on 3, verified on {len(ks)-4}): {'HOLDS' if ok else 'does not hold'}")
      # same modulo 2^256 and 2^64 (LCG-style state)
      for bits in (256, 64, 32):
          M = 1 << bits; m = [k % M for k in ks]
          if (m[1] - m[0]) % 2 == 0: print(f"[-] mod 2^{bits}: first difference even, multiplier not invertible, skipped"); continue
          a = (m[2] - m[1]) * pow((m[1] - m[0]) % M, -1, M) % M; c = (m[1] - a * m[0]) % M
          ok = all((a * m[i] + c) % M == m[i+1] for i in range(3, len(m) - 1))
          print(f"[{'!' if ok else '-'}] affine recurrence mod 2^{bits}: {'HOLDS' if ok else 'does not hold'}")
  # 4. shared high or low words between consecutive nonces (stream slicing)
  for sh in (0, 128, 192, 224):
      eq = sum(1 for i in range(len(ks) - 1) if (ks[i] >> sh) & 0xffffffff == (ks[i+1] >> sh) & 0xffffffff)
      print(f"[{'!' if eq else '-'}] consecutive nonces sharing the 32-bit word at bit {sh}: {eq}")
  json.dump([{k: (format(v, "x") if isinstance(v, int) and k in ("r", "s", "z", "d", "k") else v)
              for k, v in x.items() if k != "rawtx"} for x in rows], open("novel/nonce_table.json", "w"), indent=1)

if __name__ == "__main__":
    main()
