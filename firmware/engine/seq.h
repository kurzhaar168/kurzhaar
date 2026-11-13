/* SPDX-License-Identifier: GPL-3.0-only */
/* The sequencer's data (docs/DESIGN.md 5.1, 5.2): patterns of steps, the key and scale, and how a step becomes a note.
 *
 * A step stores a scale degree and an octave, not a MIDI note: change the key or the scale and every pattern follows
 * musically (C minor -> F minor transposes it; minor -> dorian recolours it). A note outside the scale (entered in
 * chromatic mode) is the scale degree below it plus a chromatic offset (1 or 2 semitones: pentatonic, blues and
 * harmonic minor have gaps of 3), so it stays the same distance from its neighbour.
 * Plain C, no hardware: the FM-1's interface and audio interrupt use it, tests/seq_test.c checks it. */
#ifndef SEQ_H
#define SEQ_H
#include <stdint.h>

#define SEQ_NPAT 16u
#define SEQ_MAXLEN 64u

enum { SEQ_ON = 1u, SEQ_ACC = 2u, SEQ_SLIDE = 4u, SEQ_TIE = 8u };
enum { SEQ_GATE25 = 0, SEQ_GATE50 = 1, SEQ_GATE75 = 2, SEQ_GATE100 = 3 };

typedef struct {                                 /* one step: 4 bytes */
    uint8_t flags;                               /* SEQ_ON | SEQ_ACC | SEQ_SLIDE | SEQ_TIE */
    int8_t deg;                                  /* scale degree from the root (0 = root), any sign */
    int8_t oct;                                  /* octave offset */
    uint8_t gate_chrom;                          /* gate (bits 0-1: 25/50/75/100 %), chromatic offset 0..3 (bits 2-3) */
} seq_step_t;

typedef struct {
    uint8_t len;                                 /* 1..64 steps */
    uint8_t reserved[3];
    seq_step_t s[SEQ_MAXLEN];
} seq_pat_t;

typedef struct {
    uint8_t root;                                /* 0 = C .. 11 = B */
    uint8_t scale;                               /* index into seq_scales */
    uint8_t chromatic;                           /* the keyboard: 0 = in scale, 1 = a normal keyboard */
    int8_t transpose;                            /* in scale degrees */
} seq_key_t;

typedef struct { const char *name; uint8_t n; uint8_t iv[7]; } seq_scale_t;
extern const seq_scale_t seq_scales[];
extern const uint32_t seq_nscales;

#define SEQ_BASE_NOTE 24                         /* the root's octave 0 starts at C1: A1 = 33 */

static inline uint32_t seq_gate(const seq_step_t *s) { return s->gate_chrom & 3u; }
static inline uint32_t seq_chrom(const seq_step_t *s) { return (s->gate_chrom >> 2) & 3u; }

int32_t seq_note(const seq_key_t *k, const seq_step_t *s);          /* the MIDI note a step plays */
void seq_set_note(const seq_key_t *k, seq_step_t *s, int32_t midi); /* the step to play that note (degree, octave, offset) */
int32_t seq_key_note(const seq_key_t *k, uint32_t key, int32_t oct); /* the FM-1's key 0..26 (F3..G5): its note, or -1
                                                                        (a black key with the keyboard in scale) */
int32_t seq_white_index(uint32_t key);           /* key 0..26 -> 0..15 for a white key, -1 for a black one */
int32_t seq_black_index(uint32_t key);           /* key 0..26 -> 0..10 for a black key, -1 for a white one */
void seq_rotate(seq_pat_t *p, int32_t dir);      /* by one step, right (dir > 0) or left */
void seq_clear(seq_pat_t *p, uint32_t len);
#endif
