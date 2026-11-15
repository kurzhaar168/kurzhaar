/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* The sequencer's data (seq.h). */
#include "seq.h"

const seq_scale_t seq_scales[] = {
    {"MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},       {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},      {"PHRYGIAN", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"MIXOLYDIAN", 7, {0, 2, 4, 5, 7, 9, 10}},  {"LYDIAN", 7, {0, 2, 4, 6, 7, 9, 11}},
    {"HARM MINOR", 7, {0, 2, 3, 5, 7, 8, 11}},  {"MIN PENT", 5, {0, 3, 5, 7, 10}},
    {"MAJ PENT", 5, {0, 2, 4, 7, 9}},           {"BLUES", 6, {0, 3, 5, 6, 7, 10}},
};
const uint32_t seq_nscales = sizeof seq_scales / sizeof seq_scales[0];

static int32_t floordiv(int32_t a, int32_t b) { int32_t q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }

int32_t seq_note(const seq_key_t *k, const seq_step_t *s)
{
    const seq_scale_t *sc = &seq_scales[k->scale < seq_nscales ? k->scale : 0];
    int32_t n = sc->n, d = (int32_t)s->deg + k->transpose;
    int32_t o = floordiv(d, n), dd = d - o * n;
    return SEQ_BASE_NOTE + (int32_t)k->root + sc->iv[dd] + 12 * (o + s->oct) + (int32_t)seq_chrom(s);
}

void seq_set_note(const seq_key_t *k, seq_step_t *s, int32_t midi)
{
    const seq_scale_t *sc = &seq_scales[k->scale < seq_nscales ? k->scale : 0];
    int32_t n = sc->n, rel = midi - SEQ_BASE_NOTE - (int32_t)k->root;
    int32_t o = floordiv(rel, 12), pc = rel - 12 * o, i, best = 0;
    for (i = 0; i < n; i++)                      /* the scale note at or below it */
        if (sc->iv[i] <= pc) best = i;
    s->deg = (int8_t)(best - k->transpose);       /* (the degree before the pattern's transpose) */
    s->oct = (int8_t)o;
    s->gate_chrom = (uint8_t)((s->gate_chrom & 3u) | (uint32_t)(pc - sc->iv[best]) << 2);
    /* normalise: deg in 0..n-1 after transpose would fold octaves into deg; keep deg as entered and the octave
     * explicit, so seq_note(seq_set_note(x)) == x for every x */
    {
        seq_step_t t = *s;
        int32_t back = seq_note(k, &t);
        s->oct = (int8_t)(s->oct + floordiv(midi - back, 12));
    }
}

static const int8_t KEY_BLACK[27] = {            /* F3..G5: which keys are black */
    0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0};

int32_t seq_white_index(uint32_t key)
{
    int32_t i, w = 0;
    if (key > 26u || KEY_BLACK[key]) return -1;
    for (i = 0; i < (int32_t)key; i++) w += !KEY_BLACK[i];
    return w;
}

int32_t seq_black_index(uint32_t key)
{
    int32_t i, b = 0;
    if (key > 26u || !KEY_BLACK[key]) return -1;
    for (i = 0; i < (int32_t)key; i++) b += KEY_BLACK[i];
    return b;
}

int32_t seq_key_note(const seq_key_t *k, uint32_t key, int32_t oct)
{
    if (key > 26u) return -1;
    if (k->chromatic)                            /* a normal keyboard: F3 is MIDI 53, octave 0 = one octave down */
        return 41 + (int32_t)key + 12 * oct;
    {
        int32_t w = seq_white_index(key);
        seq_step_t s = {SEQ_ON, 0, 0, 0};
        if (w < 0) return -1;
        s.deg = (int8_t)(w - k->transpose);      /* the root on the first white key, whatever the transpose */
        s.oct = (int8_t)(oct + 1);               /* octave 0: the root in octave 2 (A2 for A) */
        return seq_note(k, &s);
    }
}

void seq_rotate(seq_pat_t *p, int32_t dir)
{
    uint32_t n = p->len, i;
    seq_step_t t;
    if (n < 2u) return;
    if (dir > 0) { t = p->s[n - 1u]; for (i = n - 1u; i > 0; i--) p->s[i] = p->s[i - 1u]; p->s[0] = t; }
    else { t = p->s[0]; for (i = 0; i + 1u < n; i++) p->s[i] = p->s[i + 1u]; p->s[n - 1u] = t; }
}

void seq_clear(seq_pat_t *p, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < SEQ_MAXLEN; i++) { p->s[i].flags = 0; p->s[i].deg = 0; p->s[i].oct = 0; p->s[i].gate_chrom = SEQ_GATE50; }
    p->len = (uint8_t)(len < 1u ? 1u : len > SEQ_MAXLEN ? SEQ_MAXLEN : len);
}

