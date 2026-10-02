#!/usr/bin/env python3
"""Idea 8: public key side channels.

A hash-only puzzle (71, 72, ... : 2^69 hash evaluations) becomes a 2^36
group operation kangaroo problem the moment its public key is known, and a
public key is revealed by ANY signature made with the key, on ANY chain and
in ANY address form of that key.  Everything below is computable from the
puzzle address alone:

  * P2WPKH (bc1q...) and P2SH-P2WPKH (3...) of the same compressed key use the
    same hash160, so they can be looked up without knowing the key;
  * every pre-fork UTXO of a puzzle address also exists on Bitcoin Cash (Aug
    2017), Bitcoin Gold (Oct 2017) and Bitcoin SV (Nov 2018) (and eCash, whose
    index only speaks protobuf and is not queried here), where the same key signs; a spend there exposes the public key even
    though the bitcoin side is untouched.

For every puzzle address this script asks each of those places whether any
output has ever been spent.  Spent outputs on a puzzle whose key is not
public (private_key empty in puzzle-all.xlsx) is a hit: the spending
transaction carries the public key.  Results: novel/sidechannel.json.
Usage: sidechannel.py [first_puzzle [last_puzzle [unsolved]]]   ("unsolved" skips solved puzzles)
"""
import hashlib, json, sys, time, urllib.request, openpyxl

A58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
def sha256d(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def b58dec(s):
    n = 0
    for c in s: n = n * 58 + A58.index(c)
    raw = n.to_bytes(25, "big"); assert sha256d(raw[:-4])[:4] == raw[-4:]
    return raw[0], raw[1:21]
def b58enc(ver, h):
    raw = bytes([ver]) + h; raw += sha256d(raw)[:4]
    n = int.from_bytes(raw, "big"); s = ""
    while n: n, r = divmod(n, 58); s = A58[r] + s
    return "1" * (len(raw) - len(raw.lstrip(b"\0"))) + s
CH = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
def bech32_polymod(v):
    G = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]; c = 1
    for x in v:
        b = c >> 25; c = (c & 0x1ffffff) << 5 ^ x
        for i in range(5): c ^= G[i] if (b >> i) & 1 else 0
    return c
def bech32_p2wpkh(h):
    data = [0]; acc = bits = 0
    for byte in h:
        acc = (acc << 8) | byte; bits += 8
        while bits >= 5: bits -= 5; data.append((acc >> bits) & 31)
    if bits: data.append((acc << (5 - bits)) & 31)
    hrp = "bc"; exp = [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp]
    pm = bech32_polymod(exp + data + [0] * 6) ^ 1
    return hrp + "1" + "".join(CH[d] for d in data + [(pm >> 5 * (5 - i)) & 31 for i in range(6)])
def hash160(b): return hashlib.new("ripemd160", hashlib.sha256(b).digest()).digest()

def get(url, tries=4, pause=1.5):
    for a in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            return json.loads(urllib.request.urlopen(req, timeout=30).read())
        except urllib.error.HTTPError as e:
            if e.code == 404: return None
            time.sleep(pause * (a + 1))
        except Exception: time.sleep(pause * (a + 1))
    return "ERR"

def btc_form(addr):                       # blockstream: spent_txo_count
    d = get("https://blockstream.info/api/address/" + addr)
    if d in (None, "ERR"): return d
    c = d["chain_stats"]; return {"funded": c["funded_txo_count"], "spent": c["spent_txo_count"], "txs": c["tx_count"]}
def blockbook(base, addr):                # totalSent > 0 means some output was spent
    d = get(f"{base}/api/v2/address/{addr}?details=basic")
    if d in (None, "ERR"): return d
    return {"received": int(d["totalReceived"]), "sent": int(d["totalSent"]), "txs": d["txs"]}
def bsv(addr):                            # whatsonchain: history txs that are not unspent funding txs
    h = get(f"https://api.whatsonchain.com/v1/bsv/main/address/{addr}/confirmed/history")
    u = get(f"https://api.whatsonchain.com/v1/bsv/main/address/{addr}/unspent/all")
    if "ERR" in (h, u) or h is None or u is None: return "ERR"
    hist = {x["tx_hash"] for x in h.get("result", [])}; unsp = {x["tx_hash"] for x in u.get("result", [])}
    return {"txs": len(hist), "unspent_funding": len(unsp), "spent_evidence": len(hist - unsp)}
def xec(h160):                            # chronik
    d = get(f"https://chronik.e.cash/script/p2pkh/{h160.hex()}/history?page=0&page_size=25")
    if d in (None, "ERR"): return d
    return {"txs": d.get("numTxs", len(d.get("txs", [])))}

def exposed(r):
    if not isinstance(r, dict): return False
    return (r.get("spent", 0) or r.get("sent", 0) or r.get("spent_evidence", 0)) > 0

def main():
    lo = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    hi = int(sys.argv[2]) if len(sys.argv) > 2 else 160
    only_unsolved = len(sys.argv) > 3 and sys.argv[3] == "unsolved"
    wb = openpyxl.load_workbook("puzzle-all.xlsx", read_only=True); ws = wb["puzzle-all"]
    rows = {}
    for row in ws.iter_rows(min_row=2, values_only=True):
        if isinstance(row[0], int) and row[0] not in rows: rows[row[0]] = row
    out = {}
    for n in range(lo, hi + 1):
        row = rows[n]; addr = row[3]; solved = bool(row[7]); btc_pub = bool(row[6])
        if only_unsolved and solved: continue
        ver, h = b58dec(addr)
        forms = {"p2wpkh": bech32_p2wpkh(h), "p2sh-p2wpkh": b58enc(5, hash160(b"\x00\x14" + h)), "btg": b58enc(38, h)}
        r = {"puzzle": n, "address": addr, "solved": solved, "btc_pubkey_known": btc_pub, "forms": forms}
        r["btc_p2wpkh"] = btc_form(forms["p2wpkh"]); time.sleep(0.25)
        r["btc_p2sh_p2wpkh"] = btc_form(forms["p2sh-p2wpkh"]); time.sleep(0.25)
        r["bch"] = blockbook("https://bchblockexplorer.com", addr); time.sleep(0.35)
        r["btg"] = blockbook("https://btgexplorer.com", forms["btg"]); time.sleep(0.35)
        r["bsv"] = bsv(addr); time.sleep(0.5)
        hits = [k for k in ("btc_p2wpkh", "btc_p2sh_p2wpkh", "bch", "btg", "bsv") if exposed(r[k])]
        r["exposed_on"] = hits
        out[n] = r
        flag = "!!!" if (hits and not solved and not btc_pub) else "   "
        errs = [k for k in ("btc_p2wpkh", "btc_p2sh_p2wpkh", "bch", "btg", "bsv") if r[k] == "ERR"]
        print(f"{flag} puzzle {n:3d} {'solved  ' if solved else 'unsolved'} btc-pubkey {'known' if btc_pub else 'hidden'}: spent on {hits or '-'}"
              + (f"  [errors: {errs}]" if errs else ""), flush=True)
        json.dump(out, open("novel/sidechannel.json", "w"), indent=1)
    new = [n for n, r in out.items() if r["exposed_on"] and not r["solved"] and not r["btc_pubkey_known"]]
    print(f"[+] puzzles with a hidden bitcoin public key whose key is unsolved and which have a spend elsewhere: {new or 'none'}")

if __name__ == "__main__": main()
