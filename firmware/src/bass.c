/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* Lucca: das Bassmaschine. The bass machine (docs/DESIGN.md): an analog-style mono bass synth with a step sequencer, locked to a DAW's clock
 * or its own tempo, in a key and scale. Build: DINGHY_APP=bass DINGHY_DEFS="-mcpu=r3 -DBE_RAM_CODE" ./build.sh
 * The control map is docs/DESIGN.md 6.3 (approved from the clickable mockup).
 *
 *   firmware/engine/   the sound (be), the clock (clk), the player (player), the sequencer's data (seq): no hardware
 *   this file          the audio interrupt (engine + clock + pattern playback), the panel, the screen, storage
 *
 * Threads: the audio interrupt owns the engine and the clock. The main loop owns the interface and edits the
 * patterns and the patch; the interrupt reads them (a step is 4 bytes, written whole; the patch is copied at a block
 * start when its generation number moved). Notes, clock and transport go to the interrupt through queues. */
#define CV_MAX (240u * 150u)                     /* the screen's middle: one canvas */
static void app_idle(void);
#define LCD_IDLE app_idle
#define APP_NAME "LUCCA"                        /* Lucca: das Bassmaschine */
#define STORE_MAGIC 0x53534142u                  /* "BASS": this firmware's own store (store.h) */
#define APP_USB_MANUFACTURER "JazzMasterT"       /* USB: the maker's name (the MIDI port keeps the platform's name) */
#define FM1_OUT_SHIFT 8                          /* the DAC's full range: +6 dB on Dinghy (our limiter holds the peaks
                                                    below full scale; the output was measured clean to -1 dBFS) */
#include "platform.c"
#ifdef DINGHY_HOST
#include "../engine/be.c"                        /* the Mac: one file */
#include "../engine/clk.c"
#include "../engine/player.c"
#include "../engine/seq.c"
#include "../engine/verb.c"
#else
#include "../engine/be.h"                        /* the FM-1: their own objects, -O2 (tools/build.py) */
#include "../engine/be_math.h"                   /* (the screen's curves use the engine's maths) */
#include "../engine/clk.h"
#include "../engine/player.h"
#include "../engine/seq.h"
#include "../engine/verb.h"
#endif
#include "../engine/be_patches.h"
#include <stddef.h>

#define FS_AUDIO 44117.647f                      /* the I2S clock, not 44,100 */
#define LUCCA_VERSION APP_VER                    /* this firmware's version (the ABOUT page): build.sh --release X.Y */
#define LUCCA_AUTHOR "JazzMasterT"
static const uint8_t RATE_TPS[6] = {24u, 12u, 8u, 6u, 4u, 3u};   /* MIDI clock ticks a step */
static const char *const RATE_NAME[6] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

/* ======================================================================= shared state */
static seq_pat_t pats[SEQ_NPAT];
static seq_key_t key = {9, 0, 0, 0};             /* A minor */
static volatile uint32_t cur_pat, next_pat = 0xFFu;
#define NPATCH 32u
static be_patch_t patches[NPATCH];
static be_patch_t ui_patch;                      /* what the knobs edit; the interrupt takes a copy when it changes */
static volatile uint32_t patch_gen;
static uint32_t patch_no;
static struct {
    uint16_t bpm10;                              /* internal tempo, 0.1 BPM */
    uint8_t swing;                               /* 50..75 % */
    uint8_t rate;                                /* the step: 0 1/4, 1 1/8, 2 1/8T, 3 1/16, 4 1/16T, 5 1/32 */
    uint8_t clock;                               /* 0 auto, 1 internal only, 2 external only */
    uint8_t midi_ch;                             /* 0 omni (out: 1), 1..16 */
    uint8_t notes_out;                           /* the sequencer's notes over USB */
    uint8_t cc;                                  /* cutoff CC 74, resonance CC 71: 0 off, 1 in, 2 out, 3 in + out */
    int8_t offset10;                             /* timing offset, 0.1 ms: + plays early */
    int8_t oct;                                  /* the keyboard's octave */
} glo = {1200u, 54u, 3u, 0u, 0u, 1u, 1u, 0, 0};

/* ======================================================================= audio interrupt */
static be_t eng;
static clk_t clk;
static player_cfg_t pcfg = {0.5f, 0.0f};
static float rbuf[256];                          /* the engine's output for one block */
static float sbuf[512];                          /* the same in stereo, with the reverb */
static verb_t verb;
static uint32_t aS, aT;                          /* samples at this block's start; fm1_ticks then */
static float a_r = FS_AUDIO / 24000000.0f;       /* samples per fm1_tick, measured */
static uint32_t a_patch_gen = 0xFFFFFFFFu;

#define LQ 32u                                   /* live notes: note | velocity << 8 (0 = off) */
static uint16_t lq[LQ];
static volatile uint32_t lq_w, lq_r;
#define CQ 64u                                   /* clock / transport: status | d1 << 8 | d2 << 16, with the time */
static uint32_t cq[CQ], cq_t[CQ];
static volatile uint32_t cq_w, cq_r;
enum { CMD_PLAY = 0x01u, CMD_HALT = 0x02u };
#define OQ 64u                                   /* to the main loop: MIDI out */
static uint32_t oq[OQ];
static volatile uint32_t oq_w, oq_r;

static volatile int32_t play_idx = -1;           /* the step playing (index in the pattern) */
static volatile uint32_t play_gen;               /* moves on every step */
static volatile uint32_t a_running;
static uint32_t ppos;                            /* the pattern's position */
static uint32_t off_s[4];                        /* pending note-offs: sample time, note (0 = free) */
static uint8_t off_n[4];
static uint8_t seq_note_on;                      /* the sequencer's sounding note (0 = none), its off slot */
static int8_t seq_note_slot = -1;
static volatile uint32_t isr_t_max, isr_t_sum, isr_t_n;
static uint32_t cpu_avg10, cpu_max10;            /* the last second's load, in 0.1 % (the MIDI page shows it) */

static void live_note(uint32_t note, uint32_t vel)          /* main loop */
{
    uint32_t w = lq_w;
    if (w - lq_r >= LQ) return;
    lq[w & (LQ - 1u)] = (uint16_t)((note & 127u) | (vel & 127u) << 8);
    lq_w = w + 1u;
}
static void clock_msg(uint32_t m, uint32_t t)                 /* main loop */
{
    uint32_t w = cq_w;
    if (w - cq_r >= CQ) return;
    cq[w & (CQ - 1u)] = m;
    cq_t[w & (CQ - 1u)] = t;
    cq_w = w + 1u;
}
static void out_q(uint32_t st, uint32_t d1, uint32_t d2)      /* the interrupt */
{
    uint32_t w = oq_w;
    if (!glo.notes_out || w - oq_r >= OQ) return;
    oq[w & (OQ - 1u)] = st | d1 << 8 | d2 << 16;
    oq_w = w + 1u;
}
static uint32_t out_ch(void) { return glo.midi_ch ? glo.midi_ch - 1u : 0u; }

static void a_all_off(void)
{
    uint32_t k;
    for (k = 0; k < 4u; k++)
        if (off_n[k]) { out_q(0x80u | out_ch(), off_n[k], 0); off_n[k] = 0; }
    seq_note_on = 0;
    seq_note_slot = -1;
    be_all_off(&eng);
}

static void a_play_step(uint32_t at)             /* a step starts at sample `at` */
{
    static const float GATE[4] = {0.25f, 0.5f, 0.75f, 0.97f};
    const seq_pat_t *p;
    seq_step_t st;
    float len = clk_step_len(&clk);
    uint32_t idx, note, vel, k;
    if (next_pat != 0xFFu && clk.step % (int32_t)(96u / clk.tps) == 0) {   /* a queued pattern: at the bar line */
        cur_pat = next_pat;
        next_pat = 0xFFu;
        ppos = 0;
    }
    p = &pats[cur_pat];
    idx = ppos % (p->len ? p->len : 1u);
    ppos++;
    play_idx = (int32_t)idx;
    play_gen++;
    st = p->s[idx];
    if (!(st.flags & SEQ_ON)) return;
    if (st.flags & SEQ_TIE) {                    /* hold the note that sounds into this step */
        if (seq_note_on && seq_note_slot >= 0 && off_n[seq_note_slot] == seq_note_on)
            off_s[seq_note_slot] = at + (uint32_t)(GATE[seq_gate(&st)] * len);
        return;
    }
    note = (uint32_t)seq_note(&key, &st) & 127u;
    vel = st.flags & SEQ_ACC ? 127u : 90u;       /* 100 and up is an accent to the engine */
    for (k = 0; k < 4u; k++)                     /* a slide into the same note: its pending off must not end this one */
        if (off_n[k] == note) { out_q(0x80u | out_ch(), note, 0); off_n[k] = 0; }
    be_note_on(&eng, (uint8_t)note, (uint8_t)vel);
    out_q(0x90u | out_ch(), note, vel);
    for (k = 0; k < 4u; k++)
        if (!off_n[k]) {
            off_n[k] = (uint8_t)note;
            off_s[k] = at + (uint32_t)((st.flags & SEQ_SLIDE ? 1.5f : GATE[seq_gate(&st)]) * len);   /* a slide ends
                                                         after the next step starts, at any swing (max: half a step) */
            seq_note_on = (uint8_t)note;
            seq_note_slot = (int8_t)k;
            break;
        }
}

