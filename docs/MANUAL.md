# Kurzhaar, der Basshund: the manual

Kurzhaar turns the M-VAVE FM-1 into a monophonic bass synth with a step sequencer. This manual covers version 1.0.
For installing, see the [README](../README.md#install).

1. [The panel at a glance](#1-the-panel-at-a-glance)
2. [The screen](#2-the-screen)
3. [Playing and patches](#3-playing-and-patches)
4. [The sound pages](#4-the-sound-pages)
5. [Key and scale](#5-key-and-scale)
6. [The sequencer](#6-the-sequencer)
7. [Ideas: new basslines at a button](#7-ideas-new-basslines-at-a-button)
8. [Tempo, clock and MIDI (GLO)](#8-tempo-clock-and-midi-glo)
9. [With a DAW](#9-with-a-daw)
10. [Saving](#10-saving)
11. [The factory patches and patterns](#11-the-factory-patches-and-patterns)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. The panel at a glance

| Control | Tap | Hold |
| --- | --- | --- |
| **SELECT** (encoder) | Cutoff, on every page | |
| **ALGORITHM** (encoder) | Resonance, on every page | |
| **PRESETS** (encoder) | Choose the patch (1–32) | |
| **KNOB1–4** | The four values at the bottom of the screen | |
| **HOME** | Performance page | Undo the last pattern change |
| **EDIT** | Oscillators; again: mixer | |
| **ENV** | Filter; again: filter envelope; again: amp envelope | |
| **LFO** | LFO 1, LFO 2, LFO settings, motion (glide, accent) | |
| **FX** | Drive; again: filter tone; again: reverb | |
| **SEL** | Key and scale | + a key: set the root |
| **GLO** | Clock; again: MIDI; again: about | + PLAY: tap tempo |
| **SEQ** | The sequencer: step page / pattern page | + a white key: choose pattern 1–16 |
| **SEQ + SAVE** | | Copy the pattern |
| **SEQ + HOME** | | Paste the pattern |
| **PLAY** | Start / stop | |
| **REC** | Record on / off | |
| **SAVE** | | Store everything |
| **ARP** | Mutate the pattern | Write a new bassline |
| **OCT− / OCT+** | Keyboard octave (−2 to +2); in the sequencer: bar 1–4 | OCT− at power-on: USB rescue |

Pressing a page button again steps through its pages. A button you hold for about half a second does its "hold" job
instead of its tap job.

## 2. The screen

- **Header:** the pattern number (P01–P16), the key and scale, the tempo, and the transport (playing, stopped, or
  following the DAW).
- **The middle:** the pattern as a pitch lane: each step is a bar at its pitch. Accents have a red mark, slides
  draw a line to the next note, tied steps are drawn long and darker, and the note playing is white. On the sound
  pages, a picture of the page: the waveform, the filter's response, the envelope shape, and so on.
- **Info line:** the patch number and name, or what a control just changed.
- **Knob boxes:** the four values KNOB1–4 change, with a bar for where each one sits.

## 3. Playing and patches

The 27 keys play Kurzhaar like any monosynth: the last key pressed sounds. OCT− and OCT+ shift the keyboard by
octaves; their lights show when you are off the middle.

Turn **PRESETS** to choose a patch. 1–16 are the factory patches (see [section 11](#11-the-factory-patches-and-patterns));
17–32 start as a plain saw for your own sounds. Changes you make stay with the patch while the FM-1 is on; hold
**SAVE** to keep them.

**SELECT** (cutoff) and **ALGORITHM** (resonance) work on every page. Turn them slowly for fine steps, quickly to
sweep.

By default the white keys play the notes of the scale (see [section 5](#5-key-and-scale)), so you cannot hit a wrong
note. Set KEYS to CHROM on the SEL page for a normal keyboard.

## 4. The sound pages

The signal path: two oscillators, a sub and noise → the mixer → the 4-pole ladder filter → the amp → drive and
saturation → reverb.

### HOME: PERFORM

The four controls you reach for most while a pattern plays.

| Knob | Value | What it does |
| --- | --- | --- |
| 1 | ENV AMT | How far the filter envelope opens the filter, in octaves (−4 to +8) |
| 2 | DECAY | The filter envelope's decay: short = pluck, long = sweep |
| 3 | ACCENT | How much harder accented steps hit (louder and brighter) |
| 4 | GLIDE | Glide time (OFF to 1 s) |

### EDIT: OSC and MIX

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- |
| OSC | WAVE: saw, pulse, triangle | PW: pulse width (5–95 %) | OSC 2: its level | DETUNE: osc 2, ±50 cents |
| MIX | SEMI: osc 2, ±24 semitones | SUB: its level | SUB OCT: −1 or −2 octaves | NOISE: its level |

The oscillators run at four times the sample rate and are band-limited, so high notes and fast sweeps stay clean.

### ENV: FILTER, FILT ENV and AMP ENV

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- |
| FILTER | CUTOFF (20 Hz–20 kHz) | RESO | ENV AMT | KEYTRK: the cutoff follows the note |
| FILT ENV | ATTACK | DECAY | SUSTAIN | RELEASE |
| AMP ENV | ATTACK | DECAY | SUSTAIN | RELEASE |

The FILTER page draws the filter's real response: the curve, the 100 Hz / 1 kHz / 10 kHz marks, and a shaded band
for how far the envelope sweeps it. Turn the resonance up far enough and the filter rings on its own
(self-oscillation).

### LFO: LFO 1, LFO 2, LFO SET and MOTION

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- |
| LFO 1 / LFO 2 | RATE | SHAPE | TO: the destination | DEPTH (±100 %) |
| LFO SET | 1 SYNC | 1 TRIG | 2 SYNC | 2 TRIG |
| MOTION | GLIDE | G.MODE | ACCENT | VELO |

- **SHAPE:** sine, triangle, saw down, saw up, square, S&H (random steps).
- **TO:** off, pitch, cutoff, resonance, pulse width, amp, drive, reverb.
- **SYNC:** FREE runs in Hz; TEMPO locks the LFO to the tempo, and RATE then goes from 4 bars down to 1/32
  (with dotted and triplet values).
- **TRIG:** FREE keeps running; NOTE starts the LFO again at every note.
- **G.MODE:** glide OFF, LEGATO (only between overlapping notes, and on slide steps), or ALWAYS.
- **VELO:** how much key or MIDI velocity changes the sound.

### FX: DRIVE, TONE and REVERB

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- |
| DRIVE | DRIVE: into the filter | SAT: saturation after it | ANALOG: slow drift, like old hardware | LEVEL: the patch's volume (−24 to +6 dB) |
| TONE | BASSCMP: keeps the lows when the resonance is up | SLOPE: 24 or 12 dB/oct | LOWCUT: off, 20 Hz or 30 Hz | |
| REVERB | MIX | DECAY (0.3–8 s) | TONE: dark to bright | LOWCUT: the lows stay dry below this |

## 5. Key and scale

Press **SEL**.

| Knob | Value | What it does |
| --- | --- | --- |
| 1 | ROOT | The key's root note |
| 2 | SCALE | Minor, major, dorian, phrygian, mixolydian, lydian, harmonic minor, minor pentatonic, major pentatonic, blues |
| 3 | KEYS | SCALE: the white keys play the scale, the black keys are silent (or the step tools); CHROM: a normal keyboard |
| 4 | TRANSP | Moves the patterns up or down by scale steps |

Shortcut: **hold SEL and press a key** to set the root.

The sequencer stores each step as a step of the scale, not as a fixed note. Change the root and every pattern moves
with it; change the scale and every pattern takes on the new scale's color. With KEYS on SCALE, the white keys
that play the root have a soft light.

## 6. The sequencer

There are 16 patterns. Each is 1 to 64 steps (up to 4 bars of 16). Every step has a note, an octave, a gate length,
and three switches:

- **Accent:** the step hits harder.
- **Slide:** the note glides into the next one without restarting the envelopes (the 303 slide).
- **Tie:** the step holds the note before it instead of playing a new one.

A step can also be off (a rest).

### Choosing a pattern

Hold **SEQ** and press a white key: the 1st white key is pattern 1, the 16th is pattern 16. While holding SEQ, the
current pattern's key is lit. While the sequencer plays, the new pattern starts at the next bar line.

### Playing

Press **PLAY** to start and stop. With a DAW's clock coming in, the DAW starts and stops it instead (see
[section 9](#9-with-a-daw)).

### Writing steps one at a time (step record)

1. Stop the sequencer and press **REC** ("STEP RECORD").
2. Play a key: the note goes into the current step and the cursor moves on. The cursor's key is lit.
3. The first five black keys are the step tools (with KEYS on SCALE):

| Black key | Tool |
| --- | --- |
| 1st (F#) | **ACC:** the next note is accented |
| 2nd (G#) | **SLIDE:** the next note slides |
| 3rd (A#) | **TIE:** this step holds the note before; move on |
| 4th (C#) | **REST:** this step is silent; move on |
| 5th (D#) | **BACK:** the cursor goes back one step |

4. Press **REC** again to finish.

### Recording while it plays (live record)

Start the sequencer and press **REC** ("LIVE RECORD"). Whatever you play goes into the step playing at that moment.
Press **REC** again to stop recording.

### Editing steps (the SEQ page)

Press **SEQ**. The white keys are now the 16 steps of the bar you are looking at (OCT− / OCT+ change the bar).
Steps with a note have a soft light; the step playing is lit.

- **Tap a white key** to select that step. An empty step gets the last note you used.
- **Tap the selected step again** to turn it on or off.
- The black keys work on the selected step: ACC, SLIDE and TIE switch it, REST clears it, BACK selects the step
  before.

The **STEP** page's knobs change the selected step:

| Knob | Value |
| --- | --- |
| 1 | NOTE: up and down the scale |
| 2 | OCTAVE (−2 to +3) |
| 3 | GATE: 25, 50, 75 or 100 % of the step |
| 4 | ACC/SLD: none, accent, slide, or both |

Press **SEQ** again for the **PATTERN** page:

| Knob | Value |
| --- | --- |
| 1 | LENGTH: 1 to 64 steps |
| 2 | ROTATE: moves the whole pattern a step left or right |
| 3 | TRANSP: up or down by scale steps |
| 4 | SWING |

### Copy, paste and undo

- **Hold SEQ, press SAVE:** copy the pattern.
- **Hold SEQ, press HOME:** paste it over the current pattern.
- **Hold HOME:** undo the last change to the pattern. Hold it again to redo.

## 7. Ideas: new basslines at a button

- **Tap ARP:** mutate. Two or three steps of the pattern change (never the first), in key. Tap again for more.
- **Hold ARP:** a new 16-step bassline in the key, from scratch.

Both can be undone: hold **HOME**. Ideas work while the pattern plays, so you can keep tapping until something
grabs you, then save it.

## 8. Tempo, clock and MIDI (GLO)

Press **GLO**, and again for the next page.

**CLOCK**

| Knob | Value | What it does |
| --- | --- | --- |
| 1 | TEMPO | 40 to 240 BPM |
| 2 | SWING | Delays every second step |
| 3 | STEP | The step length: 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 |
| 4 | OFFSET | Moves the sequencer earlier (−) or later (+), up to 10 ms, to line it up with a DAW |

Tap tempo: hold **GLO** and tap **PLAY** in time, at least twice.

**MIDI**

| Knob | Value | What it does |
| --- | --- | --- |
| 1 | MIDI CH | The channel Kurzhaar listens on and sends on: OMNI (all) or 1–16 |
| 2 | CLOCK | AUTO: follows a DAW's clock when one comes in, otherwise its own tempo. INT: always its own tempo. EXT: only the DAW's clock |
| 3 | NOTES | ON: sends the sequencer's notes out over MIDI |
| 4 | CC | Cutoff (CC 74) and resonance (CC 71): OFF, IN, OUT or IN+OUT |

**ABOUT** shows the version, the license and the credits.

## 9. With a DAW

Connect the FM-1 over USB. It appears as a MIDI port and a USB audio input, both named **Kurzhaar FM-1**. Nothing
needs installing.

- **Clock:** set the DAW to send MIDI clock to Kurzhaar FM-1. With CLOCK on AUTO, Kurzhaar starts and stops with the
  DAW and follows its tempo. PLAY on the FM-1 then says "FOLLOWING THE DAW".
- **Starting mid-song:** DAWs that send Song Position (Ableton Live, Logic, Bitwig, Reaper) can start anywhere, and
  Kurzhaar picks up at the matching step. Studio One does not send it, so start playback on a bar line there.
- **Notes from the DAW:** a MIDI track can play Kurzhaar. It plays each note a fixed time after it arrives (about
  13 ms), so set a negative track delay on the MIDI track to line it up: about −11 ms in Studio One with USB audio
  at 48 kHz.
- **Recording the sequencer as MIDI:** turn NOTES on (GLO › MIDI) and record the Kurzhaar FM-1 MIDI input.
- **Automation:** with CC on IN or IN+OUT, CC 74 moves the cutoff and CC 71 the resonance. With OUT, turning SELECT
  and ALGORITHM sends them, so the DAW can record your moves.
- **Patches:** program change 1–32 selects the patch.
- **Audio over USB:** record the Kurzhaar FM-1 audio input (16-bit stereo, 44.1 or 48 kHz). The level is fixed: the
  MASTER knob sets only the speaker and headphones. If your DAW uses another audio interface, combine the two in an
  aggregate device (macOS: Audio MIDI Setup) and tick drift correction for the FM-1.
- **Audio over a cable:** the headphone output into your interface's line input.

**Lining up with the grid:** record a few bars, measure how late Kurzhaar's notes land from bar 2 on (the first
note after Start can be up to one render late), and turn GLO › CLOCK › OFFSET to the left by that much.

## 10. Saving

**Hold SAVE.** This stores all 32 patches, all 16 patterns, the key and scale, and the GLO settings. "SAVED" confirms
it. Anything you have not saved is gone when the FM-1 is switched off.

Kurzhaar keeps its own memory. It does not read what other firmwares saved. If another firmware writes over it,
Kurzhaar starts from its factory patches and patterns next time.

## 11. The factory patches and patterns

| # | Name | The sound |
| --- | --- | --- |
| 1 | Classic Pluck | One saw, low cutoff, a short filter envelope |
| 2 | Fat Two Osc | Two detuned saws and a square an octave down, driving the filter hard |
| 3 | Resonant Squelch | A square through a resonant filter, big envelope, accents and glide |
| 4 | Round Sub | A triangle and a sine sub, barely filtered: deep and soft |
| 5 | House Pump | A pulse and a sub; LFO 1 dips the level every beat, like a sidechain |
| 6 | Synthpop PWM | A pulse whose width breathes, a thin octave above, a little room |
| 7 | Acid Line | One saw, squelchy resonance, big accents, slides, driven |
| 8 | Reese | Two saws detuned against each other and a sub; a slow filter sweep |
| 9 | Wobble | Saws an octave apart; the filter opens and closes in 1/8s, in tempo |
| 10 | Deep Dub | Triangle and sub, dark, with a reverb that swells every two bars |
| 11 | Funk Slap | A short, bright filter snap that follows how hard you play |
| 12 | Growl | Driven to the edge; LFOs shake the filter and the drive |
| 13 | Pluck Verb | A short pluck into a bright reverb, the low end dry |
| 14 | Octave Disco | A saw and a saw an octave up, bouncy and bright |
| 15 | Singing Line | An open saw with gentle vibrato and glide on every note |
| 16 | Trance Roll | Short detuned saws; the filter lifts over every bar |

### The factory patterns

All 16 patterns start with a bassline in A minor. They are written in scale steps, so they follow the key and scale
you set on the SEL page. (Version 1.0 has only patterns 1 and 2; patterns 3–16 come with the next release.)

| # | Style | What's in it | Try it with patch |
| --- | --- | --- | --- |
| 1 | Bassline | Accents, octave jumps, two slides | any |
| 2 | Bassline | Sparser, with one slide | any |
| 3 | Acid squelch | Octave jumps and two slides | 7 Acid Line, 3 Resonant Squelch |
| 4 | Octave pump | House and disco octaves walking A, F, D, G | 5 House Pump |
| 5 | Rolling offbeat | A trance gallop that skips the downbeat | 16 Trance Roll |
| 6 | Reese | Long tied notes, one slide into the root | 8 Reese |
| 7 | Funk sixteenths | Short staccato octave stabs | 11 Funk Slap |
| 8 | Dub | Half time, lots of space, a low E | 10 Deep Dub |
| 9 | Electro | Syncopated root hits, a slide up an octave | 2 Fat Two Osc |
| 10 | Slide melody | Glides up the scale and back down | 15 Singing Line |
| 11 | Gallop | Driving 16ths, an accent every three steps | 12 Growl |
| 12 | Minimal | Sparse, one slide from a low G | 1 Classic Pluck |
| 13 | Walking | A melodic line up and back | 4 Round Sub |
| 14 | Wobble | Half time, tied notes | 9 Wobble |
| 15 | Acid ties | Slides into tied notes, 303-style | 7 Acid Line |
| 16 | Disco octaves | An octave bounce on A, C and G | 14 Octave Disco |

A pattern you saved empty gets its factory bassline back the next time the FM-1 starts; a pattern with notes in it
is never changed.

## 12. Troubleshooting

- **No sound from the speaker or headphones:** turn up MASTER. Check that the patch's LEVEL (FX › DRIVE) is not all
  the way down and the cutoff is not closed.
- **PLAY does nothing, or says "FOLLOWING THE DAW":** a DAW's clock is driving Kurzhaar. Start and stop from the DAW,
  or set GLO › MIDI › CLOCK to INT.
- **"CLOCK: DAW ONLY":** CLOCK is on EXT. Set it to AUTO or INT to play on its own.
- **The black keys are silent:** KEYS is on SCALE. That is on purpose: every white key is in the scale. Set KEYS to
  CHROM for a normal keyboard.
- **"TAP A STEP FIRST":** on the SEQ page, tap a white key to choose a step before using the step tools or the STEP
  knobs.
- **The FM-1 is missing as an audio input on a Mac:** close any browser tab that talks to it (the Firmware Hub, for
  one) and plug it in again.
- **Kurzhaar does not start:** hold **OCT−** while switching on. The FM-1 starts in the USB rescue mode, where any
  firmware can be installed (with the FM-1 Firmware Hub, or `tools/fm1_install.py`).
- **Going back to another firmware:** use the [FM-1 Firmware Hub](https://thefirmwarehub.com) in Chrome or Edge.

---

Kurzhaar is GPL-3.0 free software by JazzMasterT, built on Dinghy (zvenson) and Felucca (Hügelton Instruments).
Credits are in [LICENSING.md](../LICENSING.md).
