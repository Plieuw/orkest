/* SPDX-License-Identifier: GPL-3.0-only */
/* The keyboard and MIDI in, once per audio block (audio ISR: lowest latency, no locking needed).
 *
 * Keys: the 27 note keys (fm1_in.notes, debounced by the TIMER5 scan in hal/fm1_input.h) play the synth at the
 * octave OCT- / OCT+ set, and go out as MIDI (USB, channel 1). A key remembers the note it started, so changing
 * the octave while a key is down never leaves a note hanging.
 * MIDI in (USB and the TRS jack, usb.c / midi_uart.c rings): note on / off on any channel plays the synth;
 * CC 123 (all notes off) releases everything.
 * A note held by several sources (two keys at different octaves, a key and MIDI) ends when the last lets go.
 *
 *   keys_block()     audio ISR, before the block renders
 *   keys_panic_req   main loop: set to release everything at the next block */
#define KB_SILENT 0xFFu
#define KEY_VEL 100u                             /* the keys have no velocity: a fixed one */
#define MIDI_OUT_CH 0u                           /* channel 1 */

static volatile uint8_t kb_boot_hold;            /* set from power-on to the end of the splash (main.c): keys silent */
static volatile uint8_t keys_panic_req;
static uint32_t kb_prev;                         /* fm1_in.notes at the last block */
static uint8_t kb_note[NKEYS];                   /* the note key k started (KB_SILENT: none) */
static uint8_t note_held[128];                   /* sources holding each note */

static void note_start(uint32_t note, uint32_t vel)
{
    if (note_held[note] < 255u)
        note_held[note]++;
    synth_note_on(note, vel);
}

static void note_end(uint32_t note)
{
    if (!note_held[note])
        return;
    if (--note_held[note] == 0u)
        synth_note_off(note);
}

static void keys_all_off(void)
{
    uint32_t k;
    for (k = 0; k < 128u; k++)
        note_held[k] = 0;
    for (k = 0; k < NKEYS; k++)
        if (kb_note[k] != KB_SILENT) {
            midi_out_event(0x08u | (0x80u | MIDI_OUT_CH) << 8 | (uint32_t)kb_note[k] << 16);
            kb_note[k] = KB_SILENT;
        }
    synth_all_off();
}

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k;
    kb_prev = cur;
    if (!ch)
        return;
    for (k = 0; k < NKEYS; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u) {                   /* down */
            int32_t n = KEY_BASE_NOTE + (int32_t)k + 12 * sys.octave;
            if (kb_boot_hold || n < 0 || n > 127) {
                kb_note[k] = KB_SILENT;
                continue;
            }
            kb_note[k] = (uint8_t)n;
            note_start((uint32_t)n, KEY_VEL);
            midi_out_event(0x09u | (0x90u | MIDI_OUT_CH) << 8 | (uint32_t)n << 16 | KEY_VEL << 24);
        } else if (kb_note[k] != KB_SILENT) {    /* up: the note it started */
            note_end(kb_note[k]);
            midi_out_event(0x08u | (0x80u | MIDI_OUT_CH) << 8 | (uint32_t)kb_note[k] << 16);
            kb_note[k] = KB_SILENT;
        }
    }
}

static void midi_in_block(void)
{
    if (midi_in_overflow) {                      /* the ring overflowed: a note-off may be lost, release all */
        mi_r = mi_w;
        keys_all_off();
        RING_PUBLISH();
        midi_in_overflow = 0;
        return;
    }
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        mi_r++;
        if (st == 0x90u && d2)
            note_start(d1, d2);
        else if (st == 0x80u || st == 0x90u)     /* (note on with velocity 0 is a note off) */
            note_end(d1);
        else if (st == 0xB0u && (d1 == 123u || d1 == 120u))
            keys_all_off();
        /* add here: CC -> parameters, pitch bend, clock (status >= 0xF8: pkt >> 8 & 0xFF) ... */
    }
}

static void keys_init(void)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        kb_note[k] = KB_SILENT;
}

static void keys_block(void)
{
    if (keys_panic_req) {
        keys_panic_req = 0;
        keys_all_off();
        synth_kill();
    }
    keyboard_block();
    midi_in_block();
}