typedef struct { uint32_t off; int32_t step; } ev_t;
static ev_t a_ev[8];
static uint32_t a_nev;
static void a_emit(void *ctx, uint32_t offset, int32_t step)
{
    (void)ctx;
    if (a_nev < 8u) { a_ev[a_nev].off = offset; a_ev[a_nev].step = step; a_nev++; }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), i, pos, e, k, dt;
    if (n > 256u) n = 256u;
    if (aT) {                                    /* the sample rate from the blocks (each exactly n samples) */
        uint32_t d = t0 - aT;
        if (d > 100000u && d < 180000u) a_r += 0.002f * ((float)n / (float)d - a_r);
    }
    aT = t0;
    if (a_patch_gen != patch_gen) {              /* the knobs moved: take the patch */
        a_patch_gen = patch_gen;
        eng.p = ui_patch;
        be_patch_changed(&eng);
        {
            verb_cfg_t vc = {eng.p.verb_mix, eng.p.verb_decay, eng.p.verb_tone, eng.p.verb_lowcut};
            verb_set(&verb, &vc);
        }
    }
    if (clk.bpm10 != glo.bpm10) clk_set_bpm(&clk, glo.bpm10, aS);
    pcfg.swing = (float)glo.swing * 0.01f;
    pcfg.lead_ms = (float)glo.offset10 * 0.1f;
    while (lq_r != lq_w) {                       /* live notes: at the block's start */
        uint32_t v = lq[lq_r & (LQ - 1u)];
        lq_r++;
        if (v >> 8) be_note_on(&eng, (uint8_t)(v & 127u), (uint8_t)(v >> 8));
        else be_note_off(&eng, (uint8_t)(v & 127u));
    }
    while (cq_r != cq_w) {                       /* clock and transport, each at its arrival sample */
        uint32_t m = cq[cq_r & (CQ - 1u)], tt = cq_t[cq_r & (CQ - 1u)], st = m & 0xFFu;
        float off = (float)(int32_t)(tt - t0) * a_r;
        int32_t oi = (int32_t)off;
        if ((float)oi > off) oi--;
        cq_r++;
        if (st >= 0xF2u && glo.clock == 1u) continue;          /* internal only: ignore the DAW */
        switch (st) {
        case 0xF8u: clk_ext_tick(&clk, aS + (uint32_t)oi, off - (float)oi); break;
        case 0xFAu: a_all_off(); clk_ext_start(&clk); ppos = 0; break;
        case 0xFBu: clk_ext_continue(&clk); break;
        case 0xFCu: clk_ext_stop(&clk); a_all_off(); break;
        case 0xF2u: clk_ext_spp(&clk, ((m >> 8) & 127u) | ((m >> 16) & 127u) << 7);
                    ppos = (((m >> 8) & 127u) | ((m >> 16) & 127u) << 7); break;
        case CMD_PLAY: if (glo.clock != 2u && !(clk.ext && clk.have_ref)) { ppos = 0; clk_play(&clk, aS); } break;
        case CMD_HALT: if (!(clk.ext && clk.have_ref)) { clk_stop(&clk); a_all_off(); } break;
        default: break;
        }
    }
    clk_ext_poll(&clk, aS);
    if (clk.tps != RATE_TPS[glo.rate % 6u]) clk_set_rate(&clk, RATE_TPS[glo.rate % 6u]);
    {                                            /* the tempo, and where we are in the song (the synced LFOs lock to it) */
        float pos = -1.0f;
        if (clk.running) {
            float len = clk_step_len(&clk), steps = (float)clk.step - clk_step_offset(&clk, clk.step, aS) / (len > 1.0f ? len : 1.0f);
            pos = steps * (float)clk.tps * (1.0f / 24.0f);
            if (pos < 0.0f) pos = 0.0f;
        }
        be_set_tempo(&eng, clk_tempo(&clk), pos);
    }
    a_nev = 0;
    player_block(&clk, &pcfg, aS, n, a_emit, 0);
    a_running = clk.running;

    for (pos = 0, e = 0; pos < n;) {             /* render, cut at each step and each note-off due in this block */
        uint32_t next = n, which = 0xFFu;
        for (k = 0; k < 4u; k++)
            if (off_n[k]) {
                int32_t o = (int32_t)(off_s[k] - aS);
                uint32_t oo = o < (int32_t)pos ? pos : (uint32_t)o;
                if (oo < next) { next = oo; which = k; }
            }
        if (e < a_nev && a_ev[e].off < next) { next = a_ev[e].off; which = 0x80u; }   /* offs first at a tie */
        if (next > pos) { be_render(&eng, rbuf + pos, next - pos); pos = next; }
        if (which == 0xFFu) break;
        if (which == 0x80u) { a_play_step(aS + a_ev[e].off); e++; }
        else {
            be_note_off(&eng, off_n[which]);
            out_q(0x80u | out_ch(), off_n[which], 0);
            if ((int32_t)which == seq_note_slot) { seq_note_on = 0; seq_note_slot = -1; }
            off_n[which] = 0;
        }
    }
    {                                            /* an LFO on the reverb's mix */
        float vm = eng.p.verb_mix + eng.mod_verb;
        verb.cfg.mix = vm < 0.0f ? 0.0f : vm > 1.0f ? 1.0f : vm;
    }
    if (verb.cfg.mix > 0.001f) {                 /* the reverb: stereo around the dry bass */
        for (i = 0; i < n; i++) sbuf[2u * i] = sbuf[2u * i + 1u] = rbuf[i];
        verb_process(&verb, rbuf, sbuf, n);
        for (i = 0; i < 2u * n; i++) {
            float x = sbuf[i];
            out[i] = (int32_t)((x > 0.999f ? 0.999f : x < -0.999f ? -0.999f : x) * 32767.0f);
        }
    } else
        for (i = 0; i < n; i++) {
            int32_t s = (int32_t)(rbuf[i] * 32767.0f);
            out[2u * i] = s;
            out[2u * i + 1u] = s;
        }
    aS += n;
    dt = fm1_ticks() - t0;
    if (dt > isr_t_max) isr_t_max = dt;
    isr_t_sum += dt;
    isr_t_n++;
}

/* ======================================================================= parameters */
enum { T_LIN, T_LOG, T_ENUM, T_APP };
enum { F_PCT, F_OCT, F_HZ, F_TIME, F_CENT, F_SEMI, F_DB, F_WAVE, F_SUBOCT, F_GMODE, F_POLES, F_LFODEST, F_LFOSHAPE,
       F_SYNC, F_RETRIG, F_BIPCT, F_INT };
typedef struct { const char *label; uint8_t type, fmt; float min, max, step; uint16_t off; } prm_t;
#define PF(lbl, fld, ty, fm, mn, mx, st) {lbl, ty, fm, mn, mx, st, (uint16_t)offsetof(be_patch_t, fld)}
enum { A_ROOT, A_SCALE, A_KEYS, A_TRANSP, A_TEMPO, A_SWING, A_RATE, A_OFFSET, A_MIDICH, A_CLOCK, A_NOTESOUT, A_CC,
       A_L1RATE, A_L2RATE,
       A_ST_NOTE, A_ST_OCT, A_ST_GATE, A_ST_ACCSLD, A_PT_LEN, A_PT_ROT, A_LOWCUT };
