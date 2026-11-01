/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Orkest on FM1 Open: the orchestral sample player and its sound bank.
 *
 *   orkest_player.c  the voices (IMA ADPCM, layers, loops, envelope), the room; audio ISR
 *   orkest_bank.c    the bank in the user flash: checking, binding, the SysEx upload; control task
 *   orkest_ui.c      screen, keys, knobs, LEDs, settings; control task
 *   orkest_sdk.c     what the FM1 SDK calls in an APP (MIDI in, the instrument hooks it no longer needs)
 *
 * The bank (v14/tools/gen_bank.py) holds every table and sample; the firmware has none of its own. */
#ifndef ORKEST_H
#define ORKEST_H
#include <stdint.h>
#include <stddef.h>
#include "fm1.h"

/* ------------------------------------------------------------ the bank --- */
/* bank format 2 (v14/tools/gen_bank.py): every note in three parts, played as independent streams: the
 * TRANSIENT (once), the SUSTAIN (a loop, swelling in over [w_t0, w_t1) source samples), and the instrument's RELEASE
 * (from note off) */
typedef struct {
    uint32_t t_off, t_n;         /* transient: data offset (bytes), samples */
    uint32_t l_off, l_n, l_ls;   /* sustain: data offset, samples (a short preroll, then the loop), loop start */
    int16_t l_pred;              /* the sustain's ADPCM state at its loop start (where it starts too) */
    uint8_t l_idx, lo, hi, flags;   /* keys lo..hi; flags bit 0: has a sustain */
    uint8_t pad[2];
    uint32_t rate;               /* the zone's sample rate / 44100, Q16 */
    int16_t root16;              /* root pitch, 1/16 semitone */
    uint16_t w_t0;               /* the sustain's swell: 0 before w_t0, 1 from w_t1 (source samples) */
    uint32_t w_t1;
} ork_zone_t;
typedef struct {
    char name[14];
    uint8_t z0, nz, one, has_rel;   /* zones; one = one-shot (ignores note off); has a release */
    uint16_t gain, rel_gain;     /* Q8 */
    int16_t rel_root16;
    uint32_t rel_off, rel_n, rel_rate;
} ork_inst_t;
typedef struct {
    uint8_t inst, lo, hi;        /* instrument; the keys it plays */
    int8_t tr;                   /* transposition, semitones */
    uint16_t gain;               /* Q8 */
    int8_t pan;                  /* -64 left .. +63 right */
    uint8_t tinst;               /* the instrument whose transients it plays (255: its own) */
} ork_layer_t;
typedef struct {
    char name[14];
    uint8_t attack, release, nl, pad[3];
    ork_layer_t l[3];
} ork_patch_t;
_Static_assert(sizeof(ork_zone_t) == 40 && sizeof(ork_inst_t) == 36 && sizeof(ork_patch_t) == 44, "bank layout");

struct ork_bank_header {
    uint32_t magic;              /* ORK_MAGIC */
    uint16_t version, header_bytes;
    uint32_t total, crc;         /* bytes; CRC-32 of [header_bytes, total) */
    uint32_t commit;             /* ORK_COMMIT once completely written (0xFFFFFFFF in the file) */
    uint16_t nzones, ninst, npatch, maxl;
    uint32_t zones_off, inst_off, patch_off, data_off, data_bytes;
    char name[16];
};
_Static_assert(sizeof(struct ork_bank_header) == 64, "bank header");
#define ORK_MAGIC 0x424B524FU    /* "ORKB" */
#define ORK_COMMIT 0x54494D43U   /* "CMIT" */
enum { ORK_MAX_ZONES = 160, ORK_MAX_INST = 40, ORK_MAX_PATCH = 64 };

struct ork_bank {
    const uint8_t *data;         /* the ADPCM, mapped from the flash */
    uint32_t data_bytes;
    uint16_t nz, ni, np;
    char name[16];
    ork_zone_t zone[ORK_MAX_ZONES];
    ork_inst_t inst[ORK_MAX_INST];
    ork_patch_t patch[ORK_MAX_PATCH];
};
extern struct ork_bank ork_bank;
/* 0 = the bank's tables are valid, else why not (ORK_BANK_*) */
enum { ORK_BANK_OK, ORK_BANK_NONE, ORK_BANK_BAD, ORK_BANK_CRC, ORK_BANK_LOADING };
/* check a bank image (in flash or RAM) and copy its tables: 0 or ORK_BANK_*. Control task, the player stopped */
int ork_bank_bind(const uint8_t *image, uint32_t bytes);
unsigned ork_bank_state(void);
void ork_bank_boot(void);        /* control task: map and bind the bank in the user flash */
void ork_bank_poll(void);        /* control task: a pending upload message */
void ork_bank_sysex(const uint8_t *m, size_t n, unsigned source);   /* routing task: queue an upload message */
uint32_t ork_bank_progress(void); /* bytes received of the upload in progress (0: none) */

