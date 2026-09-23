#!/usr/bin/env python3
"""Decode CAN frames from Tektronix waveform captures and render annotated figures."""
import json, base64, struct, sys, os, math
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

W = os.path.expanduser("~/.cache/fnt_can")
OUT = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(OUT, exist_ok=True)
BIT_US = 4.0                       # 250 kbit/s
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9, "axes.titlesize": 11})
C_H, C_L, C_D = "#d62728", "#1f77b4", "#2ca02c"


def load(fn):
    caps = json.load(open(os.path.join(W, fn)))
    out = []
    for c in caps:
        chans = {}
        for k in ("ch1", "ch2"):
            d = c[k]; raw = np.frombuffer(base64.b64decode(d["raw"]), dtype=np.int8).astype(float)
            v = (raw - d["yoff"]) * d["ymult"] + d["yzero"]
            t = (np.arange(len(raw)) * d["xincr"] + d["xzero"]) * 1e6   # µs
            chans[k] = (t, v)
        t, l = chans["ch1"]; _, h = chans["ch2"]
        out.append({"t": t, "canl": l, "canh": h, "diff": h - l, "tdiv": c["tdiv"]})
    return out


def crc15(bits):
    crc = 0
    for b in bits:
        crc_next = ((crc >> 14) & 1) ^ b
        crc = (crc << 1) & 0x7FFF
        if crc_next:
            crc ^= 0x4599
    return crc


def find_frames(t, diff, thr=0.9):
    """Sequential: decode a frame, then look for the next SOF after its EOF (frames may be
    separated by only the 3-bit intermission)."""
    dom = diff > thr
    dt = t[1] - t[0]
    res = []
    i = 0
    while True:
        idx = np.where(dom[i:])[0]
        if len(idx) == 0:
            break
        sof = i + idx[0]
        fr = decode(t, diff, t[sof], thr)
        if "id" in fr:
            res.append((t[sof], fr))
            i = sof + int((fr["nbits_raw"] + 2) * fr["bit_us"] / dt)
        else:
            i = sof + int(3 * BIT_US / dt)
            if (len(t) - sof) * dt < 20 * BIT_US:
                break
    return res


