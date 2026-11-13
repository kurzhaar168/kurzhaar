/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* The reverb: a small stereo feedback-delay network for a bass voice.
 *
 *   mono in -> low cut (the send: the bass's fundamental stays dry) -> 2 allpass diffusers -> 4 delay lines,
 *   mixed by a Householder matrix, each with a one-pole damping low-pass and a gain for the decay time
 *   -> lines 0+2 left, 1+3 right (added to the dry signal by the caller)
 *
 * The room's size follows the decay (longer decay, longer lines). Plain float C, no libm; the delay lines live in the
 * pool on the FM-1 (VERB_POOL). */
#ifndef VERB_H
#define VERB_H
#include <stdint.h>

#define VERB_LINE 4800u                          /* the longest delay line, samples (~109 ms) */
#define VERB_AP 512u                             /* the diffusers' buffers */

typedef struct {
    float mix;                                   /* 0..1: how much reverb */
    float decay;                                 /* seconds to -60 dB, 0.3..8 */
    float tone;                                  /* 0..1: dark .. bright (the damping) */
    float lowcut;                                /* Hz, 60..600: below it the bass stays dry */
} verb_cfg_t;

typedef struct {
    float fs;
    verb_cfg_t cfg;                              /* the settings the coefficients were made for */
    uint32_t len[4], ap_len[2];
    uint32_t pos;                                /* (all lines share one write position) */
    float g[4];                                  /* per-line gain for the decay */
    float damp, hp_c, hp_s, hp2_s, lp[4];         /* damping; the send's low cut (two one-poles) */
    float *line[4], *ap[2];
    uint32_t ap_pos[2];
} verb_t;

void verb_init(verb_t *v, float fs);
void verb_set(verb_t *v, const verb_cfg_t *c);   /* recomputes only when something changed */
/* in: n mono samples; out: n stereo frames (L R), the reverb ADDED to them (the caller puts the dry signal there) */
void verb_process(verb_t *v, const float *in, float *out, uint32_t n);
#endif
