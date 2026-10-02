#!/usr/bin/env python3
"""Fetch every on-chain signature made by the puzzle keys whose public key is
exposed (puzzles 65, 70, 75, ... 160): the spending transaction, the input
index, the DER signature, the pubkey and the raw transaction hex needed to
rebuild the signed message hash.  Output: novel/nonce_sigs.json."""
import json, sys, time, urllib.request, openpyxl

API = "https://blockstream.info/api"
def get(path, binary=False):
    for attempt in range(5):
        try:
            with urllib.request.urlopen(API + path, timeout=30) as r:
                d = r.read()
                return d if binary else d.decode()
        except Exception as e:
            time.sleep(2 * (attempt + 1))
    raise SystemExit("fetch failed: " + path)

wb = openpyxl.load_workbook("puzzle-all.xlsx", read_only=True); ws = wb["puzzle-all"]
rows = {}
for row in ws.iter_rows(min_row=2, values_only=True):
    if isinstance(row[0], int) and row[0] not in rows: rows[row[0]] = row
targets = [n for n in sorted(rows) if rows[n][6]]          # pubkey exposed
out = []
for n in targets:
    addr, pub, priv = rows[n][3], rows[n][6], rows[n][7]
    txs, last = [], None
    while True:
        page = json.loads(get(f"/address/{addr}/txs" + (f"/chain/{last}" if last else "")))
        if not page: break
        txs += page; last = page[-1]["txid"]
        if len(page) < 25: break
    for tx in txs:
        for i, vin in enumerate(tx["vin"]):
            if vin.get("prevout", {}).get("scriptpubkey_address") != addr: continue
            out.append({"puzzle": n, "address": addr, "pubkey": pub, "privkey": priv,
                        "txid": tx["txid"], "vin": i, "block_time": tx.get("status", {}).get("block_time"),
                        "block_height": tx.get("status", {}).get("block_height"),
                        "scriptsig": vin["scriptsig"], "prev_spk": vin["prevout"]["scriptpubkey"],
                        "rawtx": get(f"/tx/{tx['txid']}/hex").strip()})
            print(f"puzzle {n:3d} spend {tx['txid']}:{i} at block {tx.get('status',{}).get('block_height')}", file=sys.stderr)
    time.sleep(0.3)
json.dump(out, open("novel/nonce_sigs.json", "w"), indent=1)
print(f"{len(out)} signatures from {len(set(o['puzzle'] for o in out))} puzzle keys", file=sys.stderr)