def decode(t, diff, t_sof, thr=0.9):
    """Sample bits from SOF using edges for resync; return dict."""
    dt = t[1] - t[0]
    dom = (diff > thr).astype(int)
    i0 = int(np.searchsorted(t, t_sof))
    # refine SOF: first index where dom==1 at/after i0
    while i0 < len(dom) and dom[i0] == 0:
        i0 += 1
    # estimate bit time from all edges in the frame region (up to 160 bits)
    seg = dom[i0:i0 + int(160 * BIT_US / dt)]
    edges = np.where(np.diff(seg) != 0)[0]
    if len(edges) > 4:
        spans = np.diff(edges) * dt
        mult = np.round(spans / BIT_US); ok = mult > 0
        bit_us = float(np.sum(spans[ok]) / np.sum(mult[ok]))
    else:
        bit_us = BIT_US
    bits = []; raw_bits = []; stuff_positions = []
    tb = t[i0]                     # SOF start time
    k = 0; same = 0; prev = None
    # sample at bit centres, with hard resync on each edge (CAN style)
    pos = i0
    while k < 200:
        centre = pos + int(0.5 * bit_us / dt)
        if centre >= len(dom):
            break
        b = 1 - dom[centre]        # logic: dominant = 0
        raw_bits.append(b)
        if prev is not None and b == prev:
            same += 1
        else:
            same = 1
        if same == 6:
            # this is a stuff bit? No — after 5 equal bits the 6th must be complementary;
            # 6 equal bits = error/EOF. Handled below.
            pass
        prev = b
        k += 1
        # resync: an edge close to the nominal bit boundary re-aligns the sampling grid
        nxt = pos + int(bit_us / dt)
        lo = pos + int(0.6 * bit_us / dt); hi = pos + int(1.4 * bit_us / dt)
        win = dom[lo:hi]
        e = np.where(np.diff(win) != 0)[0]
        if len(e):
            cand = lo + e + 1
            pos = int(cand[np.argmin(np.abs(cand - nxt))])
        else:
            pos = nxt
    # de-stuff: after 5 identical bits (from SOF up to CRC), the next bit is a stuff bit
    unst = []; run = 0; prevb = None; i = 0
    # stuffing applies to SOF..CRC (excluding CRC delimiter onward); we don't know the
    # length yet, so de-stuff progressively while parsing.
    def parse(raw):
        out = []; stuffed = []; run = 0; prevb = None; i = 0
        need = None  # number of unstuffed bits covered by stuffing
        while i < len(raw):
            b = raw[i]
            if prevb is not None and b == prevb:
                run += 1
            else:
                run = 1
            out.append(b); prevb = b
            if run == 5 and (need is None or len(out) < need):
                # next raw bit is a stuff bit (opposite polarity) — skip it
                if i + 1 < len(raw):
                    stuffed.append(i + 1); i += 1; prevb = raw[i]; run = 1
            i += 1
            if need is None and len(out) >= 19:
                dlc = int("".join(map(str, out[15:19])), 2)
                need = 19 + 8 * min(dlc, 8) + 15      # bits subject to stuffing: SOF..CRC
            if need is not None and len(out) >= need + 10:
                break
        return out, stuffed
    bitsu, stuffed = parse(raw_bits)
    r = {"bit_us": bit_us, "t_sof": tb, "raw_bits": raw_bits, "bits": bitsu, "stuffed": stuffed}
    try:
        idf = int("".join(map(str, bitsu[1:12])), 2)
        rtr, ide, r0 = bitsu[12], bitsu[13], bitsu[14]
        dlc = int("".join(map(str, bitsu[15:19])), 2); n = min(dlc, 8)
        data = [int("".join(map(str, bitsu[19 + 8 * j:27 + 8 * j])), 2) for j in range(n)]
        p = 19 + 8 * n
        crc = int("".join(map(str, bitsu[p:p + 15])), 2)
        crc_ok = crc15(bitsu[0:p]) == crc
        ack = bitsu[p + 16] == 0
        r.update({"id": idf, "rtr": rtr, "ide": ide, "dlc": dlc, "data": data, "crc": crc, "crc_ok": crc_ok,
                  "ack": ack, "nbits_unstuffed": p + 15 + 1 + 1 + 1 + 7, "nbits_raw": p + 15 + 10 + len(stuffed),
                  "fields": [("SOF", 0, 1), ("Identifier", 1, 12), ("RTR", 12, 13), ("IDE", 13, 14), ("r0", 14, 15),
                             ("DLC", 15, 19)] + [(f"D{j}", 19 + 8 * j, 27 + 8 * j) for j in range(n)] +
                            [("CRC", p, p + 15), ("CRC-Del", p + 15, p + 16), ("ACK", p + 16, p + 17),
                             ("ACK-Del", p + 17, p + 18), ("EOF", p + 18, p + 25)]})
    except Exception as e:
        r["error"] = str(e)
    return r


