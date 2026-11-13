/* SPDX-License-Identifier: GPL-3.0-only */
/* The player: per audio block, the steps that fall inside it, each at its exact sample (docs/DESIGN.md 5.1).
 * Swing delays the odd 16ths; the timing offset plays everything early by a fixed time (to cancel the output's
 * latency, 5.4). The caller (the audio interrupt) renders the engine up to each step's sample, then plays it. */
#ifndef PLAYER_H
#define PLAYER_H
#include "clk.h"

typedef struct {
    float swing;                                 /* 0.5 (straight) .. 0.75 (hard shuffle): the odd 16th's place in the pair */
    float lead_ms;                               /* play this much early (-20..20 ms) */
} player_cfg_t;

typedef void (*player_emit_fn)(void *ctx, uint32_t offset, int32_t step);

/* block [S, S + n): call fn for each step in it, in order, offset 0..n-1 */
void player_block(clk_t *c, const player_cfg_t *cfg, uint32_t S, uint32_t n, player_emit_fn fn, void *ctx);
#endif
