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
typedef struct {
    uint32_t off, n, ls, le;     /* data offset (bytes), samples, loop [ls, le) */
    uint32_t rate;               /* the zone's sample rate / 44100, Q16 */
    int16_t root16;              /* root pitch, 1/16 semitone */
    int16_t pred;                /* ADPCM state at ls */
    uint8_t idx, lo, hi, looped;
} ork_zone_t;
typedef struct {
    char name[14];
    uint8_t z0, nz, one, pad;    /* zones; one = one-shot (ignores note off) */
    uint16_t gain;               /* Q8 */
} ork_inst_t;
typedef struct {
    uint8_t inst, lo, hi;        /* instrument; the keys it plays */
    int8_t tr;                   /* transposition, semitones */
    uint16_t gain;               /* Q8 */
    int8_t pan;                  /* -64 left .. +63 right */
    uint8_t pad;
} ork_layer_t;
typedef struct {
    char name[14];
    uint8_t attack, release, nl, pad[3];
    ork_layer_t l[3];
} ork_patch_t;
_Static_assert(sizeof(ork_zone_t) == 28 && sizeof(ork_inst_t) == 20 && sizeof(ork_patch_t) == 44, "bank layout");

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
};
extern struct ork_settings ork_set;
/* What the player reports (written by the audio ISR). */
struct ork_status {
    volatile uint8_t voices, last_note, last_vel, idle;   /* idle: not playing from the bank (ork_player_stop) */
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
