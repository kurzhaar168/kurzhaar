/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* The splash at power-on: a synth-sunset sky fades up, Lucca (the dog Kurzhaar is named for) rises into the
 * frame, the KURZHAAR logo wipes in and a glint runs across it, "der Basshund" fades in. ~4 s; any key or button skips it.
 * The art (three layers, smooth edges): splash_art.h, made by tools/gen_splash_art.py (from assets/splash/dog.png). */
#ifndef SPLASH_H
#define SPLASH_H
#include "splash_art.h"

#define SUB_Y 56                                 /* the subtitle's row */

static inline uint16_t spl_blend(uint16_t b, uint16_t f, uint32_t a)   /* f over b, a 0..255 */
{
    uint32_t w = a + (a >> 7), iw = 256u - w;   /* 0..256 */
    uint32_t r = (((f >> 11) & 31u) * w + ((b >> 11) & 31u) * iw) >> 8;
    uint32_t g = (((f >> 5) & 63u) * w + ((b >> 5) & 63u) * iw) >> 8;
    uint32_t bl = ((f & 31u) * w + (b & 31u) * iw) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static inline uint16_t spl_dim(uint16_t c, uint32_t k)   /* k 0..256 */
{
    return (uint16_t)(((((c >> 11) & 31u) * k >> 8) << 11) | ((((c >> 5) & 63u) * k >> 8) << 5) | ((c & 31u) * k >> 8));
}

/* rows y0..y0+h of the frame: the sky at `bright`, Lucca `rise` px below her place, the logo revealed to `wipe` (x)
 * with the glint at `glint` (x, or off), the subtitle at `sub` (0..256) */
static void spl_band(int32_t y0, int32_t h, uint32_t bright, int32_t rise, int32_t wipe, int32_t glint, uint32_t sub)
{
    static const uint16_t SUBCOL = RGB(255, 235, 204);
    int32_t y, x;
    cv_begin(240, (uint32_t)h, C_BLACK);
    for (y = 0; y < h; y++) {
        int32_t sy = y0 + y, dy = sy - ART_DOG_Y - rise;
        const uint16_t *bgr = &ART_BG[(uint32_t)sy * 240u];
        for (x = 0; x < 240; x++) {
            uint16_t c = bgr[x];
            if (bright < 256u) c = spl_dim(c, bright);
            if (dy >= 0 && dy < (int32_t)ART_DOG_H && x >= ART_DOG_X && x < ART_DOG_X + (int32_t)ART_DOG_W) {
                uint32_t i = (uint32_t)dy * ART_DOG_W + (uint32_t)(x - ART_DOG_X), a = ART_DOG_A[i];
                if (a) c = spl_blend(c, ART_DOG[i], a);
            }
            if (sy < (int32_t)ART_LOGO_H && x < wipe) {
                uint32_t i = (uint32_t)sy * ART_LOGO_W + (uint32_t)x, a = ART_LOGO_A[i];
                if (a) {
                    int32_t d = x + sy / 2 - glint;               /* the glint: a slanted bright band */
                    uint16_t f = ART_LOGO[i];
                    if (d > -8 && d < 8) f = spl_blend(f, 0xFFFFu, (uint32_t)(150 - (d < 0 ? -d : d) * 18));
                    if (wipe - x < 12) a = a * (uint32_t)(wipe - x) / 12u;   /* a soft leading edge */
                    c = spl_blend(c, f, a);
                }
            }
            if (sub && sy >= SUB_Y && sy < SUB_Y + (int32_t)ART_SUB_H) {
                uint32_t a = ART_SUB_A[(uint32_t)(sy - SUB_Y) * ART_SUB_W + (uint32_t)x];
                if (a) c = spl_blend(c, SUBCOL, a * sub >> 8);
            }
            cv_pset(x, y, c);
        }
    }
    cv_blit(0, (uint32_t)y0);
}

static void splash(void)
{
    uint32_t t0 = fm1_ms, last_t = 0, rel, shown_hash = 0xFFFFFFFFu;
    for (;;) {
        uint32_t t, bright, sub, hash;
        int32_t rise, wipe, glint;
        platform_poll();
        t = fm1_ms - t0;
        if (t > 4000u) break;
        if (fm1_input_edges(&rel) || fm1_input_note_edges() || fm1_in.notes) break;   /* any key or button: skip */
        if (t - last_t < 33u && t) continue;     /* ~30 frames a second, at most */
        last_t = t;
        bright = t >= 600u ? 256u : t * 256u / 600u;                                 /* the sky fades up */
        if (t < 300u) rise = 240;
        else if (t < 1400u) {                    /* she rises, easing out: (1 - x)^3 */
            int32_t f = 1100 - ((int32_t)t - 300);
            rise = (240 - ART_DOG_Y) * f / 1100 * f / 1100 * f / 1100;
        } else rise = 0;
        wipe = t < 1300u ? 0 : t < 1800u ? (int32_t)(t - 1300u) * 252 / 500 : 252;   /* the logo, left to right */
        glint = t >= 1800u && t < 2300u ? (int32_t)(t - 1800u) * 320 / 500 - 40 : -100;
        sub = t < 2000u ? 0u : t < 2400u ? (t - 2000u) * 256u / 400u : 256u;
        hash = bright ^ (uint32_t)rise << 9 ^ (uint32_t)wipe << 17 ^ (uint32_t)(glint + 100) << 22 ^ sub << 3;
        if (hash == shown_hash) continue;        /* nothing moved: leave the screen as it is */
        shown_hash = hash;
        spl_band(0, 120, bright, rise, wipe, glint, sub);
        spl_band(120, 120, bright, rise, wipe, glint, sub);
    }
    lcd_fill(0, 0, 240, 240, C_BLACK);
}
#endif