def describe(fr):
    idf = fr.get("id"); d = fr.get("data", [])
    if idf is None:
        return "not decodable"
    node = idf & 0x7F; fc = idf & 0x780
    if fc == 0x700:
        st = {0x00: "Boot-up", 0x04: "Stopped", 0x05: "Operational", 0x7F: "Pre-operational"}.get(d[0] if d else -1, "?")
        return f"Heartbeat Node {node}: state 0x{d[0]:02X} = {st}"
    if fc == 0x180 and len(d) >= 7:
        p = struct.unpack("<f", bytes(d[0:4]))[0]
        st = {1: "Off", 2: "On", 3: "Auto", 5: "Fault"}.get(d[4], str(d[4]))
        return f"TPDO1 Node {node}: pressure {p:.2f} bar, state {st}, relay {'on' if d[5] else 'off'}, fault code {d[6]}"
    if fc == 0x280 and len(d) >= 8:
        a, b = struct.unpack("<ff", bytes(d[0:8])); return f"TPDO2 Node {node}: filtered pressure {a:.2f} bar, slope {b:.3f} bar/s"
    if fc == 0x380 and len(d) >= 8:
        a, b = struct.unpack("<II", bytes(d[0:8])); return f"TPDO3 Node {node}: uptime {a} s, pump runtime {b} s"
    if fc == 0x480 and len(d) >= 8:
        tmp = struct.unpack("<f", bytes(d[0:4]))[0]; return f"TPDO4 Node {node}: temperature {tmp:.1f} °C, Link-Score {d[4]}, Demand {d[5]}, Starts/h {d[6]}, Power-Mode {d[7]}"
    if fc == 0x600 and len(d) >= 4:
        idx = d[1] | (d[2] << 8); cs = d[0]
        kind = "SDO request (upload/read)" if cs & 0xE0 == 0x40 else ("SDO request (download/write)" if cs & 0xE0 == 0x20 else f"SDO request cs=0x{cs:02X}")
        return f"{kind} to Node {node}: object 0x{idx:04X} sub {d[3]}"
    if fc == 0x580 and len(d) >= 4:
        idx = d[1] | (d[2] << 8); cs = d[0]
        if cs & 0xE0 == 0x40:
            nb = 4 - ((cs >> 2) & 3) if cs & 1 else 4
            val = d[4:4 + nb]
            extra = ""
            if idx == 0x2019 and nb == 4:
                extra = f" = {struct.unpack('<f', bytes(val))[0]:.2f} bar (Fon_Current_Pressure)"
            elif idx == 0x200B and nb == 4:
                extra = f" = {struct.unpack('<I', bytes(val))[0]} s (System_Uptime)"
            return f"SDO response from Node {node}: object 0x{idx:04X} sub {d[3]}, data {' '.join(f'{x:02X}' for x in val)}{extra}"
        if cs == 0x60:
            return f"SDO response from Node {node}: write 0x{idx:04X} confirmed"
        if cs == 0x80:
            return f"SDO abort from Node {node}: 0x{idx:04X}"
        return f"SDO response from Node {node}: cs 0x{cs:02X}"
    if fc == 0x080 and idf != 0x80:
        return f"EMCY Node {node}: error code 0x{(d[1] << 8) | d[0]:04X}, error register 0x{d[2]:02X}"
    if idf == 0:
        return "NMT command"
    return f"COB-ID 0x{idf:03X}"


def levels(diff, canh, canl, thr=0.9):
    dom = diff > thr; rec = diff < 0.3
    mv = lambda x: int(round(float(np.median(x)) * 1000))
    return {"H_rec": mv(canh[rec]), "H_dom": mv(canh[dom]), "L_rec": mv(canl[rec]), "L_dom": mv(canl[dom]),
            "D_rec": mv(diff[rec]), "D_dom": mv(diff[dom])}


