/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Basis core: the sizes and the few shared values every other file uses.
 * Everything here is deliberately small; grow it as your firmware grows. */
#include <stdint.h>

/* ------------------------------------------------------------ audio --- */
#define FS 44100                 /* nominal sample rate (the I2S clock runs at ~44,117.6 Hz) */
#define HALF_FRAMES 128          /* I2S half buffer: 2.9 ms; one audio interrupt renders one half */
#define CTL 32                   /* frames per block: keys and MIDI are handled once a block (~0.7 ms) */
#define NVOICE 8                 /* synth voices (synth.c) */

/* --------------------------------------------------------- keyboard --- */
#define NKEYS 27u                /* the note keys: bit n of fm1_in.notes, 0 = F3 .. 26 = G5 */
#define KEY_BASE_NOTE 53         /* key 0 at octave 0: F3 = MIDI note 53 */
#define OCT_MIN (-3)
#define OCT_MAX 3

/* ----------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */

/* boot-loop guard (main.c): two boots in a row that die in the first 30 s -> UBOOT */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* What the main loop and the audio interrupt share. Single writer per field:
 *   main loop writes:  octave, wave, attack, release, master_q12
 *   audio ISR writes:  last_note, last_vel, cpu_q8, voices */
static volatile struct {
    int8_t octave;               /* OCT- / OCT+: -3..+3 */
    uint8_t wave;                /* synth.c W_* */
    uint8_t attack, release;     /* 0..127 */
    uint16_t master_q12;         /* MASTER pot: 0..4096 */
    uint8_t last_note, last_vel; /* the last note started (keys or MIDI); 0xFF = none yet */
    uint8_t voices;              /* voices sounding */
    uint32_t cpu_q8;             /* audio load, 256 = 100 % */
    int32_t batt_raw;            /* battery ADC, smoothed (main loop) */
} sys = {0, 1, 2, 40, 2048, 0xFF, 0, 0, 0, 0};
