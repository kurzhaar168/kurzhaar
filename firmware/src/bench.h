/* SPDX-License-Identifier: GPL-3.0-only */
/* M0 benchmark: does the AC791N run hardware float (-mcpu=r3), how fast, and what does a realistic
 * analog-style bass voice cost? Built only with -DDINGHY_BENCH (see tools/bench.sh). Not part of the product.
 *
 * bench_voice() is pure C, also compiled on the Mac (tests/bench_ref.c) to check the device's float results.
 * On the device: PLAY runs it; results are drawn on the screen and sent as MIDI CC on channel 16
 * (CC 100 = result index, CC 101..105 = the value in 7-bit pieces, low first). */
#ifndef BENCH_H
#define BENCH_H
#include <stdint.h>

#define BENCH_N 256u                          /* one audio block */

/* ---- a realistic mono bass voice in float, 2x oversampled ladder ---- */
typedef struct {
    float ph1, ph2, inc1, inc2;               /* two saws, phase 0..1 */
    float s[4];                               /* ladder stage states (TPT integrators) */
    float y4;                                 /* last output, for feedback */
    float g, res, drive;                      /* cutoff coefficient, resonance 0..4, input drive */
    float env, env_dec;                       /* a decaying filter envelope */
    float hb[4];                              /* half-band decimator history */
} bvoice_t;

static float b_tanh(float x)                  /* Pade 3/2, clamped: tanh within ~2 % */
{
    float x2;
    if (x > 3.0f) x = 3.0f;
    if (x < -3.0f) x = -3.0f;
    x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static float b_blep(float t, float dt)        /* polyBLEP residual */
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static float b_saw(float *ph, float inc)
{
    float v = 2.0f * *ph - 1.0f - b_blep(*ph, inc);
    *ph += inc;
    if (*ph >= 1.0f) *ph -= 1.0f;
    return v;
}

static float b_ladder(bvoice_t *v, float in)  /* one step of a nonlinear 4-pole ladder (Huovilainen-style) */
{
    float g = v->g * (0.5f + v->env), x;
    int k;
    if (g > 0.95f) g = 0.95f;
    x = b_tanh(in * v->drive - v->res * v->y4);
    for (k = 0; k < 4; k++) {
        float t = b_tanh(v->s[k]);
        v->s[k] += g * (x - t);
        x = v->s[k];
    }
    v->y4 = x;
    return x;
}

/* render n frames; returns a checksum of the output */
static float bench_voice(bvoice_t *v, float *out, uint32_t n)
{
    uint32_t i;
    float sum = 0.0f;
    for (i = 0; i < n; i++) {
        float a, b, y;
        a = b_saw(&v->ph1, v->inc1 * 0.5f) + b_saw(&v->ph2, v->inc2 * 0.5f);   /* oversampled 2x */
        a = b_ladder(v, a * 0.5f);
        b = b_saw(&v->ph1, v->inc1 * 0.5f) + b_saw(&v->ph2, v->inc2 * 0.5f);
        b = b_ladder(v, b * 0.5f);
        y = 0.25f * (v->hb[0] + b) + 0.5f * a;                                    /* crude decimator */
        v->hb[0] = b;
        v->env *= v->env_dec;
        out[i] = y;
        sum += y * (float)(i + 1u);
    }
    return sum;
}

static void bench_voice_init(bvoice_t *v)
{
    uint32_t k;
    for (k = 0; k < 4u; k++) v->s[k] = 0.0f, v->hb[k] = 0.0f;
    v->ph1 = 0.0f; v->ph2 = 0.3f;
    v->inc1 = 55.0f / 44100.0f; v->inc2 = 55.3f / 44100.0f;   /* A1, slightly detuned */
    v->y4 = 0.0f; v->g = 0.12f; v->res = 3.2f; v->drive = 1.8f;
    v->env = 1.0f; v->env_dec = 0.9995f;
}

/* ---- the real bass engine: two patches, one note held; checksum of one block (host and device must agree) */
#ifdef BENCH_STANDALONE
#include "../engine/be.c"                        /* the Mac: one file */
#else
#include "../engine/be.h"                        /* the device: the engine is its own object (tools/build.py) */
#ifdef BE_PROFILE
extern volatile uint32_t be_prof_skip;
#endif
#endif
static be_t b_eng;

static void bench_engine_setup(int heavy)
{
    be_init(&b_eng, 44100.0f);
    if (heavy) {                                 /* P2: two saws + a square sub, driven */
        b_eng.p.osc2_level = 0.8f; b_eng.p.osc2_detune = 7.0f; b_eng.p.sub_level = 0.7f;
        b_eng.p.drive = 0.6f; b_eng.p.cutoff = 450.0f; b_eng.p.res = 0.0f; b_eng.p.sat = 0.3f;
    } else {                                     /* P1: one saw */
        b_eng.p.cutoff = 180.0f; b_eng.p.res = 0.25f;
    }
    b_eng.p.analog = 0.5f;
    be_patch_changed(&b_eng);
    be_note_on(&b_eng, 33, 100);
}

static float bench_engine_checksum(float *out)   /* from setup: 8 blocks, the checksum of the 8th */
{
    uint32_t k, i;
    float sum = 0.0f;
    bench_engine_setup(1);
    for (k = 0; k < 8u; k++) be_render(&b_eng, out, BENCH_N);
    for (i = 0; i < BENCH_N; i++) sum += out[i] * (float)(i + 1u);
    return sum;
}

#ifndef BENCH_STANDALONE
/* ---- device side ---- */
static volatile int32_t b_isink;
static volatile float b_fsink;

static int32_t __attribute__((noinline)) b_int_mac(const int32_t *a, uint32_t n)
{
    int32_t acc = 0; uint32_t i;
    for (i = 0; i < n; i++) acc += a[i] * a[(i + 7u) & (BENCH_N - 1u)];
    return acc;
}
static int32_t __attribute__((noinline)) b_q31_mac(const int32_t *a, uint32_t n)   /* 32x32->64 >> 31 */
{
    int32_t acc = 0; uint32_t i;
    for (i = 0; i < n; i++) acc += (int32_t)(((int64_t)a[i] * a[(i + 7u) & (BENCH_N - 1u)]) >> 31);
    return acc;
}
static float __attribute__((noinline)) b_flt_mac(const float *a, uint32_t n)
{
    float acc = 0.0f; uint32_t i;
    for (i = 0; i < n; i++) acc += a[i] * a[(i + 7u) & (BENCH_N - 1u)];
    return acc;
}
static float __attribute__((noinline)) b_flt_div(const float *a, uint32_t n)
{
    float acc = 0.0f; uint32_t i;
    for (i = 0; i < n; i++) acc += a[i] / (a[(i + 7u) & (BENCH_N - 1u)] + 2.0f);
    return acc;
}

static int32_t b_ia[BENCH_N];
static float b_fa[BENCH_N], b_out[BENCH_N];

#define B_RUNS 16u
#define B_TIME(expr) ({ uint32_t _best = 0xFFFFFFFFu, _r; for (_r = 0; _r < B_RUNS; _r++) { \
    uint32_t _t0 = fm1_ticks(); expr; _t0 = fm1_ticks() - _t0; if (_t0 < _best) _best = _t0; } _best; })

