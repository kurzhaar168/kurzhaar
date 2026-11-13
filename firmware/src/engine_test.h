/* SPDX-License-Identifier: GPL-3.0-only */
/* Engine test (-DDINGHY_ENGINE): the bass engine with the clock and player (docs/DESIGN.md 5) on the FM-1.
 *   keys, USB/TRS MIDI notes   play the engine live
 *   KNOB1                      patch           KNOB2   internal tempo (1 BPM a detent)
 *   PLAY                       start/stop the test bassline on the internal clock (when no MIDI clock comes)
 *   MIDI clock + Start/Stop/Continue/Song Position from a DAW: the test bassline follows it
 *   screen                     patch, clock (INT/EXT, BPM), step, the audio interrupt's CPU, the measured sample rate
 * Not the product UI: the sequencer's own interface comes in M3.
 *
 * The audio interrupt owns the engine and the clock. The main loop hands it notes and clock messages through
 * queues; clock messages carry their arrival time (fm1_ticks, stamped where USB takes them), which the interrupt
 * turns into a sample position with the measured sample rate. Steps and their note-offs are rendered at their
 * exact samples: the block is cut there. */
#ifndef ENGINE_TEST_H
#define ENGINE_TEST_H
#include "../engine/be.h"
#include "../engine/be_patches.h"
#include "../engine/clk.h"
#include "../engine/player.h"

#define ENG_FS 44117.647f                       /* the I2S clock (~44,117.6 Hz, not 44,100): the engine and the
                                                   internal tempo run at the real rate */
static be_t eng;
static clk_t eng_clk;
static player_cfg_t eng_pcfg = {0.5f, 0.0f};
static float eng_buf[256];
static uint32_t eng_S;                          /* samples since power-on, at this block's start */
static uint32_t eng_T;                          /* fm1_ticks at this block's start */
static float eng_r = ENG_FS / 24000000.0f;      /* samples per fm1_tick, measured */

/* ---- queues from the main loop */
#define ENG_Q 32u                               /* live notes: note | velocity << 8 (velocity 0 = off) */
static uint16_t eng_q[ENG_Q];
static volatile uint32_t eng_qw, eng_qr;
#define ENG_CQ 64u                              /* clock / transport: status | d1 << 8 | d2 << 16, and the time */
static uint32_t eng_cq[ENG_CQ], eng_cq_t[ENG_CQ];
static volatile uint32_t eng_cqw, eng_cqr;
enum { ENG_PLAY = 0x01u, ENG_HALT = 0x02u, ENG_BPM = 0x03u };   /* internal commands in the clock queue */
#define ENG_OQ 32u                              /* to the main loop: notes the sequencer played (MIDI out) */
static uint32_t eng_oq[ENG_OQ];
static volatile uint32_t eng_oqw, eng_oqr;

static volatile uint32_t eng_patch_req, eng_patch_cur = 0xFFu;
static volatile uint32_t eng_t_max, eng_t_sum, eng_t_n;
static volatile int32_t eng_step_shown = -1;

static void eng_note(uint32_t note, uint32_t vel)          /* main loop: a live note */
{
    uint32_t w = eng_qw;
    if (w - eng_qr >= ENG_Q) return;
    eng_q[w & (ENG_Q - 1u)] = (uint16_t)((note & 127u) | (vel & 127u) << 8);
    eng_qw = w + 1u;
}

static void eng_clock_msg(uint32_t st, uint32_t d1, uint32_t d2, uint32_t t)   /* main loop */
{
    uint32_t w = eng_cqw;
    if (w - eng_cqr >= ENG_CQ) return;
    eng_cq[w & (ENG_CQ - 1u)] = st | d1 << 8 | d2 << 16;
    eng_cq_t[w & (ENG_CQ - 1u)] = t;
    eng_cqw = w + 1u;
}

/* ---- the test bassline: the reference one (refs/midi/01_bassline.mid, bar 1), A minor */
typedef struct { uint8_t note, vel, gate8; } tstep_t;   /* gate in 8ths of a step; > 8 = a slide into the next */
static const tstep_t PAT[16] = {
    {33, 127, 4}, {0, 0, 0}, {33, 90, 4}, {45, 90, 4}, {33, 90, 4}, {0, 0, 0}, {36, 127, 9}, {38, 90, 4},
    {33, 90, 4}, {0, 0, 0}, {45, 127, 4}, {31, 90, 13}, {33, 90, 4}, {0, 0, 0}, {40, 90, 4}, {38, 90, 4}};

/* ---- the audio interrupt */
typedef struct { uint32_t off; int32_t step; } eng_ev_t;
static eng_ev_t eng_ev[8];
static uint32_t eng_nev;
static uint32_t eng_off_s[4];                   /* pending note-offs: sample time, note (0 = free) */
static uint8_t eng_off_n[4];

