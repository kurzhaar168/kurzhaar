/* SPDX-License-Identifier: GPL-3.0-only */
/* The bass engine (be.h). Float throughout; the FM-1 runs it with its FPU (-mcpu=r3). */
#include "be.h"
#include "be_math.h"
#include "be_hb.h"

/* BE_RAM_CODE (device builds): be_render runs from RAM (.ram_text), not from the flash in place, and its tables
 * live in RAM too. Everything it uses is inlined: RAM code may not call (tools/build.py checks). */
#ifdef BE_RAM_CODE
#define BE_HOT __attribute__((section(".ram_text.be"), noinline))
#else
#define BE_HOT
#endif

#ifdef BE_PROFILE                                /* benchmark builds: switch parts off to see what they cost */
volatile uint32_t be_prof_skip;                  /* 1 ladder, 2 decimators, 4 oscillators */
#define BE_SKIP(b) (be_prof_skip & (b))
#else
#define BE_SKIP(b) 0
#endif

#define OS 4                                     /* oversampling of the oscillators and the ladder (at 2x the
                                                    ladder's nonlinearity aliased to -42 dB with drive: measured) */
#define BE_PRE_G 0.372058856f                    /* tan(pi * 20 kHz / 176.4 kHz): the oscillators' low-pass */
#define ACCENT_VEL 100u                          /* velocity from which a note counts as accented */

/* ------------------------------------------------------------------ patch */

void be_patch_default(be_patch_t *p)
{
    p->osc1_wave = BE_SAW; p->osc1_pw = 0.5f; p->osc1_level = 1.0f;
    p->osc2_wave = BE_SAW; p->osc2_pw = 0.5f; p->osc2_level = 0.0f;
    p->osc2_semi = 0.0f; p->osc2_detune = 0.0f;
    p->sub_wave = BE_SUB_SQUARE; p->sub_oct = 1.0f; p->sub_level = 0.0f;
    p->noise_level = 0.0f;
    p->drive = 0.3f; p->cutoff = 400.0f; p->res = 0.2f; p->bass_comp = 0.5f; p->pole2 = 0.0f;
    p->keytrack = 0.5f; p->fenv_amt = 3.0f;
    p->f_a = 0.001f; p->f_d = 0.25f; p->f_s = 0.0f; p->f_r = 0.15f;
    p->a_a = 0.001f; p->a_d = 0.3f; p->a_s = 1.0f; p->a_r = 0.08f;
    p->vel_sens = 0.5f; p->accent = 0.5f;
    p->glide = 0.06f; p->glide_mode = BE_GLIDE_LEGATO;
    p->analog = 0.5f; p->sat = 0.2f; p->hpf = 20.0f; p->level = 0.0f;
    p->lfo_rate = 3.0f; p->lfo_depth = 0.0f; p->lfo_dest = 1.0f; p->lfo_shape = 0.0f;
    p->verb_mix = 0.0f; p->verb_decay = 1.5f; p->verb_tone = 0.5f; p->verb_lowcut = 200.0f;
}

static float coef(float tau, float fs)           /* one-pole coefficient: the step toward the target per sample */
{
    if (tau < 1e-6f) return 1.0f;
    return 1.0f - be_exp(-1.0f / (tau * fs));
}

static void env_coefs(be_env_t *v, float a, float d, float s, float r, float fs)
{
    /* attack aims at 1.3 and stops at 1: reaching 1 takes a (ln(1.3/0.3) = 1.466 time constants);
     * decay and release: the time to cover 99 % of the way (ln 100 = 4.6 time constants) */
    v->ca = coef(be_clamp(a, 0.0003f, 10.0f) / 1.466f, fs);
    v->cd = coef(be_clamp(d, 0.002f, 20.0f) / 4.6f, fs);
    v->cr = coef(be_clamp(r, 0.002f, 20.0f) / 4.6f, fs);
    v->s = be_clamp(s, 0.0f, 1.0f);
}

