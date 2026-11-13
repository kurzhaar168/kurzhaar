/* SPDX-License-Identifier: GPL-3.0-only */
/* The clock (clk.h). */
#include "clk.h"

/* The tick tracker: a least-squares line (phase, period) through the ticks since the last (re)start, growing until it
 * is as smooth as a fading memory of gain A_SLOW (n = 4 / A_SLOW ticks), then that. A tempo change restarts it. */
#ifndef A_SLOW
#define A_SLOW 0.01f                             /* steady: ~400 ticks of memory (8 s at 120 BPM) */
#endif
#ifndef RESTART_N
#define RESTART_N 8u                             /* a drift restarts the fit keeping this many ticks' worth */
#endif
#ifndef TEMPO_JUMP_FRAC
#define TEMPO_JUMP_FRAC 0.06f                     /* one error beyond 6 % of a tick period ... */
#endif
#ifndef TEMPO_JUMP_MS
#define TEMPO_JUMP_MS 2.5f                        /* ... and beyond 2.5 ms (USB jitter is ~1 ms): a jump */
#endif
#ifndef DRIFT_MS
#define DRIFT_MS 0.7f                             /* the last 6 errors averaging beyond this (~3 sigma of +-1 ms
                                                     jitter): a tempo change */
#endif
#ifndef DROPOUT_S
#define DROPOUT_S 0.5f                            /* no tick for this long: the clock stopped (keep the tempo) */
#endif

static float absf(float x) { return x < 0.0f ? -x : x; }

void clk_init(clk_t *c, float fs)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)c;
    for (i = 0; i < sizeof *c; i++) b[i] = 0;
    c->fs = fs;
    c->P = fs * 60.0f / (120.0f * 24.0f);
    clk_set_bpm(c, 1200u, 0u);
}

void clk_set_bpm(clk_t *c, uint32_t bpm10, uint32_t now)
{
    (void)now;
    if (bpm10 < 300u) bpm10 = 300u;
    if (bpm10 > 3000u) bpm10 = 3000u;
    c->bpm10 = bpm10;
    c->num = (uint32_t)(c->fs * 150.0f + 0.5f);  /* 15 * fs * 10: a step of a 16th at bpm10 / 10 BPM */
    c->den = bpm10;
    c->q = c->num / c->den;
    c->r = c->num % c->den;
    if (c->rem >= c->den) c->rem = 0;            /* a new tempo: the next step stays where it was, then the new spacing */
}

void clk_play(clk_t *c, uint32_t now)
{
    c->ext = 0;
    c->step = 0;
    c->next_s = now;
    c->rem = 0;
    c->running = 1;
}

void clk_stop(clk_t *c)
{
    c->running = 0;
    c->armed = 0;
}

void clk_advance(clk_t *c)                       /* the step c->step has been played: on to the next */
{
    c->step++;
    if (!c->ext) {
        c->next_s += c->q;
        c->rem += c->r;
        if (c->rem >= c->den) { c->rem -= c->den; c->next_s++; }
    }
}

float clk_step_len(const clk_t *c)
{
    return c->ext ? 6.0f * c->P : (float)c->num / (float)c->den;
}

float clk_bpm(const clk_t *c)
{
    return c->ext ? c->fs * 60.0f / (24.0f * c->P) : (float)c->bpm10 * 0.1f;
}

float clk_step_offset(const clk_t *c, int32_t k, uint32_t from)
{
    if (!c->ext) {
        float base = (float)(int32_t)(c->next_s - from);
        return k == c->step ? base : base + (float)(k - c->step) * clk_step_len(c);
    }
    /* tick 6k, from the estimate of tick c->tick */
    return (float)(int32_t)(c->ref_s - from) + c->ref_f + (float)(6 * k - c->tick) * c->P;
}

/* ---- external */

static void ref_set(clk_t *c, uint32_t s, float f)   /* keep the fraction in 0..1 */
{
    int32_t whole = (int32_t)f;
    if (f < 0.0f && (float)whole != f) whole--;
    c->ref_s = s + (uint32_t)whole;
    c->ref_f = f - (float)whole;
}