def draw_frame(cap, fr, fname, title, zoom=None):
    t, h, l, d = cap["t"], cap["canh"], cap["canl"], cap["diff"]
    t0 = fr["t_sof"]; bu = fr["bit_us"]
    nraw = fr.get("nbits_raw", 60) + 1
    if zoom:
        a, b = zoom
    else:
        a, b = -3 * bu, (nraw + 3) * bu
    m = (t - t0 >= a) & (t - t0 <= b)
    tt = t[m] - t0
    lv = levels(d, h, l)
    fig, axs = plt.subplots(3, 1, figsize=(13, 8.2), sharex=True, gridspec_kw={"height_ratios": [3, 2.2, 1.6]})
    ax = axs[0]
    ax.plot(tt, h[m] * 1000, color=C_H, lw=1.1, label="CAN_H (CH2)")
    ax.plot(tt, l[m] * 1000, color=C_L, lw=1.1, label="CAN_L (CH1)")
    for y, c, txt, va in ((lv["H_dom"], C_H, f"CAN_H dominant {lv['H_dom']} mV", "center"), (lv["H_rec"], C_H, f"CAN_H recessive {lv['H_rec']} mV", "bottom"),
                          (lv["L_rec"], C_L, f"CAN_L recessive {lv['L_rec']} mV", "top"), (lv["L_dom"], C_L, f"CAN_L dominant {lv['L_dom']} mV", "center")):
        ax.axhline(y, color=c, ls=":", lw=0.8, alpha=0.7)
        ax.text(b, y, " " + txt, color=c, va=va, ha="left", fontsize=8)
    ax.set_ylabel("Voltage vs. GND [mV]"); ax.grid(alpha=0.3); ax.legend(loc="upper left", fontsize=8)
    ax.set_title(title, loc="left", fontweight="bold")
    ax.set_xlim(a, b + 0.32 * (b - a))
    ax = axs[1]
    ax.plot(tt, d[m] * 1000, color=C_D, lw=1.1, label="V_diff = CAN_H − CAN_L")
    ax.axhline(lv["D_dom"], color=C_D, ls=":", lw=0.8); ax.text(b, lv["D_dom"], f" dominant {lv['D_dom']} mV (logic 0)", color=C_D, va="center", fontsize=8)
    ax.axhline(lv["D_rec"], color=C_D, ls=":", lw=0.8); ax.text(b, lv["D_rec"], f" recessive {lv['D_rec']} mV (logic 1)", color=C_D, va="center", fontsize=8)
    ax.axhline(900, color="gray", ls="--", lw=0.6); ax.text(a, 950, "Threshold 900 mV", color="gray", fontsize=7)
    ax.set_ylabel("Difference [mV]"); ax.grid(alpha=0.3); ax.legend(loc="upper left", fontsize=8)
    # bit ruler
    ax = axs[2]; ax.set_ylim(0, 1); ax.set_yticks([])
    raw = fr["raw_bits"]; stuffed = set(fr.get("stuffed", []))
    show = [i for i in range(min(len(raw), nraw)) if a <= i * bu <= b]
    for i in show:
        x = i * bu
        ax.add_patch(Rectangle((x, 0.55), bu, 0.3, facecolor="#ffcc00" if i in stuffed else ("#dddddd" if raw[i] else "#8fb8de"), edgecolor="white", lw=0.5))
        if bu * (b - a) / (b - a) >= 0 and len(show) <= 70:
            ax.text(x + bu / 2, 0.70, str(raw[i]), ha="center", va="center", fontsize=7 if len(show) > 40 else 8)
    # field labels (unstuffed positions -> raw positions)
    if "fields" in fr:
        u2r = []; s = sorted(stuffed); ri = 0; k = 0
        # map unstuffed index -> raw index
        for ri in range(len(raw)):
            if ri in stuffed:
                continue
            u2r.append(ri)
        # EOF is 7 recessive bits after ACK-Del: ensure the ruler shows them
        
        for name, u0, u1 in fr["fields"]:
            if u1 - 1 < len(u2r):
                r0 = u2r[u0]; r1 = u2r[u1 - 1] + 1
                x0, x1 = r0 * bu, r1 * bu
                if x1 < a or x0 > b:
                    continue
                if name == "EOF": print("   EOF label at", round(x0), round(x1), "b=", round(b))
                ax.plot([x0, x1], [0.42, 0.42], color="k", lw=1); ax.plot([x0, x0], [0.38, 0.46], color="k", lw=1); ax.plot([x1, x1], [0.38, 0.46], color="k", lw=1)
                val = ""
                if name == "Identifier": val = f" = 0x{fr['id']:03X}"
                elif name == "DLC": val = f" = {fr['dlc']}"
                elif name.startswith("D") and name[1:].isdigit(): val = f" = 0x{fr['data'][int(name[1:])]:02X}"
                elif name == "CRC": val = f" = 0x{fr['crc']:04X} {'✓' if fr['crc_ok'] else '✗'}"
                elif name == "ACK": val = " ✓" if fr["ack"] else " –"
                ax.text((x0 + x1) / 2, 0.22, name + val, ha="center", va="center", fontsize=7.5, rotation=0 if (x1 - x0) > 0.04 * (b - a) else 90)
    ax.text(a, 0.95, f"Bit time {bu:.2f} µs = {1000 / bu:.0f} kbit/s   yellow = stuff bit   blue = dominant (0)   grey = recessive (1)", fontsize=8, va="top")
    ax.set_xlabel("Time from SOF [µs]")
    fig.text(0.01, 0.01, f"Fountainer Rev.1 (FNT-000003, TJA1051T/3) ↔ Raspberry Pi (MCP2515/MCP2562), 250 kbit/s, Tektronix MSO2002B, {len(t)} Samples @ {(t[1]-t[0])*1000:.0f} ns", fontsize=7, color="gray")
    fig.tight_layout(rect=(0, 0.02, 1, 1))
    fig.savefig(os.path.join(OUT, fname), dpi=150); plt.close(fig)
    print("wrote", fname)


