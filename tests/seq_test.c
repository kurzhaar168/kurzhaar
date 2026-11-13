/* SPDX-License-Identifier: GPL-3.0-only */
/* The sequencer's data (firmware/engine/seq.c) on the Mac.   cc -o build/host/seq_test tests/seq_test.c && ... */
#include <stdio.h>
#include "../firmware/engine/seq.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

int main(void)
{
    seq_key_t k = {9, 0, 0, 0};                  /* A minor */
    seq_step_t s = {SEQ_ON, 0, 0, SEQ_GATE50};
    seq_pat_t p;
    uint32_t root, sc, key, i, n;
    int32_t tr, m, prev;

    /* the reference bassline's notes in A minor */
    CHECK(seq_note(&k, &s) == 33, "A minor root = A1 (33), got %d", seq_note(&k, &s));
    s.deg = 2; CHECK(seq_note(&k, &s) == 36, "degree 2 = C2");
    s.deg = 6; s.oct = -1; CHECK(seq_note(&k, &s) == 31, "degree 6 octave -1 = G1");
    s.deg = 0; s.oct = 1; CHECK(seq_note(&k, &s) == 45, "octave +1 = A2");

    /* every note survives being stored, in every key, scale and transpose */
    for (root = 0; root < 12u; root++)
        for (sc = 0; sc < seq_nscales; sc++)
            for (tr = -7; tr <= 7; tr++)
                for (m = 12; m <= 108; m++) {
                    seq_key_t kk = {(uint8_t)root, (uint8_t)sc, 0, (int8_t)tr};
                    seq_step_t t = {SEQ_ON, 0, 0, SEQ_GATE75};
                    seq_set_note(&kk, &t, m);
                    CHECK(seq_note(&kk, &t) == m, "root %u scale %s transpose %d: %d stored, %d back", root,
                          seq_scales[sc].name, tr, m, seq_note(&kk, &t));
                    CHECK(seq_gate(&t) == SEQ_GATE75, "the gate is kept");
                }

    /* a scale change recolours: A minor C (degree 2) becomes C# in A major */
    s.deg = 2; s.oct = 0; s.gate_chrom = SEQ_GATE50;
    k.scale = 1; CHECK(seq_note(&k, &s) == 37, "degree 2 in A major = C#2");
    k.scale = 0; k.root = 5; CHECK(seq_note(&k, &s) == 32, "degree 2 in F minor = G#1 (Ab1)");
    k.root = 9;

    /* the keyboard in scale: white keys rise through the scale from the root; black keys play nothing */
    for (sc = 0; sc < seq_nscales; sc++) {
        seq_key_t kk = {9, (uint8_t)sc, 0, 0};
        prev = -1; n = 0;
        for (key = 0; key < 27u; key++) {
            int32_t note = seq_key_note(&kk, key, 0);
            if (seq_white_index(key) < 0) { CHECK(note == -1, "black key %u silent in scale", key); continue; }
            CHECK(note > prev, "%s: white keys rise (key %u: %d after %d)", seq_scales[sc].name, key, note, prev);
            if (n == 0) CHECK(note == 45, "the first white key plays the root (A2), got %d", note);
            prev = note; n++;
        }
        CHECK(n == 16, "16 white keys");
    }
    {   /* chromatic: a normal keyboard */
        seq_key_t kk = {9, 0, 1, 0};
        for (key = 0; key < 27u; key++) CHECK(seq_key_note(&kk, key, 0) == 41 + (int32_t)key, "chromatic key %u", key);
    }
    CHECK(seq_black_index(1) == 0 && seq_black_index(3) == 1 && seq_black_index(5) == 2 && seq_black_index(8) == 3
          && seq_black_index(10) == 4 && seq_black_index(25) == 10, "black key order (F#3 G#3 A#3 C#4 D#4 ... F#5)");

    /* rotate */
    seq_clear(&p, 16);
    for (i = 0; i < 16u; i++) p.s[i].deg = (int8_t)i;
    seq_rotate(&p, 1); CHECK(p.s[0].deg == 15 && p.s[1].deg == 0, "rotate right");
    seq_rotate(&p, -1); seq_rotate(&p, -1); CHECK(p.s[0].deg == 1 && p.s[15].deg == 0, "rotate left");
    CHECK(sizeof(seq_step_t) == 4 && sizeof(seq_pat_t) == 260, "sizes: step %zu, pattern %zu", sizeof(seq_step_t), sizeof(seq_pat_t));

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
