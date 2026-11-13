/* SPDX-License-Identifier: GPL-3.0-only */
/* The clock (docs/DESIGN.md section 5): where the steps (16ths) fall, in samples.
 *
 * Time is the audio sample count (uint32, wraps after ~27 h; compare with (int32_t)(a - b)).
 *  - Internal: the tempo as an exact fraction, step length = 15 * fs * 10 / bpm10 samples; each step's position is
 *    floor(k * num / den) carried exactly (Bresenham), so the grid never drifts.
 *  - External (MIDI clock, 24 per quarter, 6 per step): a phase-locked loop on the ticks' arrival times estimates
 *    where tick n falls and the tick period; steps are placed on that estimate, not on the (jittery) arrivals.
 *    Steady: heavy smoothing. When the ticks keep disagreeing with the prediction (a tempo change): fast tracking,
 *    then back. No ticks: carries on at the last tempo.
 * Plain C, no hardware: the FM-1 calls it from the audio interrupt, tests/clock_test.c on the Mac. */
#ifndef CLK_H
#define CLK_H
#include <stdint.h>

typedef struct {
    float fs;
    uint8_t running;                             /* steps are being played */
    uint8_t ext;                                 /* following an external clock */
    uint8_t armed;                               /* Start/Continue received: the next tick starts playback */
    int32_t step;                                /* the next step to play (song position in 16ths) */
    /* internal: the next step's time = next_s exactly; step length num/den samples (q + r/den) */
    uint32_t next_s, num, den, q, r, rem;
    uint32_t bpm10;
    /* external PLL: tick `tick` is estimated at ref_s + ref_f; P = samples per tick */
    uint8_t have_ref;
    uint32_t nticks;                             /* ticks seen since the PLL (re)started */
    int32_t tick;
    uint32_t ref_s;
    float ref_f, P;
    float e_hist[6];
    uint32_t e_i;
    uint32_t last_arrival;
    uint32_t n_fast;                             /* diagnostics: fast-tracking triggers (jumps | drifts << 16) */
} clk_t;

void clk_init(clk_t *c, float fs);
void clk_set_bpm(clk_t *c, uint32_t bpm10, uint32_t now);    /* internal tempo, 0.1 BPM steps (300..3000) */
void clk_play(clk_t *c, uint32_t now);           /* internal: start at step 0, now */
void clk_stop(clk_t *c);
void clk_advance(clk_t *c);                      /* step c->step has been played: on to the next */

/* external MIDI clock and transport, with each message's arrival time in samples (integer + fraction) */
void clk_ext_tick(clk_t *c, uint32_t t, float tf);
void clk_ext_start(clk_t *c);                   /* 0xFA: from step 0 at the next tick */
void clk_ext_continue(clk_t *c);                /* 0xFB: from the current position at the next tick */
void clk_ext_stop(clk_t *c);                    /* 0xFC */
void clk_ext_spp(clk_t *c, uint32_t pos16);     /* 0xF2: song position, in 16ths */
void clk_ext_poll(clk_t *c, uint32_t now);      /* each block: a clock that stopped coming */

/* the time of step k (when running), as an offset in samples from `from` (may be negative: late) */
float clk_step_offset(const clk_t *c, int32_t k, uint32_t from);
float clk_step_len(const clk_t *c);              /* the current step length in samples */
float clk_tempo(const clk_t *c);                   /* the tempo now (internal or estimated) */
#endif