def draw_overview(cap, frames, fname, title):
    t, h, l, d = cap["t"], cap["canh"], cap["canl"], cap["diff"]
    lv = levels(d, h, l)
    fig, axs = plt.subplots(2, 1, figsize=(13, 7.2), sharex=True, gridspec_kw={"height_ratios": [2.6, 1.4]})
    ax = axs[0]
    ax.plot(t, h * 1000, color=C_H, lw=0.7, label="CAN_H"); ax.plot(t, l * 1000, color=C_L, lw=0.7, label="CAN_L")
    ax.set_ylabel("Voltage [mV]"); ax.grid(alpha=0.3); ax.legend(loc="upper right", fontsize=8)
    ax.set_title(title, loc="left", fontweight="bold")
    ax.set_ylim(lv["L_dom"] - 1300, lv["H_dom"] + 1300)
    ax = axs[1]
    ax.plot(t, d * 1000, color=C_D, lw=0.7); ax.set_ylabel("V_diff [mV]"); ax.grid(alpha=0.3); ax.set_xlabel("Time [µs]")
    prev_end = None
    for i, fr in enumerate(frames):
        dur = fr["nbits_raw"] * fr["bit_us"] if "nbits_raw" in fr else 200
        x0 = fr["t_sof"]
        is_req = fr.get("id", 0) & 0x780 == 0x600
        axs[0].axvspan(x0, x0 + dur, color="#ffe680" if is_req else "#d5f0d5", alpha=0.5)
        axs[1].axvspan(x0, x0 + dur, color="#ffe680" if is_req else "#d5f0d5", alpha=0.35)
        lab = describe(fr).replace(": ", ":\n", 1)
        y = lv["H_dom"] + 1150 if i % 2 == 0 else lv["L_dom"] - 1150
        axs[0].annotate(f"COB-ID 0x{fr.get('id', 0):03X}, DLC {fr.get('dlc', 0)}, {dur:.0f} µs\n" + lab, (x0 + dur / 2, y), ha="center", va="top" if i % 2 == 0 else "bottom", fontsize=7.5,
                        bbox=dict(boxstyle="round,pad=0.3", fc="#fff8dc" if is_req else "#eefaee", ec="gray", alpha=0.95))
        if prev_end is not None:
            gap = x0 - prev_end
            axs[1].annotate("", xy=(x0, lv["D_dom"] * 0.5), xytext=(prev_end, lv["D_dom"] * 0.5), arrowprops=dict(arrowstyle="<->", color="purple", lw=0.9))
            axs[1].text((x0 + prev_end) / 2, lv["D_dom"] * 0.55, f"Response time {gap:.0f} µs" if is_req is False else f"{gap:.0f} µs", ha="center", va="bottom", color="purple", fontsize=8)
        prev_end = x0 + dur
    fig.text(0.01, 0.025, f"Levels: CAN_H recessive {lv['H_rec']} mV / dominant {lv['H_dom']} mV, CAN_L recessive {lv['L_rec']} mV / dominant {lv['L_dom']} mV, V_diff dominant {lv['D_dom']} mV, recessive {lv['D_rec']} mV", fontsize=8)
    fig.text(0.01, 0.005, "Fountainer Rev.1 (TJA1051T/3) ↔ Raspberry Pi (MCP2515/MCP2562), 250 kbit/s, Tektronix MSO2002B; yellow = SDO request from the master, green = response/heartbeat/PDO from the slave", fontsize=7, color="gray")
    fig.tight_layout(rect=(0, 0.04, 1, 1)); fig.savefig(os.path.join(OUT, fname), dpi=150); plt.close(fig); print("wrote", fname)


