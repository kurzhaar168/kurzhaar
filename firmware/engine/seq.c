/* SPDX-License-Identifier: GPL-3.0-only */
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