/* ---- ideas */
static uint32_t rnd(uint32_t *s)                 /* xorshift32 */
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static uint32_t chance(uint32_t *s, uint32_t pct) { return rnd(s) % 100u < pct; }

static int32_t deg_near(const seq_key_t *k, int32_t semis)   /* the scale degree nearest an interval */
{
    const seq_scale_t *sc = &seq_scales[k->scale < seq_nscales ? k->scale : 0];
    int32_t i, best = 0, bd = 99;
    for (i = 0; i < sc->n; i++) {
        int32_t d = sc->iv[i] - semis;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void seq_generate(const seq_key_t *k, seq_pat_t *p, uint32_t *rng)
{
    /* where notes fall: the downbeats always, beats often, 8ths sometimes, 16ths now and then (a syncopated bass);
     * what they play: mostly the root, then its octave, the fifth, the third, the seventh below, the fourth */
    static const uint8_t ON_PCT[16] = {100, 25, 55, 40, 80, 25, 55, 45, 95, 25, 55, 40, 75, 30, 55, 45};
    static const struct { int8_t semis, oct; uint8_t w; } NOTE[] = {
        {0, 1, 44}, {0, 2, 15}, {7, 1, 14}, {3, 1, 9}, {10, 0, 9}, {5, 1, 9}};   /* (octave 1: the root in C2..B2, where
                                                    the keyboard writes at OCT 0; octave 0 put a C root at 33 Hz) */
    uint32_t i, j;
    seq_clear(p, 16);
    for (i = 0; i < 16u; i++) {
        seq_step_t *s = &p->s[i];
        uint32_t r;
        if (!chance(rng, ON_PCT[i])) continue;
        r = rnd(rng) % 100u;
        for (j = 0; j + 1u < sizeof NOTE / sizeof NOTE[0] && r >= NOTE[j].w; j++) r -= NOTE[j].w;
        s->flags = SEQ_ON;
        s->deg = (int8_t)deg_near(k, NOTE[j].semis);   /* (TRANSP moves it, as it moves every pattern) */
        s->oct = NOTE[j].oct;
        s->gate_chrom = (uint8_t)(chance(rng, 70) ? SEQ_GATE50 : chance(rng, 50) ? SEQ_GATE75 : SEQ_GATE25);
        if (i == 0u || chance(rng, 18)) s->flags |= SEQ_ACC;
    }
    for (i = 0; i + 1u < 16u; i++)               /* slides: into a different note, now and then */
        if ((p->s[i].flags & SEQ_ON) && (p->s[i + 1u].flags & SEQ_ON) && chance(rng, 14)
            && (p->s[i].deg != p->s[i + 1u].deg || p->s[i].oct != p->s[i + 1u].oct))
            p->s[i].flags |= SEQ_SLIDE;
}

uint32_t seq_mutate(const seq_key_t *k, seq_pat_t *p, uint32_t *rng)
{
    uint32_t n = 2u + rnd(rng) % 2u, done = 0, tries;
    int8_t base = (p->s[0].flags & SEQ_ON) && !(p->s[0].flags & SEQ_TIE) ? p->s[0].oct : 1;   /* the anchor's octave */
    for (tries = 0; done < n && tries < 40u; tries++) {
        uint32_t i = 1u + rnd(rng) % (p->len > 1u ? p->len - 1u : 1u);   /* never the first step: the anchor */
        seq_step_t s = p->s[i];
        uint32_t what = rnd(rng) % 100u;
        if (s.flags & SEQ_TIE) continue;
        if (!(s.flags & SEQ_ON)) {               /* an empty step: sometimes a note appears (the root or its octave) */
            if (what >= 35u) continue;
            s.flags = SEQ_ON; s.deg = 0; s.oct = (int8_t)(base + (chance(rng, 50) ? 1 : 0));
            s.gate_chrom = SEQ_GATE50;
        } else if (what < 40u) {                 /* a neighbouring note in the scale */
            s.deg = (int8_t)(s.deg + (chance(rng, 50) ? 1 : -1) * (chance(rng, 70) ? 1 : 2));
            s.gate_chrom &= 3u;
        } else if (what < 60u) {                 /* an octave jump, back toward the middle */
            s.oct = (int8_t)(s.oct > base ? s.oct - 1 : s.oct < base ? s.oct + 1 : base + (chance(rng, 50) ? 1 : -1));
        } else if (what < 75u) s.flags ^= SEQ_ACC;
        else if (what < 88u) s.flags ^= SEQ_SLIDE;
        else s.flags = 0;                        /* a rest */
        p->s[i] = s;
        done++;
    }
    return done;
}