void clk_ext_start(clk_t *c) { c->armed = 1; c->step = 0; c->tick = -1; }
void clk_ext_continue(clk_t *c) { c->armed = 1; c->tick = 6 * c->step - 1; }
void clk_ext_spp(clk_t *c, uint32_t pos16) { c->step = (int32_t)pos16; c->tick = 6 * c->step - 1; }

void clk_ext_stop(clk_t *c)
{
    c->running = 0;
    c->armed = 0;
}

void clk_ext_tick(clk_t *c, uint32_t t, float tf)
{
    float dt, e, a, b, jump, n, m = 0.0f;
    uint32_t i, missed = 0;
    c->ext = 1;
    c->last_arrival = t;
    if (!c->have_ref) {                          /* the first tick: a reference; the period from before (or 120) */
        c->have_ref = 1;
        c->nticks = 1;
        c->tick++;
        ref_set(c, t, tf);
        goto started;
    }
    dt = (float)(int32_t)(t - c->ref_s) + (tf - c->ref_f);   /* since the estimate of the last tick */
    if (c->nticks >= 2u) {                       /* ticks lost on the way (USB, a busy host, a dropout) */
        float k = dt / c->P + 0.5f;
        missed = k >= 2.0f ? (uint32_t)k - 1u : 0u;
    }
    e = dt - ((float)missed + 1.0f) * c->P;      /* how far off the prediction was */
    if (c->nticks >= 8u) {                       /* a tempo change: one big error, or the last 6 drifting one way */
        c->e_hist[c->e_i++ % 6u] = e;
        for (i = 0; i < 6u; i++) m += c->e_hist[i];
        m *= (1.0f / 6.0f);
        jump = TEMPO_JUMP_FRAC * c->P;
        if (jump < TEMPO_JUMP_MS * 0.001f * c->fs) jump = TEMPO_JUMP_MS * 0.001f * c->fs;
        if (absf(e) > jump || absf(m) > DRIFT_MS * 0.001f * c->fs) {
            c->n_fast += absf(e) > jump ? 1u : 0x10000u;   /* (diagnostics: jumps | drifts << 16) */
            c->nticks = absf(e) > jump ? 2u : RESTART_N;   /* start the fit again from the newest ticks */
            for (i = 0; i < 6u; i++) c->e_hist[i] = 0.0f;
        }
    }
    /* the gains: a least-squares line through all ticks since the fit (re)started (expanding memory: exact for
     * n ticks), until that is as smooth as the steady state, then a fixed fading memory (alpha-beta, critically
     * damped). After a start or a tempo change the period is right within a few ticks; steady, jitter averages
     * over hundreds. */
    n = (float)(c->nticks + 1u);                 /* the ticks in the fit, this one included */
    if (n < 4.0f / A_SLOW) {
        a = 2.0f * (2.0f * n - 1.0f) / (n * (n + 1.0f));
        b = 6.0f / (n * (n + 1.0f));
    } else {
        a = A_SLOW;
        b = a * a / (2.0f - a);
    }
    c->tick += (int32_t)missed + 1;
    ref_set(c, c->ref_s, c->ref_f + ((float)missed + 1.0f) * c->P + a * e);   /* (missed ticks at the old period) */
    c->P += b * e / ((float)missed + 1.0f);
    if (c->nticks < 0xFFFFu) c->nticks++;
started:
    if (c->armed && c->tick >= 6 * c->step) {    /* Start / Continue: the first tick is the step's time */
        c->armed = 0;
        c->running = 1;
        c->tick = 6 * c->step;
    }
}

void clk_ext_poll(clk_t *c, uint32_t now)
{
    if (c->have_ref && (float)(int32_t)(now - c->last_arrival) > DROPOUT_S * c->fs) {
        c->have_ref = c->ext && c->running ? c->have_ref : 0;   /* playing: carry on (flywheel) at the last tempo */
        c->nticks = c->nticks > 2u ? 2u : c->nticks;             /* and re-acquire fast when ticks return */
    }
}
