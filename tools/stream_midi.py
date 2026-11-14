#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 JazzMasterT
"""Play a MIDI file to the FM-1 over USB with a running MIDI clock (24 per quarter) and Start/Stop, the way a DAW
would. Load and timing test for the engine. .venv/bin/python tools/stream_midi.py [file.mid] [--loops N] [--port P]"""
import argparse, time
import mido

ap = argparse.ArgumentParser()
ap.add_argument("midi", nargs="?", default="refs/midi/00_all.mid")
ap.add_argument("--loops", type=int, default=2)
ap.add_argument("--port", default="Kurzhaar")
ap.add_argument("--bpm", type=float, default=120.0)
ap.add_argument("--clock-only", type=float, metavar="SECONDS", help="no notes: Start, clock for SECONDS, Stop")
ap.add_argument("--bpm-to", type=float, help="with --clock-only: ramp the tempo to this by the end")
a = ap.parse_args()

if a.clock_only:                                   # a DAW's transport and clock, nothing else
    with mido.open_output(a.port) as out:
        out.send(mido.Message("start"))
        t0 = time.perf_counter(); t = 0.0
        while t < a.clock_only:
            bpm = a.bpm if not a.bpm_to else a.bpm + (a.bpm_to - a.bpm) * t / a.clock_only
            out.send(mido.Message("clock"))
            t += 60.0 / bpm / 24.0
            while time.perf_counter() - t0 < t:
                time.sleep(0.0005)
        out.send(mido.Message("stop"))
    print("done"); raise SystemExit

mf = mido.MidiFile(a.midi)
events, t = [], 0.0
for m in mf:                                       # seconds (the file is at 120 BPM)
    t += m.time
    if m.type in ("note_on", "note_off"):
        events.append((t, m))
length = t
tick = 60.0 / a.bpm / 24.0
with mido.open_output(a.port) as out:
    out.send(mido.Message("start"))
    t0 = time.perf_counter()
    for loop in range(a.loops):
        base = loop * length
        print(f"loop {loop + 1}/{a.loops}", flush=True)
        q = [(base + et, m) for et, m in events]
        next_clock = base
        i = 0
        while i < len(q) or next_clock < base + length:
            now = time.perf_counter() - t0
            due = min(q[i][0] if i < len(q) else 1e9, next_clock if next_clock < base + length else 1e9)
            if due > now:
                time.sleep(min(due - now, 0.002))
                continue
            if i < len(q) and q[i][0] <= now:
                out.send(q[i][1]); i += 1
            if next_clock <= now and next_clock < base + length:
                out.send(mido.Message("clock")); next_clock += tick
    out.send(mido.Message("stop"))
    for n in range(128):
        out.send(mido.Message("note_off", note=n))
print("done")
