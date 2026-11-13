#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 JazzMasterT
"""Render MIDI through the bass engine on the Mac (tests/render.c): one WAV per patch.
  .venv/bin/python tools/render.py [--midi refs/midi/00_all.mid] [patches/P1.txt ...]   -> refs/ours/<patch>.wav
With no patches given, renders every patches/*.txt. Rebuilds build/host/render when its sources changed."""
import argparse, subprocess, sys
from pathlib import Path
import mido

ROOT = Path(__file__).resolve().parent.parent
FS = 44100
BIN = ROOT / "build" / "host" / "render"
SRCS = [ROOT / "tests" / "render.c", *sorted((ROOT / "firmware" / "engine").glob("*.[ch]"))]


def build():
    if BIN.exists() and all(s.stat().st_mtime < BIN.stat().st_mtime for s in SRCS):
        return
    BIN.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-std=c99", "-Wall", "-Wextra", "-Wno-unused-function", "-o", str(BIN),
                    str(ROOT / "tests" / "render.c")], check=True)


def events(midi_path, tail_s=1.5):
    lines, t = [], 0.0
    for msg in mido.MidiFile(midi_path):          # iterating a MidiFile yields delta times in seconds
        t += msg.time
        at = int(round(t * FS))
        if msg.type == "note_on" and msg.velocity:
            lines.append(f"{at} on {msg.note} {msg.velocity}")
        elif msg.type in ("note_off", "note_on"):
            lines.append(f"{at} off {msg.note}")
    lines.append(f"{int(round((t + tail_s) * FS))} end")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--midi", default=str(ROOT / "refs" / "midi" / "00_all.mid"))
    ap.add_argument("--out", default=str(ROOT / "refs" / "ours"))
    ap.add_argument("patches", nargs="*")
    a = ap.parse_args()
    build()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    ev = out / (Path(a.midi).stem + ".events")
    ev.write_text(events(a.midi))
    patches = [Path(p) for p in a.patches] or sorted((ROOT / "patches").glob("*.txt"))
    for p in patches:
        subprocess.run([str(BIN), str(p), str(ev), str(out / (p.stem + ".wav"))], check=True)


if __name__ == "__main__":
    sys.exit(main())
