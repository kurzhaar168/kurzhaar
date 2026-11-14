#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 JazzMasterT
"""Timing tests (docs/DESIGN.md section 1, "Timing"): the clock and player (tests/clock_test.c) against scripted
clock streams. Exit 1 if a criterion fails.   .venv/bin/python tools/timing_tests.py [-v]

Tick timestamps model the FM-1: host jitter +-0.5 ms, the next 1 ms USB frame, then up to 0.5 ms until the 2 kHz
USB poll stamps it, and, while the engine renders (~55 % of the time), up to one render slice (~0.2 ms) more
(platform_midi_catchup). A constant mean delay is expected (the timing offset calibrates it): jitter and drift are
measured around it."""
import subprocess, sys, tempfile
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "host" / "clock_test"
SRCS = [ROOT / "tests" / "clock_test.c", ROOT / "firmware" / "engine" / "clk.c", ROOT / "firmware" / "engine" / "player.c",
        ROOT / "firmware" / "engine" / "clk.h", ROOT / "firmware" / "engine" / "player.h"]
FS = 44100.0
MS = FS / 1000.0
VERBOSE = "-v" in sys.argv
import os
rng = np.random.default_rng(int(os.environ.get("SEED", "1234")))
results = []


DEFS = [a for a in sys.argv[1:] if a.startswith("-D")]          # (a parameter sweep: -DA_SLOW=0.02 ...)
if DEFS:
    BIN = BIN.with_name("clock_test_sweep")


def build():
    if not DEFS and BIN.exists() and all(s.stat().st_mtime < BIN.stat().st_mtime for s in SRCS):
        return
    BIN.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-std=c99", "-Wall", "-Wextra", *DEFS, "-o", str(BIN), str(ROOT / "tests" / "clock_test.c")], check=True)


def run(lines, end_s, swing=0.5, lead=0.0):
    p = Path(tempfile.mkstemp(suffix=".txt")[1])
    hdr = [f"fs {FS}", "block 256", f"swing {swing}", f"lead {lead}", f"end {int(end_s * FS)}"]
    p.write_text("\n".join(hdr + [f"{t:.4f} {m}" for t, m in sorted(lines, key=lambda x: x[0])]) + "\n")
    out = subprocess.run([str(BIN), str(p)], check=True, capture_output=True, text=True).stdout
    steps = {}
    for ln in out.splitlines():
        w = ln.split()
        if w[0] == "step":
            steps.setdefault(int(w[1]), int(w[2]))
    return steps


def usb_stamp(t_true, jitter=True):
    """arrival timestamp (samples) of a tick sent at t_true (samples)"""
    if not jitter:
        return t_true
    t = t_true + rng.uniform(-0.5, 0.5) * MS                 # host scheduling
    t = np.ceil(t / MS) * MS                                 # the next 1 ms USB frame
    t += rng.uniform(0.0, 0.5) * MS                          # until the 2 kHz poll stamps it
    if rng.uniform() < 0.55:                                 # a render is running: the poll waits for its slice
        t += rng.uniform(0.0, 0.2) * MS
    return t


def ticks(tick_times, jitter=True):
    return [(usb_stamp(t, jitter), "tick") for t in tick_times]


def tick_grid(bpm_at, t0_s, n_ticks):
    """true tick times (samples) for a tempo function of the tick index"""
    t, out = t0_s * FS, []
    for k in range(n_ticks):
        out.append(t)
        t += FS * 60.0 / (bpm_at(k) * 24.0)
    return np.array(out)


def check(name, value, limit, unit="ms", better="lower"):
    ok = value <= limit if better == "lower" else value >= limit
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {value:.3f} {unit} (limit {'<=' if better == 'lower' else '>='} {limit} {unit})")


def residuals(steps, true_tick, first=0, last=None):
    ks = sorted(k for k in steps if k >= first and (last is None or k <= last) and 6 * k < len(true_tick))
    return np.array(ks), np.array([steps[k] - true_tick[6 * k] for k in ks]) / MS


build()

# ---- 1. internal clock: exact, no drift (10 minutes)
for bpm10 in (1200, 973, 1740):
    steps = run([(0, "play"), (1, f"bpm {bpm10}")] if False else [(0, f"bpm {bpm10}"), (0, "play")], 600.5)
    # play at the first block boundary after the messages: S = 256
    step_len = 15.0 * FS * 10.0 / bpm10
    ks = np.array(sorted(steps))
    err = np.array([steps[k] for k in ks]) - (256 + ks * step_len)
    check(f"internal {bpm10 / 10:.1f} BPM, 10 min: max error vs ideal grid", np.abs(err).max(), 1.0, "samples")

# ---- 2/3. external, steady 120 BPM with jitter: onset jitter and drift over 10 minutes
true = tick_grid(lambda k: 120.0, 0.5, 24 * 2 * 600)          # 10 minutes
ev = [(0.1 * FS, "start")] + ticks(true)
steps = run(ev, 600.0)
ks, r = residuals(steps, true, first=8)                         # after 2 beats of settling
dev = r - np.median(r)
check("external 120 BPM, +-1 ms jitter: onset jitter (99th percentile)", np.percentile(np.abs(dev), 99), 0.12)
print(f"info  constant latency {np.median(r):.2f} ms (calibrated by the timing offset), max deviation {np.abs(dev).max():.3f} ms")
first_min = r[(ks * 6 < 24 * 2 * 60) & (ks > 8)].mean()
last_min = r[ks * 6 > 24 * 2 * 540].mean()
check("external 120 BPM: drift over 10 minutes", abs(last_min - first_min), 0.1)

