/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 JazzMasterT */
/* The player (player.h). */
#include "player.h"

void player_block(clk_t *c, const player_cfg_t *cfg, uint32_t S, uint32_t n, player_emit_fn fn, void *ctx)
{
    while (c->running) {
        float len = clk_step_len(c);
        float off = clk_step_offset(c, c->step, S);
        int32_t o;
        if (c->step & 1)
            off += (cfg->swing - 0.5f) * 2.0f * len;
        off -= cfg->lead_ms * 0.001f * c->fs;
        o = (int32_t)(off + (off >= 0.0f ? 0.5f : -0.5f));   /* the nearest sample */
        if (o >= (int32_t)n)
            break;                               /* in a later block */
        if ((float)o < -0.5f * len) {            /* far in the past (a position jump): skip, do not burst */
            clk_advance(c);
            continue;
        }
        fn(ctx, o < 0 ? 0u : (uint32_t)o, c->step);   /* a little late (the first tick after Start): now */
        clk_advance(c);
    }
}
