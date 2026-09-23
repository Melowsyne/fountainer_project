#!/usr/bin/env python3
# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
"""analyze.py — evaluates the JSONL logs of a stress test run.

    python3 analyze.py <results-dir>

Expects in the folder: web_*.jsonl, cpp_*.jsonl, can_*.jsonl (any number),
optionally pi_offset_ms (file with VM clock minus Pi clock in ms, applied to
can_*), *.summary.json (stdout summaries of the tools).
Output: Markdown on stdout (counters per path, output latencies per
switching operation, uptime/heap trend).
"""
import glob
import json
import os
import statistics
import sys
from collections import defaultdict


def load(path, offset=0):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            r["t"] = r.get("t", 0) + offset
            r["_file"] = os.path.basename(path)
            out.append(r)
    return out


def st(xs):
    if not xs:
        return "-"
    xs = sorted(xs)
    return f"min {xs[0]:.0f} / avg {statistics.fmean(xs):.0f} / p95 {xs[int(len(xs) * 0.95) - 1]:.0f} / max {xs[-1]:.0f} ms"


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    offset = 0
    if os.path.exists(os.path.join(d, "pi_offset_ms")):
        offset = int(open(os.path.join(d, "pi_offset_ms")).read().strip() or 0)
    recs = []
    for p in sorted(glob.glob(os.path.join(d, "*.jsonl"))):
        recs += load(p, offset if os.path.basename(p).startswith("can_") else 0)
    recs.sort(key=lambda r: r["t"])
    # Recalibrate the Pi clock: the TPDO1 edge (event-driven, < 200 ms after
    # switching) must coincide with the web/C++ toggles; the offset measured
    # via SSH is only accurate to ~1 s and the VM clock drifts.
    edges = [r for r in recs if r.get("path") == "can" and r.get("op") == "tpdo1"]
    pairs = []                                  # (t_can, diff) per edge
    for tg in recs:
        if tg.get("op") != "toggle" or tg.get("path") == "can":
            continue
        want = 1 if tg.get("action", "").lower() == "on" else 0 if tg.get("action", "").lower() == "off" else None
        cands = [e for e in edges if e.get("relay") == want and abs(e["t"] - tg["t"]) < 30000]
        if cands:
            e = min(cands, key=lambda e: abs(e["t"] - tg["t"]))
            pairs.append((e["t"], tg["t"] - e["t"]))
    # Offset AND drift (the VM clock runs ~1 s/min against the Pi): linear
    # regression over all edges, constant offset with only one edge.
    a = b = 0.0
    if len(pairs) >= 2:
        xs = [x for x, _ in pairs]; ys = [y for _, y in pairs]
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        vx = sum((x - mx) ** 2 for x in xs)
        b = sum((x - mx) * (y - my) for x, y in pairs) / vx if vx else 0.0
        a = my - b * mx
    elif pairs:
        a = pairs[0][1]
    for r in recs:
        if r.get("path") == "can":
            r["t"] = int(r["t"] + a + b * r["t"])
    recs.sort(key=lambda r: r["t"])
    print(f"# Evaluation {d}\n")
    print(f"Records: {len(recs)}  (Pi clock offset {offset} ms via SSH; recalibration over "
          f"{len(pairs)} relay edges: correction {[round(y) for _, y in pairs]} ms, "
          f"drift {b * 60000:+.0f} ms/min)\n")

    # ---- Counters per file/path --------------------------------------------------------------
    print("## Operations per log file\n")
    print("| File | Path | Reads ok | Read latency | Writes ok | Errors | Error kinds |")
    print("|---|---|---|---|---|---|---|")
    by_file = defaultdict(list)
    for r in recs:
        by_file[r["_file"]].append(r)
    for f, rs in sorted(by_file.items()):
        reads = [r for r in rs if r.get("op") == "read" and "error" not in r]
        writes = [r for r in rs if r.get("op") == "write" and "error" not in r]
        errs = [r for r in rs if r.get("error") or (r.get("op") == "toggle" and r.get("status") not in (None, "applied"))]
        kinds = sorted({(r.get("error") or r.get("status") or "")[:50] for r in errs})
        print(f"| {f} | {rs[0].get('path')} | {len(reads)} | {st([r['ms'] for r in reads if 'ms' in r])} | "
              f"{len(writes)} | {len(errs)} | {'; '.join(kinds)[:120]} |")

    # ---- Connection events ---------------------------------------------------------------------
    conn = [r for r in recs if r.get("op") in ("conn", "churn", "emcy")]
    if conn:
        print("\n## Connection/special events\n")
        for r in conn[:60]:
            print(f"- t={r['t']} {r['_file']}: {r['op']} " +
                  " ".join(f"{k}={v}" for k, v in r.items() if k not in ("t", "_file", "op", "path")))
        if len(conn) > 60:
            print(f"- ... {len(conn) - 60} more")

    # ---- Output: visibility latency per switching operation ---------------------------------
    toggles = [r for r in recs if r.get("op") == "toggle"]
    if toggles:
        print("\n## Output (Fon_Relay_Output): visibility per path after each switching action\n")
        print("| t (ms) | triggered by | Action | Status | Web sees after | C++ sees after | CAN sees after |")
        print("|---|---|---|---|---|---|---|")
        observers = {
            "web": [r for r in recs if r.get("path") == "web" and r.get("op") == "read" and "relay" in r],
            "cpp": [r for r in recs if r.get("path") == "cpp" and r.get("op") == "read" and "relay" in r],
            "can": [r for r in recs if r.get("path") == "can" and
                    ((r.get("op") == "tpdo1") or (r.get("op") == "read" and r.get("name") == "Fon_Relay_Output"))],
        }
        for tg in toggles:
            want = 1 if tg.get("action", "").lower() == "on" else 0 if tg.get("action", "").lower() == "off" else None
            cells = []
            for p in ("web", "cpp", "can"):
                seen = "-"
                if want is not None:
                    for r in observers[p]:
                        if r["t"] < tg["t"]:
                            continue
                        val = r.get("relay") if "relay" in r else r.get("value")
                        if val is not None and int(float(val)) == want:
                            seen = f"{(r['t'] - tg['t']) / 1000:.1f} s"
                            break
                        if r["t"] - tg["t"] > 60000:
                            seen = ">60 s"
                            break
                cells.append(seen)
            print(f"| {tg['t']} | {tg.get('path')} | {tg.get('action')} | {tg.get('status', '')[:40]} | " + " | ".join(cells) + " |")

    # ---- Uptime / Heap -----------------------------------------------------------------------------
    # Check uptime per path (the paths read values of different freshness:
    # dp_read refreshes System_*, SDO reads the up to 5 s old stored value).
    print("\n## Device\n")
    for p in ("web", "cpp", "can"):
        ups = [(r["t"], r["uptime"]) for r in recs if r.get("path") == p and r.get("uptime") not in (None, -1)]
        ups += [(r["t"], r["value"]) for r in recs if r.get("path") == p and r.get("op") == "read"
                and r.get("name") == "System_Uptime" and "value" in r]
        ups.sort()
        if ups:
            resets = [(a, b) for a, b in zip(ups, ups[1:]) if b[1] < a[1] - 5]
            print(f"- System_Uptime ({p}): {ups[0][1]:.0f} s -> {ups[-1][1]:.0f} s, "
                  f"{'NO reboot' if not resets else f'{len(resets)} REBOOT(S): {resets[:3]}'}")
    mf = [r["min_free"] for r in recs if r.get("min_free") not in (None, -1)]
    lg = [r["largest"] for r in recs if r.get("largest") not in (None, -1)]
    if mf:
        print(f"- System_Min_Memory_Free: first value {mf[0]:.0f}, minimum {min(mf):.0f}, last {mf[-1]:.0f} B")
    if lg:
        print(f"- System_Largest_Free_Block: first value {lg[0]:.0f}, minimum {min(lg):.0f}, last {lg[-1]:.0f} B")

    # ---- Tool summaries ---------------------------------------------------------------------------
    sums = sorted(glob.glob(os.path.join(d, "*.summary.json")))
    if sums:
        print("\n## Tool summaries\n")
        for p in sums:
            txt = open(p).read().strip()
            for line in txt.splitlines():
                line = line.strip()
                if line.startswith("{"):
                    print(f"- `{os.path.basename(p)}`: `{line[:400]}`")
    return 0


if __name__ == "__main__":
    sys.exit(main())
