/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* The reverb (verb.h). */
#include "verb.h"
#include "be_math.h"

#ifdef BE_RAM_CODE                               /* the FM-1: the lines in the pool (big, zeroed by verb_init) */
#define VERB_POOL __attribute__((section(".pool")))
#else
#define VERB_POOL
#endif
static float vl[4][VERB_LINE] VERB_POOL;
static float vap[2][VERB_AP] VERB_POOL;

static const uint32_t BASE[4] = {1559u, 1907u, 2297u, 2671u};   /* the lines at size 1 (35..61 ms at 44.1 kHz) */
static const uint32_t APL[2] = {223u, 379u};                    /* the diffusers */
#define AP_G 0.6f

void verb_init(verb_t *v, float fs)
{
    uint32_t i, k;
    uint8_t *b = (uint8_t *)v;
    for (i = 0; i < sizeof *v; i++) b[i] = 0;
    v->fs = fs;
    for (k = 0; k < 4u; k++) { v->line[k] = vl[k]; for (i = 0; i < VERB_LINE; i++) vl[k][i] = 0.0f; }
    for (k = 0; k < 2u; k++) { v->ap[k] = vap[k]; v->ap_len[k] = APL[k]; for (i = 0; i < VERB_AP; i++) vap[k][i] = 0.0f; }
    v->cfg.decay = -1.0f;                        /* (so the first verb_set computes everything) */
}

void verb_set(verb_t *v, const verb_cfg_t *c)
{
    float d, size, fc;
    uint32_t k;
    if (c->decay == v->cfg.decay && c->tone == v->cfg.tone && c->lowcut == v->cfg.lowcut) { v->cfg.mix = c->mix; return; }
    v->cfg = *c;
    d = be_clamp(c->decay, 0.3f, 8.0f);
    size = be_clamp(0.55f + 0.11f * d, 0.6f, 1.55f);   /* a longer decay, a bigger room */
    for (k = 0; k < 4u; k++) {
        uint32_t len = (uint32_t)((float)BASE[k] * size * v->fs / 44100.0f);
        v->len[k] = len < 64u ? 64u : len > VERB_LINE - 1u ? VERB_LINE - 1u : len;
        v->g[k] = be_exp2(-9.96578f * (float)v->len[k] / (d * v->fs));   /* -60 dB after `decay` seconds */
    }
    fc = 1500.0f * be_exp2(be_clamp(c->tone, 0.0f, 1.0f) * 3.5f);      /* damping: 1.5 .. 17 kHz */
    v->damp = 1.0f - be_exp(-2.0f * BE_PI * fc / v->fs);
    v->hp_c = 1.0f - be_exp(-2.0f * BE_PI * be_clamp(c->lowcut, 40.0f, 1000.0f) / v->fs);
}

void verb_process(verb_t *v, const float *in, float *out, uint32_t n)
{
    float hp1 = v->hp_s, hp2 = v->hp2_s, wet = v->cfg.mix * 1.4f;   /* (mix 50 %: the tail ~14 dB under a bassline) */
    uint32_t i, k, pos = v->pos;
    for (i = 0; i < n; i++) {
        float x = in[i], h, r[4], s;
        hp1 += (x - hp1) * v->hp_c;              /* the send's low cut: 12 dB/oct (two one-poles) */
        h = x - hp1;
        hp2 += (h - hp2) * v->hp_c;
        h -= hp2;
        for (k = 0; k < 2u; k++) {               /* diffusion: two Schroeder allpasses */
            float *b = v->ap[k], dd = b[v->ap_pos[k]], y = dd - AP_G * h;
            b[v->ap_pos[k]] = h + AP_G * y;
            if (++v->ap_pos[k] >= v->ap_len[k]) v->ap_pos[k] = 0;
            h = y;
        }
        for (k = 0; k < 4u; k++) {               /* the lines, damped */
            uint32_t rd = pos >= v->len[k] ? pos - v->len[k] : pos + VERB_LINE - v->len[k];
            v->lp[k] += (v->line[k][rd] - v->lp[k]) * v->damp;
            r[k] = v->lp[k];
        }
        s = 0.5f * (r[0] + r[1] + r[2] + r[3]);  /* Householder: r - (2/N) sum, energy kept, every line feeds every line */
        for (k = 0; k < 4u; k++)
            v->line[k][pos] = 0.7f * h + v->g[k] * (r[k] - s);
        if (++pos >= VERB_LINE) pos = 0;
        out[2u * i] += wet * (r[0] - r[2] * 0.6f);
        out[2u * i + 1u] += wet * (r[1] - r[3] * 0.6f);
    }
    v->pos = pos;
    v->hp_s = hp1;
    v->hp2_s = hp2;
}
