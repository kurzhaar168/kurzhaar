<p align="center"><img src="docs/img/splash.png" width="240" alt="Lucca: das Bassmaschine"></p>

# Lucca: das Bassmaschine

Firmware that turns the **M-VAVE FM-1** into a monophonic analog-style bass synth with a step sequencer. It is
meant to sit next to a DAW: it follows MIDI clock tightly or runs on its own tempo, and it takes notes, program
changes and filter automation over USB.

Named after Lucca, a German Shorthaired Pointer. By JazzMasterT. Built on [Dinghy](https://github.com/zvenson/dinghy)
and [Felucca](https://github.com/hugelton/Felucca). GPL-3.0.

<p align="center">
  <img src="docs/img/sequencer.png" width="240" alt="The sequencer, playing">
  <img src="docs/img/filter.png" width="240" alt="The filter page">
  <img src="docs/img/lfo.png" width="240" alt="The LFO page">
</p>

## The sound

- **Oscillators:** two oscillators (saw, pulse with PWM, triangle), a square or sine sub one or two octaves down,
  and noise.
  They run at 4× the sample rate and are band-limited, so high notes and fast pitch sweeps stay clean.
- **Filter:** a nonlinear 4-pole ladder (24 or 12 dB/oct) with self-oscillation, keytracking and bass
  compensation, driven from the mixer.
- **Envelopes and modulation:** filter and amp envelopes, accent, velocity, and glide (legato or always; a slide step
  always glides). Two **LFOs** run free or synced to the bar, with six shapes and seven destinations: pitch, cutoff,
  resonance, pulse width, amp, drive and reverb.
- **Effects:** drive, saturation, "analog" drift, and a reverb that keeps the low end dry.
- **Presets:** 16 factory patches, level-matched, from Classic Pluck and Acid Line to Reese, Wobble and Deep Dub.
  There are 32 patch slots in all.

## The sequencer

- 16 patterns of up to 4 bars × 16 steps, with accent, slide, tie and gate on each step.
- Entry: step record (stopped) or live record (playing), always in the scale you choose.
- Step rate 1/4 to 1/32 including triplets, swing, and a timing offset to line up with your DAW.
- Pattern changes wait for the end of the bar.
- **Ideas (ARP):** tap for a variation of the current pattern; hold for a new bassline in the key. Hold HOME to
  undo either.

## Controls

| Control | Function |
| --- | --- |
| SELECT / ALGORITHM | Cutoff / resonance, on every page |
| PRESETS | Patch |
| KNOB1–4 | The four parameters at the bottom of the screen |
| HOME | Performance page (env amount, decay, accent, glide). Hold: undo |
| EDIT | Oscillators (press again for page 2) |
| ENV | Filter, then the filter envelope, then the amp envelope |
| LFO | LFO 1, LFO 2, LFO settings (sync, retrigger), then glide and accent |
| FX | Drive and saturation, then filter options, then reverb |
| SEL | Key and scale. Hold + a key: set the root |
| GLO | Tempo, swing, step rate; MIDI settings; ABOUT. Hold + PLAY: tap tempo |
| SEQ | White keys = steps. Hold + a white key: pattern 1–16 |
| REC / PLAY | Record / start and stop |
| SAVE | Hold: store the patch and the pattern |
| OCT− / OCT+ | Keyboard octave; in the sequencer, bar 1–4 |
| ARP | Tap: mutate the pattern. Hold: a new bassline |
| Black keys | While writing steps: ACC · SLIDE · TIE and REST · BACK |

The full map is in [docs/DESIGN.md](docs/DESIGN.md#63-panel-map-settled-2026-11-13-from-a-clickable-mockup).

## With a DAW

Connect the FM-1 over USB. It appears as a MIDI port named **Felucca** (the name comes from the platform).

- **Clock:** GLO › MIDI › CLOCK set to AUTO follows the DAW's clock and its start and stop; with no clock coming in,
  Lucca runs on its own tempo.
- **Notes:** play Lucca from the DAW on the MIDI channel set in GLO (OMNI by default). NOTES OUT sends the
  sequencer's notes back, so you can record them.
- **Automation:** CC 74 is cutoff and CC 71 is resonance, received and/or sent (GLO › MIDI › CC).
- **Patches:** a program change (1–32) selects the patch.
- **Audio:** take the headphone output into your interface. To compensate for the round trip, adjust the timing
  offset (GLO).

## Install

Download `lucca-X.Y.fwsc` from the [releases](../../releases), then:

```sh
pip3 install mido python-rtmidi
python3 tools/fm1_install.py lucca-1.0.fwsc      # over USB; asks before it writes
```

**Installing firmware is at your own risk.** To return to another firmware, or to the official one, use the
[FM-1 Firmware Hub](https://thefirmwarehub.com) in Chrome or Edge. If Lucca will not start, hold **OCT−** while
switching on: the FM-1 starts in the USB rescue, where any firmware can be installed.

## Build

The setup is Dinghy's: Python 3, the JieLi toolchain and three files of the AC79 SDK. See
[docs/DINGHY.md](docs/DINGHY.md#2-set-up-once). Then:

```sh
./build.sh                          # -> build/lucca.fwsc
./build.sh --release 1.0            # -> build/lucca-1.0.fwsc
tests/run_host.sh tests/scripts/bass_tour.txt   # Lucca on your computer: screens (PNG) and sound (WAV) in build/host
sh tests/run_lucca_tests.sh         # the sequencer, the sound engine, the clock (needs numpy and scipy)
```

To render the patches to WAV, use `tools/render.py`. The patches themselves are plain text in `patches/`; after
editing one, run `tools/gen_patches.py`.

## License

GPL-3.0-only. Anyone who gets this firmware gets its source too. Credits and third-party parts are in
[LICENSING.md](LICENSING.md).

Lucca (the app, the sound engine, the sequencer, the clock, the patches) is Copyright (C) 2026 JazzMasterT. The
platform underneath is Felucca's, by Leo Kuroshita (Hügelton Instruments), with the work of SLOOP (isod89), the
SMK-37 layer (M. Geisler) and Dinghy (zvenson). "M-VAVE" and "FM-1" belong to their owners; Lucca is not made or
endorsed by them.
