/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* Render the bass engine on the Mac: a patch file and a note event list in, a 24-bit mono WAV out.
 *   build/host/render PATCH.txt EVENTS.txt OUT.wav
 * PATCH.txt: "name = value" lines (be_patch_t field names), '#' comments; unset fields keep the defaults.
 * EVENTS.txt: "sample on note vel" / "sample off note" lines, in order, at 44.1 kHz; "end sample" ends.
 * tools/render.py turns MIDI files into event lists and runs this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include "../firmware/engine/be.c"

#define FS 44100u

static const struct { const char *name; size_t off; } FIELDS[] = {
#define F(n) {#n, offsetof(be_patch_t, n)}
    F(osc1_wave), F(osc1_pw), F(osc1_level), F(osc2_wave), F(osc2_pw), F(osc2_level), F(osc2_semi),
    F(osc2_detune), F(sub_wave), F(sub_oct), F(sub_level), F(noise_level), F(drive), F(cutoff), F(res),
    F(bass_comp), F(pole2), F(keytrack), F(fenv_amt), F(f_a), F(f_d), F(f_s), F(f_r), F(a_a), F(a_d), F(a_s),
    F(a_r), F(vel_sens), F(accent), F(glide), F(glide_mode), F(analog), F(sat), F(hpf), F(level),
    F(verb_mix), F(verb_decay), F(verb_tone), F(verb_lowcut),
    F(lfo1_rate), F(lfo1_shape), F(lfo1_dest), F(lfo1_depth), F(lfo1_sync), F(lfo1_retrig),
    F(lfo2_rate), F(lfo2_shape), F(lfo2_dest), F(lfo2_depth), F(lfo2_sync), F(lfo2_retrig),
#undef F
};

static void load_patch(be_patch_t *p, const char *path)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        char name[64], *h = strchr(line, '#');
        float v;
        size_t i;
        if (h) *h = 0;
        if (sscanf(line, " %63[a-z0-9_] = %f", name, &v) != 2) continue;
        for (i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++)
            if (!strcmp(FIELDS[i].name, name)) { *(float *)((char *)p + FIELDS[i].off) = v; break; }
        if (i == sizeof FIELDS / sizeof FIELDS[0]) { fprintf(stderr, "%s: unknown parameter %s\n", path, name); exit(1); }
    }
    fclose(f);
}

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }

int main(int argc, char **argv)
{
    static be_t e;
    FILE *ev, *wav;
    char line[128];
    uint32_t pos = 0, total = 0, at, note, vel, i;
    float *buf, peak = 0.0f;
    double t_cpu = 0;
    char kind[8];
    typedef struct { uint32_t at; uint8_t on, note, vel; } evt_t;
    evt_t *evs = NULL;
    uint32_t nev = 0, cap = 0;
    if (argc != 4) { fprintf(stderr, "usage: render PATCH.txt EVENTS.txt OUT.wav\n"); return 2; }
    be_init(&e, (float)FS);
    load_patch(&e.p, argv[1]);
    be_patch_changed(&e);
    ev = fopen(argv[2], "r");
    if (!ev) { perror(argv[2]); return 1; }
    while (fgets(line, sizeof line, ev)) {       /* all events first */
        evt_t *x;
        int nv = sscanf(line, "%u %7s %u %u", &at, kind, &note, &vel);
        if (nv < 2) continue;
        if (!strcmp(kind, "end")) { total = at; continue; }
        if (nev == cap) evs = realloc(evs, (cap = cap ? cap * 2 : 256) * sizeof *evs);
        x = &evs[nev++];
        x->at = at; x->on = !strcmp(kind, "on"); x->note = (uint8_t)note; x->vel = (uint8_t)(nv >= 4 ? vel : 0u);
    }
    fclose(ev);
    if (!total) { fprintf(stderr, "%s: no end line\n", argv[2]); return 1; }
    buf = calloc(total, sizeof *buf);
    for (i = 0; i <= nev; i++) {
        uint32_t next = i < nev && evs[i].at < total ? evs[i].at : total;
        clock_t c0 = clock();
        while (pos < next) {                     /* render up to the event, in blocks of the FM-1's size */
            uint32_t n = next - pos > 256u ? 256u : next - pos;
            be_render(&e, buf + pos, n);
            pos += n;
        }
        t_cpu += (double)(clock() - c0) / CLOCKS_PER_SEC;
        if (i < nev) {
            if (evs[i].on) be_note_on(&e, evs[i].note, evs[i].vel);
            else be_note_off(&e, evs[i].note);
        }
    }
    wav = fopen(argv[3], "wb");
    if (!wav) { perror(argv[3]); return 1; }
    fwrite("RIFF", 1, 4, wav); put32(wav, 36 + total * 3); fwrite("WAVEfmt ", 1, 8, wav);
    put32(wav, 16); put16(wav, 1); put16(wav, 1); put32(wav, FS); put32(wav, FS * 3); put16(wav, 3); put16(wav, 24);
    fwrite("data", 1, 4, wav); put32(wav, total * 3);
    for (i = 0; i < total; i++) {
        float a = buf[i] < 0 ? -buf[i] : buf[i];
        int32_t s = (int32_t)(buf[i] * 8388607.0f);
        if (a > peak) peak = a;
        fputc(s & 255, wav); fputc(s >> 8 & 255, wav); fputc(s >> 16 & 255, wav);
    }
    fclose(wav);
    printf("%s: %.2f s, peak %.1f dBFS, engine %.1f ms on this Mac (%.2f%% of real time)\n", argv[3],
           total / (double)FS, 20 * __builtin_log10(peak > 1e-9f ? peak : 1e-9f), t_cpu * 1000,
           100 * t_cpu / (total / (double)FS));
    free(buf);
    return 0;
}
