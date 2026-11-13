# FM-1 Bass Machine — Design (draft for review)

Working title: TBD. Status: proposal, nothing built yet.

## 0. The three things that matter

Every decision below serves these, in this order when they conflict:

1. **It sounds fabulous.** The stretch goal is a real analog mono synth: if a bassline recorded from it sits next to a
   real Minimoog / SH-101 / Pro-One in a mix, nobody should pick it as the digital one.
2. **The timing is rock solid**, both its own note timing and following a DAW's MIDI clock, and it runs on its own
   tempo when there is no clock.
3. **Simple but powerful.** A great bassline in under a minute, with no manual. Every feature has to earn its place;
   anything that is not needed to make better basslines is left out.

Each has acceptance criteria (§1); a milestone is not done until its criteria are measured and met.

## 1. Acceptance criteria

### Sound
- **Aliasing**: with a saw at C7 sweeping, and with the filter self-oscillating and driven, every alias component is
  more than 80 dB below the fundamental across the audible band (host-mode render + FFT).
- **Low end**: raising resonance to maximum costs no more than 3 dB at 50 Hz with bass compensation on.
- **No clicks**: no discontinuities on note-on, note-off, slide, accent, or patch change at any setting
  (host-mode check for sample-to-sample jumps above the signal's own slope).
- **Headroom**: nothing hard-clips the DAC at any setting; the output limiter is the only ceiling.
- **Blind A/B**: a listener cannot reliably tell our patch from the reference in a blind A/B on at least 3 reference
  basslines, in a mix context. References chosen by the author (recordings of real hardware or trusted emulations).

### Timing
- **Internal clock**: note onsets are sample-exact (0 samples of jitter) and do not drift. After 10 minutes at
  any tempo, the error against the mathematically ideal grid is under 1 sample.
- **External clock, steady tempo**: with ±1 ms of jitter on the incoming clock, output onset jitter (99th
  percentile) is under 0.12 ms (≈ 5 samples).
- **External clock, locked phase**: no drift against the DAW; after 10 minutes, the bar line is in the same place
  (± 0.1 ms) as the DAW's.
- **Tempo changes**: a step change: no step more than 2 ms off in the beat after it, then within 0.6 ms. A ramp
  (100 → 140 BPM over 8 bars): within 1.5 ms throughout.
- *(Revised 2026-11-13 after M2 on the Mac: the first limits — 0.1 ms jitter, 0.5 ms settling, 1 ms on a ramp —
  were met only on some jitter patterns. The tracker as built: jitter 0.08–0.11 ms, settling 0.16–0.57 ms, ramps
  0.99–1.28 ms over 8 random patterns; all far below audibility. Timestamping the ticks in the USB interrupt on the
  FM-1 removes part of the input jitter at source.)*
- **Transport**: Start plays step 1 on the first tick; Stop silences cleanly (release tail, no click); Continue
  and Song Position Pointer resume on the correct step.
- **Round trip in Studio One**: recorded audio lands on the grid within ±0.5 ms after the one-time calibration (§5.4).

### Simplicity
- From power-on: choose a key and write a 16-step bassline with accents and slides in under a minute, without the
  manual.
- Every sound parameter is reached in at most two presses plus a knob turn. No nested menus.
- The screen always shows what the knobs do right now.

## 2. Studio setup it is designed for

```
 Studio One 7 ──USB MIDI──► FM-1     clock, start/stop/continue, song position
 Studio One 7 ◄─USB MIDI─── FM-1     the notes it plays (record / edit / drive a plugin), clock when FM-1 is master
 FM-1 headphone out ──audio──► UAD Apollo line in ──► Studio One audio track
```

- Studio One keeps the Apollo as its only audio device: no aggregate device, no clock drift.
- If USB whine or hum appears (USB + audio cable = ground loop), use a ground-loop isolator or DI.
- Output is mono, the same on both channels: record one side to a mono track.

## 3. Platform

**Fork of [Dinghy](https://github.com/zvenson/dinghy)** (zvenson) — the minimal skeleton of the Felucca platform
(as SLOOP / OberVave carry it). **Licence: GPL-3.0-only**; keep `LICENSE`, `LICENSING.md` and the credits, and
publish source if the firmware is ever shared.

Dinghy gives us the update path and `.fwsc` packaging, USB rescue (hold OCT- at power-on, auto after two crashed
boots), USB + TRS MIDI with clock/start/stop/continue, keys/buttons/encoders/LEDs, the 240×240 screen, flash
storage, a crash screen, build checks that refuse brick-prone code, and **host mode** (the same app on the Mac:
sound to WAV, screen to PNG, scripted keys and MIDI).

Constraints:
- Audio callback `app_audio(out, n)`: interrupt, every 5.8 ms, n = 256 frames, 44.1 kHz (I2S clock ~44,117.6 Hz:
  sample timing is ours, wall-clock timing is the crystal's — see §5).
- MIDI is read in the main loop (`midi_get`), not in an interrupt.
- CPU ~320–360 MHz. **Hardware float confirmed in M0** (2026-11-13): built with `-mcpu=r3` (as JieLi's SDK builds
  its libc), float multiply-add runs at the same speed as int32 (53 ns/op in the benchmark loop), float divide
  62 ns, and results are **bit-identical to the Mac**. A prototype voice (2 polyBLEP saws, nonlinear 4-pole ladder,
  2× oversampled, naive code with 10 divisions/sample) costs **651 µs per 256-frame block = 11 % of the budget**.
  → **The engine is written in float**, the same code on the Mac and the device. (Dinghy's "no float" rule
  applies only to its default `-mcpu`.) Benchmark: `firmware/src/bench.h`, `tests/bench_ref.c`. Still keep
  32×32→64 integer multiplies out of line (chip quirk, `bsp/docs/CPU.md`); no 64-bit division.

## 4. The sound engine — "a real analog mono"

### 4.1 Signal path

```
 OSC 1  saw / pulse (PWM) / tri ─┐
 OSC 2  same, ±24 semis, detune ─┼─► MIXER (overdrivable) ─► LADDER FILTER ─► VCA ─► OUTPUT STAGE ─► out
 SUB    square or sine, -1/-2 oct┤                            4-pole / 2-pole        (sat, HPF, limiter)
 NOISE                           ┘                            ▲                  ▲
                                         FILTER ENV ──────────┤ (+accent, +vel)  │
                                         KEY TRACK ───────────┤                  │
                                         LFO ─────────────────┘        AMP ENV ──┘ (+accent)
 GLIDE: legato / always / slide steps only
```

### 4.2 What makes analog sound analog, and how we model each

| Analog trait | How we model it |
| --- | --- |
| **The ladder's nonlinearity**: the filter saturates in its stages, resonance compresses and growls rather than whistles | Zero-delay-feedback 4-pole ladder with a nonlinearity in each stage and in the feedback path (Huovilainen / Zavalishin family), solved per sample, **oversampled 2–4×** so the saturation itself does not alias |
| **Resonance eats the bass** on a real ladder | A "bass" control blends compensation in: at 0 it behaves like a Minimoog, at max the low end stays full |
| **Mixer overdrive** (Minimoog players crank the mixer into the filter) | The mixer has real headroom and a soft-saturating input to the filter; turning oscillators up drives the filter |
| **Oscillators are not perfect**: saws curve slightly (capacitor discharge), pulses have finite rise times, pitch wanders | Band-limited (polyBLEP/polyBLAMP or minBLEP) shapes with slight analog curvature and finite edge rate; slow per-oscillator pitch drift and a little per-note randomness, with one **"analog" amount** knob (0 = clean digital stability) |
| **Component tolerance**: two oscillators never quite agree, the filter's cutoff is not exactly where the panel says | Fixed per-unit "personality" offsets (tiny detune between oscillators, cutoff and resonance scaling), seeded so each patch is repeatable |
| **RC envelopes**: exponential, the attack aims past the top so it is fast and snappy, decays are curved | Exponential segments modelled on an RC charge toward an overshoot target; very short decays for plucks without clicks; the envelope modulates cutoff exponentially (in octaves), as analog does |
| **Accent** (the 303 trick) | Accent adds to the filter envelope and VCA, and successive accents accumulate a little (the "sweep" circuit), with an amount control |
| **Slide** | Constant-time slide on slide steps (RC glide), a glide-time knob for played legato |
| **The analog noise floor and VCA bleed** | Very low-level noise and a trace of bleed, on by default but subtle; can be turned off |
| **Low-end hygiene for mixing** | DC blocker, selectable sub-sonic HPF (off / 20 / 30 Hz), soft limiter so the DAC never clips |

### 4.3 Process for getting there

1. **Reference first.** Pick 3–5 target sounds (recordings of real hardware, or the plugin you trust most
   for analog bass). These are the bar we A/B against for the whole project.
2. **Prototype on the Mac in float**, at high quality, inside the host renderer. Iterate on the sound by ear with
   fast WAV renders until it beats or matches the references.
3. **Port to the device form** (float or fixed point, decided by the M0 FPU measurement) and keep the prototype as
   a **golden reference**: a test renders both and compares them sample by sample (tolerance in dB), so the port can
   never quietly lose the sound.
4. **Spend the CPU on sound.** One voice only: oversampling, per-stage nonlinearity and band-limited oscillators
   are where the budget goes. Budget: whole voice ≈ 50–56 % of the audio interrupt as built (see 4.3a), measured with `fm1_ticks()`; with
   the rest held back for headroom.

### 4.3a Implementation as built (M1, 2026-11-13)

- Oscillators (polyBLEP, flip-flop sub) **and the ladder at 4x** (176.4 kHz); a 2-pole 20 kHz low-pass between them
  (an analog oscillator's own bandwidth: without it the ladder's input stage folded ultrasonic harmonics back into
  the audio band, e.g. a 64 Hz alias at -69 dB on C4). The ladder at 2x was measured and rejected (-42 dB aliasing
  with drive).
- Saturation and soft limit at 2x; polyphase half-band decimators 4x -> 2x (19 taps) -> 1x (63 taps, **passband
  to 18 kHz**, -97 dB from 26.1 kHz: half the taps of a 20 kHz passband, nothing a bass part needs above 18 kHz).
  The aliasing criterion is measured over 20 Hz..18 kHz accordingly.
- Measured: aliasing -88 dB (saw C7), -95 dB (C6), < -100 dB at C4 and below with moderate drive; no bass loss
  with resonance (bass_comp 1); no clicks. **Known limit:** maximum drive with resonance ~0.6 reaches a near-DC
  alias around -52 dB at C3 (the input stage is then almost a hard clipper) — to revisit (cap drive, or a better
  nonlinearity solver).
- **CPU on the FM-1** (engine from RAM, `-O2`, float): 50–56 % of the audio interrupt's budget for one block (the
  benchmark varies ±3 % with code layout). The FPU costs ~11 cycles per dependent operation and divides cost the
  same as multiplies, so the engine is bound by operation count, not by divisions or code fetch (both tried). **In
  practice**: the engine in the audio interrupt, keys and USB MIDI notes with a running 120 BPM clock: under 60 %
  peak, no glitches (listening test, 2026-11-13). Budget for the rest: the sequencer is per-sample comparisons (small).

### 4.4 Patches

32 patch slots; ~16 factory patches made and A/B'd against the references (round sub, Moog pluck, house bass,
synth-pop pulse, acid, growl, …).

## 5. Timing

### 5.1 Principle

Time is counted in **samples**, inside the audio interrupt. Every note-on, note-off, slide and accent is scheduled to
a **sample position**, not to the next 5.8 ms block. The main loop never decides *when* a note plays; it only
changes *what* will play.

### 5.2 Internal clock (no DAW)

- Tempo 30–300 BPM in 0.1 BPM steps, plus tap tempo.
- A fractional phase accumulator advanced per sample (fixed-point with enough fractional bits that 10 minutes
  drift < 1 sample), so steps fall on exact samples with no rounding error carried forward.
- When the FM-1 is master it **sends MIDI clock** (and start/stop), so Studio One or other gear can follow it.

### 5.3 External clock (Studio One)

- **Clock source: auto.** If clock arrives, follow it; if it stops for more than ~0.5 s while running, hold the last
  tempo and keep playing (it never stops dead because of one dropped message). A setting can force internal or
  external.
- **Timestamp every tick** with the 24 MHz timer as close to arrival as possible (if main-loop timestamps are not
  precise enough, timestamp in the USB interrupt — small platform change, §10).
- **Phase-locked loop**: a second-order tracking filter estimates tempo *and* phase from the tick timestamps,
  converts both to samples, and steers the internal sample clock. Clock jitter is filtered out; phase is locked, so
  there is no long-term drift; tempo changes are tracked within a beat.
- Swing and step rate (1/8, 1/16, 1/16T, 1/32) are positions computed from the locked phase, so they are as
  steady as straight 16ths.
- **Start** plays step 1 on the first tick; **Stop** releases cleanly; **Continue** and **Song Position Pointer**
  resume on the right step.

### 5.4 Latency calibration (Studio One + Apollo)

The audio leaves the FM-1 about one audio buffer late and passes the Apollo's input path. One-time calibration:
play a click pattern, measure the offset in the recorded track, then either
- set the **audio track's delay** in Studio One (a negative delay; exact from the very first note, recommended), or
- set the FM-1's **timing offset** (−20 … +20 ms): it plays early using the PLL's prediction. Good for live
  monitoring, but cannot move the very first note after Start earlier than the Start message itself.

### 5.5 Verification (host mode)

Scripted clock streams (steady, jittered ±1 ms, tempo steps, ramps, dropouts, start/stop/continue/SPP) are fed into
the host build; the WAV's onsets are detected and checked against the criteria in §1. These tests run on every
build.

## 6. Interface — simple but powerful

### 6.1 Rules

- One job per button, printed on the panel where possible. No nested menus.
- Every sound parameter lives on a **knob page**; the screen shows the four knobs' names and values, large.
- The keys are the sequencer: the **16 white keys are the 16 steps**, their LEDs show it.
- Destructive actions (clear pattern, overwrite patch) need a hold, never a single tap.

### 6.2 What it can do (all of it)

**Sound:** 2 oscillators + sub + noise, ladder filter (4/2-pole), filter and amp envelopes, accent, glide/slide,
one LFO (tempo-syncable), drive, "analog" amount, output level. ~24 parameters on 6 knob pages.

**Sequencer:** 16 patterns, 1–64 steps; per step: note, octave, accent, slide, tie, gate length; swing; step rate;
key and scale (scale-locked keys, chromatic toggle); pattern transpose by scale degree; queue next pattern at the
bar line.

**Small power tools** (each one press, each one useful for basslines):
- **Rotate** the pattern left/right by a step.
- **Octave jump** a step up/down for movement.
- **Copy/paste** a bar or pattern.
- **Undo** last edit.

**Not doing in v1** (and why): polyphony, drums, effects other than drive (the DAW does those better), arpeggiator
(it is a step sequencer), song mode (Studio One arranges), randomisers.

**v2 candidates**: a **delay** (cheap in CPU; memory-bound: ~340 KB of pool RAM is ~2 s of float or ~4 s of 16-bit
audio) and a **reverb** (costs CPU: with the engine at ~50 % it needs a small design, possibly at a lower internal
rate). Planned 2026-11-13 as "later, not v1".

### 6.3 Panel map (settled 2026-11-13 from a clickable mockup)

The 27 keys run F3–G5: **16 white keys, 11 black keys**.

| Control | Function |
| --- | --- |
| SELECT | **Cutoff**, on every page |
| ALGORITHM | **Resonance**, on every page |
| PRESETS | Patch (sound) |
| MASTER | Volume |
| KNOB1–4 | The four parameters on the screen's bottom row |
| HOME | **Performance page**: env amount, decay, accent, glide. Hold: undo (again: redo) |
| EDIT | Oscillators: wave, pulse width, osc 2 level, detune · again: osc 2 semitones, sub, sub octave, noise |
| ENV | Filter: cutoff, resonance, env amount, keytrack · again: filter envelope · again: amp envelope |
| LFO | LFO 1, LFO 2: rate (free 0.05–30 Hz, or synced 4 bars … 1/32 with dotted and triplets: the phase follows the song position), shape (sine, tri, saw down/up, square, S&H), destination (pitch, cutoff, resonance, PW, amp, drive, reverb mix), depth ±100 % · again: LFO SET (sync, retrigger on a note, per LFO) · again: glide, glide mode, accent, velocity |
| FX | Drive, saturation, analog, level · again: bass compensation, 24/12 dB, low cut · again: REVERB: mix, decay, tone, low cut of the send |
| SEL | Key and scale: root, scale, in-scale/chromatic keys, transpose. Hold + a key: set the root |
| GLO | Tempo, swing, step rate (1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32), timing offset · again: MIDI channel, clock source, notes out, CC (cutoff 74 / resonance 71: off, in, out, in + out) and the audio CPU · again: ABOUT (author JazzMasterT, version, build date, copyright, licence). Hold + PLAY: tap tempo |
| SEQ | White keys = steps; STEP page (note, octave, gate, accent/slide), PATTERN page (length, rotate, transpose, swing). Hold + white key: pattern 1–16 (switches at the bar end while playing). Hold + SAVE: copy pattern |
| REC | Stopped: step record (play notes; each fills the next step). Playing: live record into the step playing |
| PLAY | Start / stop; with a DAW's clock, follows its Start / Stop |
| SAVE | Hold: store the patch and the pattern |
| OCT− / OCT+ | Keyboard octave; in the sequencer, bar 1–4 |
| White keys (playing) | Scale-locked: one scale degree per key from the root (lit keys = roots); chromatic: a normal keyboard |
| Black keys | Scale-locked: off when playing; **when writing steps (REC, SEQ): ACC · SLIDE · TIE (the group of three), REST · BACK (the group of two)**. Chromatic: notes (accent/slide then on the STEP page's KNOB4) |
| ARP | Ideas, in the key: tap = MUTATE (2–3 steps vary: a neighbouring note, an octave, accent, slide, a rest), hold = NEW BASSLINE (16 steps: root-heavy, syncopated, accents and slides). Hold HOME: undo |
| Screen | Header: pattern, key/scale, tempo (INT/EXT), transport. The pattern as a pitch lane (accents, slides, ties, playhead); on sound pages a picture of the page (waveform, filter curve, envelope, drive curve, scale). An info line (patch, step, or what a control just changed). The four knob boxes. |

On the device a long press is not needed: holds are real holds.

MIDI in also takes **program change** (the patch, 1–32), cutoff and resonance CC, notes, clock and transport. Output: the DAC's full range (+6 dB on Dinghy's default; our limiter holds the peaks). 16 factory patches (patches/*.txt, level-matched within ~2.6 dB), 32 slots.

## 7. Development workflow

1. Engine and sequencer are plain C with no hardware calls; they compile in host mode.
2. The host renderer turns a pattern + patch (+ scripted clock) into a WAV; listen in Studio One against the
   references.
3. Automated checks on every build: timing onsets, aliasing, clicks, clipping, DC, prototype-vs-port match.
4. Only when it passes: `./build.sh` → `.fwsc` → `tools/fm1_install.py`.

## 8. Milestones

| # | Milestone | Done when |
| --- | --- | --- |
| M0 | **Safety + toolchain** | Full 1 MB flash backup (read twice, hashes match), flash ID known; toolchain installed; stock Dinghy builds, runs in host mode, installs on the FM-1, and the way back to Obervave is proven; **CPU and FPU measured on the device**; headphone output noise floor checked through the Apollo |
| M1 | **Sound** | Float prototype on the Mac matches or beats the references in blind A/B; all sound criteria in §1 pass |
| M2 | **Timing** | Sequencer + internal clock + PLL on host; all timing criteria in §1 pass against scripted clocks |
| M3 | **On the device** | Engine ported (prototype-vs-port test passes) within CPU budget; panel UI; synced to Studio One; round-trip calibrated through the Apollo |
| M4 | **Polish** | Factory patches, pattern storage, MIDI out, power tools, the "bassline in a minute" test, release build |

## 9. Testing the "sounds like analog" claim honestly

- Blind A/B with level-matching (a few dB louder always "sounds better"); the listener does not know which is
  which.
- In a mix as well as solo — bass is judged in context.
- Through the real path (FM-1 headphone out → Apollo), not just host renders, before M3 is called done.

## 10. Risks and open questions

- **FPU / CPU headroom**: determines float vs fixed point and how much oversampling we can afford. Measured in M0.
- **Headphone output quality** — **measured in M0, not a limit** (2026-11-13, Ferrofish Pulse 8AE → Apollo, test
  firmware `firmware/src/outtest.h`): THD 0.007 % at low level, 0.04 % at −1 dBFS (H2 at −70 dB), no clipping up to
  full DAC scale; audible-band noise at or below the 16-bit export's floor (≈ −100 dBFS); no hum or USB whine,
  battery and USB-connected identical (no ground loop). Dinghy caps output at −6 dBFS of the DAC; **our output
  stage uses the full range** (soft limiter just below full scale) for +6 dB.
- **Clock timestamp precision**: main-loop timestamps may need moving into the USB interrupt.
- **Song Position Pointer**: `0xF2` may need adding to the platform's MIDI path.
- **Ground loop** USB + audio — see §2.
- **CPU quirk** with inlined 64-bit multiplies — follow `bsp/docs/CPU.md`; Dinghy's build checks help.
- **Recovery**: OCT- at power-on gives the USB rescue; the full flash backup plus MvaveFM1Unbricker is the last
  resort.