idle = load("caps_idle.json"); loadc = load("caps_load.json")
decoded = []
for ci, cap in enumerate(idle):
    for (ta, fr) in find_frames(cap["t"], cap["diff"]):
        if "id" in fr:
            decoded.append((ci, fr)); print(f"idle cap {ci}: id 0x{fr['id']:03X} dlc {fr['dlc']} data {[hex(x) for x in fr['data']]} crc_ok {fr['crc_ok']} ack {fr['ack']} bit {fr['bit_us']:.3f} -> {describe(fr)}")
        else:
            print("idle cap", ci, "decode error", fr.get("error"))
seen = {}
for ci, fr in decoded:
    if fr["crc_ok"]:
        seen.setdefault(fr["id"] & 0x780, (ci, fr))
if 0x700 in seen:
    ci, fr = seen[0x700]
    draw_frame(idle[ci], fr, "can_01_heartbeat_frame.png", f"Complete CAN frame — {describe(fr)}")
    draw_frame(idle[ci], fr, "can_02_bit_level_sof_identifier.png", f"Bit level: SOF + Identifier — {describe(fr)}", zoom=(-1.5 * fr["bit_us"], 20 * fr["bit_us"]))
for fc, name in ((0x180, "can_03_tpdo1_frame.png"), (0x280, "can_03b_tpdo2_frame.png"), (0x380, "can_03c_tpdo3_frame.png"), (0x480, "can_03d_tpdo4_frame.png")):
    if fc in seen:
        ci, fr = seen[fc]; draw_frame(idle[ci], fr, name, f"Process data — {describe(fr)}")
# load captures: overview with SDO pairs
best = None
for ci, cap in enumerate(loadc):
    frs = []
    for (ta, fr) in find_frames(cap["t"], cap["diff"]):
        if "id" in fr:
            frs.append(fr); print(f"load cap {ci}: id 0x{fr['id']:03X} dlc {fr['dlc']} data {[hex(x) for x in fr['data']]} crc_ok {fr['crc_ok']} -> {describe(fr)}")
    ok = [f for f in frs if f["crc_ok"]]
    if best is None or len(ok) > len(best[1]):
        best = (ci, ok)
if best and best[1]:
    ci, frs = best
    draw_overview(loadc[ci], frs, "can_04_sdo_request_response.png", "SDO traffic under load (fcm bench): requests from the master and responses from the slave")
    req = [f for f in frs if f["id"] & 0x780 == 0x600]; rsp = [f for f in frs if f["id"] & 0x780 == 0x580]
    if req:
        draw_frame(loadc[ci], req[0], "can_05_sdo_request_frame.png", f"SDO request from the master — {describe(req[0])}")
    if rsp:
        draw_frame(loadc[ci], rsp[0], "can_06_sdo_response_frame.png", f"SDO response from the slave — {describe(rsp[0])}")
