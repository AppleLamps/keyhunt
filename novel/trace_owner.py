#!/usr/bin/env python3
"""Trace the puzzle creator's wallet on chain: every transaction that FUNDED a
puzzle address, the inputs of those transactions (walked backwards while the
chain stays narrow), and the outputs of the creator's own spends (walked
forwards).  Prints a cluster of creator-controlled addresses with their
public-key exposure, so the novel/ hypotheses can be tested on them too."""
import json, sys, time, urllib.request, openpyxl
from collections import defaultdict
API = "https://blockstream.info/api"
cache = {}
def get(path):
    if path in cache: return cache[path]
    for a in range(5):
        try:
            with urllib.request.urlopen(API + path, timeout=30) as r: d = r.read().decode(); cache[path] = d; return d
        except Exception: time.sleep(2 * (a + 1))
    raise SystemExit("fetch failed " + path)
def txs_of(addr):
    out, last = [], None
    while True:
        page = json.loads(get(f"/address/{addr}/txs" + (f"/chain/{last}" if last else "")))
        if not page: break
        out += page; last = page[-1]["txid"]
        if len(page) < 25: break
    return out
def tx(txid): return json.loads(get(f"/tx/{txid}"))
def when(t): import datetime; return datetime.datetime.utcfromtimestamp(t["status"]["block_time"]).strftime("%Y-%m-%d") if t.get("status", {}).get("block_time") else "mempool"

wb = openpyxl.load_workbook("puzzle-all.xlsx", read_only=True); ws = wb["puzzle-all"]
puzzle = {}
for row in ws.iter_rows(min_row=2, values_only=True):
    if isinstance(row[0], int) and row[0] not in puzzle: puzzle[row[3]] = row[0]
paddrs = set(puzzle)

# 1. funding transactions: any tx with an output to a puzzle address whose inputs are NOT puzzle addresses
# sample several puzzle addresses across the range to find all distinct funding txs
funding = {}
for a in [k for k, v in puzzle.items() if v in (71, 100, 140, 160, 159)]:
    for t in txs_of(a):
        outs = [o for o in t["vout"] if o.get("scriptpubkey_address") in paddrs]
        if outs and t["txid"] not in funding: funding[t["txid"]] = t
print("[+] transactions paying INTO puzzle addresses (found via 9 sample addresses):")
for txid, t in sorted(funding.items(), key=lambda kv: kv[1]["status"].get("block_height", 0)):
    ins = [v.get("prevout", {}).get("scriptpubkey_address") for v in t["vin"]]
    n_puz_out = sum(1 for o in t["vout"] if o.get("scriptpubkey_address") in paddrs)
    tot = sum(o["value"] for o in t["vout"] if o.get("scriptpubkey_address") in paddrs) / 1e8
    kind = "creator" if not any(i in paddrs for i in ins) else "puzzle-internal"
    print(f"    {when(t)} {txid} {kind}: {len(t['vin'])} in, {len(t['vout'])} out, {n_puz_out} puzzle outputs, {tot:.4f} BTC to puzzles; inputs from {sorted(set(ins))[:4]}")

# 2. walk backwards from the creator funding inputs
def walk_back(txid, depth, seen, chain):
    if depth == 0 or txid in seen: return
    seen.add(txid); t = tx(txid)
    ins = [(v["prevout"]["scriptpubkey_address"], v["txid"], v["prevout"]["value"], v.get("scriptsig", "")) for v in t["vin"] if v.get("prevout")]
    chain.append((depth, txid, when(t), len(ins), len(t["vout"]), ins))
    if len(ins) > 6:   # a wide fan-in looks like an exchange/service hot wallet: stop
        return
    for _, ptx, _, _ in ins: walk_back(ptx, depth - 1, seen, chain)

print("\n[+] backwards from the creator's funding transactions (stop at >6 inputs):")
creator_addrs = defaultdict(set)   # addr -> set of roles
for txid, t in funding.items():
    ins = [v.get("prevout", {}).get("scriptpubkey_address") for v in t["vin"]]
    if any(i in paddrs for i in ins): continue
    chain = []; walk_back(txid, 6, set(), chain)
    for depth, tid, date, nin, nout, ins in chain:
        print(f"    {'  ' * (6 - depth)}{date} {tid[:16]} {nin} in / {nout} out  <- {[i[0][:12] + '..' for i in ins][:5]}")
        for a, _, _, ss in ins:
            creator_addrs[a].add("funding-input" if depth == 6 else f"funding-ancestor-{6-depth}")

# 3. forwards from the creator's own spends out of puzzle addresses (the 2019 consolidation and any 2017 moves)
print("\n[+] forwards from creator spends (inputs from puzzle addresses, outputs not to puzzle addresses):")
sigs = json.load(open("novel/nonce_sigs.json"))
creator_tx_ids = set()
for o in sigs:
    if o["block_height"] and o["block_height"] < 600000: creator_tx_ids.add(o["txid"])   # before 2019-10: creator era (solved low puzzles were swept by others too, so filter below)
for tid in sorted(creator_tx_ids):
    t = tx(tid)
    ins = [v["prevout"]["scriptpubkey_address"] for v in t["vin"]]
    if sum(1 for i in ins if i in paddrs) < 3: continue   # creator moves bundle many puzzle inputs; solvers sweep one
    outs = [(o.get("scriptpubkey_address"), o["value"] / 1e8) for o in t["vout"]]
    print(f"    {when(t)} {tid}: {len(ins)} inputs ({sorted(puzzle[i] for i in ins if i in paddrs)}) -> {outs}")
    for a, v in outs:
        if a and a not in paddrs:
            creator_addrs[a].add("spend-output")
            for t2 in txs_of(a):
                if any(vi.get("prevout", {}).get("scriptpubkey_address") == a for vi in t2["vin"]):
                    o2 = [(o.get("scriptpubkey_address"), o["value"] / 1e8) for o in t2["vout"]]
                    print(f"        {when(t2)} {a} spent in {t2['txid'][:16]} ({len(t2['vin'])} in) -> {o2[:4]}")
                    for a2, _ in o2:
                        if a2 and a2 not in paddrs: creator_addrs[a2].add("spend-output-hop2")

# 4. cluster summary with pubkey exposure
print("\n[+] creator-linked addresses (outside the puzzle set):")
for a, roles in sorted(creator_addrs.items(), key=lambda kv: sorted(kv[1])):
    info = json.loads(get(f"/address/{a}"))
    cs = info["chain_stats"]; spent = cs["spent_txo_count"] > 0
    pub = None
    if spent:
        for t in txs_of(a):
            for v in t["vin"]:
                if v.get("prevout", {}).get("scriptpubkey_address") == a and v.get("scriptsig"):
                    ss = bytes.fromhex(v["scriptsig"]); l = ss[0]; pub = ss[l + 2:l + 2 + ss[l + 1]].hex(); break
            if pub: break
    print(f"    {a:36s} {sorted(roles)} funded {cs['funded_txo_count']} spent {cs['spent_txo_count']} balance {(cs['funded_txo_sum']-cs['spent_txo_sum'])/1e8:.8f} pubkey {'exposed ' + pub[:10] + '..' if pub else 'not exposed'}")
json.dump({a: sorted(r) for a, r in creator_addrs.items()}, open("novel/owner_cluster.json", "w"), indent=1)
