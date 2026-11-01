/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the keyboard (firmware/src/keys.c) and the synth (synth.c), the same sources as the firmware,
 * against stubs of the key scan and the USB-MIDI rings. Checks: a key plays its note at the octave set and sends
 * MIDI, the octave changing under a held key, notes held by several sources, MIDI in (note on, velocity 0, CC 123,
 * a ring overflow), the boot hold, pitch, levels, voice stealing, silence at MASTER 0.
 *   tests/basis_test [OUT.wav]   (OUT.wav: a short render of the keys, to listen to) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define memset felucca_memset              /* (libc.c's own, beside the host's) */
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;

#include "../firmware/src/core.h"

/* the USB-MIDI rings of usb.c */
#define MQ 64u
static uint32_t midi_in_q[MQ];
static volatile uint32_t mi_w, mi_r;
static volatile uint8_t midi_in_overflow;
static uint32_t out_log[256], out_n;
static void midi_out_event(uint32_t pkt) { if (out_n < 256u) out_log[out_n++] = pkt; }
static void midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_in_q[mi_w % MQ] = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
    mi_w++;
}

#include "../firmware/src/synth.c"
#include "../firmware/src/keys.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static int32_t buf[2 * CTL];
static int32_t peak;
static FILE *wav;
static uint32_t wav_frames;
static void block(void)                      /* audio.c's audio_block, minus the USB tap and the 24-bit shift */
{
    uint32_t i;
    keys_block();
    synth_render(buf, CTL);
    for (i = 0; i < 2u * CTL; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak)
            peak = a;
    }
    if (wav) {
        for (i = 0; i < CTL; i++) {
            int16_t s[2] = {(int16_t)buf[2 * i], (int16_t)buf[2 * i + 1]};
            fwrite(s, 2, 2, wav);
        }
        wav_frames += CTL;
    }
}
static void run_ms(uint32_t ms)
{
    uint32_t n = ms * FS / 1000u / CTL + 1u;
    while (n--)
        block();
}
static uint32_t sounding(void) { uint32_t i, n = 0; for (i = 0; i < NVOICE; i++) n += voice[i].stage != ST_OFF; return n; }
static uint32_t key_of(uint32_t note) { return note - KEY_BASE_NOTE; }   /* at octave 0 */
static int sent(uint32_t i, uint32_t status, uint32_t note, uint32_t vel)
{
    uint32_t p = out_log[i];
    return i < out_n && ((p >> 8) & 0xFFu) == status && ((p >> 16) & 0x7Fu) == note && ((p >> 24) & 0x7Fu) == vel;
}

static double measure_hz(uint32_t note, uint32_t wave)   /* sine / tri: upward zero crossings over 1 s */
{
    uint32_t frames = 0, cross = 0, i;
    int32_t prev = 0;
    synth_kill();
    sys.wave = (uint8_t)wave;
    sys.attack = 0;
    synth_note_on(note, 127);
    run_ms(20);
    while (frames < FS) {
        synth_render(buf, CTL);
        for (i = 0; i < CTL; i++, frames++) {
            if (prev < 0 && buf[2 * i] >= 0)
                cross++;
            prev = buf[2 * i];
        }
    }
    synth_kill();
    return cross;
}

static void wav_hdr(FILE *f, uint32_t frames)
{
    uint32_t v;
    uint16_t a = 1, ch = 2, ba = 4, bits = 16;
    uint32_t sr = FS, br = FS * 4u;
    fwrite("RIFF", 1, 4, f); v = 36 + frames * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    fwrite(&a, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); v = frames * 4; fwrite(&v, 4, 1, f);
}