static void b_utoa(char *p, uint32_t v)
{
    char t[12]; int i = 0;
    do { t[i++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (i) *p++ = t[--i];
    *p = 0;
}

static void b_line(uint32_t row, const char *label, uint32_t val, const char *unit)
{
    char s[40], *p = s;
    while (*label) *p++ = *label++;
    b_utoa(p, val);
    while (*p) p++;
    while (*unit) *p++ = *unit++;
    *p = 0;
    draw_text_box(0, (int32_t)(40u + row * 22u), 240, &FONT_S, s, C_WHITE, 0);
}

static void b_send(uint32_t idx, uint32_t val)
{
    uint32_t k;
    midi_send(0xBFu, 100, (uint8_t)idx);
    for (k = 0; k < 5u; k++)
        midi_send(0xBFu, (uint8_t)(101u + k), (uint8_t)((val >> (7u * k)) & 0x7Fu));
    {                                            /* let USB send them before the next value (the queue is short) */
        uint32_t t0 = fm1_ms;
        while (fm1_ms - t0 < 20u) platform_poll();
    }
}

static void bench_run(void)
{
    uint32_t i, t_int, t_q31, t_fmac, t_fdiv, t_voice, pct, ns;
    float sum;
    bvoice_t v;
    union { float f; uint32_t u; } cs;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 8, 240, &FONT_S, "M0 BENCH running...", C_WHITE, 1);
    for (i = 0; i < BENCH_N; i++) {
        b_ia[i] = (int32_t)(i * 2654435761u);
        b_fa[i] = (float)(int32_t)(i * 37u % 101u) * 0.01f - 0.5f;
    }
    t_int = B_TIME(b_isink = b_int_mac(b_ia, BENCH_N));
    t_q31 = B_TIME(b_isink = b_q31_mac(b_ia, BENCH_N));
    t_fmac = B_TIME(b_fsink = b_flt_mac(b_fa, BENCH_N));
    t_fdiv = B_TIME(b_fsink = b_flt_div(b_fa, BENCH_N));
    t_voice = B_TIME((bench_voice_init(&v), b_fsink = bench_voice(&v, b_out, BENCH_N)));

    bench_voice_init(&v);                     /* the checksum: one clean block from init */
    sum = bench_voice(&v, b_out, BENCH_N);
    cs.f = sum;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 8, 240, &FONT_S, "M0 BENCH (ns per op)", C_ACC, 1);
    ns = t_int * 1000u / FM1_TICKS_PER_US / BENCH_N;       /* ticks -> ns per iteration (x1000/24) */
    b_line(0, "int mac   ", t_int * 1000u / 24u * 10u / BENCH_N, " ns/10");
    b_line(1, "q31 mac   ", t_q31 * 1000u / 24u * 10u / BENCH_N, " ns/10");
    b_line(2, "float mac ", t_fmac * 1000u / 24u * 10u / BENCH_N, " ns/10");
    b_line(3, "float div ", t_fdiv * 1000u / 24u * 10u / BENCH_N, " ns/10");
    b_line(4, "voice 256 ", t_voice / 24u, " us");
    pct = t_voice * 1000u / (24u * 5805u);                  /* of a 5.805 ms block, in 0.1 % */
    b_line(5, "budget    ", pct, " /1000");
    b_line(6, "checksum  ", cs.u, "");
    (void)ns;

    b_send(0, t_int); b_send(1, t_q31); b_send(2, t_fmac); b_send(3, t_fdiv);
    {                                            /* the real engine: one 256-frame block, P1 and P2 */
        uint32_t t_p1, t_p2, k;
        union { float f; uint32_t u; } ec;
        bench_engine_setup(0);
        for (k = 0; k < 8u; k++) be_render(&b_eng, b_out, BENCH_N);
        t_p1 = B_TIME(be_render(&b_eng, b_out, BENCH_N));
        bench_engine_setup(1);
        for (k = 0; k < 8u; k++) be_render(&b_eng, b_out, BENCH_N);
        t_p2 = B_TIME(be_render(&b_eng, b_out, BENCH_N));
        ec.f = bench_engine_checksum(b_out);
        b_line(7, "engine P1 ", t_p1 * 1000u / (24u * 5805u), " /1000");
        b_line(8, "engine P2 ", t_p2 * 1000u / (24u * 5805u), " /1000");
        b_send(7, t_p1); b_send(8, t_p2); b_send(9, ec.u);
#ifdef BE_PROFILE
        {                                        /* P2 with parts switched off: 10 no ladder, 11 no decimators,
                                                    12 no oscillators, 13 none of the three (the per-sample rest) */
            static const uint32_t SK[4] = {1u, 2u, 4u, 7u};
            for (k = 0; k < 4u; k++) {
                uint32_t t;
                be_prof_skip = SK[k];
                bench_engine_setup(1);
                for (t = 0; t < 8u; t++) be_render(&b_eng, b_out, BENCH_N);
                t = B_TIME(be_render(&b_eng, b_out, BENCH_N));
                b_send(10u + k, t);
            }
            be_prof_skip = 0;
        }
        {                                        /* 14: a chain of dependent float multiply-adds, 15: of divides,
                                                    16: a RAM load + add + store */
            volatile float xv = 1.0f;
            float x = xv;
            uint32_t t0, j;
            t0 = fm1_ticks();
            for (j = 0; j < 4096u; j++) x = x * 1.0000001f + 1e-9f;
            b_send(14, fm1_ticks() - t0);
            t0 = fm1_ticks();
            for (j = 0; j < 4096u; j++) x = 1.0f / (x + 1.0f);
            b_send(15, fm1_ticks() - t0);
            t0 = fm1_ticks();
            for (j = 0; j < 4096u; j++) xv = xv + 1.0f;
            b_send(16, fm1_ticks() - t0);
            b_fsink = x;
            {                                    /* 17: a chain of 8 dependent integer ops per iteration (the
                                                    clock: integer ALU ops take one cycle) */
                volatile uint32_t seed = 1u;
                uint32_t a = seed, bb = 2u, c = 3u, d = 4u;
                t0 = fm1_ticks();
                for (j = 0; j < 4096u; j++) {
                    a += j; bb ^= a; c += bb; d ^= c; a += d; bb += j; c ^= a; d += bb;
                }
                b_send(17, fm1_ticks() - t0);
                b_isink = (int32_t)(a ^ bb ^ c ^ d);
            }
        }
#endif
    }
    b_send(4, t_voice); b_send(5, cs.u); b_send(6, 0x5A5A);   /* 6: end marker */
}
#endif /* !BENCH_STANDALONE */
#endif /* BENCH_H */