void be_patch_changed(be_t *e)
{
    be_patch_t *p = &e->p;
    uint32_t seed = 0x2545F491u;                 /* the "unit's" component spread: the same every time */
    env_coefs(&e->fe, p->f_a, p->f_d, p->f_s, p->f_r, e->fs);
    env_coefs(&e->ae, p->a_a, p->a_d, p->a_s, p->a_r, e->fs);
    e->glide_c = coef(be_clamp(p->glide, 0.0f, 5.0f) / 2.3f, e->fs);   /* ~90 % of the way in glide seconds */
    e->spread_cut = p->analog * 0.06f * be_noise(&seed);               /* up to ~1/16 octave */
    e->spread_res = p->analog * 0.03f * be_noise(&seed);
    if (p->hpf > 1.0f)
        e->hp_c = be_tan(BE_PI * be_clamp(p->hpf, 5.0f, 200.0f) / e->fs);
    else
        e->hp_c = 0.0f;
    e->out_gain = be_exp2(be_clamp(p->level, -24.0f, 12.0f) / 6.0206f);
    e->sweep_c = coef(0.25f, e->fs);
    e->sm_c = coef(0.002f, e->fs);
}

void be_init(be_t *e, float fs)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)e;
    for (i = 0; i < sizeof *e; i++) b[i] = 0;
    e->fs = fs;
    e->fs2 = fs * (float)OS;
    e->inv_fs2 = 1.0f / e->fs2;
    e->rng = 0x9E3779B9u;
    e->pitch = e->pitch_target = 36.0f;
    be_patch_default(&e->p);
    be_patch_changed(e);
}

/* ------------------------------------------------------------------ notes */

static void env_gate(be_env_t *v, int on)
{
    v->stage = on ? 1u : (v->stage ? 3u : 0u);
}

static void start_note(be_t *e, uint8_t note, int legato)
{
    be_patch_t *p = &e->p;
    int glide = p->glide > 0.0005f && (p->glide_mode == BE_GLIDE_ALWAYS || (p->glide_mode == BE_GLIDE_LEGATO && legato));
    e->pitch_target = (float)note;
    if (!glide || e->ae.stage == 0u)
        e->pitch = e->pitch_target;
    e->pitch_new = 1;
    if (!legato) {
        if (e->ae.v < 0.001f) {                  /* silent: start the oscillators in phase (punchy, repeatable);
                                                    sounding: leave them running (no click) */
            e->ph1 = 0.0f; e->ph2 = 0.0f; e->subcnt = 0; e->tri1 = e->tri2 = 0.0f;
        }
        e->note_off1 = p->analog * 0.03f * be_noise(&e->rng);   /* a few cents, new every note */
        e->note_off2 = p->analog * 0.03f * be_noise(&e->rng);
        env_gate(&e->fe, 1);
        env_gate(&e->ae, 1);
    }
}

void be_note_on(be_t *e, uint8_t note, uint8_t vel)
{
    uint32_t i, j;
    int legato = e->nheld > 0u;
    if (!vel) { be_note_off(e, note); return; }
    for (i = j = 0; i < e->nheld; i++)           /* take it out if it is already held, then on top */
        if (e->held[i] != note) e->held[j++] = e->held[i];
    e->nheld = (uint8_t)j;
    if (e->nheld == sizeof e->held) {
        for (i = 1; i < e->nheld; i++) e->held[i - 1] = e->held[i];
        e->nheld--;
    }
    e->held[e->nheld++] = note;
    e->vel = (float)vel * (1.0f / 127.0f);
    e->acc = vel >= ACCENT_VEL ? e->p.accent : 0.0f;
    if (e->ae.v < 0.001f) {                      /* from silence: the new note's level and accent at once (sharp
                                                    attacks); while one sounds they glide (be_render: no click) */
        e->vel_s = e->vel;
        e->acc_s = e->acc;
    }
    if (e->acc > 0.0f)                           /* accents pile up a little, like the 303's sweep circuit */
        e->sweep = be_clamp(e->sweep + e->acc * 0.5f, 0.0f, 1.5f);
    start_note(e, note, legato);
}