static void eng_emit(void *ctx, uint32_t offset, int32_t step)
{
    (void)ctx;
    if (eng_nev < 8u) { eng_ev[eng_nev].off = offset; eng_ev[eng_nev].step = step; eng_nev++; }
}

static void eng_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t w = eng_oqw;
    if (w - eng_oqr >= ENG_OQ) return;
    eng_oq[w & (ENG_OQ - 1u)] = st | d1 << 8 | d2 << 16;
    eng_oqw = w + 1u;
}

static void eng_all_off(void)
{
    uint32_t k;
    for (k = 0; k < 4u; k++)
        if (eng_off_n[k]) { eng_out(0x80u, eng_off_n[k], 0); eng_off_n[k] = 0; }
    be_all_off(&eng);
}

static void eng_play_step(int32_t step, uint32_t at)        /* at: the step's sample time */
{
    const tstep_t *p = &PAT[(uint32_t)step & 15u];
    uint32_t k;
    eng_step_shown = step;
    if (!p->note) return;
    be_note_on(&eng, p->note, p->vel);
    eng_out(0x90u, p->note, p->vel);
    for (k = 0; k < 4u; k++)
        if (!eng_off_n[k]) {
            eng_off_n[k] = p->note;
            eng_off_s[k] = at + (uint32_t)((float)p->gate8 * 0.125f * clk_step_len(&eng_clk) + 0.5f);
            break;
        }
}

static void eng_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), i, pos, e, k, dt;
    if (n > 256u) n = 256u;
    if (eng_T) {                                /* the sample rate, from the blocks (each exactly 256 samples) */
        uint32_t d = t0 - eng_T;
        if (d > 100000u && d < 180000u)
            eng_r += 0.002f * ((float)n / (float)d - eng_r);
    }
    eng_T = t0;
    if (eng_patch_req != eng_patch_cur) {
        eng_patch_cur = eng_patch_req;
        BE_PATCH_FN[eng_patch_cur](&eng.p);
        be_patch_changed(&eng);
    }
    while (eng_qr != eng_qw) {                  /* live notes: at the block's start */
        uint32_t v = eng_q[eng_qr & (ENG_Q - 1u)];
        eng_qr++;
        if (v >> 8) be_note_on(&eng, (uint8_t)(v & 127u), (uint8_t)(v >> 8));
        else be_note_off(&eng, (uint8_t)(v & 127u));
    }
    while (eng_cqr != eng_cqw) {                /* clock and transport, each at its arrival sample */
        uint32_t m = eng_cq[eng_cqr & (ENG_CQ - 1u)], tt = eng_cq_t[eng_cqr & (ENG_CQ - 1u)], st = m & 0xFFu;
        float off = (float)(int32_t)(tt - t0) * eng_r;        /* (negative: it came during the last block) */
        int32_t oi = (int32_t)off;
        if ((float)oi > off) oi--;
        eng_cqr++;
        switch (st) {
        case 0xF8u: clk_ext_tick(&eng_clk, eng_S + (uint32_t)oi, off - (float)oi); break;
        case 0xFAu: eng_all_off(); clk_ext_start(&eng_clk); break;
        case 0xFBu: clk_ext_continue(&eng_clk); break;
        case 0xFCu: clk_ext_stop(&eng_clk); eng_all_off(); break;
        case 0xF2u: clk_ext_spp(&eng_clk, ((m >> 8) & 127u) | ((m >> 16) & 127u) << 7); break;
        case ENG_PLAY: if (!eng_clk.ext || !eng_clk.have_ref) clk_play(&eng_clk, eng_S); break;
        case ENG_HALT: clk_stop(&eng_clk); eng_all_off(); break;
        case ENG_BPM: clk_set_bpm(&eng_clk, m >> 8, eng_S); break;
        default: break;
        }
    }
    clk_ext_poll(&eng_clk, eng_S);
    eng_nev = 0;
    player_block(&eng_clk, &eng_pcfg, eng_S, n, eng_emit, 0);

    /* render, cut at each step and each note-off due in this block (note-offs first at the same sample) */
    for (pos = 0, e = 0; pos < n;) {
        uint32_t next = n, which = 0xFFu;
        for (k = 0; k < 4u; k++)
            if (eng_off_n[k]) {
                int32_t o = (int32_t)(eng_off_s[k] - eng_S);
                uint32_t oo = o < (int32_t)pos ? pos : (uint32_t)o;
                if (oo < next || (oo == next && which >= 0x80u)) { next = oo; which = k; }
            }
        if (e < eng_nev && eng_ev[e].off < next) { next = eng_ev[e].off; which = 0x80u; }
        if (next > pos) { be_render(&eng, eng_buf + pos, next - pos); pos = next; }
        if (which == 0xFFu) break;
        if (which == 0x80u) {
            eng_play_step(eng_ev[e].step, eng_S + eng_ev[e].off);
            e++;
        } else {
            be_note_off(&eng, eng_off_n[which]);
            eng_out(0x80u, eng_off_n[which], 0);
            eng_off_n[which] = 0;
        }
    }
    for (i = 0; i < n; i++) {
        int32_t s = (int32_t)(eng_buf[i] * 32767.0f);
        out[2u * i] = s;
        out[2u * i + 1u] = s;
    }
    eng_S += n;
    dt = fm1_ticks() - t0;
    if (dt > eng_t_max) eng_t_max = dt;
    eng_t_sum += dt;
    eng_t_n++;
}