/* ---------------------------------------------------------- the player --- */
/* Shared settings. Single writer: the control task. */
struct ork_settings {
    volatile uint8_t patch, attack, release, reverb;   /* patch index; 0..127 */
    volatile uint8_t master;                            /* 0..127 */
    /* the LFO (one for all voices, each voice fades it in after its note on): on, rate, vibrato (0..127 = 0..60
     * cents), tremolo (0..127 = 0..90 % level), delay (0..127 = 0..3 s); lfo_inc / lfo_dstep: the control task's
     * phase step per block and fade-in step per block (Q16) for rate and delay */
    volatile uint8_t lfo_on, lfo_rate, lfo_vib, lfo_trem, lfo_delay;
    volatile uint32_t lfo_inc, lfo_dstep;
    /* tempo sync: lfo_sync 1 = the LFO follows the tempo, one cycle per lfo_ticks MIDI clock ticks (24 a quarter
     * note); MIDI clock in sets the tempo while it runs, else lfo_inc carries the internal tempo (control task) */
    volatile uint8_t lfo_sync;
    volatile uint16_t lfo_ticks;
    /* the parts' levels: transient (0..127, 64 = as recorded; below, the sustain also takes over the start: 0 = the
     * sustain alone from the first sample), tail = the release sample (0 = none, 64 = as recorded) */
    volatile uint8_t transient, tail;
    /* ARP: a key plays a phrase (orkest_phrases.inc) from its note instead of the note: on, phrase, speed (0 half,
     * 1 as written, 2 double), gate (10..100 % of each note's length), mode (0 HOLD: loops while held, 1 ONCE: once
     * through per press, 2 LATCH: loops until another key or ARP off); arp_inc: the internal tempo, MIDI clock ticks a
     * block (Q16; MIDI clock in takes over while it runs) */
    volatile uint8_t arp_on, arp_phrase, arp_speed, arp_gate, arp_mode;
    /* SEQ: the scale ARP keeps to (orkest_scales.inc, 0 off) and its root (0 C .. 11 B); a phrase's notes outside it
     * move to the nearest note in it (the lower one at a tie) */
    volatile uint8_t scale, scale_root;
    /* ARP velocity, apart from the phrase: velo_pat (orkest_velo.inc; 0 the phrase's own), one step a note played;
     * velo_depth 0..100 % of the pattern; velo_human 0..100 (a random +-0..30); velo_reset 0 FREE (runs on over the
     * phrase's repeats), 1 LOOP (from its start with each repeat) */
    volatile uint8_t velo_pat, velo_depth, velo_human, velo_reset;
    /* SEQ PARTS: with parts_on, ARP plays up to four parts from each key, each its own sound (part_patch: 0 off,
     * else the patch + 1), phrase, velocity pattern and octave (-2..+2) */
    volatile uint8_t parts_on;
    volatile uint8_t part_patch[4], part_phrase[4], part_velo[4];
    volatile int8_t part_oct[4];
    volatile uint32_t arp_inc;
};
/* a phrase's note: start (ticks, 24 a quarter note), length (ticks), interval from the key (semitones), velocity */
typedef struct { uint16_t at; uint8_t len; int8_t iv; uint8_t vel; } ork_pev_t;   /* vel: 100 = the key's velocity */
typedef struct { uint16_t first, n, len; } ork_phrase_t;   /* its notes in PEV, its length (ticks) */
extern struct ork_settings ork_set;
/* What the player reports (written by the audio ISR). */
struct ork_status {
    volatile uint8_t voices, last_note, last_vel, idle;   /* idle: not playing from the bank (ork_player_stop) */
    volatile uint8_t clock;                               /* MIDI clock is coming in */
    volatile uint16_t clock_bpm10;                        /* its tempo, 0.1 BPM */
};
extern struct ork_status ork_stat;

/* Note events into the player: lane 0 the control task (keys), lane 1 the MIDI routing task. One producer each.
 * 0 = queued, -1 = full. status: 0x90 note on, 0x80 note off, 0xB0 controller (123/120: all off). */
int ork_post(unsigned lane, uint8_t status, uint8_t d1, uint8_t d2);
/* control task: stop every voice and keep the player away from the bank until ork_player_start (bank rewrites) */
void ork_player_stop(void);
void ork_player_start(void);
void ork_panic(void);            /* any task: release everything at the next block */
/* audio ISR: one block (FM1_BLOCK_FRAMES stereo frames, signed Q23) */
FM1_AUDIO_CODE int orkest_render(int32_t *out);
FM1_AUDIO_CODE unsigned orkest_output_level(void);   /* MASTER, 0..127 */
/* audio.c: the audio ISR's load, 256 = a whole block */
extern volatile uint16_t orkest_load_q8;
#endif