void be_note_off(be_t *e, uint8_t note)
{
    uint32_t i, j, was = e->nheld;
    for (i = j = 0; i < e->nheld; i++)
        if (e->held[i] != note) e->held[j++] = e->held[i];
    e->nheld = (uint8_t)j;
    if (!e->nheld) {
        if (was) { env_gate(&e->fe, 0); env_gate(&e->ae, 0); }
    } else if (j != was && i == was) {           /* back to the note still held: legato */
        start_note(e, e->held[e->nheld - 1u], 1);
    }
}

void be_all_off(be_t *e)
{
    e->nheld = 0;
    env_gate(&e->fe, 0);
    env_gate(&e->ae, 0);
}

/* ------------------------------------------------------------------ the sound */

BE_INL float env_tick(be_env_t *v)
{
    switch (v->stage) {
    case 1: v->v += (1.3f - v->v) * v->ca; if (v->v >= 1.0f) { v->v = 1.0f; v->stage = 2; } break;
    case 2: v->v += (v->s - v->v) * v->cd; break;
    case 3: v->v -= v->v * v->cr; if (v->v < 1e-6f) { v->v = 0.0f; v->stage = 0; } break;
    default: break;
    }
    return v->v;
}

BE_INL float blep(float t, float dt)      /* polyBLEP residual of a unit step at t = 0 */
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
BE_INL float frac1(float t) { return t >= 1.0f ? t - 1.0f : t; }

BE_INL float square(float t, float dt, float pw)   /* +1 for t < pw, -1 after, band-limited, zero mean */
{
    return (t < pw ? 1.0f : -1.0f) + blep(t, dt) - blep(frac1(t + 1.0f - pw), dt) - (2.0f * pw - 1.0f);
}

BE_INL float osc(float wave, float t, float dt, float pw, float *tri)
{
    if (wave < 0.5f)                             /* saw */
        return 2.0f * t - 1.0f - blep(t, dt);
    if (wave < 1.5f)                             /* pulse */
        return square(t, dt, pw);
    *tri = *tri * 0.9998f + 4.0f * dt * square(t, dt, 0.5f);   /* triangle: the integrated square */
    return *tri;
}

/* the ladder: four one-pole stages, each a transistor pair (tanh), the feedback through the input pair (tanh).
 * Zero-delay feedback: each tanh is replaced by its slope through the origin at the last sample's operating
 * point, tanh(x) ~ x * t(x_prev), and the resulting linear loop is solved exactly for this sample. */
