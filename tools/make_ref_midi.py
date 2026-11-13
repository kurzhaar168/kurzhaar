#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write the reference test patterns (refs/midi/*.mid) that are rendered through Diva and through our engine.
120 BPM, 480 PPQ, channel 1. Run: .venv/bin/python tools/make_ref_midi.py"""
import mido
from pathlib import Path

PPQ, BPM = 480, 120
S16 = PPQ // 4                                     # a 16th note
OUT = Path(__file__).resolve().parent.parent / "refs" / "midi"


def write(name, notes, bars):
    """notes: (start_16th, length_16ths, midi_note, velocity); lengths > 1 step that overlap the next note = legato/slide"""
    ev = []
    for st, ln, n, v in notes:
        ev.append((st * S16, 1, n, v))
        ev.append((st * S16 + int(ln * S16), 0, n, 0))
    ev.sort(key=lambda e: (e[0], e[1]))            # note-offs before note-ons at the same tick
    mid = mido.MidiFile(ticks_per_beat=PPQ)
    tr = mido.MidiTrack(); mid.tracks.append(tr)
    tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(BPM), time=0))
    tr.append(mido.MetaMessage("track_name", name=name, time=0))
    t = 0
    for tick, on, n, v in ev:
        tr.append(mido.Message("note_on" if on else "note_off", note=n, velocity=v if on else 0, time=tick - t))
        t = tick
    tr.append(mido.MetaMessage("end_of_track", time=bars * 16 * S16 - t))
    OUT.mkdir(parents=True, exist_ok=True)
    mid.save(OUT / f"{name}.mid")
    print("wrote", OUT / f"{name}.mid")


A1, C2, D2, E2, G1, A2, A3 = 33, 36, 38, 40, 31, 45, 57
N, ACC = 90, 127                                   # normal and accented velocity

# 1. bassline: two bars of 16ths in A minor with accents, octave jumps, a rest, and two slides (overlaps)
line = [(0, .5, A1, ACC), (2, .5, A1, N), (3, .5, A2, N), (4, .5, A1, N), (6, 1.0, C2, ACC), (7, .5, D2, N),
        (8, .5, A1, N), (10, .5, A2, ACC), (11, 1.5, G1, N), (12, .5, A1, N), (14, .5, E2, N), (15, .5, D2, N)]
bar2 = [(s + 16, l, n, v) for s, l, n, v in line[:10]] + [(27, 1.6, E2, N), (28, 1.0, G1, ACC), (30, .5, A1, N)]
write("01_bassline", line + bar2, 2)

# 2. long notes across the range: A1, A2, A3 (oscillator + filter character, decay and sustain)
write("02_long_notes", [(0, 8, A1, N), (16, 8, A2, N), (32, 8, A3, N)], 3)

# 3. plucks: short staccato notes, every 8th, rising and falling (envelope snap, click-free starts)
pl = [A1, A1, C2, A1, E2, A1, D2, C2]
write("03_plucks", [(i * 2, .25, n, N) for i, n in enumerate(pl * 2)], 2)

# 4. accents and legato: same note repeated with alternating velocity, then a legato run (glide)
acc = [(i, .5, A1, ACC if i % 4 == 0 else N) for i in range(16)]
leg = [(16, 2.2, A1, N), (18, 2.2, C2, N), (20, 2.2, E2, N), (22, 2.2, A2, N), (24, 2.2, G1, N), (26, 4, A1, N)]
write("04_accent_legato", acc + leg, 2)


# 0. everything in one file, one bar of silence between sections: one render per patch
def combine():
    out = mido.MidiFile(ticks_per_beat=PPQ)
    tr = mido.MidiTrack(); out.tracks.append(tr)
    tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(BPM), time=0))
    tr.append(mido.MetaMessage("track_name", name="00_all", time=0))
    carry = 0
    for f in sorted(OUT.glob("0[1-9]_*.mid")):
        for m in mido.MidiFile(f).tracks[0]:
            if m.is_meta and m.type in ("set_tempo", "track_name"):
                carry += m.time; continue
            if m.is_meta and m.type == "end_of_track":
                carry += m.time + 16 * S16           # + one bar of silence
                continue
            tr.append(m.copy(time=m.time + carry)); carry = 0
    tr.append(mido.MetaMessage("end_of_track", time=carry))
    out.save(OUT / "00_all.mid")
    print("wrote", OUT / "00_all.mid", f"({out.length:.1f} s)")


combine()