# ---- 4. tempo step 120 -> 140 at bar 9 (tick 768)
true = tick_grid(lambda k: 120.0 if k < 768 else 140.0, 0.5, 24 * 4 * 20)
steps = run([(0.1 * FS, "start")] + ticks(true), 0.5 + 20 * 4 * 60 / 125)
ks, r = residuals(steps, true, first=8)
lat = np.median(r[ks < 120])
after = r[ks * 6 >= 768] - lat
k_after = ks[ks * 6 >= 768]
beat1 = after[k_after * 6 < 768 + 24]
settled = after[k_after * 6 >= 768 + 24]
check("tempo step 120 -> 140: worst step in the beat after the change", np.abs(beat1).max(), 2.0)
check("tempo step 120 -> 140: from one beat after: worst step", np.abs(settled).max(), 0.6)

# ---- 5. tempo ramp 100 -> 140 BPM over 8 bars
n = 24 * 4 * 16
true = tick_grid(lambda k: 100.0 if k < 384 else min(140.0, 100.0 + 40.0 * (k - 384) / 768.0), 0.5, n)
steps = run([(0.1 * FS, "start")] + ticks(true), 0.5 + 16 * 4 * 60 / 110)
ks, r = residuals(steps, true, first=8)
lat = np.median(r[ks < 60])
check("tempo ramp 100 -> 140 over 8 bars: worst step", np.abs(r - lat).max(), 1.5)

# ---- 6. dropout: no ticks for 1 s while playing, then they return
true = tick_grid(lambda k: 120.0, 0.5, 24 * 2 * 20)
gap = (true > 5.0 * FS) & (true < 6.0 * FS)
steps = run([(0.1 * FS, "start")] + ticks(true[~gap]), 20.0)
ks, r = residuals(steps, true, first=8)
lat = np.median(r[ks < 30])
in_gap = (ks * 6 < len(true)) & (true[np.minimum(ks * 6, len(true) - 1)] > 5.0 * FS)
check("dropout of 1 s: steps keep playing (none missing)", float(len(set(range(8, ks.max() + 1)) - set(ks))), 0.0, "missing")
check("dropout of 1 s: worst step during and after", np.abs(r[in_gap] - lat).max(), 1.0)

# ---- 7. transport: Start, Stop, Continue from a Song Position
true = tick_grid(lambda k: 120.0, 0.5, 24 * 2 * 12)
t_stop = true[6 * 32] + 0.4 * 6 * (true[1] - true[0])          # stop during step 32 ...
t_cont = true[6 * 48]                                            # ... continue at song position 40 at tick 6*48
ev = [(0.1 * FS, "start")] + [(usb_stamp(t), "tick") for t in true]
ev += [(t_stop - 0.2 * MS, "stop"), (t_cont - 3 * MS, "spp 40"), (t_cont - 2 * MS, "cont")]
steps = run(ev, 12.0)
first = steps.get(0)
check("Start: step 0 plays within a block (+ USB) of the first tick", (first - true[0]) / MS if first else 99, 8.0)
check("Stop: no step after Stop until Continue", float(sum(1 for k in steps if 33 <= k < 40)), 0.0, "steps")
cont = steps.get(40)
check("Continue at song position 40: step 40 plays at the tick", abs((cont - t_cont) / MS) if cont else 99, 8.0)
nxt = steps.get(41)
check("after Continue: step 41 on the grid", abs((nxt - true[6 * 49]) / MS - 0.75) if nxt else 99, 1.0)

# ---- 8. swing and timing offset (internal clock, exact)
steps = run([(0, "bpm 1200"), (0, "play")], 5.0, swing=0.62, lead=4.0)
L = 15.0 * FS * 10.0 / 1200
e_even = steps[10] - (256 + 10 * L - 4.0 * MS)
e_odd = steps[11] - (256 + 11 * L + (0.62 - 0.5) * 2 * L - 4.0 * MS)
check("swing 62 % and 4 ms lead: placement error", max(abs(e_even), abs(e_odd)), 1.0, "samples")

# ---- 9. step rates: internal, exact (1/8T: 8 ticks, 1/32: 3, 1/4: 24); external 1/8 (12 ticks) on the tick grid
for tps, name in ((8, "1/8T"), (3, "1/32"), (24, "1/4")):
    steps = run([(0, "bpm 1200"), (0, f"rate {tps}"), (0, "play")], 60.5)
    L = 25.0 * FS * tps / 1200
    ks = np.array(sorted(steps))
    err = np.array([steps[k] for k in ks]) - (256 + ks * L)
    check(f"internal {name} at 120 BPM, 1 min: max error vs ideal grid", np.abs(err).max(), 1.0, "samples")
true = tick_grid(lambda k: 120.0, 0.5, 24 * 2 * 600)          # 10 minutes, as the 1/16 test (30 s measured mostly
steps = run([(0, "rate 12"), (0.1 * FS, "start")] + ticks(true), 600.0)   # the tracker settling, not its jitter)
ks = np.array(sorted(k for k in steps if k >= 4 and 12 * k < len(true)))
r = np.array([steps[k] - true[12 * k] for k in ks]) / MS
check("external 1/8 (12 ticks a step): jitter around the tick grid (99th percentile)", np.percentile(np.abs(r - np.median(r)), 99), 0.12)
# a rate change while playing: from 1/16 to 1/8 at 2 s, the steps after it on the 1/8 grid of the same timeline
steps = run([(0, "bpm 1200"), (0, "play"), (2.0 * FS, "rate 12")], 5.0)
L16, L8 = 25.0 * FS * 6 / 1200, 25.0 * FS * 12 / 1200
late = [steps[k] for k in sorted(steps) if steps[k] > 2.2 * FS]
off = [((t - 256) / L8) % 1.0 for t in late]
check("rate change 1/16 -> 1/8 while playing: later steps on the 1/8 grid (worst, steps)", max(min(o, 1 - o) for o in off), 0.01, "steps")

print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