BE_INL float ladder(be_t *e, float x, float g, float r, float pole2)
{
    /* the five tanh ratios t = n / d, and the four stage gains 1 / (1 + g t): each group shares one division
     * (the product of the denominators, then each reciprocal from the others' partial products). Exact: the same
     * values as ten divisions, to rounding. Divisions are slow on this FPU; multiplies overlap. */
    float ni, di, n0, q0, n1, q1, n2, q2, n3, q3, R, p1, p2, p3, s1, s2, s3, ti, t0, t1, t2, t3;
    float c0, c1, c2, c3, Q, d0, d1, d2, d3;
    be_tanh_nd(e->u, &ni, &di);
    be_tanh_nd(e->y[0], &n0, &q0); be_tanh_nd(e->y[1], &n1, &q1);
    be_tanh_nd(e->y[2], &n2, &q2); be_tanh_nd(e->y[3], &n3, &q3);
    p1 = di * q0; p2 = p1 * q1; p3 = p2 * q2;    /* prefix products */
    s3 = q3; s2 = q2 * s3; s1 = q1 * s2;          /* suffix products (of q1..q3) */
    R = 1.0f / (p3 * q3);
    ti = ni * R * q0 * s1;
    t0 = n0 * R * di * s1;
    t1 = n1 * R * p1 * s2;
    t2 = n2 * R * p2 * s3;
    t3 = n3 * R * p3;
    c0 = 1.0f + g * t0; c1 = 1.0f + g * t1; c2 = 1.0f + g * t2; c3 = 1.0f + g * t3;
    p1 = c0 * c1; s2 = c2 * c3;
    Q = 1.0f / (p1 * s2);
    d0 = Q * c1 * s2; d1 = Q * c0 * s2; d2 = Q * p1 * c3; d3 = Q * p1 * c2;
    {
    float a0 = g * ti * d0, b0 = e->s[0] * d0;
    float a1 = g * t0 * d1, b1 = e->s[1] * d1;
    float a2 = g * t1 * d2, b2 = e->s[2] * d2;
    float a3 = g * t2 * d3, b3 = e->s[3] * d3;
    float G = a0 * a1 * a2 * a3;
    float S = b3 + a3 * (b2 + a2 * (b1 + a1 * b0));
    float u = (x - r * S) / (1.0f + r * G);
    float y0 = a0 * u + b0, y1 = a1 * y0 + b1, y2 = a2 * y1 + b2, y3 = a3 * y2 + b3;
    e->s[0] = 2.0f * y0 - e->s[0]; e->s[1] = 2.0f * y1 - e->s[1];
    e->s[2] = 2.0f * y2 - e->s[2]; e->s[3] = 2.0f * y3 - e->s[3];
    e->y[0] = y0; e->y[1] = y1; e->y[2] = y2; e->y[3] = y3;
    e->u = u;
    return pole2 > 0.5f ? y1 : y3;
    }
}

/* Half-band decimation by 2, polyphase: of each input pair (xa first, xb second) the symmetric taps only ever see
 * the xb samples, the centre tap (0.5) only an xa sample (nodd - 1) pairs back. The xb line is a mirrored buffer
 * (each sample written twice, L apart) so the window is always contiguous: no index masking in the loop, and two
 * accumulators so the FPU's long latency overlaps. */
BE_INL float halfband(be_hb_t *h, const float *c, uint32_t nodd, uint32_t amask, float xa, float xb)
{
    const uint32_t L = 2u * nodd;
    uint32_t w = h->wb, j;
    const float *win;
    float center, s0 = 0.0f, s1 = 0.0f;
    h->a[h->wa & amask] = xa;
    center = h->a[(h->wa - (nodd - 1u)) & amask];
    h->wa++;
    h->b[w] = xb; h->b[w + L] = xb;
    if (++w == L) w = 0;
    h->wb = w;
    win = &h->b[w];                              /* oldest .. newest */
    for (j = 0; j + 1u < nodd; j += 2u) {
        s0 += c[j] * (win[j] + win[L - 1u - j]);
        s1 += c[j + 1u] * (win[j + 1u] + win[L - 2u - j]);
    }
    if (j < nodd) s0 += c[j] * (win[j] + win[L - 1u - j]);
    return 0.5f * center + (s0 + s1);
}