/* ---- the main loop */
static void eng_init(void)
{
    be_init(&eng, ENG_FS);
    clk_init(&eng_clk, ENG_FS);
}

static char *eng_num(char *p, uint32_t v, uint32_t dec)   /* v, or v / 10 with one decimal if dec */
{
    char t[12];
    int i = 0;
    uint32_t w = dec ? v / 10u : v;
    do { t[i++] = (char)('0' + w % 10u); w /= 10u; } while (w);
    while (i) *p++ = t[--i];
    if (dec) { *p++ = '.'; *p++ = (char)('0' + v % 10u); }
    return p;
}

static void eng_ui(uint32_t buttons)            /* buttons: pressed since the last call */
{
    static uint32_t t_last, t_clk, shown = 0xFFu, bpm10 = 1200u, playing;
    int32_t e = fm1_enc_take(ENC_KNOB1);
    while (eng_oqr != eng_oqw) {                /* the sequencer's notes out over USB */
        uint32_t m = eng_oq[eng_oqr & (ENG_OQ - 1u)];
        eng_oqr++;
        midi_send(m & 0xFFu, (m >> 8) & 0xFFu, (m >> 16) & 0xFFu);
    }
    if (e) {
        int32_t p = (int32_t)eng_patch_req + e;
        eng_patch_req = (uint32_t)(p < 0 ? 0 : p >= (int32_t)BE_NPATCH ? (int32_t)BE_NPATCH - 1 : p);
    }
    if ((e = fm1_enc_take(ENC_KNOB2)) != 0) {
        int32_t b = (int32_t)bpm10 + 10 * e;
        bpm10 = (uint32_t)(b < 400 ? 400 : b > 2400 ? 2400 : b);
        eng_clock_msg(ENG_BPM, bpm10, 0, 0);
    }
    if (buttons & (1u << BTN_PLAY)) {
        playing = !eng_clk.running;
        eng_clock_msg(playing ? ENG_PLAY : ENG_HALT, 0, 0, 0);
    }
    if (eng_patch_req != shown) {
        shown = eng_patch_req;
        draw_text_box(0, 18, 240, &FONT_S, BE_PATCH_NAME[shown], C_ACC, 1);
    }
    if (fm1_ms - t_clk >= 250u) {               /* the clock: source, tempo, step */
        char s[40], *p = s;
        const char *a = eng_clk.ext && eng_clk.have_ref ? "EXT " : "INT ";
        uint32_t b = (uint32_t)(clk_tempo(&eng_clk) * 10.0f + 0.5f);
        t_clk = fm1_ms;
        while (*a) *p++ = *a++;
        p = eng_num(p, b, 1);
        a = eng_clk.running ? " BPM  step " : " BPM  stopped";
        while (*a) *p++ = *a++;
        if (eng_clk.running) p = eng_num(p, (uint32_t)(eng_step_shown < 0 ? 0 : eng_step_shown) % 16u + 1u, 0);
        *p = 0;
        draw_text_box(0, 120, 240, &FONT_S, s, C_WHITE, 1);
    }
    if (fm1_ms - t_last >= 1000u) {             /* the audio interrupt's cost, the measured sample rate */
        char s[40] = "CPU ", *p = s + 4;
        uint32_t nn = eng_t_n, avg = nn ? eng_t_sum / nn : 0u, mx = eng_t_max;
        uint32_t pa = avg * 1000u / (24u * 5805u), pm = mx * 1000u / (24u * 5805u);
        uint32_t fs10 = (uint32_t)(eng_r * 240000000.0f + 0.5f);   /* Hz x 10 */
        eng_t_sum = 0; eng_t_n = 0; eng_t_max = 0;
        t_last = fm1_ms;
        p = eng_num(p, pa, 1); *p++ = '/'; p = eng_num(p, pm, 1);
        { const char *a = "%  fs "; while (*a) *p++ = *a++; }
        p = eng_num(p, fs10, 1);
        *p = 0;
        draw_text_box(0, 196, 240, &FONT_S, s, pm > 800u ? RGB(255, 80, 60) : C_WHITE, 1);
    }
}
#endif
