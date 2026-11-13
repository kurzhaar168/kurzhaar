/* SPDX-License-Identifier: GPL-3.0-only */
/* Engine test (-DDINGHY_ENGINE): the FM-1's keys and USB/TRS MIDI notes play the bass engine; KNOB1 picks the patch;
 * the screen shows what the audio interrupt costs (average and the peak of the last second). Not the product UI.
 *
 * Notes go from the main loop to the audio interrupt through a small queue: the interrupt owns the engine, the main
 * loop never touches it (no half-applied note while a block renders). */
#ifndef ENGINE_TEST_H
#define ENGINE_TEST_H
#include "../engine/be.h"
#include "../engine/be_patches.h"

static be_t eng;
static float eng_buf[256];

#define ENG_Q 32u                               /* note events: note | velocity << 8 (velocity 0 = off) */
static uint16_t eng_q[ENG_Q];
static volatile uint32_t eng_qw, eng_qr;
static volatile uint32_t eng_patch_req, eng_patch_cur = 0xFFu;
static volatile uint32_t eng_t_max, eng_t_sum, eng_t_n;   /* audio interrupt ticks (24 MHz): peak, sum, count */

static void eng_note(uint32_t note, uint32_t vel)          /* main loop */
{
    uint32_t w = eng_qw;
    if (w - eng_qr >= ENG_Q) return;            /* full: drop (a burst of 32 notes in 5.8 ms) */
    eng_q[w & (ENG_Q - 1u)] = (uint16_t)((note & 127u) | (vel & 127u) << 8);
    eng_qw = w + 1u;                            /* publish after the entry is written (one writer each side) */
}

static void eng_init(void)
{
    be_init(&eng, 44100.0f);
}

static void eng_audio(int32_t *out, uint32_t n)            /* the audio interrupt */
{
    uint32_t t0 = fm1_ticks(), i, dt;
    if (eng_patch_req != eng_patch_cur) {
        eng_patch_cur = eng_patch_req;
        BE_PATCH_FN[eng_patch_cur](&eng.p);
        be_patch_changed(&eng);
    }
    while (eng_qr != eng_qw) {
        uint32_t v = eng_q[eng_qr & (ENG_Q - 1u)];
        eng_qr++;
        if (v >> 8) be_note_on(&eng, (uint8_t)(v & 127u), (uint8_t)(v >> 8));
        else be_note_off(&eng, (uint8_t)(v & 127u));
    }
    if (n > 256u) n = 256u;
    be_render(&eng, eng_buf, n);
    for (i = 0; i < n; i++) {
        int32_t s = (int32_t)(eng_buf[i] * 32767.0f);
        out[2u * i] = s;
        out[2u * i + 1u] = s;
    }
    dt = fm1_ticks() - t0;
    if (dt > eng_t_max) eng_t_max = dt;
    eng_t_sum += dt;
    eng_t_n++;
}

static void eng_ui(void)                         /* main loop: KNOB1 = patch; the CPU line, every second */
{
    static uint32_t t_last, shown = 0xFFu;
    int32_t e = fm1_enc_take(ENC_KNOB1);
    if (e) {
        int32_t p = (int32_t)eng_patch_req + e;
        eng_patch_req = (uint32_t)(p < 0 ? 0 : p >= (int32_t)BE_NPATCH ? (int32_t)BE_NPATCH - 1 : p);
    }
    if (eng_patch_req != shown) {
        shown = eng_patch_req;
        draw_text_box(0, 18, 240, &FONT_S, BE_PATCH_NAME[shown], C_ACC, 1);
    }
    if (fm1_ms - t_last >= 1000u) {
        char s[32] = "CPU avg ", *p = s + 8;
        uint32_t n = eng_t_n, avg = n ? eng_t_sum / n : 0u, mx = eng_t_max, v, k;
        uint32_t pa = avg * 1000u / (24u * 5805u), pm = mx * 1000u / (24u * 5805u);   /* in 0.1 % */
        eng_t_sum = 0; eng_t_n = 0; eng_t_max = 0;
        t_last = fm1_ms;
        for (k = 0; k < 2u; k++) {
            v = k ? pm : pa;
            if (v >= 1000u) *p++ = (char)('0' + v / 1000u % 10u);
            if (v >= 100u) *p++ = (char)('0' + v / 100u % 10u);
            *p++ = (char)('0' + v / 10u % 10u); *p++ = '.'; *p++ = (char)('0' + v % 10u); *p++ = '%';
            if (!k) { const char *m = "  max "; while (*m) *p++ = *m++; }
        }
        *p = 0;
        draw_text_box(0, 196, 240, &FONT_S, s, pm > 800u ? RGB(255, 80, 60) : C_WHITE, 1);
    }
}
#endif
