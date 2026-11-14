#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 JazzMasterT
# Kurzhaar's tests on this computer (no hardware): the sequencer, the sound engine, the clock and the app.
#   sh tests/run_kurzhaar_tests.sh          (Python 3 with numpy and scipy; any C compiler as cc)
set -e
cd "$(dirname "$0")/.."
PY="${PYTHON:-python3}"
mkdir -p build/host
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

cc -std=c99 -O2 -Wall -Wextra -o build/host/seq_test tests/seq_test.c
run "sequencer: notes in every key and scale, the keyboard, new basslines and mutations" build/host/seq_test
run "sound engine: tuning, aliasing, the filter, the LFOs, level" "$PY" tools/engine_tests.py
run "clock: the internal tempo, MIDI clock with jitter, step rates, start/stop" "$PY" tools/timing_tests.py
run "the app on the host: the pages, ideas, a program change" env DINGHY_APP=bass sh tests/run_host.sh tests/scripts/bass_ideas.txt
run "the app on the host: the LFO, clock, MIDI and ABOUT pages" env DINGHY_APP=bass sh tests/run_host.sh tests/scripts/bass_pages.txt

[ $fail = 0 ] && echo "KURZHAAR TESTS PASSED" || { echo "KURZHAAR TESTS FAILED"; exit 1; }