int main(int argc, char **argv)
{
    uint32_t k, w;
    keys_init();
    sys.master_q12 = 4096;

    /* a key: its note (octave 0: key 0 = F3 = 53), MIDI out on channel 1, sound, release */
    fm1_in.notes = 1u << key_of(60);
    run_ms(10);
    check("key C4 down: note 60 sounds", sounding() == 1 && voice[0].note == 60 && sys.last_note == 60);
    check("... and goes out as MIDI note on 90 3C 64", out_n == 1 && sent(0, 0x90, 60, KEY_VEL));
    peak = 0;
    run_ms(100);
    check("... at a sane level (peak 2000..32767)", peak > 2000 && peak <= 32767);
    fm1_in.notes = 0;
    run_ms(5);
    check("key up: MIDI note off 80 3C", out_n == 2 && sent(1, 0x80, 60, 0));
    run_ms(2000);
    check("... the release ends the voice", sounding() == 0);

    /* the octave changes while a key is held: the key-up ends the note it started */
    out_n = 0;
    sys.octave = 1;
    fm1_in.notes = 1u << key_of(60);
    run_ms(10);
    sys.octave = -1;
    fm1_in.notes = 0;
    run_ms(10);
    check("octave +1: the key plays 72; changed to -1 while held, 72 still ends",
          sent(0, 0x90, 72, KEY_VEL) && sent(1, 0x80, 72, 0) && voice[0].stage == ST_RELEASE);
    sys.octave = 0;
    run_ms(2000);

    /* the lowest and the highest key */
    out_n = 0;
    fm1_in.notes = 1u | 1u << 26;
    run_ms(10);
    fm1_in.notes = 0;
    run_ms(10);
    check("keys 0 and 26: F3 (53) and G5 (79)",
          out_n == 4 && sent(0, 0x90, 53, KEY_VEL) && sent(1, 0x90, 79, KEY_VEL));
    run_ms(2000);

    /* a note held by a key and by MIDI ends with the last of them */
    fm1_in.notes = 1u << key_of(64);
    midi_in(0x90, 64, 90);
    run_ms(10);
    fm1_in.notes = 0;
    run_ms(10);
    check("key + MIDI on the same note: the key let go, it still sounds", voice[0].note == 64 && voice[0].stage == ST_SUSTAIN);
    midi_in(0x80, 64, 0);
    run_ms(10);
    check("... MIDI note off ends it", voice[0].stage == ST_RELEASE);
    run_ms(2000);

    /* MIDI in: note on, velocity 0 as note off, CC 123 */
    midi_in(0x93, 48, 127);
    midi_in(0x95, 55, 64);
    run_ms(10);
    check("MIDI in, any channel: two notes", sounding() == 2 && sys.last_vel == 64);
    midi_in(0x93, 48, 0);
    run_ms(10);
    check("note on with velocity 0 is a note off", note_held[48] == 0 && note_held[55] == 1);
    midi_in(0xB0, 123, 0);
    run_ms(10);
    check("CC 123 releases everything", note_held[55] == 0 && voice[0].stage != ST_SUSTAIN && voice[1].stage != ST_SUSTAIN);
    run_ms(2000);

    /* a ring overflow: everything let go */
    midi_in(0x90, 50, 100);
    run_ms(10);
    midi_in_overflow = 1;
    run_ms(10);
    check("MIDI ring overflow: held notes released, ring emptied",
          note_held[50] == 0 && mi_r == mi_w && !midi_in_overflow);
    run_ms(2000);

    /* the boot hold: keys pressed under the splash are silent, down and up */
    out_n = 0;
    kb_boot_hold = 1;
    fm1_in.notes = 1u << 3;
    run_ms(10);
    kb_boot_hold = 0;
    run_ms(10);
    fm1_in.notes = 0;
    run_ms(10);
    check("a key held from boot: no note, no MIDI, not even on release", out_n == 0 && sounding() == 0);

    /* every key at once: NVOICE voices, no more */
    fm1_in.notes = (1u << NKEYS) - 1u;
    peak = 0;
    for (w = 0; w < W_COUNT; w++) {
        sys.wave = (uint8_t)w;
        run_ms(200);
    }
    check("27 keys at once: 8 voices sound, the output stays within 16 bits", sounding() == NVOICE && peak <= 32767);
    fm1_in.notes = 0;
    run_ms(3000);
    check("... all released", sounding() == 0);

    /* pitch */
    {
        double a4 = measure_hz(69, W_SINE), c2 = measure_hz(36, W_TRI), top = measure_hz(127, W_SINE);
        check("A4 sine: 440 Hz", fabs(a4 - 440.0) <= 1.0);
        check("C2 triangle: 65.4 Hz", fabs(c2 - 65.4) <= 1.0);
        check("G9 (note 127) sine: 12544 Hz (within 0.2 %)", fabs(top - 12543.9) <= 25.0);
    }

    /* MASTER at 0: silence */
    sys.master_q12 = 0;
    peak = 0;
    synth_note_on(60, 127);
    run_ms(50);
    check("MASTER 0: silence", peak == 0);
    synth_kill();
    sys.master_q12 = 4096;

    /* a short render to listen to: a C major arpeggio, each wave in turn */
    if (argc > 1 && (wav = fopen(argv[1], "wb")) != 0) {
        static const uint8_t ARP[] = {60, 64, 67, 72, 67, 64};
        wav_hdr(wav, 0);
        sys.attack = 4;
        sys.release = 50;
        for (w = 0; w < W_COUNT; w++) {
            sys.wave = (uint8_t)w;
            for (k = 0; k < sizeof ARP; k++) {
                fm1_in.notes = 1u << key_of(ARP[k]);
                run_ms(150);
                fm1_in.notes = 0;
                run_ms(50);
            }
            run_ms(300);
        }
        fseek(wav, 0, SEEK_SET);
        wav_hdr(wav, wav_frames);
        fclose(wav);
        printf("wrote %s (%u frames)\n", argv[1], wav_frames);
    }

    printf(bad ? "basis test: %d FAILED\n" : "basis test passed\n", bad);
    return bad != 0;
}
