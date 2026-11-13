/* SPDX-License-Identifier: GPL-3.0-only */
/* The clock and player on the Mac, driven by a scenario: messages with arrival times (in samples), delivered at the
 * next audio block like on the FM-1 (the main loop timestamps them; the audio interrupt takes them at its next
 * block). Prints where each step was played. tools/timing_tests.py writes the scenarios and checks the results.
 *   build/host/clock_test SCENARIO.txt
 * SCENARIO lines: "fs F", "block N", "swing X", "lead MS", "end SAMPLE", then "TIME msg [arg]" with msg one of
 * tick, start, stop, cont, spp POS16, play, halt, bpm BPM10 (TIME in samples, may have a fraction). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/engine/clk.c"
#include "../firmware/engine/player.c"

typedef struct { double t; char msg[8]; uint32_t arg; } msg_t;

static void emit(void *ctx, uint32_t offset, int32_t step)
{
    uint32_t S = *(uint32_t *)ctx;
    printf("step %d %u\n", (int)step, S + offset);
}

int main(int argc, char **argv)
{
    static clk_t c;
    player_cfg_t cfg = {0.5f, 0.0f};
    msg_t *m = NULL;
    size_t nm = 0, cap = 0, i = 0;
    double fs = 44100.0;
    uint32_t block = 256, end = 0, S;
    char line[128];
    FILE *f;
    if (argc != 2) { fprintf(stderr, "usage: clock_test SCENARIO\n"); return 2; }
    f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    while (fgets(line, sizeof line, f)) {
        double t; char w[16]; unsigned a = 0; float x;
        if (sscanf(line, "fs %lf", &fs) == 1) continue;
        if (sscanf(line, "block %u", &block) == 1) continue;
        if (sscanf(line, "swing %f", &x) == 1) { cfg.swing = x; continue; }
        if (sscanf(line, "lead %f", &x) == 1) { cfg.lead_ms = x; continue; }
        if (sscanf(line, "end %u", &end) == 1) continue;
        if (sscanf(line, "%lf %15s %u", &t, w, &a) >= 2) {
            if (nm == cap) m = realloc(m, (cap = cap ? cap * 2 : 1024) * sizeof *m);
            m[nm].t = t; strncpy(m[nm].msg, w, sizeof m[nm].msg - 1); m[nm].msg[7] = 0; m[nm].arg = a; nm++;
        }
    }
    fclose(f);
    clk_init(&c, (float)fs);
    for (S = 0; S < end; S += block) {
        while (i < nm && m[i].t < (double)S) {    /* arrived during the last block: delivered now, with its time */
            uint32_t ti = (uint32_t)m[i].t;
            float tf = (float)(m[i].t - (double)ti);
            const char *w = m[i].msg;
            if (!strcmp(w, "tick")) clk_ext_tick(&c, ti, tf);
            else if (!strcmp(w, "start")) clk_ext_start(&c);
            else if (!strcmp(w, "stop")) clk_ext_stop(&c);
            else if (!strcmp(w, "cont")) clk_ext_continue(&c);
            else if (!strcmp(w, "spp")) clk_ext_spp(&c, m[i].arg);
            else if (!strcmp(w, "play")) clk_play(&c, S);
            else if (!strcmp(w, "halt")) clk_stop(&c);
            else if (!strcmp(w, "bpm")) clk_set_bpm(&c, m[i].arg, S);
            i++;
        }
        clk_ext_poll(&c, S);
        player_block(&c, &cfg, S, block, emit, &S);
    }
    printf("bpm %.3f\n", clk_bpm(&c));
    printf("fast %u jumps %u drifts\n", c.n_fast & 0xFFFFu, c.n_fast >> 16);
    free(m);
    return 0;
}
