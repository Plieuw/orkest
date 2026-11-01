/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the keyboard (firmware/src/keys.c) and Orkest's sample player (synth.c), the same sources as the
 * firmware, against stubs of the key scan and the USB-MIDI rings. Checks: a key plays its note at the octave set and
 * sends MIDI, the octave changing under a held key, notes held by several sources, MIDI in (note on, velocity 0,
 * CC 123, a ring overflow), the boot hold; the ADPCM decoder against the generator's (loops included), the pitch
 * arithmetic, every sound (level, loops sustain, one-shots end, release), layers, voice stealing, the room's tail,
 * silence at MASTER 0.
 *   tests/basis_test [OUT.wav]   (OUT.wav: every sound in turn, a phrase each, to listen to) */
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
#define ORK_HOST_TEST 1

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

static int patch_of(const char *name)
{
    int i;
    for (i = 0; i < ORK_NPATCH; i++)
        if (!strcmp(ORK_PATCH[i].name, name))
            return i;
    fprintf(stderr, "no patch %s\n", name);
    exit(2);
}
static void use_patch(int p)                 /* as ui.c patch_select */
{
    sys.patch = (uint8_t)p;
    sys.attack = ORK_PATCH[p].attack;
    sys.release = ORK_PATCH[p].release;
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
    uint32_t k, z;
    int p;
    keys_init();
    sys.master_q12 = 4096;
    sys.reverb = 0;
    use_patch(patch_of("VIOLINS"));

    /* the decoder: every zone decodes to what tools/gen_orkest.py decoded (a loop: one more pass, from its
     * stored state) */
    {
        int same = 1;
        for (z = 0; z < ORK_NZONES; z++) {
            const ork_zone_t *zn = &ORK_ZONES[z];
            voice_t v;
            int64_t sum = 0;
            uint32_t i, tot = zn->n + (zn->looped ? zn->le - zn->ls : 0u);
            memset(&v, 0, sizeof v);
            v.z = zn;
            for (i = 0; i < tot; i++)
                sum += adpcm_next(&v);
            if (sum != ORK_CHECK[z]) {
                printf("  zone %u: sum %lld, the generator's %lld\n", z, (long long)sum, (long long)ORK_CHECK[z]);
                same = 0;
            }
        }
        check("ADPCM: every zone decodes as the generator's decoder (loops too)", same);
    }
    /* zones: inside their data, loops inside the zone, key ranges cover 0..127 without gaps */
    {
        int ok = 1;
        uint32_t i;
        for (i = 0; i < ORK_NINST; i++) {
            const ork_inst_t *in = &ORK_INST[i];
            uint32_t next = 0;
            for (z = in->z0; z < (uint32_t)in->z0 + in->nz; z++) {
                const ork_zone_t *zn = &ORK_ZONES[z];
                ok &= zn->off + (zn->n + 1u) / 2u <= ORK_DATA_BYTES && zn->lo == next && zn->hi >= zn->lo;
                ok &= !zn->looped || (zn->ls < zn->le && zn->le == zn->n);
                ok &= zn->idx <= 88u && zn->rate > 0u;
                next = zn->hi + 1u;
            }
            ok &= next == 128u && in->one == !ORK_ZONES[in->z0].looped;
        }
        check("zones: inside the data, loops inside the zones, keys 0..127 covered", ok);
    }
    /* pitch: at its root a zone plays at its own rate; an octave up twice as fast, down half */
    {
        int ok = 1;
        for (z = 0; z < ORK_NZONES; z++) {
            const ork_zone_t *zn = &ORK_ZONES[z];
            int32_t r = (zn->root16 + 8) / 16;
            double base = zn->rate * pow(2.0, (r * 16 - zn->root16) / 192.0);
            ok &= fabs(zone_step(zn, r) - base) <= 2.0 && fabs(zone_step(zn, r + 12) - 2 * base) <= 4.0 &&
                  fabs(zone_step(zn, r - 12) - base / 2) <= 2.0 && fabs(zone_step(zn, r + 7) - base * 1.4983) <= 6.0;
        }
        check("pitch: root, octave up / down, a fifth (2^(7/12)) within 0.01 %", ok);
    }

    /* a key: its note (octave 0: key 0 = F3 = 53), MIDI out on channel 1, sound, release */
    fm1_in.notes = 1u << key_of(60);
    run_ms(10);
    check("key C4 down: note 60 sounds", sounding() == 1 && voice[0].note == 60 && sys.last_note == 60);
    check("... and goes out as MIDI note on 90 3C 64", out_n == 1 && sent(0, 0x90, 60, KEY_VEL));
    peak = 0;
    run_ms(400);
    check("... at a sane level (peak 1000..32767)", peak > 1000 && peak <= 32767);
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
    {
        uint32_t i, held = 0;
        for (i = 0; i < NVOICE; i++)
            held += voice[i].note == 64 && voice[i].stage == ST_SUSTAIN;
        check("key + MIDI on the same note: the key let go, it still sounds", held == 1);
        midi_in(0x80, 64, 0);
        run_ms(10);
        for (held = 0, i = 0; i < NVOICE; i++)
            held += voice[i].stage == ST_SUSTAIN || voice[i].stage == ST_ATTACK;
        check("... MIDI note off ends it", held == 0);
    }
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

    /* every sound: C4 held 1.5 s and the lowest / highest keys at octave -2 / +2: a sane level, never past 16 bits;
     * a sustained sound still sounds after 1.5 s (its loop), a one-shot has ended on its own; the release ends all */
    {
        int lvl = 1, sus = 1, one = 1, rel = 1;
        static const int32_t NOTES[3] = {60, 53 - 24, 79 + 24};
        for (p = 0; p < ORK_NPATCH; p++) {
            const ork_patch_t *pt = &ORK_PATCH[p];
            int oneshot = ORK_INST[pt->l[0].inst].one;
            use_patch(p);
            for (k = 0; k < 3; k++) {
                int32_t pk;
                synth_kill();
                peak = 0;
                synth_note_on((uint32_t)NOTES[k], 100);
                run_ms(k ? 300 : 1500);
                pk = peak;
                if (pk < 300 || pk > 32767) {
                    printf("  %s note %d: peak %d\n", pt->name, NOTES[k], pk);
                    lvl = 0;
                }
                if (!k && !oneshot && sounding() == 0) {
                    printf("  %s: silent after 1.5 s held\n", pt->name);
                    sus = 0;
                }
                if (!k && oneshot && pt->nl == 1 && sounding() != 0 && ORK_INST[pt->l[0].inst].z0 >= 0) {
                    run_ms(1000);
                    if (sounding()) {
                        printf("  %s: still sounding 2.5 s after a one-shot\n", pt->name);
                        one = 0;
                    }
                }
                synth_note_off((uint32_t)NOTES[k]);
                run_ms(3500);
                if (sounding()) {
                    printf("  %s note %d: %u voices 3.5 s after note off\n", pt->name, NOTES[k], sounding());
                    rel = 0;
                }
            }
        }
        check("every sound, low / middle / high: a sane level within 16 bits", lvl);
        check("... sustained sounds hold (loops) for 1.5 s", sus);
        check("... one-shots end on their own", one);
        check("... and everything ends after note off", rel);
    }

    /* layers: STRINGS plays two voices per note (violins + celli an octave down), TUTTI three */
    synth_kill();
    use_patch(patch_of("STRINGS"));
    synth_note_on(67, 100);
    run_ms(10);
    {
        const ork_inst_t *ce = &ORK_INST[ORK_PATCH[patch_of("CELLI")].l[0].inst];
        check("STRINGS: 2 layers per note, the celli an octave down", sounding() == 2 &&
              voice[1].z >= &ORK_ZONES[ce->z0] && voice[1].z < &ORK_ZONES[ce->z0 + ce->nz] &&
              voice[1].z->lo <= 55 && voice[1].z->hi >= 55);
    }
    synth_kill();
    use_patch(patch_of("TUTTI"));
    synth_note_on(67, 100);
    run_ms(10);
    check("TUTTI: 3 layers per note", sounding() == 3);
    synth_kill();

    /* every key at once on TUTTI: NVOICE voices, no more; the output stays within 16 bits */
    fm1_in.notes = (1u << NKEYS) - 1u;
    peak = 0;
    run_ms(1000);
    check("27 keys at once on TUTTI: 24 voices sound, the output stays within 16 bits",
          sounding() == NVOICE && peak <= 32767);
    fm1_in.notes = 0;
    run_ms(4000);
    check("... all released", sounding() == 0);

    /* the room: a tail after the note, then silence */
    use_patch(patch_of("VIOLINS"));
    sys.reverb = 100;
    synth_note_on(60, 127);
    run_ms(800);
    synth_note_off(60);
    run_ms(700);
    peak = 0;
    run_ms(200);
    check("ROOM 100: a tail after the voice has ended", sounding() == 0 && peak > 30);
    run_ms(9000);
    peak = 0;
    run_ms(500);
    check("... that dies away (< -60 dB within 10 s)", peak < 32);
    sys.reverb = 0;

    /* MASTER at 0: silence */
    sys.master_q12 = 0;
    peak = 0;
    synth_note_on(60, 127);
    run_ms(50);
    check("MASTER 0: silence", peak == 0);
    synth_kill();
    sys.master_q12 = 4096;

    /* to listen to: every sound in turn, a short phrase each (a chord, then a scale up), with the room */
    if (argc > 1 && (wav = fopen(argv[1], "wb")) != 0) {
        static const uint8_t SCALE[] = {60, 62, 64, 65, 67, 69, 71, 72};
        wav_hdr(wav, 0);
        sys.reverb = 40;
        for (p = 0; p < ORK_NPATCH; p++) {
            use_patch(p);
            synth_note_on(48, 90);
            synth_note_on(55, 90);
            synth_note_on(64, 90);
            run_ms(1100);
            synth_note_off(48);
            synth_note_off(55);
            synth_note_off(64);
            run_ms(250);
            for (k = 0; k < sizeof SCALE; k++) {
                synth_note_on(SCALE[k], 100);
                run_ms(170);
                synth_note_off(SCALE[k]);
            }
            run_ms(900);
        }
        fseek(wav, 0, SEEK_SET);
        wav_hdr(wav, wav_frames);
        fclose(wav);
        printf("wrote %s (%u frames)\n", argv[1], wav_frames);
    }

    printf(bad ? "orkest test: %d FAILED\n" : "basis test passed\n", bad);
    return bad != 0;
}
