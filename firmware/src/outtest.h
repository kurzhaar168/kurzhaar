/* SPDX-License-Identifier: GPL-3.0-only */
/* M0 output test: pure sines on the LEFT channel only (right silent), 24-bit, full DAC range, MASTER ignored.
 * PLAY starts the sequence: 10 s silence (noise floor), then 220 Hz at -30 -20 -12 -6 -3 -1 dBFS (3 s each),
 * then 784 Hz at -6 and -1 dBFS, each tone with 10 ms fades and 0.5 s gaps. Built with -DDINGHY_OUTTEST
 * (needs -mcpu=r3: float). The left-only output lets a stereo plug into one balanced input read the full signal.
 * Not part of the product. */
#ifndef OUTTEST_H
#define OUTTEST_H

#define OT_FS 44100u
#define OT_FULL 8388607.0f                      /* 24-bit full scale: what platform.c's DAC word takes */

typedef struct { float sec, c, s, lvl; } ot_seg_t;
static const ot_seg_t OT_SEQ[] = {
    {10.0f, 0, 0, 0},                                      /* silence: the noise floor */
    {0.5f, 0, 0, 0}, {3.0f, 0.999508795f, 0.031339556f, 0.0316228f},   /* 220 Hz -30 dBFS */
    {0.5f, 0, 0, 0}, {3.0f, 0.999508795f, 0.031339556f, 0.1000000f},   /* -20 */
    {0.5f, 0, 0, 0}, {3.0f, 0.999508795f, 0.031339556f, 0.2511886f},   /* -12 */
    {0.5f, 0, 0, 0}, {3.0f, 0.999508795f, 0.031339556f, 0.5011872f},   /* -6 */
    {0.5f, 0, 0, 0}, {3.0f, 0.999508795f, 0.031339556f, 0.7079458f},   /* -3 */
    {0.5f, 0, 0, 0}, {3.0f, 0.999508795f, 0.031339556f, 0.8912509f},   /* -1 */
    {0.5f, 0, 0, 0}, {3.0f, 0.993767919f, 0.111468932f, 0.5011872f},   /* 784 Hz -6 */
    {0.5f, 0, 0, 0}, {3.0f, 0.993767919f, 0.111468932f, 0.8912509f},   /* -1 */
    {2.0f, 0, 0, 0},
};
#define OT_NSEG (sizeof OT_SEQ / sizeof OT_SEQ[0])

static volatile uint32_t ot_run, ot_seg;        /* written by the main loop (start) and the ISR (progress) */
static uint32_t ot_pos, ot_len;
static float ot_x, ot_y, ot_fade;

static void ot_enter(uint32_t k)
{
    ot_seg = k;
    ot_pos = 0;
    ot_len = (uint32_t)(OT_SEQ[k].sec * (float)OT_FS);
    ot_x = 1.0f; ot_y = 0.0f; ot_fade = 0.0f;
}

static void outtest_start(void) { ot_enter(0); ot_run = 1; }

static void outtest_audio(int32_t *out, uint32_t n)   /* out is zero; values are 24-bit DAC words */
{
    uint32_t i;
    if (!ot_run) return;
    for (i = 0; i < n; i++) {
        const ot_seg_t *g = &OT_SEQ[ot_seg];
        float v = 0.0f;
        if (g->lvl > 0.0f) {
            float nx = ot_x * g->c - ot_y * g->s, ny = ot_y * g->c + ot_x * g->s;
            float r = 1.5f - 0.5f * (nx * nx + ny * ny);   /* keep the rotation on the unit circle */
            ot_x = nx * r; ot_y = ny * r;
            if (ot_pos + 441u >= ot_len) { if (ot_fade > 0.0f) ot_fade -= 1.0f / 441.0f; }   /* 10 ms out */
            else if (ot_fade < 1.0f) ot_fade += 1.0f / 441.0f;                               /* 10 ms in */
            if (ot_fade < 0.0f) ot_fade = 0.0f;
            if (ot_fade > 1.0f) ot_fade = 1.0f;
            v = ot_y * g->lvl * ot_fade * OT_FULL;
        }
        out[2u * i] = (int32_t)v;               /* left only */
        out[2u * i + 1u] = 0;
        if (++ot_pos >= ot_len) {
            if (ot_seg + 1u < OT_NSEG) ot_enter(ot_seg + 1u);
            else { ot_run = 0; break; }
        }
    }
}
#endif /* OUTTEST_H */
