# Reference kit: Diva vs our engine

Target: get our bass voice as close as possible to **u-he Diva** (Triple VCO + Ladder, the Minimoog-style
path). The same MIDI goes through Diva and through our engine's host renderer; we compare them level-matched,
by ear and by analysis.

## The MIDI

`midi/00_all.mid` (26 s, 120 BPM) = these four sections with a bar of silence between them:

| File | Tests |
| --- | --- |
| `01_bassline.mid` | A 2-bar 16th-note bassline in A minor: accents (velocity 127 vs 90), octave jumps, two slides (overlapping notes) |
| `02_long_notes.mid` | A1, A2, A3 held two beats each: oscillator and filter character across the range, decay, sustain |
| `03_plucks.mid` | Short staccato 8ths: envelope snap, clean note starts |
| `04_accent_legato.mid` | One note repeated with alternating accents, then a legato run (glide) |

Regenerate with `.venv/bin/python tools/make_ref_midi.py`.

## The three Diva patches

Start each from Diva's **init patch**. Settings below are the intent with suggested values; exact values do
not matter because you will **save each patch** and the saved preset file tells me exactly what you set.

Common to all three:
- **Main**: voice mode **Mono**, **Accuracy: Divine** (offline render quality), glide off unless stated.
- **Oscillator model: Triple VCO**. **Filter model: Ladder** (24 dB). **Envelope model: Analogue**.
- **Effects off** (no chorus, delay, reverb). Keep the output well below clipping.
- **Velocity → filter envelope amount** moderate, so the accents (velocity 127) are clearly brighter than the
  normal notes (velocity 90).

| Patch | Name to save | Intent |
| --- | --- | --- |
| P1 | `FM1REF P1 Moog Pluck` | OSC 1 saw only. Cutoff low (A1 sounds round, not buzzy); resonance ~25 %; filter env: attack 0, short decay, sustain 0, moderate amount; amp env: attack 0, sustain full, short release. The classic Minimoog bass pluck. |
| P2 | `FM1REF P2 Fat Two-Osc` | OSC 1 saw + OSC 2 saw slightly detuned + OSC 3 square one octave down. Mixer levels high enough to drive the filter. Cutoff mid, resonance 0, gentle filter env. Thick and warm. |
| P3 | `FM1REF P3 Resonant Squelch` | OSC 1 square/pulse. Resonance ~70 %, high filter env amount, medium decay. Voice mode **Legato** with a short glide (~50–100 ms). Tests resonance, its effect on the low end, and glide. |

## Rendering (Studio One)

1. Instrument track with Diva, drag `midi/00_all.mid` onto it at bar 1.
2. Song sample rate **44.1 kHz** (the FM-1's rate), export / bounce the track: **WAV, 24-bit, no normalising, no
   dither, no master plugins**. Include a bar of tail after the last note.
3. One file per patch: `refs/diva/P1.wav`, `refs/diva/P2.wav`, `refs/diva/P3.wav`.
4. Save each patch in Diva and copy the preset files (`.h2p`, usually in
   `~/Music/u-he/Diva/Presets/User/` or `~/Library/Audio/Presets/u-he/Diva/User/`) to `refs/diva/`.

The WAVs stay out of git (large, and Diva's output); the MIDI, this sheet and the preset files can go in.
