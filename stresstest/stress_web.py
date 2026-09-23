#!/usr/bin/env python3
# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
"""stress_web.py — load and output observation via the web path
(REST API of the cloud server -> signed WSS session -> device).

    python3 stress_web.py --seconds 60 --workers 3 --jsonl results/web.jsonl \
        [--toggle 0:On,45:Off,...] [--url http://localhost:8010] [--device ID]

Worker mix (each worker runs its loop until the end):
  worker 0: dp_read [Fon_Relay_Output, Fon_Current_State, System_Uptime,
            System_Min_Memory_Free] at 1 s intervals (output observer)
  worker 1: dp_write Fon_Event_Label every 5 s + GET /api/devices in between
  worker 2+: dp_read of various names without pause (as much as the path allows)
Toggles (set_state) run in the main thread according to the schedule.

Each operation is logged as a JSONL line: t (epoch ms), path=web,
op, ms, ok/error, and for reads relay/state/uptime. At the end a
summary is printed as JSON on stdout.
"""
import argparse
import json
import statistics
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "samples" / "web"))
from web_api_client import FountainerWebClient  # noqa: E402

LOCK = threading.Lock()


class Recorder:
    def __init__(self, path):
        self.f = open(path, "a") if path else None
        self.lat = {"read": [], "write": [], "devices": [], "toggle": []}
        self.count = {"read": 0, "write": 0, "devices": 0, "toggle": 0}
        self.errors = []

    def log(self, op, ms, ok, **extra):
        rec = {"t": int(time.time() * 1000), "path": "web", "op": op, "ms": round(ms), **extra}
        with LOCK:
            if self.f:
                self.f.write(json.dumps(rec) + "\n"); self.f.flush()
            if ok:
                self.count[op] += 1; self.lat[op].append(ms)
            else:
                self.errors.append(rec)

    def summary(self, seconds):
        def st(xs):
            if not xs:
                return {}
            xs = sorted(xs)
            return {"min_ms": round(xs[0]), "avg_ms": round(statistics.fmean(xs)),
                    "p95_ms": round(xs[int(len(xs) * 0.95) - 1]), "max_ms": round(xs[-1])}
        return {"path": "web", "seconds": seconds, "counts": self.count,
                "errors": len(self.errors),
                "error_kinds": sorted({e.get("error", "")[:60] for e in self.errors}),
                "latency": {k: st(v) for k, v in self.lat.items() if v}}


def timed(rec, op, fn, **extra):
    t0 = time.perf_counter()
    try:
        r = fn()
        rec.log(op, (time.perf_counter() - t0) * 1e3, True, **extra, **(r or {}))
        return True
    except Exception as exc:  # noqa: BLE001 — every error is a measurement
        rec.log(op, (time.perf_counter() - t0) * 1e3, False, error=str(exc)[:120], **extra)
        return False


def worker_observer(c, dev, rec, stop):
    names = ["Fon_Relay_Output", "Fon_Current_State", "System_Uptime",
             "System_Min_Memory_Free", "System_Largest_Free_Block"]
    while not stop.is_set():
        t0 = time.time()
        timed(rec, "read", lambda: _read(c, dev, names))
        time.sleep(max(0.0, 1.0 - (time.time() - t0)))


def _read(c, dev, names):
    dp = c.dp_read(dev, names)
    out = {}
    if "Fon_Relay_Output" in dp:
        out["relay"] = int(bool(dp["Fon_Relay_Output"]))
    if "Fon_Current_State" in dp:
        out["state"] = dp["Fon_Current_State"]
    for k, o in (("System_Uptime", "uptime"), ("System_Min_Memory_Free", "min_free"),
                 ("System_Largest_Free_Block", "largest")):
        if k in dp:
            out[o] = dp[k]
    return out


def worker_writer(c, dev, rec, stop):
    i = 0
    while not stop.is_set():
        timed(rec, "write", lambda: _write(c, dev, i % 8))
        i += 1
        for _ in range(4):
            if stop.is_set():
                break
            timed(rec, "devices", lambda: _devices(c, dev))
            time.sleep(1.0)


def _write(c, dev, v):
    res = c.dp_write(dev, Fon_Event_Label=v)
    if res.get("status") != "applied":
        raise RuntimeError(f"dp_write {res.get('status')} {res.get('errors')}")
    return {}


def _devices(c, dev):
    d = [x for x in c.devices() if x["device_id"] == dev][0]
    if not d["online"]:
        raise RuntimeError("device offline")
    return {"online": True}


def worker_reader(c, dev, rec, stop, k):
    groups = [["Fon_Current_Pressure", "Fon_Pressure_Filtered", "Fon_Fault_Code"],
              ["Can_State", "Can_Sdo_Count", "Can_Bus_Errors", "Net_Link_Score"],
              ["Fon_Min_Pressure", "Fon_Max_Pressure", "Fon_Min_On_Time"]]
    i = k
    while not stop.is_set():
        timed(rec, "read", lambda: (c.dp_read(dev, groups[i % len(groups)]) and {}))
        i += 1
        time.sleep(0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--url", default="http://localhost:8010")
    ap.add_argument("--device", default="esp32-94a990ddc1a8")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--jsonl", default="")
    ap.add_argument("--toggle", default="", help="e.g. 0:On,45:Off (set_state according to schedule)")
    args = ap.parse_args()

    if args.device == "esp32-a1b2c3d4e5f6":
        sys.exit("production pump is off-limits")
    c = FountainerWebClient(args.url, timeout=20)
    rec = Recorder(args.jsonl)
    stop = threading.Event()
    threads = [threading.Thread(target=worker_observer, args=(c, args.device, rec, stop), daemon=True)]
    if args.workers > 1:
        threads.append(threading.Thread(target=worker_writer, args=(c, args.device, rec, stop), daemon=True))
    for k in range(max(0, args.workers - 2)):
        threads.append(threading.Thread(target=worker_reader, args=(c, args.device, rec, stop, k), daemon=True))
    for t in threads:
        t.start()

    toggles = []
    for item in filter(None, args.toggle.split(",")):
        at, _, st = item.partition(":")
        toggles.append((float(at), st))
    t_start = time.monotonic()
    while time.monotonic() - t_start < args.seconds:
        if toggles and time.monotonic() - t_start >= toggles[0][0]:
            _, st = toggles.pop(0)
            ok = timed(rec, "toggle", lambda: _toggle(c, args.device, st), action=st)
            print(f"[{time.monotonic() - t_start:6.1f}s] toggle {st} -> {'applied' if ok else 'ERROR'}",
                  flush=True)
        time.sleep(0.2)
    stop.set()
    for t in threads:
        t.join(timeout=25)
    print(json.dumps(rec.summary(args.seconds)))
    return 1 if rec.errors else 0


def _toggle(c, dev, state):
    res = c.set_state(dev, state)
    if res.get("status") != "applied":
        raise RuntimeError(f"set_state {state}: {res.get('status')} {res.get('error')}")
    return {"status": "applied"}


if __name__ == "__main__":
    sys.exit(main())
