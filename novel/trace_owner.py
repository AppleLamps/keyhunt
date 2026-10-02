#!/usr/bin/env python3
"""Focused on-chain trace of the puzzle creator: the transactions that
provably came from the creator (2015 funding, 2017 top-up, 2019 dust priming
and consolidation, 2021 top-up, 2023 prize increase), walked backwards
through their inputs (stopping at wide fan-ins, which look like services)
and forwards through their non-puzzle outputs.  Reports the resulting
address cluster with public-key exposure (novel/owner_cluster.json)."""
import json, sys, time, urllib.request, openpyxl, datetime
from collections import defaultdict, deque
API = "https://blockstream.info/api"; cache = {}
def get(path):
    if path in cache: return cache[path]
    for a in range(5):
        try:
            with urllib.request.urlopen(API + path, timeout=30) as r: d = r.read().decode(); cache[path] = d; return d
        except Exception: time.sleep(2 * (a + 1))
    raise SystemExit("fetch failed " + path)
def tx(txid): return json.loads(get(f"/tx/{txid}"))
def outspends(txid): return json.loads(get(f"/tx/{txid}/outspends"))
def when(t): return datetime.datetime.utcfromtimestamp(t["status"]["block_time"]).strftime("%Y-%m-%d") if t.get("status", {}).get("block_time") else "mempool"
wb = openpyxl.load_workbook("puzzle-all.xlsx", read_only=True); ws = wb["puzzle-all"]
paddrs = {row[3] for row in ws.iter_rows(min_row=2, values_only=True) if isinstance(row[0], int)}

CREATOR_TXS = {
 "08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15": "2015 funding (256 outputs)",
 "5d45587cfd1d5b0fb826805541da7d94c61fe432259e68ee26f4a04544384164": "2017 top-up (97 inputs, 109 outputs)",
 "7c432398c7631600af01695c9767eff109cbfae4f7ecccaff388043a474d4f1e": "2019 dust priming of the 21 pubkey addresses",
 "17e4e323cfbc68d7f0071cad09364e8193eedf8fefbcbd8a21b4b65717a4b3d3": "2019 consolidation (signatures from 65..160)",
 "e1f668b8cc9915fcd3de6ec922acf98cdf4c14f75de9530b6ad750693d44076b": "2021 top-up (11 outputs)",
 "12f34b58b04dfb0233ce889f674781c0e0c7ba95482cca469125af41a78d13b3": "2023 prize increase (872 BTC, 85 outputs)",
}
roles = defaultdict(set)
def note(addr, role):
    if addr and addr not in paddrs: roles[addr].add(role)

print("[+] the creator transactions, their inputs and non-puzzle outputs")
for txid, label in CREATOR_TXS.items():
    t = tx(txid)
    ins = [(v["prevout"]["scriptpubkey_address"], v["prevout"]["value"] / 1e8) for v in t["vin"] if v.get("prevout")]
    outs = [(o.get("scriptpubkey_address"), o["value"] / 1e8) for o in t["vout"]]
    nonp = [(a, v) for a, v in outs if a not in paddrs]
    print(f"  {when(t)} {label}\n     {txid}\n     inputs: {len(ins)} from {len(set(a for a, _ in ins))} addresses, total {sum(v for _, v in ins):.4f} BTC; first: {ins[:3]}")
    print(f"     outputs: {len(outs)}, non-puzzle: {nonp}")
    for a, _ in ins: note(a, f"input of {label}")
    for a, _ in nonp: note(a, f"change/output of {label}")

print("\n[+] backwards from each creator transaction (depth 8, stop at >8 inputs = service-like fan-in)")
for txid, label in CREATOR_TXS.items():
    print(f"  {label}")
    q = deque([(txid, 0)]); seen = set()
    while q:
        tid, d = q.popleft()
        if tid in seen or d > 8: continue
        seen.add(tid); t = tx(tid)
        ins = [(v["prevout"]["scriptpubkey_address"], v["txid"], v["prevout"]["value"] / 1e8) for v in t["vin"] if v.get("prevout")]
        if d: print(f"  {'  ' * d}{when(t)} {tid[:16]} {len(ins)} in / {len(t['vout'])} out, {sum(v for *_, v in ins):.4f} BTC from {sorted(set(a for a, *_ in ins))[:3]}")
        if len(ins) > 8:
            print(f"  {'  ' * d}  (wide fan-in, stop: likely an exchange or service)"); continue
        for a, ptx, _ in ins:
            if d: note(a, f"ancestor-{d} of {label}")
            q.append((ptx, d + 1))

print("\n[+] forwards from non-puzzle outputs of creator transactions (depth 4)")
for txid, label in CREATOR_TXS.items():
    t = tx(txid); sp = outspends(txid)
    for i, (o, s) in enumerate(zip(t["vout"], sp)):
        a = o.get("scriptpubkey_address")
        if a in paddrs or not s.get("spent"): continue
        print(f"  {label}, output {i} ({a}, {o['value']/1e8:.4f} BTC):")
        q = deque([(s["txid"], 1)]); seen = set()
        while q:
            tid, d = q.popleft()
            if tid in seen or d > 4: continue
            seen.add(tid); t2 = tx(tid); sp2 = outspends(tid)
            outs = [(x.get("scriptpubkey_address"), x["value"] / 1e8) for x in t2["vout"]]
            print(f"  {'  ' * d}{when(t2)} {tid[:16]} ({len(t2['vin'])} in / {len(outs)} out) -> {outs[:4]}")
            if len(t2["vin"]) > 8 or len(outs) > 8: print(f"  {'  ' * d}  (wide, stop)"); continue
            for (a2, _), s2 in zip(outs, sp2):
                note(a2, f"descendant-{d} of {label}")
                if s2.get("spent"): q.append((s2["txid"], d + 1))

print("\n[+] cluster summary")
rows = []
for a, rs in roles.items():
    info = json.loads(get(f"/address/{a}")); cs = info["chain_stats"]
    pub = None
    if cs["spent_txo_count"]:
        for t in json.loads(get(f"/address/{a}/txs")):
            for v in t["vin"]:
                if v.get("prevout", {}).get("scriptpubkey_address") == a:
                    if v.get("witness"): pub = v["witness"][-1]            # native or wrapped segwit: key is the last witness item
                    elif v.get("scriptsig"):
                        try:
                            ss = bytes.fromhex(v["scriptsig"]); l = ss[0]; pub = ss[l + 2:l + 2 + ss[l + 1]].hex() or None
                        except IndexError: pub = None
                    break
            if pub: break
    rows.append((a, sorted(rs), cs["funded_txo_count"], cs["spent_txo_count"], (cs["funded_txo_sum"] - cs["spent_txo_sum"]) / 1e8, pub))
rows.sort(key=lambda r: r[1])
for a, rs, f, s, bal, pub in rows:
    print(f"  {a:44s} funded {f:3d} spent {s:3d} balance {bal:12.8f}  pubkey {('exposed ' + pub[:12] + '..') if pub else 'not exposed'}\n      roles: {rs[:3]}{' ...' if len(rs) > 3 else ''}")
# addresses with heavy traffic, and everything behind the 2015 custodial withdrawal, are services rather than the creator
json.dump([{"address": a, "roles": rs, "funded": f, "spent": s, "balance": bal, "pubkey": pub,
            "service_like": f > 100 or any("2015" in r and "ancestor" in r for r in rs)}
           for a, rs, f, s, bal, pub in rows], open("novel/owner_cluster.json", "w"), indent=1)
