#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Objective tests of the bass engine against docs/DESIGN.md section 1 (sound criteria).
  .venv/bin/python tools/engine_tests.py        (exit 1 if a criterion fails)"""
import subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import get_window

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from render import build, BIN, FS                  # noqa: E402

TMP = Path(tempfile.mkdtemp(prefix="be-tests-"))
CLEAN = dict(analog=0, sat=0, drive=0, keytrack=0, fenv_amt=0, a_a=0.001, a_d=1, a_s=1, a_r=0.05, vel_sens=0,
             accent=0, hpf=0, res=0, cutoff=20000)


def render(name, patch, notes, length_s):
    """notes: (start_s, len_s, note, vel)"""
    p = TMP / f"{name}.txt"
    p.write_text("".join(f"{k} = {v}\n" for k, v in patch.items()))
    ev = []
    for st, ln, n, v in notes:
        ev += [(int(st * FS), f"on {n} {v}"), (int((st + ln) * FS), f"off {n}")]
    e = TMP / f"{name}.events"
    e.write_text("".join(f"{a} {b}\n" for a, b in sorted(ev, key=lambda t: t[0])) + f"{int(length_s * FS)} end\n")
    w = TMP / f"{name}.wav"
    subprocess.run([str(BIN), str(p), str(e), str(w)], check=True, capture_output=True)
    fs, x = wavfile.read(w)
    return x.astype(np.float64) / 2**31 if x.dtype == np.int32 else x.astype(np.float64)


def spectrum(x):
    w = get_window("blackmanharris", len(x))
    X = np.abs(np.fft.rfft(x * w)) / (np.sum(w) / 2)
    return np.fft.rfftfreq(len(x), 1 / FS), X


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-15))


results = []


def check(name, value, limit, better="lower", unit="dB"):
    ok = value <= limit if better == "lower" else value >= limit
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {value:.1f} {unit} (limit {'<=' if better == 'lower' else '>='} {limit} {unit})")


def aliasing(note, extra=None, label=""):
    patch = {**CLEAN, **(extra or {})}
    x = render(f"alias{note}", patch, [(0.0, 2.9, note, 100)], 3.0)[int(1.0 * FS):int(2.5 * FS)]
    f, X = spectrum(x)
    f0 = 440 * 2 ** ((note - 69) / 12)
    fund = X[np.abs(f - f0) < 20].max()
    harm = np.zeros_like(f, dtype=bool)
    for k in range(1, int(22050 / f0) + 2):
        harm |= np.abs(f - k * f0) < 12          # the harmonics and their window skirts
    band = (f > 20) & (f < 18000) & ~harm     # the decimator keeps 0..18 kHz clean (docs/DESIGN.md)
    i = int(np.argmax(X * band))
    worst = db(X[i] / fund)
    check(f"aliasing, {label or note} ({f0:.0f} Hz), worst non-harmonic vs fundamental (at {f[i]:.0f} Hz)", worst, -80)


build()
print(f"(renders in {TMP})")

# 1. aliasing: a high saw, filter fully open, nothing nonlinear
aliasing(96, label="saw C7")
aliasing(84, label="saw C6")
aliasing(96, {"osc1_wave": 1, "osc1_pw": 0.3}, label="pulse 30% C7")

# 2. bass loss with resonance: the 55 Hz harmonic of an A0 saw, resonance 0 vs 1, compensation on (1.0)
lv = {}
for res in (0.0, 1.0):
    x = render(f"bass{res}", {**CLEAN, "cutoff": 300, "res": res, "bass_comp": 1.0, "drive": 0.3},
               [(0.0, 2.9, 21, 100)], 3.0)[int(1.0 * FS):int(2.5 * FS)]
    f, X = spectrum(x)
    lv[res] = db(X[np.abs(f - 55.0) < 3].max())
check("bass loss at 55 Hz, resonance 0 -> max (bass_comp 1)", lv[0.0] - lv[1.0], 3.0)
lvh = {}
for res in (0.0, 1.0):
    x = render(f"bassh{res}", {**CLEAN, "cutoff": 300, "res": res, "bass_comp": 0.5, "drive": 0.3},
               [(0.0, 2.9, 21, 100)], 3.0)[int(1.0 * FS):int(2.5 * FS)]
    f, X = spectrum(x)
    lvh[res] = db(X[np.abs(f - 55.0) < 3].max())
print(f"info  bass loss with bass_comp 0.5: {lvh[0.0] - lvh[1.0]:.1f} dB")

# 3. clicks: fastest envelopes, a low note through a fairly open filter; the biggest sample-to-sample step
#    within 2 ms of each note on/off, against the biggest step while the note sustains
notes = [(0.1 + i * 0.25, 0.12, 33 + (i % 3) * 7, 100) for i in range(8)]
x = render("clicks", {**CLEAN, "cutoff": 2000, "a_a": 0, "a_r": 0, "a_d": 0.2, "a_s": 0.8, "drive": 0.3},
           notes, 2.3)
step = np.abs(np.diff(x))
edges = np.zeros_like(step, dtype=bool)
for st, ln, _, _ in notes:
    for t in (st, st + ln):
        a = int(t * FS)
        edges[max(a - 88, 0):a + 88] = True
sustain = np.zeros_like(step, dtype=bool)
for st, ln, _, _ in notes:
    sustain[int((st + 0.03) * FS):int((st + ln - 0.01) * FS)] = True
ratio = step[edges].max() / step[sustain].max()
check("note on/off step vs sustained waveform step (1.0 = no click)", ratio, 1.05, unit="x")

print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
