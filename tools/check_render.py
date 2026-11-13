#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 JazzMasterT
"""Objective checks on rendered WAVs (docs/DESIGN.md section 1): level, limiter use, DC, clicks.
  .venv/bin/python tools/check_render.py refs/ours/*.wav"""
import sys
import numpy as np
from scipy.io import wavfile


def load(path):
    fs, x = wavfile.read(path)
    if x.dtype == np.int32:
        x = x.astype(np.float64) / 2**31
    elif x.dtype == np.int16:
        x = x.astype(np.float64) / 2**15
    elif x.dtype.kind == "u":                       # scipy reads 24-bit as int32 (left-aligned) in recent versions
        x = (x.astype(np.float64) - 128) / 128
    if x.ndim > 1:
        x = x.mean(axis=1)
    return fs, x


def db(v):
    return 20 * np.log10(max(float(v), 1e-12))


for path in sys.argv[1:]:
    fs, x = load(path)
    a = np.abs(x)
    over = np.mean(a > 0.8) * 100
    # clicks: a jump between samples far larger than the local slope allows (second difference outliers)
    d2 = np.abs(np.diff(x, 2))
    local = np.convolve(np.abs(np.diff(x)), np.ones(64) / 64, "same")[1:]
    clicks = np.flatnonzero((d2 > 0.05) & (d2 > 8 * (local + 1e-4)))
    print(f"{path.split('/')[-1]:28s} peak {db(a.max()):6.1f} dBFS  rms {db(np.sqrt(np.mean(x**2))):6.1f}  "
          f">0.8 (limiter) {over:5.2f}%  DC {x.mean()*1e3:+.2f}e-3  click candidates {len(clicks)}"
          + (f" at {', '.join(f'{c/fs:.3f}s' for c in clicks[:5])}" if len(clicks) else ""))
