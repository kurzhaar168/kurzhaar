/* SPDX-License-Identifier: GPL-3.0-only */
/* The bass engine: one analog-style mono voice (docs/DESIGN.md section 4). Plain float C, no libm, no hardware:
 * the same code renders WAVs on the Mac (tests/render.c) and runs in the FM-1's audio interrupt.
 *
 *   be_init(&e, 44100.0f);  e.p = <a patch>;  be_patch_changed(&e);
 *   be_note_on(&e, note, vel);  be_note_off(&e, note);  be_render(&e, out, n);   (out: n mono floats, +-1)
 *
 * Signal path, all at 4x the output rate until the decimators (4x -> 2x -> 1x):
 *   OSC1 + OSC2 + SUB + NOISE -> mixer -> drive -> nonlinear 4-pole ladder (or 2-pole tap) -> VCA -> saturation
 *   -> soft limit -> half-band decimator -> DC blocker -> sub-sonic high-pass -> level */
#ifndef BE_H
#define BE_H
#include <stdint.h>

enum { BE_SAW = 0, BE_PULSE = 1, BE_TRI = 2 };
enum { BE_SUB_SQUARE = 0, BE_SUB_SINE = 1 };
enum { BE_GLIDE_OFF = 0, BE_GLIDE_LEGATO = 1, BE_GLIDE_ALWAYS = 2 };

typedef struct {                                 /* a patch, in musical units */
    float osc1_wave, osc1_pw, osc1_level;        /* wave BE_SAW/PULSE/TRI; pw 0.05..0.95; level 0..1 */
    float osc2_wave, osc2_pw, osc2_level;
    float osc2_semi, osc2_detune;                /* semitones -24..24; cents -50..50 */
    float sub_wave, sub_oct, sub_level;          /* square/sine; 1 or 2 octaves down; 0..1 */
    float noise_level;                           /* 0..1 */
    float drive;                                 /* 0..1: how hard the mixer drives the ladder */
    float cutoff;                                /* Hz, 20..20000 (at the key-track reference, C3) */
    float res;                                   /* 0..1 (1 = self-oscillation) */
    float bass_comp;                             /* 0..1: how much of the low end resonance takes back */
    float pole2;                                 /* 0 = 24 dB ladder, 1 = 12 dB tap */
    float keytrack;                              /* 0..1 (1 = the cutoff follows the note) */
    float fenv_amt;                              /* octaves the filter envelope opens, -4..8 */
    float f_a, f_d, f_s, f_r;                    /* filter envelope: seconds, seconds, 0..1, seconds */
    float a_a, a_d, a_s, a_r;                    /* amp envelope */
    float vel_sens;                              /* 0..1: velocity to filter env and level */
    float accent;                                /* 0..1: what velocity >= 100 adds (filter, level, sweep) */
    float glide, glide_mode;                     /* seconds (RC time); BE_GLIDE_* */
    float analog;                                /* 0..1: drift and component spread (0 = perfectly stable) */
    float sat;                                   /* 0..1: output saturation */
    float hpf;                                   /* 0, 20 or 30 Hz sub-sonic high-pass */
    float level;                                 /* dB, -24..+6 */
} be_patch_t;

typedef struct {                                 /* an analog-style ADSR (RC curves, retriggers from where it is) */
    float v, ca, cd, cr, s;
    uint8_t stage;                               /* 0 idle, 1 attack, 2 decay/sustain, 3 release */
} be_env_t;

typedef struct {                                 /* a half-band decimator's lines (be.c: halfband) */
    float b[128];                                /* the symmetric taps' line, mirrored (2 x 2 x taps per side) */
    float a[32];                                 /* the centre tap's line */
    uint32_t wb, wa;
} be_hb_t;

typedef struct {
    be_patch_t p;
    float fs, fs2, inv_fs2;                      /* output rate, oversampled rate (4x), its inverse */
    /* notes */
    uint8_t held[8], nheld;
    float pitch, pitch_target, glide_c;          /* semitones (MIDI) */
    float vel, acc;                              /* this note's velocity 0..1, accent 0..1 */
    float vel_s, acc_s, sm_c;                    /* the same, gliding (~2 ms), and the glide's coefficient */
    float sweep, sweep_c;                        /* accent sweep (accumulates over accents), octaves; its decay */
    /* oscillators */
    float ph1, ph2, tri1, tri2, dt1, dt2;         /* phases; phase steps (updated every 4th sample) */
    uint8_t pitch_new;
    uint32_t subcnt;
    float drift1, drift2, drift_f, dn1, dn2, dnf, note_off1, note_off2;
    uint32_t rng;
    /* filter: ladder states and the last values (the nonlinearities' operating points) */
    float s[4], y[4], u;
    float pre1, pre2;                            /* the oscillators' 20 kHz low-pass */
    float spread_cut, spread_res;                /* component spread (analog) */
    /* envelopes */
    be_env_t fe, ae;
    /* output */
    be_hb_t hb4, hb2;                            /* decimators 4x -> 2x, 2x -> 1x */
    float dc_x, dc_y, hp_s1, hp_s2, hp_c, out_gain;
    uint32_t tick;
} be_t;

void be_init(be_t *e, float fs);
void be_patch_changed(be_t *e);                  /* after changing e->p */
void be_note_on(be_t *e, uint8_t note, uint8_t vel);
void be_note_off(be_t *e, uint8_t note);
void be_all_off(be_t *e);
void be_render(be_t *e, float *out, uint32_t n);
void be_patch_default(be_patch_t *p);
#endif
