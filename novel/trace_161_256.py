#!/usr/bin/env python3
"""The 2015 funding transaction (08389f34...) has 256 outputs of n/1000 BTC for
n = 1..256.  Outputs 1..160 are the published puzzle addresses; this maps
outputs 161..256 (what happened to them, which public keys are exposed) and
checks the 2017 sweep that spent them."""
import json, sys, time, urllib.request, openpyxl
sys.path.insert(0, "novel")
from nonce_forensics import parse_scriptsig, parse_der
API = "https://blockstream.info/api"
def get(p):
    for a in range(5):
        try: return json.loads(urllib.request.urlopen(API + p, timeout=30).read())
        except Exception: time.sleep(2 * (a + 1))
    raise SystemExit("fetch failed " + p)
fund = get("/tx/08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15")
sp = get("/tx/08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15/outspends")
wb = openpyxl.load_workbook("puzzle-all.xlsx", read_only=True); ws = wb["puzzle-all"]
sheet = {}
for row in ws.iter_rows(min_row=2, values_only=True):
    if isinstance(row[0], int) and row[0] not in sheet: sheet[row[0]] = row[3]
rows = []
for i, (o, s) in enumerate(zip(fund["vout"], sp)):
    val = o["value"] / 1e8; n = round(val * 1000)
    rows.append((i, n, o["scriptpubkey_address"], val, s))
print("[+] output values: n/1000 BTC for n=1..256 in order:", all(abs(v * 1000 - n) < 1e-6 and n == i + 1 for i, n, _, v, _ in rows) or "NOT in order, see below")
inpuz = sum(1 for _, n, a, *_ in rows if n in sheet and sheet[n] == a)
print(f"[+] outputs matching the published puzzle address for the same n: {inpuz} of 160 (n<=160); outputs 161..256 not in the sheet: {sum(1 for _, n, *_ in rows if n > 160)}")
sweep = {}
for i, n, a, v, s in rows:
    if n <= 160 and not s.get("spent"): continue
    sweep.setdefault(s.get("txid"), []).append((n, a))
print("[+] spending transactions of the 256 outputs (txid: count, n range):")
for t, L in sorted(sweep.items(), key=lambda kv: -len(kv[1]))[:6]:
    ns = sorted(n for n, _ in L); print(f"    {str(t)[:16]}: {len(L)} outputs, n {ns[0]}..{ns[-1]}")
t2017 = get("/tx/5d45587cfd1d5b0fb826805541da7d94c61fe432259e68ee26f4a04544384164")
out = []
n_unspent = 0
for i, n, a, v, s in rows:
    if n <= 160: continue
    info = get(f"/address/{a}")["chain_stats"]
    pub = None; sweep_vin = None
    for k, vin in enumerate(t2017["vin"]):
        if vin.get("prevout", {}).get("scriptpubkey_address") == a:
            sweep_vin = k; sig, pub = parse_scriptsig(vin["scriptsig"]); r, sv, ht = parse_der(sig); pub = pub.hex(); break
    out.append({"n": n, "address": a, "value": v, "spent_in": (s.get("txid") if s.get("spent") else None), "swept_2017_vin": sweep_vin,
                "pubkey": pub, "tx_count": info["tx_count"], "balance": (info["funded_txo_sum"] - info["spent_txo_sum"]) / 1e8})
    if not s.get("spent"): n_unspent += 1
json.dump(out, open("novel/puzzle_161_256.json", "w"), indent=1)
print(f"[+] outputs 161..256: {len(out)}; swept in the 2017 top-up: {sum(1 for x in out if x['swept_2017_vin'] is not None)}; still unspent: {n_unspent}; with exposed pubkey: {sum(1 for x in out if x['pubkey'])}")
print(f"    compressed pubkeys: {sum(1 for x in out if x['pubkey'] and x['pubkey'][:2] in ('02','03'))}; later activity (tx_count > 2): {[x['n'] for x in out if x['tx_count'] > 2]}")
print("[+] the 2017 top-up outputs (what the swept coins became):")
vals = sorted(round(o['value']/1e8, 3) for o in t2017["vout"])
print(f"    {len(vals)} outputs, values {vals[:3]} .. {vals[-3:]}; to puzzle addresses of n: ", end="")
nmatch = sorted(n for n, a in sheet.items() for o in t2017["vout"] if o.get("scriptpubkey_address") == a)
print(f"{len(nmatch)} of them, n {nmatch[0]}..{nmatch[-1]}")