#define PA(lbl, id) {lbl, T_APP, 0, 0.0f, 0.0f, 0.0f, (uint16_t)(id)}
enum {                                           /* the parameters, by name (indices into PRM) */
    P_ENVAMT, P_FDEC, P_ACCENT, P_GLIDE, P_WAVE, P_PW, P_OSC2, P_DETUNE, P_SEMI, P_SUB, P_SUBOCT, P_NOISE,
    P_CUTOFF, P_RES, P_KEYTRK, P_FA, P_FS, P_FR, P_AA, P_AD, P_AS, P_AR,
    P_L1RATE, P_L1SHAPE, P_L1DEST, P_L1DEPTH, P_L1SYNC, P_L1RETRIG, P_L2RATE, P_L2SHAPE, P_L2DEST, P_L2DEPTH, P_L2SYNC, P_L2RETRIG,
    P_GMODE, P_VELO, P_DRIVE, P_SAT, P_ANALOG, P_LEVEL, P_BCOMP, P_POLES, P_LOWCUT,
    P_VMIX, P_VDECAY, P_VTONE, P_VLOWCUT,
    P_ROOT, P_SCALE, P_KEYS, P_TRANSP, P_TEMPO, P_SWING, P_RATE, P_OFFSET, P_MIDICH, P_CLOCK, P_NOTESOUT, P_CC,
    P_ST_NOTE, P_ST_OCT, P_ST_GATE, P_ST_ACCSLD, P_PT_LEN, P_PT_ROT, P_NONE
};
static const prm_t PRM[] = {
    PF("ENV AMT", fenv_amt, T_LIN, F_OCT, -4.0f, 8.0f, 0.1f), PF("DECAY", f_d, T_LOG, F_TIME, 0.005f, 4.0f, 0.125f),
    PF("ACCENT", accent, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("GLIDE", glide, T_LOG, F_TIME, 0.001f, 1.0f, 0.125f),
    PF("WAVE", osc1_wave, T_ENUM, F_WAVE, 0.0f, 2.0f, 1.0f), PF("PW", osc1_pw, T_LIN, F_PCT, 0.05f, 0.95f, 0.01f),
    PF("OSC 2", osc2_level, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("DETUNE", osc2_detune, T_LIN, F_CENT, -50.0f, 50.0f, 1.0f),
    PF("SEMI", osc2_semi, T_LIN, F_SEMI, -24.0f, 24.0f, 1.0f), PF("SUB", sub_level, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f),
    PF("SUB OCT", sub_oct, T_ENUM, F_SUBOCT, 1.0f, 2.0f, 1.0f), PF("NOISE", noise_level, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f),
    PF("CUTOFF", cutoff, T_LOG, F_HZ, 20.0f, 20000.0f, 1.0f / 24.0f), PF("RESO", res, T_LIN, F_PCT, 0.0f, 1.0f, 0.01f),
    PF("KEYTRK", keytrack, T_LIN, F_PCT, 0.0f, 1.0f, 0.05f), PF("ATTACK", f_a, T_LOG, F_TIME, 0.0005f, 4.0f, 0.125f),
    PF("SUSTAIN", f_s, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("RELEASE", f_r, T_LOG, F_TIME, 0.002f, 4.0f, 0.125f),
    PF("ATTACK", a_a, T_LOG, F_TIME, 0.0005f, 4.0f, 0.125f), PF("DECAY", a_d, T_LOG, F_TIME, 0.005f, 4.0f, 0.125f),
    PF("SUSTAIN", a_s, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("RELEASE", a_r, T_LOG, F_TIME, 0.002f, 4.0f, 0.125f),
    PA("RATE", A_L1RATE), PF("SHAPE", lfo1_shape, T_ENUM, F_LFOSHAPE, 0.0f, 5.0f, 1.0f),
    PF("TO", lfo1_dest, T_ENUM, F_LFODEST, 0.0f, 7.0f, 1.0f), PF("DEPTH", lfo1_depth, T_LIN, F_BIPCT, -1.0f, 1.0f, 0.02f),
    PF("1 SYNC", lfo1_sync, T_ENUM, F_SYNC, 0.0f, 1.0f, 1.0f), PF("1 TRIG", lfo1_retrig, T_ENUM, F_RETRIG, 0.0f, 1.0f, 1.0f),
    PA("RATE", A_L2RATE), PF("SHAPE", lfo2_shape, T_ENUM, F_LFOSHAPE, 0.0f, 5.0f, 1.0f),
    PF("TO", lfo2_dest, T_ENUM, F_LFODEST, 0.0f, 7.0f, 1.0f), PF("DEPTH", lfo2_depth, T_LIN, F_BIPCT, -1.0f, 1.0f, 0.02f),
    PF("2 SYNC", lfo2_sync, T_ENUM, F_SYNC, 0.0f, 1.0f, 1.0f), PF("2 TRIG", lfo2_retrig, T_ENUM, F_RETRIG, 0.0f, 1.0f, 1.0f),
    PF("G.MODE", glide_mode, T_ENUM, F_GMODE, 0.0f, 2.0f, 1.0f), PF("VELO", vel_sens, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f),
    PF("DRIVE", drive, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("SAT", sat, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f),
    PF("ANALOG", analog, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("LEVEL", level, T_LIN, F_DB, -24.0f, 6.0f, 0.5f),
    PF("BASSCMP", bass_comp, T_LIN, F_PCT, 0.0f, 1.0f, 0.05f), PF("SLOPE", pole2, T_ENUM, F_POLES, 0.0f, 1.0f, 1.0f),
    PA("LOWCUT", A_LOWCUT),
    PF("MIX", verb_mix, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("DECAY", verb_decay, T_LOG, F_TIME, 0.3f, 8.0f, 0.0833f),
    PF("TONE", verb_tone, T_LIN, F_PCT, 0.0f, 1.0f, 0.02f), PF("LOWCUT", verb_lowcut, T_LOG, F_HZ, 60.0f, 600.0f, 0.0833f),
    PA("ROOT", A_ROOT), PA("SCALE", A_SCALE), PA("KEYS", A_KEYS), PA("TRANSP", A_TRANSP), PA("TEMPO", A_TEMPO),
    PA("SWING", A_SWING), PA("STEP", A_RATE), PA("OFFSET", A_OFFSET), PA("MIDI CH", A_MIDICH), PA("CLOCK", A_CLOCK),
    PA("NOTES", A_NOTESOUT), PA("CC", A_CC), PA("NOTE", A_ST_NOTE), PA("OCTAVE", A_ST_OCT),
    PA("GATE", A_ST_GATE), PA("ACC/SLD", A_ST_ACCSLD), PA("LENGTH", A_PT_LEN), PA("ROTATE", A_PT_ROT),
};

enum { PG_HOME, PG_EDIT, PG_ENV, PG_LFO, PG_FX, PG_SEL, PG_GLO, PG_SEQ, PG_N };
enum { G_NONE, G_OSC, G_MIX, G_FILTER, G_FENV, G_AENV, G_LFO1, G_LFO2, G_LFOSET, G_GLIDE, G_DRIVE, G_VERB, G_SCALE,
       G_SWING, G_MIDI, G_ABOUT };
typedef struct { const char *name; uint8_t gfx; uint8_t p[4]; } page_t;
static const page_t PAGES0[] = {
    {"PERFORM", G_NONE, {P_ENVAMT, P_FDEC, P_ACCENT, P_GLIDE}},
    {"OSC", G_OSC, {P_WAVE, P_PW, P_OSC2, P_DETUNE}}, {"MIX", G_MIX, {P_SEMI, P_SUB, P_SUBOCT, P_NOISE}},
    {"FILTER", G_FILTER, {P_CUTOFF, P_RES, P_ENVAMT, P_KEYTRK}}, {"FILT ENV", G_FENV, {P_FA, P_FDEC, P_FS, P_FR}},
    {"AMP ENV", G_AENV, {P_AA, P_AD, P_AS, P_AR}},
    {"LFO 1", G_LFO1, {P_L1RATE, P_L1SHAPE, P_L1DEST, P_L1DEPTH}}, {"LFO 2", G_LFO2, {P_L2RATE, P_L2SHAPE, P_L2DEST, P_L2DEPTH}},
    {"LFO SET", G_LFOSET, {P_L1SYNC, P_L1RETRIG, P_L2SYNC, P_L2RETRIG}},
    {"MOTION", G_GLIDE, {P_GLIDE, P_GMODE, P_ACCENT, P_VELO}},
    {"DRIVE", G_DRIVE, {P_DRIVE, P_SAT, P_ANALOG, P_LEVEL}}, {"TONE", G_FILTER, {P_BCOMP, P_POLES, P_LOWCUT, P_NONE}},
    {"REVERB", G_VERB, {P_VMIX, P_VDECAY, P_VTONE, P_VLOWCUT}},
    {"SCALE", G_SCALE, {P_ROOT, P_SCALE, P_KEYS, P_TRANSP}},
    {"CLOCK", G_SWING, {P_TEMPO, P_SWING, P_RATE, P_OFFSET}}, {"MIDI", G_MIDI, {P_MIDICH, P_CLOCK, P_NOTESOUT, P_CC}},
    {"ABOUT", G_ABOUT, {P_NONE, P_NONE, P_NONE, P_NONE}},
    {"STEP", G_NONE, {P_ST_NOTE, P_ST_OCT, P_ST_GATE, P_ST_ACCSLD}}, {"PATTERN", G_NONE, {P_PT_LEN, P_PT_ROT, P_TRANSP, P_SWING}},
};
static const uint8_t PG_FIRST[PG_N] = {0, 1, 3, 6, 10, 13, 14, 17};  /* each page button's first page in PAGES0 */
static const uint8_t PG_COUNT[PG_N] = {1, 2, 3, 4, 3, 1, 3, 2};

/* ======================================================================= the interface's state */
static struct {
    uint8_t page, sub, rec, bar, arm_acc, arm_slide;
    int32_t sel, cursor;
    uint32_t toast_until;
    char toast[32];
    uint8_t undo_ok, clip_ok;
    seq_pat_t undo, clip;
    uint8_t live_note[27];                       /* the note each key started (0 = none) */
    int32_t last_deg, last_oct;
    uint32_t preview_off_at;
    uint8_t preview_note;
    uint32_t taps[4], ntaps;
} ui = {PG_HOME, 0, 0, 0, 0, 0, -1, 0, 0, "", 0, 0, {0}, {0}, {0}, 0, 0, 0, 0, {0}, 0};
static uint32_t dirty = 0xFFu;                   /* 1 header, 2 middle, 4 info line, 8 knobs */
#define D_HDR 1u
#define D_MID 2u
#define D_INFO 4u
#define D_KNOB 8u

static void toast(const char *s)
{
    uint32_t i;
    for (i = 0; s[i] && i < sizeof ui.toast - 1u; i++) ui.toast[i] = s[i];
    ui.toast[i] = 0;
    ui.toast_until = fm1_ms + 1300u;
    dirty |= D_INFO;
}
static const page_t *cur_page(void) { return &PAGES0[PG_FIRST[ui.page] + (ui.sub % PG_COUNT[ui.page])]; }
static seq_pat_t *pat(void) { return &pats[cur_pat]; }
static seq_step_t *sel_step(void) { return ui.sel >= 0 && ui.sel < (int32_t)pat()->len ? &pat()->s[ui.sel] : 0; }
static void snapshot(void) { ui.undo = *pat(); ui.undo_ok = 1; }

/* ---- small text helpers (no printf) */
static char *s_cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *s_int(char *p, int32_t v)
{
    char t[12];
    int i = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0) *p++ = '-';
    do { t[i++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (i) *p++ = t[--i];
    *p = 0;
    return p;
}
static char *s_fix1(char *p, float v)            /* one decimal */
{
    int32_t t = (int32_t)(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f));
    if (t < 0) { *p++ = '-'; t = -t; }
    p = s_int(p, t / 10);
    *p++ = '.';
    *p++ = (char)('0' + t % 10);
    *p = 0;
    return p;
}
static const char *const NOTE_N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const SCALE_SHORT[] = {"MINOR", "MAJOR", "DORIAN", "PHRYGN", "MIXOLYD", "LYDIAN", "HARMMIN",
                                          "MINPENT", "MAJPENT", "BLUES"};
static char *s_note(char *p, int32_t m) { p = s_cat(p, NOTE_N[((m % 12) + 12) % 12]); return s_int(p, m / 12 - 1); }

/* ---- reading, changing and showing a parameter */
static float *pf(const prm_t *q) { return (float *)((char *)&ui_patch + q->off); }

static void prm_text(uint32_t id, char *b)
{
    const prm_t *q = &PRM[id];
    char *p = b;
    *b = 0;
    if (id == P_NONE) return;
    if (q->type != T_APP) {
        float v = *pf(q);
        switch (q->fmt) {
        case F_PCT: p = s_int(p, (int32_t)(v * 100.0f + 0.5f)); s_cat(p, "%"); break;
        case F_OCT: p = s_fix1(p, v); s_cat(p, "oc"); break;
        case F_HZ: if (v < 1000.0f) { p = s_int(p, (int32_t)(v + 0.5f)); s_cat(p, "Hz"); } else { p = s_fix1(p, v * 0.001f); s_cat(p, "k"); } break;
        case F_TIME: if (q->min < 0.002f && v <= q->min * 1.01f && id == P_GLIDE) s_cat(p, "OFF");
                     else if (v < 1.0f) { p = s_int(p, (int32_t)(v * 1000.0f + 0.5f)); s_cat(p, "ms"); }
                     else { p = s_fix1(p, v); s_cat(p, "s"); } break;
        case F_CENT: if (v > 0.0f) *p++ = '+'; p = s_int(p, (int32_t)(v + (v < 0 ? -0.5f : 0.5f))); s_cat(p, "ct"); break;
        case F_SEMI: if (v > 0.0f) *p++ = '+'; s_int(p, (int32_t)(v + (v < 0 ? -0.5f : 0.5f))); break;
        case F_DB: if (v > 0.0f) *p++ = '+'; p = s_fix1(p, v); s_cat(p, "dB"); break;
        case F_WAVE: s_cat(p, v < 0.5f ? "SAW" : v < 1.5f ? "PULSE" : "TRI"); break;
        case F_SUBOCT: s_cat(p, v < 1.5f ? "-1 OCT" : "-2 OCT"); break;
        case F_GMODE: s_cat(p, v < 0.5f ? "OFF" : v < 1.5f ? "LEGATO" : "ALWAYS"); break;
        case F_POLES: s_cat(p, v < 0.5f ? "24 dB" : "12 dB"); break;
        case F_LFODEST: { static const char *const D[8] = {"OFF", "PITCH", "CUTOFF", "RESO", "PW", "AMP", "DRIVE", "REVERB"};
                          s_cat(p, D[(int32_t)(v + 0.5f) & 7]); } break;
        case F_LFOSHAPE: { static const char *const S[6] = {"SINE", "TRI", "SAW DN", "SAW UP", "SQUARE", "S&H"};
                           s_cat(p, S[(int32_t)(v + 0.5f) % 6]); } break;
        case F_SYNC: s_cat(p, v < 0.5f ? "FREE" : "TEMPO"); break;
        case F_RETRIG: s_cat(p, v < 0.5f ? "FREE" : "NOTE"); break;
        case F_BIPCT: if (v > 0.005f) *p++ = '+'; p = s_int(p, (int32_t)(v * 100.0f + (v < 0 ? -0.5f : 0.5f))); s_cat(p, "%"); break;
        default: s_int(p, (int32_t)v); break;
        }
        return;
    }
    switch (q->off) {
    case A_ROOT: s_cat(p, NOTE_N[key.root % 12u]); break;
    case A_SCALE: s_cat(p, SCALE_SHORT[key.scale % seq_nscales]); break;
    case A_KEYS: s_cat(p, key.chromatic ? "CHROM" : "SCALE"); break;
    case A_TRANSP: if (key.transpose > 0) *p++ = '+'; s_int(p, key.transpose); break;
    case A_TEMPO: s_fix1(p, (float)glo.bpm10 * 0.1f); break;
    case A_SWING: p = s_int(p, glo.swing); s_cat(p, "%"); break;
    case A_RATE: s_cat(p, RATE_NAME[glo.rate % 6u]); break;
    case A_L1RATE: case A_L2RATE: {
        float r = q->off == A_L1RATE ? ui_patch.lfo1_rate : ui_patch.lfo2_rate;
        float sy = q->off == A_L1RATE ? ui_patch.lfo1_sync : ui_patch.lfo2_sync;
        if (sy > 0.5f) s_cat(p, BE_LFO_DIV_NAME[(uint32_t)be_clamp(r, 0.0f, (float)(BE_LFO_NDIV - 1u))]);
        else if (r < 10.0f) { p = s_fix1(p, r); s_cat(p, "Hz"); }
        else { p = s_int(p, (int32_t)(r + 0.5f)); s_cat(p, "Hz"); }
    } break;
    case A_OFFSET: if (glo.offset10 > 0) *p++ = '-'; else if (glo.offset10 < 0) *p++ = '+';
                   p = s_fix1(p, (float)(glo.offset10 < 0 ? -glo.offset10 : glo.offset10) * 0.1f); s_cat(p, "ms"); break;
    case A_MIDICH: if (glo.midi_ch) { p = s_cat(p, "CH "); s_int(p, glo.midi_ch); } else s_cat(p, "OMNI"); break;
    case A_CLOCK: s_cat(p, glo.clock == 0u ? "AUTO" : glo.clock == 1u ? "INT" : "EXT"); break;
    case A_NOTESOUT: s_cat(p, glo.notes_out ? "ON" : "OFF"); break;
    case A_CC: { static const char *const C[4] = {"OFF", "IN", "OUT", "IN+OUT"}; s_cat(p, C[glo.cc & 3u]); } break;
    case A_LOWCUT: s_cat(p, ui_patch.hpf < 1.0f ? "OFF" : ui_patch.hpf < 25.0f ? "20Hz" : "30Hz"); break;
    case A_ST_NOTE: { seq_step_t *s = sel_step();
        if (!s || !(s->flags & SEQ_ON)) s_cat(p, "--"); else if (s->flags & SEQ_TIE) s_cat(p, "TIE"); else s_note(p, seq_note(&key, s)); } break;
    case A_ST_OCT: { seq_step_t *s = sel_step(); if (s) { if (s->oct > 0) *p++ = '+'; s_int(p, s->oct); } else s_cat(p, "--"); } break;
    case A_ST_GATE: { static const char *const G[4] = {"25%", "50%", "75%", "100%"}; seq_step_t *s = sel_step(); s_cat(p, s ? G[seq_gate(s)] : "--"); } break;
    case A_ST_ACCSLD: { seq_step_t *s = sel_step(); static const char *const A[4] = {"-", "ACCENT", "SLIDE", "ACC+SLD"};
        s_cat(p, s ? A[((s->flags & SEQ_ACC) ? 1u : 0u) | ((s->flags & SEQ_SLIDE) ? 2u : 0u)] : "--"); } break;
    case A_PT_LEN: s_int(p, pat()->len); break;
    case A_PT_ROT: s_cat(p, "< >"); break;
    default: break;
    }
}

static float prm_frac(uint32_t id)               /* 0..1 for the knob's bar, or -1: no bar */
{
    const prm_t *q = &PRM[id];
    float v, f;
    if (id == P_NONE || q->type == T_APP) {
        switch (q->off) {
        case A_TEMPO: return (float)(glo.bpm10 - 400u) / 2000.0f;
        case A_SWING: return (float)(glo.swing - 50u) / 25.0f;
        case A_TRANSP: return (float)(key.transpose + 7) / 14.0f;
        case A_PT_LEN: return (float)(pat()->len - 1u) / 63.0f;
        default: return -1.0f;
        }
    }
    v = *pf(q);
    if (q->type == T_LOG) {                      /* log2 by the exponent and a little more */
        float a = v / q->min, b = q->max / q->min, la = 0.0f, lb = 0.0f;
        while (a >= 2.0f) { a *= 0.5f; la += 1.0f; }
        while (b >= 2.0f) { b *= 0.5f; lb += 1.0f; }
        f = (la + (a - 1.0f)) / (lb + (b - 1.0f));
    } else f = (v - q->min) / (q->max - q->min);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static void patch_touched(void) { patch_gen++; dirty |= D_KNOB | D_MID; }

static void prm_turn(uint32_t id, int32_t d)
{
    const prm_t *q = &PRM[id];
    if (id == P_NONE || !d) return;
    if (q->type != T_APP) {
        float *v = pf(q), x = *v;
        if (q->type == T_LOG) {
            x = (x < q->min ? q->min : x) * be_exp2(q->step * (float)d);
        } else x += q->step * (float)d;
        if (q->type == T_ENUM) x = (float)(int32_t)(x + (x < 0 ? -0.5f : 0.5f));
        x = x < q->min ? q->min : x > q->max ? q->max : x;
        if ((id == P_L1SYNC || id == P_L2SYNC) && x != *v) {   /* the rate means something else now */
            float *r = id == P_L1SYNC ? &ui_patch.lfo1_rate : &ui_patch.lfo2_rate;
            *r = x > 0.5f ? 8.0f : 2.0f;         /* 1/8, or 2 Hz */
        }
        *v = x;
        patch_touched();
        return;
    }
    switch (q->off) {
    case A_ROOT: key.root = (uint8_t)((key.root + 12u + (uint32_t)(d % 12)) % 12u); dirty |= D_HDR | D_MID; break;
    case A_SCALE: key.scale = (uint8_t)((key.scale + seq_nscales + (uint32_t)(d % (int32_t)seq_nscales)) % seq_nscales); dirty |= D_HDR | D_MID; break;
    case A_KEYS: key.chromatic = d > 0; dirty |= D_HDR; break;
    case A_TRANSP: { int32_t t = key.transpose + d; key.transpose = (int8_t)(t < -7 ? -7 : t > 7 ? 7 : t); dirty |= D_MID; } break;
    case A_TEMPO: { int32_t b = (int32_t)glo.bpm10 + 5 * d; glo.bpm10 = (uint16_t)(b < 400 ? 400 : b > 2400 ? 2400 : b); dirty |= D_HDR; } break;
    case A_SWING: { int32_t s = (int32_t)glo.swing + d; glo.swing = (uint8_t)(s < 50 ? 50 : s > 75 ? 75 : s); dirty |= D_MID; } break;
    case A_RATE: { int32_t r = (int32_t)glo.rate + d; glo.rate = (uint8_t)(r < 0 ? 0 : r > 5 ? 5 : r); } break;
    case A_L1RATE: case A_L2RATE: {              /* synced: the next division; free: Hz, a log scale */
        float *r = q->off == A_L1RATE ? &ui_patch.lfo1_rate : &ui_patch.lfo2_rate;
        float sy = q->off == A_L1RATE ? ui_patch.lfo1_sync : ui_patch.lfo2_sync;
        if (sy > 0.5f) *r = be_clamp((float)(int32_t)(*r + 0.5f) + (float)d, 0.0f, (float)(BE_LFO_NDIV - 1u));
        else *r = be_clamp(*r * be_exp2(0.125f * (float)d), 0.05f, 30.0f);
        patch_touched();
    } break;
    case A_OFFSET: { int32_t o = glo.offset10 + d * 5; glo.offset10 = (int8_t)(o < -100 ? -100 : o > 100 ? 100 : o); } break;
    case A_MIDICH: { int32_t c = (int32_t)glo.midi_ch + d; glo.midi_ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c); } break;
    case A_CLOCK: { int32_t c = (int32_t)glo.clock + d; glo.clock = (uint8_t)(c < 0 ? 0 : c > 2 ? 2 : c); dirty |= D_HDR; } break;
    case A_NOTESOUT: glo.notes_out = d > 0; break;
    case A_CC: { int32_t c = (int32_t)glo.cc + d; glo.cc = (uint8_t)(c < 0 ? 0 : c > 3 ? 3 : c); } break;
    case A_LOWCUT: { float h = ui_patch.hpf; h = d > 0 ? (h < 1.0f ? 20.0f : 30.0f) : (h > 25.0f ? 20.0f : 0.0f); ui_patch.hpf = h; patch_touched(); } break;
    case A_ST_NOTE: case A_ST_OCT: case A_ST_GATE: case A_ST_ACCSLD: {
        seq_step_t *s = sel_step(), t;
        if (!s) { toast("TAP A STEP FIRST"); return; }
        t = *s;
        if (q->off == A_ST_NOTE) { t.deg = (int8_t)(t.deg + d); t.flags = (uint8_t)((t.flags | SEQ_ON) & ~SEQ_TIE); t.gate_chrom &= 3u; ui.last_deg = t.deg; }
        else if (q->off == A_ST_OCT) { int32_t o = t.oct + d; t.oct = (int8_t)(o < -2 ? -2 : o > 3 ? 3 : o); ui.last_oct = t.oct; }
        else if (q->off == A_ST_GATE) { int32_t g = (int32_t)seq_gate(&t) + d; t.gate_chrom = (uint8_t)((t.gate_chrom & ~3u) | (uint32_t)(g < 0 ? 0 : g > 3 ? 3 : g)); }
        else { int32_t a = (int32_t)(((t.flags & SEQ_ACC) ? 1u : 0u) | ((t.flags & SEQ_SLIDE) ? 2u : 0u)) + d;
               a = a < 0 ? 0 : a > 3 ? 3 : a; t.flags = (uint8_t)((t.flags & ~(SEQ_ACC | SEQ_SLIDE)) | ((a & 1) ? SEQ_ACC : 0u) | ((a & 2) ? SEQ_SLIDE : 0u)); }
        *s = t;                                  /* (one 4-byte store: the interrupt sees the old or the new step) */
        dirty |= D_MID | D_INFO;
    } break;
    case A_PT_LEN: { int32_t l = (int32_t)pat()->len + d; pat()->len = (uint8_t)(l < 1 ? 1 : l > 64 ? 64 : l); dirty |= D_MID; } break;
    case A_PT_ROT: snapshot(); seq_rotate(pat(), d > 0 ? 1 : -1); toast(d > 0 ? "ROTATE >" : "< ROTATE"); dirty |= D_MID; break;
    default: break;
    }
    dirty |= D_KNOB;
}

/* ======================================================================= storage */
#define BASS_MAGIC 0x32534142u                   /* "BAS2": the layout of the two slots below (BAS1: before the LFOs) */
typedef struct {
    uint32_t magic;
    seq_key_t key;
    uint8_t cur_pat, patch_no, reserved[2];
    uint8_t glo[12];
    seq_pat_t pats[SEQ_NPAT];
} store_seq_t;
typedef struct { uint32_t magic; be_patch_t p[NPATCH]; } store_patch_t;
_Static_assert(sizeof(store_seq_t) <= SLOT_MAX && sizeof(store_patch_t) <= SLOT_MAX, "a store fits a slot");
_Static_assert(sizeof glo <= 12u, "glo fits");

static void factory(void)
{
    static const struct { uint8_t i; int8_t d, o; uint8_t f; } B1[] = {   /* refs/midi/01_bassline.mid (A minor) */
        {0, 0, 0, SEQ_ACC}, {2, 0, 0, 0}, {3, 0, 1, 0}, {4, 0, 0, 0}, {6, 2, 0, SEQ_ACC | SEQ_SLIDE}, {7, 3, 0, 0},
        {8, 0, 0, 0}, {10, 0, 1, SEQ_ACC}, {11, 6, -1, SEQ_SLIDE}, {12, 0, 0, 0}, {14, 4, 0, 0}, {15, 3, 0, 0}};
    static const struct { uint8_t i; int8_t d, o; uint8_t f; } B2[] = {
        {0, 0, 0, SEQ_ACC}, {3, 0, 1, 0}, {4, 4, 0, 0}, {6, 0, 0, SEQ_ACC}, {8, 2, 0, 0}, {9, 3, 0, SEQ_SLIDE},
        {10, 4, 0, 0}, {12, 0, 0, SEQ_ACC}, {14, 6, -1, 0}, {15, 0, 0, 0}};
    uint32_t i;
    for (i = 0; i < SEQ_NPAT; i++) seq_clear(&pats[i], 16);
    for (i = 0; i < sizeof B1 / sizeof B1[0]; i++) {
        seq_step_t *s = &pats[0].s[B1[i].i];
        s->flags = (uint8_t)(SEQ_ON | B1[i].f); s->deg = B1[i].d; s->oct = B1[i].o;
    }
    for (i = 0; i < sizeof B2 / sizeof B2[0]; i++) {
        seq_step_t *s = &pats[1].s[B2[i].i];
        s->flags = (uint8_t)(SEQ_ON | B2[i].f); s->deg = B2[i].d; s->oct = B2[i].o;
    }
    for (i = 0; i < NPATCH; i++) {
        if (i < BE_NPATCH) BE_PATCH_FN[i](&patches[i]);
        else be_patch_default(&patches[i]);
    }
}

static void store_load(void)
{
    static store_seq_t ss;
    static store_patch_t sp;
    uint32_t i;
    factory();
    if (!slot_load(0, &ss, sizeof ss) && ss.magic == BASS_MAGIC) {
        key = ss.key;
        cur_pat = ss.cur_pat % SEQ_NPAT;
        patch_no = ss.patch_no % NPATCH;
        for (i = 0; i < sizeof glo; i++) ((uint8_t *)&glo)[i] = ss.glo[i];
        for (i = 0; i < SEQ_NPAT; i++) pats[i] = ss.pats[i];
    }
    if (!slot_load(1, &sp, sizeof sp) && sp.magic == BASS_MAGIC)
        for (i = 0; i < NPATCH; i++) patches[i] = sp.p[i];
    ui_patch = patches[patch_no];
    patch_gen++;
}

static int store_save(void)
{
    static store_seq_t ss;
    static store_patch_t sp;
    uint32_t i;
    patches[patch_no] = ui_patch;
    ss.magic = BASS_MAGIC; ss.key = key; ss.cur_pat = (uint8_t)cur_pat; ss.patch_no = (uint8_t)patch_no;
    for (i = 0; i < sizeof glo; i++) ss.glo[i] = ((uint8_t *)&glo)[i];
    for (i = 0; i < SEQ_NPAT; i++) ss.pats[i] = pats[i];
    sp.magic = BASS_MAGIC;
    for (i = 0; i < NPATCH; i++) sp.p[i] = patches[i];
    return slot_save(0, &ss, sizeof ss) | slot_save(1, &sp, sizeof sp);
}

/* ======================================================================= the screen */
#define K_BG RGB(7, 8, 10)
#define K_BAR RGB(16, 18, 22)
#define K_GRID RGB(27, 30, 36)
#define K_DIM RGB(58, 63, 72)
#define K_MUTE RGB(141, 144, 153)
#define K_TEXT RGB(232, 228, 220)
#define K_AMBER RGB(255, 181, 71)
#define K_TIE RGB(185, 127, 46)
#define K_CYAN RGB(90, 209, 230)
#define K_CORAL RGB(255, 106, 77)
#define K_CUR RGB(60, 26, 20)
#define K_GREEN RGB(121, 224, 139)

#include "splash.h"                             /* the power-on splash (the logo, the dog) */

static void draw_header(void)
{
    char b[40], *p = b;
    uint32_t running = a_running;
    cv_begin(240, 18, C_BLACK);
    p = s_cat(b, "P"); if (cur_pat < 9u) *p++ = '0'; s_int(p, (int32_t)cur_pat + 1);
    cv_text(4, 1, &FONT_S, b, K_AMBER);
    if (next_pat != 0xFFu) { p = s_cat(b, ">"); s_int(p, (int32_t)next_pat + 1); cv_text(34, 1, &FONT_S, b, K_MUTE); }
    p = s_cat(b, NOTE_N[key.root % 12u]); p = s_cat(p, " "); s_cat(p, key.chromatic ? "CHROM" : SCALE_SHORT[key.scale % seq_nscales]);
    cv_text(120 - text_w(&FONT_S, b) / 2, 1, &FONT_S, b, K_TEXT);
    {
        float bpm = clk_tempo(&clk);
        int ext = clk.ext && clk.have_ref;
        p = b; if (ext) p = s_cat(p, "E");
        s_fix1(p, ext ? bpm : (float)glo.bpm10 * 0.1f);
        cv_text(222 - text_w(&FONT_S, b), 1, &FONT_S, b, ext ? K_CYAN : K_TEXT);
    }
    if (ui.rec) { cv_rect(227, 4, 9, 9, K_CORAL); }
    else if (running) { int32_t y; for (y = 0; y < 9; y++) cv_rect(227, 4 + y, 1 + (y < 5 ? y : 8 - y) * 2, 1, K_GREEN); }
    else cv_rect(227, 4, 8, 8, K_MUTE);
    cv_rect(0, 17, 240, 1, K_GRID);
    cv_blit(0, 0);
}

static int32_t lane_first(int seqview, int32_t pidx)   /* the first step the lane shows */
{
    return seqview ? (int32_t)ui.bar * 16 : (a_running && pidx >= 0 ? (pidx / 16) * 16 : 0);
}

/* the pattern as a pitch lane: columns c0..c1-1 of 16, drawn at x - xoff (the whole lane: 0, 16, 0; a strip around
 * the playhead when it moves: only those columns, so a step does not repaint the screen) */
static void lane(int32_t y0, int32_t h, int seqview, int32_t pidx, int32_t c0, int32_t c1, int32_t xoff)
{
    const seq_pat_t *p = pat();
    int32_t first, i, prev = -1, lo = 127, hi = 0;
    uint32_t running = a_running;
    first = lane_first(seqview, pidx);
    for (i = 0; i < (int32_t)p->len; i++)
        if ((p->s[i].flags & SEQ_ON) && !(p->s[i].flags & SEQ_TIE)) {
            int32_t m = seq_note(&key, &p->s[i]);
            if (m < lo) lo = m;
            if (m > hi) hi = m;
        }
    if (lo > hi) { lo = 33; hi = 45; }
    lo -= 2; hi += 2;
    if (hi - lo < 14) { int32_t c = (hi + lo) / 2; lo = c - 7; hi = c + 7; }
    for (i = 0; i < c0 && first + i < (int32_t)p->len; i++) {   /* the note a tie in column c0 continues */
        const seq_step_t *s = &p->s[first + i];
        if (!(s->flags & SEQ_ON)) prev = -1;
        else if (!(s->flags & SEQ_TIE)) prev = seq_note(&key, s);
    }
    for (i = c0; i < c1; i++) cv_rect(i * 15 - xoff, y0, 1, h, (i % 4) == 0 ? K_GRID : K_BAR);
    if (running && pidx >= first + c0 && pidx < first + c1) {   /* the playhead: small marks, not a column (the screen
                                                       has no sync: a tall block changing every step shows tearing) */
        int32_t hx = (pidx - first) * 15 + 1 - xoff;
        cv_rect(hx, y0 + h - 4, 14, 4, K_CYAN);
        cv_rect(hx, y0, 14, 2, K_CYAN);
    }
    if (ui.rec && !running && ui.cursor >= first + c0 && ui.cursor < first + c1) cv_rect((ui.cursor - first) * 15 + 1 - xoff, y0, 14, h, K_CUR);
    for (i = c0; i < c1; i++) {
        int32_t k = first + i, x = i * 15 - xoff, m, y, w;
        const seq_step_t *s;
        if (k >= (int32_t)p->len) { int32_t yy; for (yy = y0; yy < y0 + h; yy += 3) cv_rect(x + 1, yy, 14, 1, K_BAR); continue; }
        s = &p->s[k];
        if (!(s->flags & SEQ_ON)) { prev = -1; continue; }
        m = (s->flags & SEQ_TIE) ? prev : seq_note(&key, s);
        if (m < 0) continue;
        y = y0 + h - 6 - (m - lo) * (h - 12) / (hi - lo);
        w = (s->flags & SEQ_TIE) ? 15 : (int32_t)(seq_gate(s) + 1u) * 14 / 4;
        cv_rect((s->flags & SEQ_TIE) ? x - 1 : x + 1, y - 3, w, 6,
                running && k == pidx ? C_WHITE : (s->flags & SEQ_TIE) ? K_TIE : K_AMBER);   /* the note playing: white */
        if (s->flags & SEQ_ACC) cv_rect(x + 1, y - 7, w, 2, K_CORAL);
        if ((s->flags & SEQ_SLIDE) && k + 1 < (int32_t)p->len && (p->s[k + 1].flags & SEQ_ON)) {
            int32_t m2 = (p->s[k + 1].flags & SEQ_TIE) ? m : seq_note(&key, &p->s[k + 1]);
            int32_t y2 = y0 + h - 6 - (m2 - lo) * (h - 12) / (hi - lo);
            cv_line(x + 1 + w, y, x + 16, y2, K_AMBER);
        }
        if (seqview && k == ui.sel) {
            cv_rect(x + 1, y0, 14, 1, K_CYAN); cv_rect(x + 1, y0 + h - 1, 14, 1, K_CYAN);
            cv_rect(x + 1, y0, 1, h, K_CYAN); cv_rect(x + 14, y0, 1, h, K_CYAN);
        }
        prev = m;
    }
}

static float lg2(float x)                        /* log2, roughly (for drawing) */
{
    float e = 0.0f;
    if (x <= 0.0f) return -30.0f;
    while (x >= 2.0f) { x *= 0.5f; e += 1.0f; }
    while (x < 1.0f) { x *= 2.0f; e -= 1.0f; }
    return e + (x - 1.0f) * (1.3466f - 0.3466f * x);
}

static void graphic(int32_t y0, int32_t h)       /* the page's picture */
{
    const page_t *pg = cur_page();
    int32_t x, px = -1, py = 0;
    uint32_t i;
#define PLOT(X, Y) do { int32_t _y = (Y); if (px >= 0) cv_line(px, py, (X), _y, K_AMBER); px = (X); py = _y; } while (0)
    if (pg->gfx == G_OSC) {                      /* OSC: one cycle and a half of the mix */
        for (x = 0; x <= 220; x++) {
            float ph = (float)x / 110.0f, v; ph -= (float)(int32_t)ph;
            v = ui_patch.osc1_wave < 0.5f ? 2.0f * ph - 1.0f : ui_patch.osc1_wave < 1.5f ? (ph < ui_patch.osc1_pw ? 1.0f : -1.0f)
              : 1.0f - 4.0f * (ph < 0.5f ? 0.5f - ph : ph - 0.5f);
            if (ui_patch.osc2_level > 0.0f) { float p2 = ph * (1.0f + ui_patch.osc2_detune / 1200.0f); p2 -= (float)(int32_t)p2;
                v = (v + ui_patch.osc2_level * (2.0f * p2 - 1.0f)) / (1.0f + ui_patch.osc2_level); }
            PLOT(10 + x, y0 + h / 2 - (int32_t)(v * (float)h * 0.36f));
        }
    } else if (pg->gfx == G_MIX) {               /* MIX: osc 1 and the sub */
        for (x = 0; x <= 220; x++) {
            float ph = (float)x / 110.0f, ps = (float)x / (110.0f * (ui_patch.sub_oct > 1.5f ? 4.0f : 2.0f)), v;
            ph -= (float)(int32_t)ph; ps -= (float)(int32_t)ps;
            v = (2.0f * ph - 1.0f) * (1.0f - 0.5f * ui_patch.sub_level) + ui_patch.sub_level * (ps < 0.5f ? 0.7f : -0.7f);
            PLOT(10 + x, y0 + h / 2 - (int32_t)(v * (float)h * 0.3f));
        }
    } else if (pg->gfx == G_FILTER) {   /* FILTER, TONE: the response, 20 Hz .. 20 kHz */
        float lc = lg2(ui_patch.cutoff / 20.0f);
        for (i = 1; i < 4u; i++) cv_rect(10, y0 + (int32_t)i * h / 4, 220, 1, K_GRID);
        for (x = 0; x <= 220; x++) {
            float lf = (float)x / 220.0f * 9.966f, r = lf - lc, mag, pk, db;   /* octaves above 20 Hz, above cutoff */
            float poles = ui_patch.pole2 > 0.5f ? 2.0f : 4.0f, d = r > 0.0f ? r : 0.0f;
            pk = ui_patch.res * 14.0f / (1.0f + r * r * 9.0f);
            db = -6.02f * poles * d + pk - ui_patch.res * 8.0f * (1.0f - ui_patch.bass_comp) * (r < 0.0f ? 1.0f : 0.0f);
            mag = db;
            PLOT(10 + x, y0 + 14 - (int32_t)(mag * (float)(h - 24) / 60.0f) > y0 + h - 2 ? y0 + h - 2 : y0 + 14 - (int32_t)(mag * (float)(h - 24) / 60.0f));
        }
        cv_rect(10 + (int32_t)(lc / 9.966f * 220.0f), y0 + 4, 1, h - 10, K_CYAN);
    } else if (pg->gfx == G_FENV || pg->gfx == G_AENV) {   /* an envelope */
        int fe = pg->gfx == G_FENV;
        float a = fe ? ui_patch.f_a : ui_patch.a_a, d = fe ? ui_patch.f_d : ui_patch.a_d, s = fe ? ui_patch.f_s : ui_patch.a_s;
        float r = fe ? ui_patch.f_r : ui_patch.a_r, tot = a + d + 0.35f + r, t;
        for (x = 0; x <= 220; x++) {
            float v;
            t = (float)x / 220.0f * tot;
            if (t < a) v = t / a;
            else if (t < a + d + 0.35f) v = s + (1.0f - s) * be_exp(-4.6f * (t - a) / d);
            else v = s * be_exp(-4.6f * (t - a - d - 0.35f) / r);
            PLOT(10 + x, y0 + h - 6 - (int32_t)(v * (float)(h - 14)));
        }
    } else if (pg->gfx == G_LFO1 || pg->gfx == G_LFO2 || pg->gfx == G_LFOSET) {   /* the LFOs' waves */
        uint32_t l, l0 = pg->gfx == G_LFO2 ? 1u : 0u, l1 = pg->gfx == G_LFO1 ? 0u : 1u;
        for (l = l0; l <= l1; l++) {
            const float *lf = l ? &ui_patch.lfo2_rate : &ui_patch.lfo1_rate;   /* rate shape dest depth sync retrig */
            int32_t sh = (int32_t)(lf[1] + 0.5f), cy = pg->gfx == G_LFOSET ? y0 + 26 + (int32_t)l * 54 : y0 + h / 2;
            float dep = lf[3] < 0.0f ? -lf[3] : lf[3], amp = (pg->gfx == G_LFOSET ? 20.0f : 38.0f) * (dep > 0.1f ? dep : 0.1f);
            char b[32], *p;
            px = -1;
            for (x = 0; x <= 220; x++) {
                float ph = (float)x / 73.0f, v;
                ph -= (float)(int32_t)ph;
                switch (sh) {
                case BE_LFO_TRI: v = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
                case BE_LFO_SAWDN: v = 1.0f - 2.0f * ph; break;
                case BE_LFO_SAWUP: v = 2.0f * ph - 1.0f; break;
                case BE_LFO_SQUARE: v = ph < 0.5f ? 1.0f : -1.0f; break;
                case BE_LFO_SH: { static const float SH[4] = {0.6f, -0.3f, 0.9f, -0.8f}; v = SH[(x / 37) & 3]; } break;
                default: v = be_sin2pi(ph); break;
                }
                if (lf[3] < 0.0f) v = -v;
                PLOT(10 + x, cy - (int32_t)(v * amp));
            }
            p = s_cat(b, l ? "2: " : "1: ");
            prm_text(l ? P_L2RATE : P_L1RATE, p); while (*p) p++;
            p = s_cat(p, " > "); prm_text(l ? P_L2DEST : P_L1DEST, p);
            cv_text(10, pg->gfx == G_LFOSET ? cy - 26 : y0 + 2, &FONT_S, b, (int32_t)(lf[2] + 0.5f) == BE_TO_OFF ? K_DIM : K_MUTE);
        }
    } else if (pg->gfx == G_GLIDE) {               /* MOTION: a glide between two notes */
        float gl = ui_patch.glide > 0.002f ? ui_patch.glide : 0.002f;
        for (x = 0; x <= 220; x++) {
            float t = (float)x / 220.0f * 0.6f - 0.2f, v = t < 0.0f ? 0.0f : 1.0f - be_exp(-2.3f * t / gl);
            PLOT(10 + x, y0 + h - 20 - (int32_t)(v * (float)(h - 40)));
        }
    } else if (pg->gfx == G_DRIVE) {               /* DRIVE: the transfer curve */
        float k = 0.5f * be_exp2(ui_patch.drive * 4.0f) * (1.0f + ui_patch.sat * 2.0f), tk = be_tanh(k);
        cv_rect(120, y0 + 2, 1, h - 4, K_GRID); cv_rect(40, y0 + h / 2, 160, 1, K_GRID);
        for (x = 0; x <= 160; x++) { float u = (float)(x - 80) / 80.0f; PLOT(40 + x, y0 + h / 2 - (int32_t)(be_tanh(u * k) / tk * (float)(h / 2 - 8))); }
    } else if (pg->gfx == G_SCALE) {              /* SCALE: two octaves, the scale lit, the root coral */
        const seq_scale_t *sc = &seq_scales[key.scale % seq_nscales];
        char b[32], *p;
        for (i = 0; i < 24u; i++) {
            uint32_t pc = i % 12u, rel = (pc + 12u - key.root) % 12u, in = key.chromatic, j, bl = (0x54Au >> pc) & 1u;
            for (j = 0; j < sc->n; j++) if (sc->iv[j] == rel) in = 1;
            cv_rect(12 + (int32_t)i * 9, y0 + (bl ? 18 : 40), 8, bl ? 28 : 40, pc == key.root ? K_CORAL : in ? K_AMBER : bl ? K_BAR : K_DIM);
        }
        p = b;
        for (i = 0; i < sc->n; i++) { p = s_cat(p, NOTE_N[(key.root + sc->iv[i]) % 12u]); p = s_cat(p, " "); }
        cv_text(120 - text_w(&FONT_S, b) / 2, y0 + h - 18, &FONT_S, b, K_MUTE);
    } else if (pg->gfx == G_SWING) {              /* CLOCK: swing as pairs */
        for (i = 0; i < 8u; i++) {
            int32_t base = 14 + (int32_t)i * 27, sw = (int32_t)(glo.swing - 50u) * 27 / 50;
            cv_rect(base, y0 + 30, 4, 50, K_AMBER); cv_rect(base + 13 + sw, y0 + 40, 4, 40, K_CYAN);
        }
    } else if (pg->gfx == G_VERB) {              /* REVERB: the tail, the dry bass below the low cut */
        float d = ui_patch.verb_decay, m = ui_patch.verb_mix;
        int32_t lc = 10 + (int32_t)(lg2(ui_patch.verb_lowcut / 20.0f) / 9.966f * 220.0f);
        for (x = 0; x <= 220; x++) {
            float t = (float)x / 220.0f * d * 1.2f, env = be_exp(-6.9f * t / d) * (0.15f + 0.85f * m);
            int32_t hh = (int32_t)(env * (float)(h - 40)) * ((x * 37 % 11) + 5) / 15;
            if (x % 3 == 0) cv_rect(10 + x, y0 + h - 22 - hh, 1, hh, x < 6 ? K_AMBER : (ui_patch.verb_tone > 0.5f ? K_CYAN : K_DIM));
        }
        cv_rect(10, y0 + h - 22, 220, 1, K_GRID);
        { char b[32], *p = s_cat(b, "BASS DRY BELOW "); prm_text(P_VLOWCUT, p); cv_text(120 - text_w(&FONT_S, b) / 2, y0 + h - 18, &FONT_S, b, K_MUTE); }
        (void)lc;
    } else if (pg->gfx == G_ABOUT) {             /* who, which version */
        char b[48], *p;
        cv_text(120 - text_w(&FONT_L, "LUCCA") / 2, y0 - 2, &FONT_L, "LUCCA", K_AMBER);
        cv_text(120 - text_w(&FONT_S, "das Bassmaschine") / 2, y0 + 30, &FONT_S, "das Bassmaschine", K_TEXT);
        cv_text(120 - text_w(&FONT_S, "by " LUCCA_AUTHOR) / 2, y0 + 48, &FONT_S, "by " LUCCA_AUTHOR, K_CYAN);
        p = s_cat(b, LUCCA_VERSION[0] >= '0' && LUCCA_VERSION[0] <= '9' ? "v" : "");   /* v1.0, or "dev" */
        p = s_cat(p, LUCCA_VERSION "  "); s_cat(p, __DATE__);
        cv_text(120 - text_w(&FONT_S, b) / 2, y0 + 64, &FONT_S, b, K_MUTE);
        cv_text(120 - text_w(&FONT_S, "(C) 2026 " LUCCA_AUTHOR) / 2, y0 + 80, &FONT_S, "(C) 2026 " LUCCA_AUTHOR, K_MUTE);
        cv_text(120 - text_w(&FONT_S, "GPL-3.0: Dinghy, Felucca") / 2, y0 + 96, &FONT_S, "GPL-3.0: Dinghy, Felucca", K_DIM);
        (void)px; (void)py;
    } else if (pg->gfx == G_MIDI) {              /* MIDI, and the audio interrupt's load */
        char b[40], *p;
        p = s_cat(b, "AUDIO CPU "); p = s_int(p, (int32_t)cpu_avg10 / 10); p = s_cat(p, "% MAX "); p = s_int(p, (int32_t)cpu_max10 / 10); s_cat(p, "%");
        cv_text(10, y0 + 6, &FONT_S, b, cpu_max10 > 800u ? K_CORAL : K_AMBER);
        cv_text(10, y0 + 30, &FONT_S, "IN: USB + TRS", K_TEXT);
        cv_text(10, y0 + 50, &FONT_S, glo.clock == 0u ? "CLOCK: WHATEVER COMES" : glo.clock == 1u ? "CLOCK: OWN TEMPO" : "CLOCK: DAW ONLY", K_TEXT);
        cv_text(10, y0 + 70, &FONT_S, glo.notes_out ? "NOTES OUT: USB" : "NOTES OUT: OFF", K_TEXT);
        cv_text(10, y0 + 90, &FONT_S, "CC: CUTOFF 74 RESO 71", glo.cc ? K_TEXT : K_DIM);
    }
#undef PLOT
}

static int32_t drawn_first = -1, drawn_head = -1;   /* what the lane on the screen shows: its first step, the playhead */

static void draw_middle(void)
{
    int32_t pidx = play_idx, sv = ui.page == PG_SEQ;
    cv_begin(240, 150, C_BLACK);
    if (ui.page == PG_HOME || ui.page == PG_SEQ) lane(2, 146, sv, pidx, 0, 16, 0);
    else { lane(2, 30, 0, pidx, 0, 16, 0); cv_rect(0, 34, 240, 1, K_GRID); graphic(36, 112); }
    cv_blit(0, 18);
    drawn_first = lane_first(sv, pidx);
    drawn_head = a_running && pidx >= drawn_first && pidx < drawn_first + 16 ? pidx - drawn_first : -1;
}

/* columns a..z of the lane, and one either side (slides drawn from a neighbour come back too), as one strip */
static void lane_strip(int32_t a, int32_t z, int32_t pidx)
{
    int32_t sv = ui.page == PG_SEQ, big = ui.page == PG_HOME || sv;
    int32_t c0 = a > 0 ? a - 1 : 0, c1 = z < 15 ? z + 2 : 16;
    cv_begin((uint32_t)(c1 - c0) * 15u, big ? 148u : 33u, C_BLACK);
    lane(2, big ? 146 : 30, sv, pidx, c0, c1, c0 * 15);
    cv_blit((uint32_t)c0 * 15u, 18);
}

/* the playhead moved: redraw only the columns where it was and where it is, not the screen; 0: the lane needs a
 * full redraw instead (it scrolled to another bar) */
static int draw_playhead(void)
{
    int32_t pidx = play_idx, first = lane_first(ui.page == PG_SEQ, pidx), col;
    if (first != drawn_first) return 0;
    col = a_running && pidx >= first && pidx < first + 16 ? pidx - first : -1;
    if (col == drawn_head) return 1;
    if (col >= 0 && drawn_head >= 0 && col - drawn_head <= 2 && drawn_head - col <= 2)
        lane_strip(col < drawn_head ? col : drawn_head, col > drawn_head ? col : drawn_head, pidx);   /* neighbours
                                                     (nearly every step): both in one strip, one paint */
    else {
        if (col >= 0) lane_strip(col, col, pidx);              /* the new place first: never no highlight */
        if (drawn_head >= 0) lane_strip(drawn_head, drawn_head, pidx);
    }
    drawn_head = col;
    return 1;
}

static void draw_info(void)
{
    char b[40], *p = b;
    uint16_t c = K_MUTE;
    if (fm1_ms < ui.toast_until) { s_cat(b, ui.toast); c = K_TEXT; }
    else if (ui.page == PG_SEQ && sel_step()) {
        seq_step_t *s = sel_step();
        p = s_cat(p, "STEP "); p = s_int(p, ui.sel + 1); p = s_cat(p, " ");
        if (!(s->flags & SEQ_ON)) s_cat(p, "OFF");
        else { if (s->flags & SEQ_TIE) p = s_cat(p, "TIE"); else p = s_note(p, seq_note(&key, s));
               if (s->flags & SEQ_ACC) p = s_cat(p, " ACC"); if (s->flags & SEQ_SLIDE) p = s_cat(p, " SLD"); }
        c = K_TEXT;
    } else if (ui.rec && !a_running) {
        p = s_cat(p, "STEP REC "); p = s_int(p, ui.cursor + 1); p = s_cat(p, "/"); p = s_int(p, pat()->len);
        if (ui.arm_acc) p = s_cat(p, " ACC"); if (ui.arm_slide) s_cat(p, " SLD");
        c = K_CORAL;
    } else {
        p = s_cat(p, "PATCH "); p = s_int(p, (int32_t)patch_no + 1); p = s_cat(p, " ");
        s_cat(p, patch_no < BE_NPATCH ? BE_PATCH_NAME[patch_no] : "USER");
    }
    draw_text_box(0, 170, 240, &FONT_S, b, c, 1);
}

static void draw_knobs(void)
{
    const page_t *pg = cur_page();
    char b[16];
    uint32_t i;
    cv_begin(240, 52, C_BLACK);
    for (i = 0; i < 4u; i++) {
        int32_t x = (int32_t)i * 60;
        uint32_t id = pg->p[i];
        float f;
        cv_rect(x + 1, 0, 58, 52, K_BAR);
        if (id == P_NONE) continue;
        cv_text(x + 30 - text_w(&FONT_S, PRM[id].label) / 2, 2, &FONT_S, PRM[id].label, K_MUTE);
        prm_text(id, b);
        cv_text(x + 30 - text_w(&FONT_S, b) / 2, 20, &FONT_S, b, K_TEXT);
        f = prm_frac(id);
        if (f >= 0.0f) { cv_rect(x + 6, 43, 48, 2, K_DIM); cv_rect(x + 6, 43, (int32_t)(48.0f * f), 2, K_AMBER); }
    }
    cv_blit(0, 188);
}

/* ======================================================================= the panel */
static void go_page(uint32_t pg)
{
    if (ui.page == pg) ui.sub = (uint8_t)((ui.sub + 1u) % PG_COUNT[pg]);
    else { ui.page = (uint8_t)pg; ui.sub = 0; }
    dirty |= D_MID | D_KNOB | D_INFO;
}

static void preview(int32_t note)                /* a step's note, briefly, while editing */
{
    if (a_running || note < 0) return;
    if (ui.preview_note) live_note(ui.preview_note, 0);
    ui.preview_note = (uint8_t)note;
    live_note((uint32_t)note, 100);
    ui.preview_off_at = fm1_ms + 180u;
}

static void step_tool(uint32_t t)                /* the black keys while writing steps: ACC SLIDE TIE REST BACK */
{
    seq_pat_t *p = pat();
    if (ui.rec && !a_running) {
        switch (t) {
        case 0: ui.arm_acc = !ui.arm_acc; toast(ui.arm_acc ? "NEXT NOTE: ACCENT" : "ACCENT OFF"); break;
        case 1: ui.arm_slide = !ui.arm_slide; toast(ui.arm_slide ? "NEXT NOTE: SLIDE" : "SLIDE OFF"); break;
        case 2: snapshot(); p->s[ui.cursor].flags = SEQ_ON | SEQ_TIE; ui.cursor = (ui.cursor + 1) % (int32_t)p->len; toast("TIE"); break;
        case 3: snapshot(); p->s[ui.cursor].flags = 0; ui.cursor = (ui.cursor + 1) % (int32_t)p->len; toast("REST"); break;
        case 4: ui.cursor = (ui.cursor + (int32_t)p->len - 1) % (int32_t)p->len; toast("BACK"); break;
        default: return;
        }
        dirty |= D_MID | D_INFO;
        return;
    }
    {
        seq_step_t *s = sel_step(), x;
        if (!s) { toast("TAP A STEP FIRST"); return; }
        snapshot();
        x = *s;
        switch (t) {
        case 0: x.flags ^= SEQ_ACC; toast(x.flags & SEQ_ACC ? "ACCENT" : "NO ACCENT"); break;
        case 1: x.flags ^= SEQ_SLIDE; toast(x.flags & SEQ_SLIDE ? "SLIDE" : "NO SLIDE"); break;
        case 2: x.flags = (uint8_t)((x.flags ^ SEQ_TIE) | SEQ_ON); toast(x.flags & SEQ_TIE ? "TIE" : "NO TIE"); break;
        case 3: x.flags = 0; toast("STEP CLEARED"); break;
        case 4: if (ui.sel > 0) ui.sel--; toast("STEP BACK"); dirty |= D_MID | D_INFO | D_KNOB; return;
        default: return;
        }
        *s = x;
        dirty |= D_MID | D_INFO | D_KNOB;
    }
}

static void write_step(int32_t i, int32_t deg, int32_t oct, int32_t chrom_midi, uint8_t flags)
{
    seq_step_t s = pat()->s[i];
    s.flags = (uint8_t)(SEQ_ON | flags);
    if (chrom_midi >= 0) seq_set_note(&key, &s, chrom_midi);
    else { s.deg = (int8_t)deg; s.oct = (int8_t)oct; s.gate_chrom &= 3u; }
    pat()->s[i] = s;
    ui.last_deg = s.deg; ui.last_oct = s.oct;
}

static void key_down(uint32_t k, uint32_t held_btns)
{
    int32_t w = seq_white_index(k), b = seq_black_index(k);
    int tools = !key.chromatic && (ui.rec || ui.page == PG_SEQ);
    if ((held_btns >> BTN_SEQ) & 1u) {           /* hold SEQ + a white key: a pattern */
        if (w < 0) return;
        if (a_running) { next_pat = (uint32_t)w; toast("PATTERN NEXT BAR"); }
        else { cur_pat = (uint32_t)w; ui.sel = -1; toast("PATTERN"); }
        dirty |= D_HDR | D_MID;
        return;
    }
    if ((held_btns >> BTN_SEL) & 1u) {           /* hold SEL + a key: the root */
        key.root = (uint8_t)((5u + k) % 12u);
        toast("ROOT");
        dirty |= D_HDR | D_MID | D_KNOB;
        return;
    }
    if (b >= 0 && tools) { if (b < 5) step_tool((uint32_t)b); return; }
    if (ui.page == PG_SEQ && !ui.rec && w >= 0) { /* the steps */
        int32_t i = (int32_t)ui.bar * 16 + w;
        seq_step_t *s;
        if (i >= (int32_t)pat()->len) { toast("PAST THE END"); return; }
        s = &pat()->s[i];
        snapshot();
        if (ui.sel == i) { seq_step_t x = *s; x.flags ^= SEQ_ON; *s = x; }
        else { ui.sel = i; ui.sub = 0; if (!(s->flags & SEQ_ON)) write_step(i, ui.last_deg, ui.last_oct, -1, 0); }
        if (s->flags & SEQ_ON) preview(seq_note(&key, s));
        dirty |= D_MID | D_INFO | D_KNOB;
        return;
    }
    {
        int32_t note = seq_key_note(&key, k, glo.oct);
        if (note < 0) { toast("BLACK KEYS OFF: IN SCALE"); return; }
        if (ui.rec && !a_running) {              /* step record */
            snapshot();
            if (key.chromatic) write_step(ui.cursor, 0, 0, note, (uint8_t)((ui.arm_acc ? SEQ_ACC : 0u) | (ui.arm_slide ? SEQ_SLIDE : 0u)));
            else write_step(ui.cursor, w - key.transpose, glo.oct + 1, -1, (uint8_t)((ui.arm_acc ? SEQ_ACC : 0u) | (ui.arm_slide ? SEQ_SLIDE : 0u)));
            ui.arm_acc = ui.arm_slide = 0;
            ui.cursor = (ui.cursor + 1) % (int32_t)pat()->len;
            preview(note);
            dirty |= D_MID | D_INFO;
            return;
        }
        if (ui.rec && a_running && play_idx >= 0) {   /* live record: into the step playing now */
            snapshot();
            if (key.chromatic) write_step(play_idx, 0, 0, note, 0);
            else write_step(play_idx, w - key.transpose, glo.oct + 1, -1, 0);
            dirty |= D_MID;
        }
        ui.live_note[k] = (uint8_t)note;
        live_note((uint32_t)note, 100);
    }
}

static void key_up(uint32_t k)
{
    if (ui.live_note[k]) { live_note(ui.live_note[k], 0); ui.live_note[k] = 0; }
}

#define HOLD_MS 550u
static uint32_t btn_t[14];
static uint8_t btn_combo[14], btn_long[14];
static int deferred(uint32_t b) { return b == BTN_SEQ || b == BTN_SEL || b == BTN_GLO || b == BTN_HOME || b == BTN_SAVE || b == BTN_ARP; }
static uint32_t idea_rng;                        /* ARP's ideas: their randomness */

static void tap(uint32_t b)
{
    switch (b) {
    case BTN_OCT_DOWN: case BTN_OCT_UP: {
        int32_t d = b == BTN_OCT_UP ? 1 : -1;
        if (ui.page == PG_SEQ) {
            int32_t bars = ((int32_t)pat()->len + 15) / 16, nb = (int32_t)ui.bar + d;
            ui.bar = (uint8_t)(nb < 0 ? 0 : nb >= bars ? bars - 1 : nb);
            toast(ui.bar ? "BAR >" : "BAR 1");
            dirty |= D_MID;
        } else {
            int32_t o = glo.oct + d;
            glo.oct = (int8_t)(o < -2 ? -2 : o > 2 ? 2 : o);
            toast(glo.oct == 0 ? "OCTAVE 0" : d > 0 ? "OCTAVE UP" : "OCTAVE DOWN");
        }
    } break;
    case BTN_FX: go_page(PG_FX); break;
    case BTN_SEL: go_page(PG_SEL); break;
    case BTN_ENV: go_page(PG_ENV); break;
    case BTN_LFO: go_page(PG_LFO); break;
    case BTN_EDIT: go_page(PG_EDIT); break;
    case BTN_GLO: go_page(PG_GLO); break;
    case BTN_HOME: ui.page = PG_HOME; ui.sub = 0; ui.sel = -1; dirty |= D_MID | D_KNOB | D_INFO; break;
    case BTN_SEQ:
        if (ui.page != PG_SEQ) { ui.page = PG_SEQ; ui.sub = ui.sel >= 0 ? 0 : 1; }
        else ui.sub ^= 1u;
        dirty |= D_MID | D_KNOB | D_INFO;
        break;
    case BTN_PLAY:
        if (clk.ext && clk.have_ref) { toast("FOLLOWING THE DAW"); break; }
        if (glo.clock == 2u) { toast("CLOCK: DAW ONLY"); break; }
        clock_msg(a_running ? CMD_HALT : CMD_PLAY, 0);
        break;
    case BTN_REC:
        ui.rec = !ui.rec;
        if (ui.rec) { ui.cursor = ui.sel >= 0 ? ui.sel : 0; toast(a_running ? "LIVE RECORD" : "STEP RECORD"); }
        else toast("RECORD OFF");
        dirty |= D_HDR | D_MID | D_INFO;
        break;
    case BTN_SAVE: toast("HOLD SAVE TO STORE"); break;
    case BTN_ARP:                                /* MUTATE: vary 2..3 steps, in key (hold HOME: undo) */
        if (!idea_rng) idea_rng = fm1_ticks() | 1u;
        snapshot();
        seq_mutate(&key, pat(), &idea_rng);
        toast("MUTATED (HOME: UNDO)");
        dirty |= D_MID | D_KNOB;
        break;
    default: break;
    }
}

static void long_press(uint32_t b)
{
    if (b == BTN_HOME) {
        if (ui.undo_ok) { seq_pat_t t = *pat(); *pat() = ui.undo; ui.undo = t; toast("UNDO"); dirty |= D_MID | D_KNOB; }
        else toast("NOTHING TO UNDO");
    } else if (b == BTN_SAVE) {
        toast(a_running ? "SAVING..." : "SAVING...");
        draw_info();
        toast(store_save() ? "SAVE FAILED" : "SAVED");
    } else if (b == BTN_ARP) {                   /* NEW BASSLINE: 16 steps in the key (hold HOME: undo) */
        if (!idea_rng) idea_rng = fm1_ticks() | 1u;
        snapshot();
        seq_generate(&key, pat(), &idea_rng);
        ui.sel = -1;
        toast("NEW BASSLINE (HOME: UNDO)");
        dirty |= D_MID | D_KNOB | D_INFO;
    } else if (b == BTN_SEQ) toast("SEQ: A WHITE KEY = PATTERN");
    else if (b == BTN_SEL) toast("SEL: A KEY = ROOT");
    else if (b == BTN_GLO) toast("GLO: PLAY = TAP TEMPO");
}

static void tap_tempo(void)
{
    uint32_t now = fm1_ms, i;
    if (ui.ntaps && now - ui.taps[(ui.ntaps - 1u) & 3u] > 2000u) ui.ntaps = 0;
    ui.taps[ui.ntaps & 3u] = now;
    ui.ntaps++;
    if (ui.ntaps >= 2u) {
        uint32_t n = ui.ntaps < 4u ? ui.ntaps : 4u, first = ui.taps[(ui.ntaps - n) & 3u], span = now - first;
        uint32_t bpm10 = span ? 600000u * (n - 1u) / span : glo.bpm10;
        glo.bpm10 = (uint16_t)(bpm10 < 400u ? 400u : bpm10 > 2400u ? 2400u : bpm10);
        dirty |= D_HDR | D_KNOB;
    }
    (void)i;
    toast("TAP TEMPO");
}

static void select_patch(uint32_t n)             /* PRESETS, or a MIDI program change */
{
    patches[patch_no] = ui_patch;                /* (the edits stay with the patch in RAM; SAVE stores them) */
    patch_no = n % NPATCH;
    ui_patch = patches[patch_no];
    patch_touched();
    dirty |= D_INFO;
}

/* speed-sensitive turning (SELECT, ALGORITHM): a slow turn moves in fine steps, a quick one sweeps. The speed is
 * detents per second, smoothed over the last couple of reads, forgotten after a pause */
static int32_t accel(uint32_t which, int32_t e)
{
    static uint32_t last[2];
    static float rate[2];
    uint32_t dt = fm1_ms - last[which];
    float inst, r;
    int32_t m;
    last[which] = fm1_ms;
    inst = (float)(e < 0 ? -e : e) * 1000.0f / (float)(dt ? dt : 1u);
    r = dt > 250u ? inst : 0.5f * rate[which] + 0.5f * inst;
    rate[which] = r;
    m = r < 12.0f ? 1 : r < 30.0f ? 2 : r < 60.0f ? 4 : 8;
    return e * m;
}

static void panel(void)
{
    uint32_t rel, pressed = fm1_input_edges(&rel), held = fm1_in.buttons, b, k;
    static uint32_t kprev;
#ifdef DINGHY_HOST
    uint32_t kh = fm1_in.notes, kp = kh & ~kprev;                 /* (the host has only the keys held) */
#else
    uint32_t kh = fm1_in.notes, kp = fm1_input_note_edges();     /* the FM-1's own presses (short ones too), and only
                                                                    those: comparing with the keys held as well counted
                                                                    a press twice when the two saw it in different passes */
#endif
    int32_t e;
    for (b = 0; b < 14u; b++) {
        if ((pressed >> b) & 1u) {
            btn_t[b] = fm1_ms; btn_combo[b] = 0; btn_long[b] = 0;
            if (((held >> BTN_SEQ) & 1u) && b == BTN_SAVE) { ui.clip = *pat(); ui.clip_ok = 1; btn_combo[BTN_SEQ] = 1; btn_long[b] = 1; toast("PATTERN COPIED"); continue; }
            if (((held >> BTN_SEQ) & 1u) && b == BTN_HOME) { if (ui.clip_ok) { snapshot(); *pat() = ui.clip; toast("PATTERN PASTED"); dirty |= D_MID; } btn_combo[BTN_SEQ] = 1; btn_long[b] = 1; continue; }
            if (((held >> BTN_GLO) & 1u) && b == BTN_PLAY) { tap_tempo(); btn_combo[BTN_GLO] = 1; continue; }
            if (!deferred(b)) tap(b);
        }
        if (((held >> b) & 1u) && deferred(b) && !btn_long[b] && fm1_ms - btn_t[b] > HOLD_MS) {
            btn_long[b] = 1;
            if (!btn_combo[b]) long_press(b);
        }
        if (((rel >> b) & 1u) && deferred(b) && !btn_combo[b] && !btn_long[b]) tap(b);
    }
    for (k = 0; k < 27u; k++) {
        if ((kp >> k) & 1u) {
            if ((held >> BTN_SEQ) & 1u) btn_combo[BTN_SEQ] = 1;
            if ((held >> BTN_SEL) & 1u) btn_combo[BTN_SEL] = 1;
            key_down(k, held);
        }
        if (((kprev & ~kh) >> k) & 1u) key_up(k);
    }
    kprev = kh;
    if ((e = fm1_enc_take(ENC_SELECT)) != 0) { prm_turn(P_CUTOFF, accel(0, e)); { char t[20], *p = s_cat(t, "CUTOFF "); prm_text(P_CUTOFF, p); toast(t); } }
    if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) { prm_turn(P_RES, accel(1, e)); { char t[20], *p = s_cat(t, "RESONANCE "); prm_text(P_RES, p); toast(t); } }
    if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
        int32_t n = (int32_t)patch_no + e;
        select_patch((uint32_t)(n < 0 ? 0 : n >= (int32_t)NPATCH ? (int32_t)NPATCH - 1 : n));
    }
    for (k = 0; k < 4u; k++)
        if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) prm_turn(cur_page()->p[k], e);
}

/* ---- the LEDs */
static void leds(void)
{
    uint32_t k, running = a_running, blink = (fm1_ms >> 7) & 1u;
    static const uint8_t PGBTN[PG_N] = {BTN_HOME, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_FX, BTN_SEL, BTN_GLO, BTN_SEQ};
    for (k = 0; k < PG_N; k++) fm1_led_key(PGBTN[k], ui.page == k);
    fm1_led_key(BTN_PLAY, running && (play_idx & 3) == 0 ? 1 : running);
    fm1_led_key(BTN_REC, ui.rec);
    fm1_led_key(BTN_OCT_DOWN, ui.page == PG_SEQ ? ui.bar > 0 : glo.oct < 0);
    fm1_led_key(BTN_OCT_UP, ui.page == PG_SEQ ? (uint32_t)(ui.bar + 1u) * 16u < pat()->len : glo.oct > 0);
    for (k = 0; k < 27u; k++) {
        int32_t w = seq_white_index(k), b = seq_black_index(k);
        int on = 0, glow = 0;
        if (w >= 0 && (fm1_in.buttons >> BTN_SEQ) & 1u) { on = (uint32_t)w == cur_pat; glow = !on; }
        else if (w >= 0 && ui.page == PG_SEQ) {
            int32_t i = (int32_t)ui.bar * 16 + w;
            if (i < (int32_t)pat()->len) {
                glow = (pat()->s[i].flags & SEQ_ON) != 0;
                on = (running && i == play_idx) || (i == ui.sel && blink);
            }
        } else if (w >= 0) {
            on = ui.live_note[k] != 0 || (ui.rec && !running && w == ui.cursor % 16);
            if (!key.chromatic) { const seq_scale_t *sc = &seq_scales[key.scale % seq_nscales]; int32_t d = w - key.transpose;
                glow = ((d % (int32_t)sc->n) + (int32_t)sc->n) % (int32_t)sc->n == 0; }
        } else if (b >= 0) {
            int tools = !key.chromatic && (ui.rec || ui.page == PG_SEQ);
            on = ui.live_note[k] != 0 || (b == 0 && ui.arm_acc) || (b == 1 && ui.arm_slide);
            glow = tools && b < 5;
        }
        fm1_led_key(KEY_LED(k), on);
        if (!on) fm1_led_key_glow(KEY_LED(k), glow);
    }
}

/* ---- MIDI in: clock and transport to the interrupt with their time; notes play (and record) */
static uint32_t idle_ok;
static uint8_t cc_sent[2] = {0xFFu, 0xFFu};      /* the cutoff and resonance last sent or received (no echo) */

static void cc_out(void)                         /* the filter's knobs out as CC 74 / 71 (a DAW can record them) */
{
    static uint32_t t_last;
    uint32_t v[2], k;
    if (!(glo.cc & 2u) || fm1_ms - t_last < 15u) return;
    t_last = fm1_ms;
    v[0] = (uint32_t)(be_clamp(lg2(ui_patch.cutoff / 20.0f) / 9.96578f, 0.0f, 1.0f) * 127.0f + 0.5f);
    v[1] = (uint32_t)(be_clamp(ui_patch.res, 0.0f, 1.0f) * 127.0f + 0.5f);
    for (k = 0; k < 2u; k++)
        if (v[k] != cc_sent[k]) { cc_sent[k] = (uint8_t)v[k]; midi_send(0xB0u | out_ch(), k ? 71u : 74u, v[k]); }
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    uint32_t t;
    while (midi_get_t(&st, &d1, &d2, &t)) {
        uint32_t ty = st & 0xF0u;
        if (st >= 0xF8u || st == 0xF2u) { clock_msg(st | (uint32_t)d1 << 8 | (uint32_t)d2 << 16, t); dirty |= D_HDR; continue; }
        if (glo.midi_ch && (st & 15u) != glo.midi_ch - 1u) continue;
        if (ty == 0xB0u && (glo.cc & 1u) && (d1 == 74u || d1 == 71u)) {   /* the DAW moves the filter */
            if (d1 == 74u) { ui_patch.cutoff = 20.0f * be_exp2((float)d2 * (9.96578f / 127.0f)); cc_sent[0] = d2; }
            else { ui_patch.res = (float)d2 * (1.0f / 127.0f); cc_sent[1] = d2; }
            patch_touched();
            continue;
        }
        if (ty == 0xC0u) { select_patch(d1); continue; }   /* program change: the DAW picks the patch (1..32) */
        if (ty == 0x90u && d2) {
            live_note(d1, d2);
            if (ui.rec && a_running && play_idx >= 0) { snapshot(); write_step(play_idx, 0, 0, d1, d2 >= 100u ? SEQ_ACC : 0u); dirty |= D_MID; }
        } else if (ty == 0x80u || ty == 0x90u) live_note(d1, 0);
    }
}
static void app_idle(void) { if (idle_ok) midi_in(); }

/* ======================================================================= main */
static void app_main(void)
{
    uint32_t last_gen = 0, t_hdr = 0, t_mid = 0, t_cpu = 0, toast_shown = 0, last_running = 0;
    be_init(&eng, FS_AUDIO);
    clk_init(&clk, FS_AUDIO);
    verb_init(&verb, FS_AUDIO);
    store_load();
    splash();
    for (;;) {
        platform_poll();
        panel();
        midi_in();
        while (oq_r != oq_w) {                   /* the sequencer's notes out */
            uint32_t m = oq[oq_r & (OQ - 1u)];
            oq_r++;
            midi_send(m & 0xFFu, (m >> 8) & 0xFFu, (m >> 16) & 0xFFu);
        }
        if (ui.preview_note && fm1_ms > ui.preview_off_at) { live_note(ui.preview_note, 0); ui.preview_note = 0; }
        cc_out();
        if (play_gen != last_gen) {              /* a step: move the playhead (a strip), or redraw if the lane scrolled */
            last_gen = play_gen;
            if (!(dirty & D_MID)) { idle_ok = 1; if (!draw_playhead()) dirty |= D_MID; idle_ok = 0; }
        }
        if (a_running != last_running) { last_running = a_running; dirty |= D_HDR | D_MID; }
        if (ui.toast_until && fm1_ms >= ui.toast_until && !toast_shown) { toast_shown = 1; dirty |= D_INFO; }
        if (fm1_ms < ui.toast_until) toast_shown = 0;
        if (clk.ext && clk.have_ref && fm1_ms - t_hdr > 500u) dirty |= D_HDR;   /* the DAW's tempo, twice a second */
        leds();
        idle_ok = 1;
        if (dirty & D_HDR) { dirty &= ~D_HDR; t_hdr = fm1_ms; draw_header(); }
        if ((dirty & D_MID) && fm1_ms - t_mid >= 33u) { dirty &= ~D_MID; t_mid = fm1_ms; draw_middle(); }
        if (dirty & D_INFO) { dirty &= ~D_INFO; draw_info(); }
        if (dirty & D_KNOB) { dirty &= ~D_KNOB; draw_knobs(); }
        idle_ok = 0;
        if (fm1_ms - t_cpu > 1000u) {
            uint32_t nn = isr_t_n;
            t_cpu = fm1_ms;
            cpu_avg10 = nn ? (isr_t_sum / nn) * 1000u / (24u * 5805u) : 0u;
            cpu_max10 = isr_t_max * 1000u / (24u * 5805u);
            isr_t_max = 0; isr_t_sum = 0; isr_t_n = 0;
            if (cur_page()->gfx == G_MIDI) dirty |= D_MID;
        }
    }
}