BE_HOT static void be_render_hot(be_t *e, float *out, uint32_t n)
{
    be_patch_t *p = &e->p;
    const float sweep_c = e->sweep_c;
    float drive_g = 0.5f * be_exp2(be_clamp(p->drive, 0.0f, 1.0f) * 4.0f);   /* 0.5 .. 8 into the ladder */
    float makeup = 0.5f / (0.6f + 0.4f * drive_g);           /* -6 dB: headroom for accents and resonance */
    float r = be_clamp(p->res, 0.0f, 1.0f) * 4.15f * (1.0f + e->spread_res);
    float comp = 1.0f + be_clamp(p->bass_comp, 0.0f, 1.0f) * r;              /* gives back what resonance takes */
    float satg = 1.0f + 3.0f * p->sat, satb = 0.12f * p->sat, sat0 = be_tanh(satb);
    float satn = 1.0f / (satg * (1.0f - sat0 * sat0));
    float pw1 = be_clamp(p->osc1_pw, 0.05f, 0.95f), pw2 = be_clamp(p->osc2_pw, 0.05f, 0.95f);
    float subn = p->sub_oct > 1.5f ? 4.0f : 2.0f;
    float drift_amt = p->analog * 0.06f;         /* semitones of slow wander */
    uint32_t i, k;

    for (i = 0; i < n; i++) {
        float f1, f2, dt1, dt2, fe, ae, fc, g, amp, velk, o, x2[2];

        if ((e->tick++ & 31u) == 0u) {           /* slow random wander (~0.5 Hz), per oscillator and filter */
            e->drift1 += 0.01f * (be_noise(&e->rng) - e->drift1);
            e->drift2 += 0.01f * (be_noise(&e->rng) - e->drift2);
            e->drift_f += 0.01f * (be_noise(&e->rng) - e->drift_f);
        }
        if (e->pitch != e->pitch_target)
            e->pitch += (e->pitch_target - e->pitch) * e->glide_c;
        if ((e->tick & 3u) == 1u || e->pitch_new) {   /* the oscillators' pitch: every 4th sample (11 kHz; glides
                                                           and drift change far slower), and at once on a new note */
            e->pitch_new = 0;
            f1 = 440.0f * be_exp2((e->pitch + e->note_off1 + drift_amt * e->drift1 * 4.0f - 69.0f) * (1.0f / 12.0f));
            f2 = 440.0f * be_exp2((e->pitch + p->osc2_semi + p->osc2_detune * 0.01f + e->note_off2
                                   + drift_amt * e->drift2 * 4.0f - 69.0f) * (1.0f / 12.0f));
            e->dt1 = be_clamp(f1 * e->inv_fs2, 0.0f, 0.45f);
            e->dt2 = be_clamp(f2 * e->inv_fs2, 0.0f, 0.45f);
        }
        dt1 = e->dt1;
        dt2 = e->dt2;

        fe = env_tick(&e->fe);
        ae = env_tick(&e->ae);
        e->sweep -= e->sweep * sweep_c * 0.2f;
        e->vel_s += (e->vel - e->vel_s) * e->sm_c;   /* velocity and accent glide (~2 ms): stepping the level of a */
        e->acc_s += (e->acc - e->acc_s) * e->sm_c;   /* sounding note (a release tail, a slide) would click */
        velk = 1.0f - p->vel_sens + p->vel_sens * e->vel_s;
        fc = p->cutoff * be_exp2(p->keytrack * (e->pitch - 48.0f) * (1.0f / 12.0f)
                                 + fe * (p->fenv_amt * velk + e->acc_s * 2.5f)
                                 + e->sweep * e->acc_s + e->spread_cut + p->analog * 0.05f * e->drift_f);
        fc = be_clamp(fc, 8.0f, 20000.0f);
        g = be_tan(BE_PI * fc * e->inv_fs2);
        amp = ae * (1.0f - 0.5f * p->vel_sens + 0.5f * p->vel_sens * e->vel_s) * (1.0f + 0.35f * e->acc_s)
              * e->out_gain * makeup;

        for (k = 0; k < 2u; k++) {               /* two 2x samples, each from two 4x samples */
            uint32_t q;
            float x4[2];
            for (q = 0; q < 2u; q++) {
                float t1 = e->ph1, t2 = e->ph2, sp, m, y;
                if (BE_SKIP(4u)) m = 0.25f; else {
                m = p->osc1_level * osc(p->osc1_wave, t1, dt1, pw1, &e->tri1);
                if (p->osc2_level > 0.0f)
                    m += p->osc2_level * osc(p->osc2_wave, t2, dt2, pw2, &e->tri2);
                if (p->sub_level > 0.0f) {       /* the sub: osc 1 divided down, like a flip-flop */
                    sp = ((float)(e->subcnt & (uint32_t)(subn - 1.0f)) + t1) / subn;
                    m += p->sub_level * (p->sub_wave > 0.5f ? be_sin2pi(sp) : square(sp, dt1 / subn, 0.5f));
                }
                if (p->noise_level > 0.0f)
                    m += p->noise_level * be_noise(&e->rng);
                e->ph1 = t1 + dt1; if (e->ph1 >= 1.0f) { e->ph1 -= 1.0f; e->subcnt++; }
                e->ph2 = t2 + dt2; if (e->ph2 >= 1.0f) e->ph2 -= 1.0f;
                }
                {                                /* the oscillators' own bandwidth: a 2-pole low-pass at ~20 kHz
                                                    (Butterworth, TPT) before the ladder. Analog oscillators do not
                                                    hand the filter 88 kHz of harmonics either; without it the
                                                    ladder's input stage folds them back into the audio band. */
                    const float pg = BE_PRE_G, pk = 1.41421356f;
                    const float a1 = 1.0f / (1.0f + pg * (pg + pk)), a2 = pg * a1, a3 = pg * a2;
                    float v3 = m - e->pre2, v1 = a1 * e->pre1 + a2 * v3, v2 = e->pre2 + a2 * e->pre1 + a3 * v3;
                    e->pre1 = 2.0f * v1 - e->pre1; e->pre2 = 2.0f * v2 - e->pre2;
                    m = v2;
                }

                y = BE_SKIP(1u) ? m : ladder(e, m * drive_g * comp, g, r, p->pole2);
                x4[q] = y * amp;
            }
            {                                    /* at 2x: saturation and the soft limit (their harmonics still
                                                    have room above the audio band before the last decimator) */
                float v = BE_SKIP(2u) ? x4[1] : halfband(&e->hb4, BE_HB4_ODD, BE_HB4_NODD, 7u, x4[0], x4[1]);
                if (p->sat > 0.0f)               /* asymmetric soft saturation, unity gain for small signals */
                    v = (be_tanh(v * satg + satb) - sat0) * satn;
                if (v > 0.8f) v = 0.8f + 0.18f * be_tanh((v - 0.8f) * (1.0f / 0.18f));     /* soft limit: < 0.98 */
                else if (v < -0.8f) v = -0.8f - 0.18f * be_tanh((-v - 0.8f) * (1.0f / 0.18f));
                x2[k] = v;
            }
        }
        o = BE_SKIP(2u) ? x2[1] : halfband(&e->hb2, BE_HB_ODD, BE_HB_NODD, 31u, x2[0], x2[1]);


        {                                        /* DC blocker (~3.5 Hz) */
            float y = o - e->dc_x + 0.9995f * e->dc_y;
            e->dc_x = o; e->dc_y = y; o = y;
        }
        if (e->hp_c > 0.0f) {                    /* 2-pole Butterworth high-pass (TPT state variable) */
            const float kq = 1.41421356f;
            float gg = e->hp_c, a1 = 1.0f / (1.0f + gg * (gg + kq)), a2 = gg * a1, a3 = gg * a2;
            float v3 = o - e->hp_s2, v1 = a1 * e->hp_s1 + a2 * v3, v2 = e->hp_s2 + a2 * e->hp_s1 + a3 * v3;
            e->hp_s1 = 2.0f * v1 - e->hp_s1; e->hp_s2 = 2.0f * v2 - e->hp_s2;
            o = o - kq * v1 - v2;
        }
        out[i] = be_clamp(o, -0.999f, 0.999f);
    }
}

/* the entry point: in device builds be_render_hot lives in RAM, too far for a direct call from the flash, so it is
 * reached through a pointer (once per block) */
static void (*volatile be_render_fn)(be_t *, float *, uint32_t) = be_render_hot;
void be_render(be_t *e, float *out, uint32_t n)
{
    be_render_fn(e, out, n);
}
