/* SPDX-License-Identifier: GPL-3.0-only */
/* The M0 benchmark's voice on the Mac: the checksum the device should match (within float tolerance). */
#include <stdio.h>
#include <string.h>
#define BENCH_STANDALONE
#include "../firmware/src/bench.h"

int main(void)
{
    bvoice_t v;
    static float out[BENCH_N];
    float sum;
    unsigned u;
    bench_voice_init(&v);
    sum = bench_voice(&v, out, BENCH_N);
    memcpy(&u, &sum, 4);
    printf("checksum %.6f  bits %u (0x%08X)  out[255] %.6f\n", sum, u, u, out[BENCH_N - 1]);
    return 0;
}
