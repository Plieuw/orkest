/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host test of Orkest on FM1 Open: the player (app/orkest_player.c) and the bank (app/orkest_bank.c), the same
 * sources as the firmware, against a simulated user flash and MIDI output.
 *   - the bank: binding, its checks (damaged, uncommitted, not a bank), the decoder against the generator's
 *   - the upload: INFO, BEGIN, DATA in 512-byte chunks, END, as the installer sends them; the flash afterwards;
 *     errors (wrong order, bad checksum, END too early) answered with NAK and nothing committed
 *   - the player: every sound (level, loops hold, one-shots end, release ends), layers, 12 voices at most,
 *     notes held by keys and MIDI, all notes off, the room's tail, the stop during a bank rewrite
 *   tests/orkest_test [OUT.wav]   (OUT.wav: every sound in turn, a phrase each) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "orkest.h"
#include "fm1_device.h"
#include "orkest_bank_file.h"
#include "orkest_lfo.inc"
#define ORK_PHRASE_EVENTS
#define ORK_PHRASE_NAMES
#include "orkest_phrases.inc"
#define ORK_VELO_STEPS
#define ORK_VELO_NAMES
#include "orkest_velo.inc"
/* ARP part 1's fields under their earlier names */
#define arp_phrase part[0].phrase
#define arp_speed part[0].speed
#define arp_gate part[0].gate
#define velo_pat part[0].velo
#define velo_depth part[0].depth
#define velo_human part[0].human

/* --------------------------------------------------- simulated SDK --- */
#define FLASH_BYTES 536576u
static uint8_t flash[FLASH_BYTES];
static uint32_t now_ms, erases, writes;
uint32_t fm1_millis(void) { return now_ms += 1u; }
uint32_t fm1_flash_size(void) { return FLASH_BYTES; }
int fm1_flash_read(uint32_t off, void *d, size_t n)
{
    if (off > FLASH_BYTES || n > FLASH_BYTES - off) return FM1_FLASH_INVALID;
    memcpy(d, flash + off, n);
    return FM1_FLASH_OK;
}
int fm1_flash_erase(uint32_t off)
{
    if (off % FM1_FLASH_SECTOR_BYTES || off >= FLASH_BYTES) return FM1_FLASH_INVALID;
    memset(flash + off, 0xFF, FM1_FLASH_SECTOR_BYTES);
    erases++;
    return FM1_FLASH_OK;
}
int fm1_flash_write(uint32_t off, const void *d, size_t n)
{
    const uint8_t *s = d;
    size_t i;
    if (off > FLASH_BYTES || n > FLASH_BYTES - off) return FM1_FLASH_INVALID;
    for (i = 0; i < n; i++) {
        if ((flash[off + i] & s[i]) != s[i]) return FM1_FLASH_NEEDS_ERASE;   /* only 1 -> 0 */
        flash[off + i] &= s[i];
    }
    writes++;
    return FM1_FLASH_OK;
}
const uint8_t *fm1_flash_map(uint32_t off, size_t n) { return off <= FLASH_BYTES && n <= FLASH_BYTES - off ? flash + off : NULL; }
int fm1_flash_invalidate(uint32_t off, size_t n) { (void)off; (void)n; return 0; }
static uint8_t reply_buf[16][96];
static unsigned reply_len[16], replies;
int fm1_midi_send_port(unsigned port, const uint8_t *m, size_t n)
{
    (void)port;
    if (replies < 16u && n <= 96u) {
        memcpy(reply_buf[replies], m, n);
        reply_len[replies++] = (unsigned)n;
    }
    return 0;
}
unsigned fm1_midi_input_source(void) { return FM1_MIDI_USB; }

/* the audio ISR, as board/audio.c drives the player, minus the DMA */
volatile uint16_t orkest_load_q8;
static int32_t blk[2 * FM1_BLOCK_FRAMES];
static int32_t peak;
static FILE *wav;
static uint32_t wav_frames;
static double energy;                    /* the sum of squares over the last render() */
static void render(uint32_t ms)
{
    energy = 0;
    uint32_t b = ms * FM1_SAMPLE_RATE / 1000u / FM1_BLOCK_FRAMES + 1u, i;
    while (b--) {
        orkest_render(blk);
        for (i = 0; i < 2u * FM1_BLOCK_FRAMES; i++) {
            int32_t a = blk[i] < 0 ? -blk[i] : blk[i];
            if (a > peak) peak = a;
            energy += (double)blk[i] * blk[i];
        }
        if (wav) {
            for (i = 0; i < 2u * FM1_BLOCK_FRAMES; i++) {
                int16_t s = (int16_t)(blk[i] >> 8);
                fwrite(&s, 2, 1, wav);
            }
            wav_frames += FM1_BLOCK_FRAMES;
        }
    }
}

static int bad;
static void check(const char *what, int ok)
{
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ------------------------------------------------- upload messages --- */
static unsigned put7(uint8_t *p, uint32_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++, v >>= 7) p[i] = (uint8_t)(v & 0x7F);
    return n;
}
static uint32_t get7(const uint8_t *p, unsigned n)
{
    uint32_t v = 0;
    while (n--) v = v << 7 | p[n];
    return v;
}
/* send one message, run the control task, return the reply (body after F0 7D 4F 4B) */
static const uint8_t *send(const uint8_t *body, unsigned n, unsigned *rn)
{
    static uint8_t m[1100];
    m[0] = 0xF0; m[1] = 0x7D; m[2] = 0x4F; m[3] = 0x4B;
    memcpy(m + 4, body, n);
    m[4 + n] = 0xF7;
    replies = 0;
    ork_bank_sysex(m, n + 5u, FM1_MIDI_USB);
    ork_bank_poll();
    render(3);                                   /* the audio keeps running between messages */
    if (!replies) { *rn = 0; return NULL; }
    *rn = reply_len[0] - 5u;
    return reply_buf[0] + 4;
}
static int data_msg(uint32_t off, const uint8_t *d, unsigned cnt, int corrupt)
{
    uint8_t b[1100];
    unsigned n = 0, i, g, sum = 0, rn;
    const uint8_t *r;
    b[n++] = 0x21;
    n += put7(b + n, off, 5);
    n += put7(b + n, cnt, 2);
    for (g = 0; g < (cnt + 6u) / 7u; g++) {
        uint8_t top = 0;
        for (i = 0; i < 7u && g * 7u + i < cnt; i++) top |= (uint8_t)((d[g * 7u + i] >> 7) << i);
        b[n++] = top;
        for (i = 0; i < 7u; i++) b[n++] = g * 7u + i < cnt ? d[g * 7u + i] & 0x7F : 0;
    }
    for (i = 0; i < cnt; i++) sum ^= d[i];
    b[n++] = (uint8_t)((sum ^ (corrupt ? 1u : 0u)) & 0x7F);
    r = send(b, n, &rn);
    return r && rn == 7u && r[0] == 0x7E && r[1] == 0x21 && get7(r + 2, 5) == off + cnt ? 0 : r && r[0] == 0x7D ? 1 : 2;
}

static int patch_of(const char *name)
{
    int i;
    for (i = 0; i < ork_bank.np; i++) if (!strcmp(ork_bank.patch[i].name, name)) return i;
    fprintf(stderr, "no patch %s\n", name);
    exit(2);
}
static void use(int p) { ork_set.patch = (uint8_t)p; ork_set.part[0].patch = (uint8_t)p; ork_set.attack = ork_bank.patch[p].attack; ork_set.release = ork_bank.patch[p].release; }
static unsigned sounding(void) { render(1); return ork_stat.voices; }
static void note(uint8_t st, uint8_t n, uint8_t v) { (void)ork_post(1, st, n, v); }

int main(int argc, char **argv)
{
    unsigned rn, i;
    const uint8_t *r;
    static uint8_t image[sizeof ORK_BANK_FILE];
    struct ork_bank_header *h = (struct ork_bank_header *)image;
    ork_set.master = 127;
    ork_set.reverb = 0;

    /* ---- the bank file: checks */
    memcpy(image, ORK_BANK_FILE, sizeof image);
    check("bank as generated (commit word unset): not used", ork_bank_bind(image, sizeof image) == ORK_BANK_BAD);
    h->commit = ORK_COMMIT;
    check("... committed: bound", ork_bank_bind(image, sizeof image) == ORK_BANK_OK && ork_bank.np == 24 && ork_bank.nz == 60);
    image[sizeof image / 2] ^= 0x10;
    check("... one bit damaged: CRC mismatch", ork_bank_bind(image, sizeof image) == ORK_BANK_CRC);
    image[sizeof image / 2] ^= 0x10;
    check("... cut short: not used", ork_bank_bind(image, sizeof image - 100) == ORK_BANK_BAD);
    check("... not a bank: none", ork_bank_bind(flash, 4096) == ORK_BANK_NONE);

    /* ---- the decoder: every wave as the generator decodes it (a sustain: from its loop start, one pass more) */
    ork_bank_bind(image, sizeof image);
    {
        int same = 1;
        extern long long ork_test_decode(unsigned zone, unsigned which, unsigned samples);
        for (i = 0; i < ork_bank.nz; i++) {
            const ork_zone_t *z = &ork_bank.zone[i];
            long long t = z->t_n ? ork_test_decode(i, 0, z->t_n) : 0;
            long long l = z->l_n ? ork_test_decode(i, 1, 2u * (z->l_n - z->l_ls)) : 0;
            if (t != ORK_BANK_CHECK[2u * i] || l != ORK_BANK_CHECK[2u * i + 1u]) {
                printf("  zone %u: %lld %lld, the generator's %lld %lld\n", i, t, l, ORK_BANK_CHECK[2u * i], ORK_BANK_CHECK[2u * i + 1u]);
                same = 0;
            }
        }
        check("decoder: every transient and sustain decodes as the generator's (loops too)", same);
        same = 1;
        for (i = 0; i < ork_bank.ni; i++) {
            const ork_inst_t *in = &ork_bank.inst[i];
            long long r = in->has_rel ? ork_test_decode(i, 2, in->rel_n) : 0;
            if (r != ORK_BANK_RCHECK[i]) { printf("  release %u: %lld, the generator's %lld\n", i, r, ORK_BANK_RCHECK[i]); same = 0; }
        }
        check("... every release too", same);
    }

    /* ---- the upload, as the installer sends it */
    memset(flash, 0xFF, sizeof flash);
    ork_bank_boot();
    check("empty flash at boot: no bank, the player silent", ork_bank_state() == ORK_BANK_NONE && ork_stat.idle);
    {
        uint8_t b[16] = {0x10};
        r = send(b, 1, &rn);
        check("INFO: answered, no bank, the flash's size", r && r[0] == 0x11 && r[1] == ORK_BANK_NONE &&
              get7(r + 7, 5) == FLASH_BYTES - 4096u);
        b[0] = 0x20;
        put7(b + 1, FLASH_BYTES, 5);
        r = send(b, 6, &rn);
        check("BEGIN larger than the flash: NAK", r && r[0] == 0x7D && r[1] == 0x20);
        put7(b + 1, sizeof ORK_BANK_FILE, 5);
        r = send(b, 6, &rn);
        check("BEGIN: ACK, loading", r && r[0] == 0x7E && r[1] == 0x20 && ork_bank_state() == ORK_BANK_LOADING);
        check("DATA out of order: NAK", data_msg(512, ORK_BANK_FILE + 512, 512, 0) == 1);
        check("DATA with a bad checksum: NAK", data_msg(0, ORK_BANK_FILE, 512, 1) == 1);
        b[0] = 0x22;
        r = send(b, 1, &rn);
        check("END before all data: NAK, nothing committed", r && r[0] == 0x7D && r[1] == 0x22);
        put7(b + 1, sizeof ORK_BANK_FILE, 5);
        b[0] = 0x20;
        r = send(b, 6, &rn);
        {
            uint32_t off;
            int ok = 1;
            erases = 0;
            for (off = 0; off < sizeof ORK_BANK_FILE && ok; off += 512u) {
                unsigned cnt = sizeof ORK_BANK_FILE - off < 512u ? (unsigned)(sizeof ORK_BANK_FILE - off) : 512u;
                ok = data_msg(off, ORK_BANK_FILE + off, cnt, 0) == 0;
            }
            check("DATA, 512 bytes a message: every one ACKed with the next offset", ok);
            check("... each sector erased once", erases == (sizeof ORK_BANK_FILE + 4095u) / 4096u);
        }
        b[0] = 0x22;
        r = send(b, 1, &rn);
        check("END: ACK, bank bound, the player running", r && r[0] == 0x7E && r[1] == 0x22 &&
              ork_bank_state() == ORK_BANK_OK && !ork_stat.idle);
        check("... the flash holds the file, its commit word written",
              !memcmp(flash + 24, ORK_BANK_FILE + 24, sizeof ORK_BANK_FILE - 24u) &&
              ((struct ork_bank_header *)flash)->commit == ORK_COMMIT);
        ork_bank_boot();
        check("a power cycle: the bank from the flash", ork_bank_state() == ORK_BANK_OK && ork_bank.np == 24);
        b[0] = 0x10;
        r = send(b, 1, &rn);
        check("INFO: the bank's size and name", r && r[1] == ORK_BANK_OK && get7(r + 2, 5) == sizeof ORK_BANK_FILE &&
              !memcmp(r + 12, "ORKEST 2", 8));
    }

    /* ---- the player */
    use(patch_of("VIOLINS"));
    note(0x90, 60, 100);
    peak = 0;
    render(400);
    check("MIDI note on: a voice, a sane level (Q23 peak 2^17 .. 2^23)", ork_stat.voices == 1 && peak > (1 << 17) && peak < (1 << 23));
    note(0x80, 60, 0);
    render(2000);
    check("... note off: the release ends it", sounding() == 0);
    (void)ork_post(0, 0x90, 64, 100);            /* a key and MIDI on the same note */
    note(0x90, 64, 90);
    render(20);
    (void)ork_post(0, 0x80, 64, 0);
    render(300);
    check("a note held by a key and MIDI: the key let go, it sounds on", sounding() == 1);
    note(0x80, 64, 0);
    render(2000);
    check("... MIDI lets go: it ends", sounding() == 0);
    note(0x90, 48, 100);
    note(0x93, 55, 100);
    render(50);
    note(0xB0, 123, 0);
    render(2000);
    check("all notes off (CC 123): everything ends", sounding() == 0);
    {
        int lvl = 1, sus = 1, one = 1, rel = 1, p;
        static const uint8_t N[3] = {60, 29, 103};
        for (p = 0; p < ork_bank.np; p++) {
            int oneshot = ork_bank.inst[ork_bank.patch[p].l[0].inst].one;
            use(p);
            for (i = 0; i < 3u; i++) {
                ork_panic();
                render(3000);
                peak = 0;
                note(0x90, N[i], 100);
                render(i ? 300 : 1500);
                if (peak < (1 << 14) || peak >= (1 << 23)) { printf("  %s note %u: peak %d\n", ork_bank.patch[p].name, N[i], peak); lvl = 0; }
                if (!i && !oneshot && !sounding()) { printf("  %s: silent after 1.5 s held\n", ork_bank.patch[p].name); sus = 0; }
                if (!i && oneshot && ork_bank.patch[p].nl == 1) {
                    render(1200);
                    if (sounding()) { printf("  %s: one-shot still sounding\n", ork_bank.patch[p].name); one = 0; }
                }
                note(0x80, N[i], 0);
                render(3500);
                if (sounding()) { printf("  %s note %u: sounds 3.5 s after note off\n", ork_bank.patch[p].name, N[i]); rel = 0; }
            }
        }
        check("every sound, low / middle / high: a sane level within 24 bits", lvl);
        check("... sustained sounds hold (their loops) for 1.5 s", sus);
        check("... one-shots end on their own", one);
        check("... everything ends after note off", rel);
    }
    use(patch_of("TUTTI"));
    note(0x90, 67, 100);
    render(10);
    check("TUTTI: 3 layers per note", ork_stat.voices == 3);
    for (i = 40; i < 80u; i++) note(0x90, (uint8_t)i, 100);
    peak = 0;
    render(800);
    check("40 notes on TUTTI: 12 voices, the output within 24 bits", ork_stat.voices == 12 && peak < (1 << 23));
    note(0xB0, 123, 0);
    render(4000);
    check("... all released", sounding() == 0);

    /* ---- the three parts: transient, sustain, release */
    {
        double e64, e127, e0, early0, t64, t0;
        unsigned m = (unsigned)patch_of("MARCATO"), si;
        const ork_layer_t *ly = &ork_bank.patch[m].l[0];
        use(patch_of("VIOLINS"));
        ork_set.attack = 0;
        note(0x90, 62, 100); render(3); early0 = energy; render(150); e64 = energy; note(0x80, 62, 0); render(3000);
        ork_set.transient = 127;
        note(0x90, 62, 100); render(3); render(150); e127 = energy; note(0x80, 62, 0); render(3000);
        ork_set.transient = 0;
        note(0x90, 62, 100); render(3); t0 = energy; render(150); e0 = energy; note(0x80, 62, 0); render(3000);
        ork_set.transient = 64;
        printf("  VIOLINS first 150 ms (dB vs 64): transient 127 %+.1f, 0 %+.1f\n", 10 * log10(e127 / e64), 10 * log10(e0 / e64));
        check("TRANSIENT 127: a stronger start than 64", e127 > e64 * 1.1);
        check("TRANSIENT 0: the sustain alone, from the first sample", t0 > 0 && e0 != e64 && early0 > 0);
        note(0x90, 62, 100); render(800); note(0x80, 62, 0); render(100); render(500); t64 = energy; render(3000);
        ork_set.tail = 0;
        note(0x90, 62, 100); render(800); note(0x80, 62, 0); render(100); render(500); t0 = energy; render(3000);
        ork_set.tail = 64;
        printf("  VIOLINS 0.1 .. 0.6 s after note off (dB): tail 64 over tail 0 %+.1f\n", 10 * log10(t64 / (t0 + 1)));
        check("TAIL 64: the recorded release after note off, over TAIL 0", t64 > t0 * 1.5);
        for (si = 0; si < ork_bank.ni && strcmp(ork_bank.inst[si].name, "SPICCATO"); si++) ;
        check("MARCATO: the violins' sustain with the spiccato's transient", si < ork_bank.ni && ly->tinst == si &&
              !strcmp(ork_bank.inst[ly->inst].name, "VIOLINS"));
        note(0x90, 62, 100); render(500);
        check("... it sounds and holds", sounding() == 1);
        note(0x80, 62, 0); render(3000);
    }
    ork_set.reverb = 100;
    use(patch_of("VIOLINS"));
    note(0x90, 60, 127);
    render(800);
    note(0x80, 60, 0);
    render(700);
    peak = 0;
    render(200);
    check("ROOM 100: a tail after the voice has ended", sounding() == 0 && peak > 4000);
    render(10000);
    peak = 0;
    render(500);
    check("... that dies away (< -60 dB within 10 s)", peak < 9000);
    ork_set.reverb = 0;
    note(0x90, 60, 100);
    render(100);
    ork_player_stop();
    render(3);
    check("a bank rewrite stops the player: silence, no voice, idle", ork_stat.idle && ork_stat.voices == 0 && blk[0] == 0);
    ork_player_start();

    /* ---- the LFO: measured on FLUTE (a steady, resynthesised sustain): the pitch from zero-crossing periods and the
     * level in 20 ms windows, over 1.2 s of its held sustain */
    {
        static int32_t out[2 * 44100 * 2];
        static int32_t ref[2 * 44100 * 2];
        double pitch_sd[6], level_rng[6];
        int run, frames = (int)(1.2 * 44100) / FM1_BLOCK_FRAMES * FM1_BLOCK_FRAMES;
        /* runs: 0 off, 1 on with depth 0, 2 vibrato, 3 tremolo, 4 vibrato with a 3 s delay (first 0.3 s), 5 mod wheel */
        for (run = 0; run < 6; run++) {
            uint32_t f, b;
            double sum = 0, sum2 = 0, n = 0, lmin = 1e9, lmax = -1e9, last = -1;
            int32_t prev = 0;
            ork_panic();
            render(3000);
            use(patch_of("FLUTE"));
            ork_set.lfo_on = run >= 1 && run <= 4;
            ork_set.lfo_rate = 92;
            ork_set.lfo_vib = run == 2 || run == 4 ? 127 : 0;
            ork_set.lfo_trem = run == 3 ? 127 : 0;
            ork_set.lfo_delay = run == 4 ? 127 : 0;
            ork_set.lfo_inc = LFO_INC[92];
            ork_set.lfo_dstep = run == 4 ? 65536u / (127u * 3u * 44100u / (127u * 128u)) : 65536u;
            note(0xB0, 1, run == 5 ? 127 : 0);
            note(0x90, 72, 100);
            render(run == 4 ? 30 : 700);            /* past the attack (the delay run: from the note on) */
            for (b = 0; b < (uint32_t)frames / FM1_BLOCK_FRAMES; b++) {
                orkest_render(blk);
                memcpy(out + 2 * b * FM1_BLOCK_FRAMES, blk, sizeof blk);
            }
            if (run == 0) memcpy(ref, out, sizeof ref);
            if (run == 1) check("LFO on with no depth: the same output as off, sample for sample", !memcmp(ref, out, sizeof ref));
            {   /* periods between upward zero crossings of the fundamental (3 one-pole low-passes at 700 Hz) */
                double y1 = 0, y2 = 0, y3 = 0, py = 0, a = 1 - exp(-2 * M_PI * 700 / 44100.0);
                (void)prev;
                for (f = 0; f < (uint32_t)(run == 4 ? frames / 4 : frames); f++) {
                    y1 += (out[2 * f] - y1) * a; y2 += (y1 - y2) * a; y3 += (y2 - y3) * a;
                    if (f > 2000 && py < 0 && y3 >= 0) {
                        double t = f - 1 + -py / (y3 - py);
                        if (last >= 0) { double hz = 44100.0 / (t - last), c = 1200 * log2(hz / 523.25); sum += c; sum2 += c * c; n++; }
                        last = t;
                    }
                    py = y3;
                }
            }
            for (f = 0; f + 882 <= (uint32_t)frames; f += 882) {
                double e = 0; uint32_t j;
                for (j = 0; j < 882; j++) e += (double)out[2 * (f + j)] * out[2 * (f + j)];
                e = 10 * log10(e / 882 + 1);
                if (e < lmin) lmin = e;
                if (e > lmax) lmax = e;
            }
            pitch_sd[run] = n > 1 ? sqrt(sum2 / n - (sum / n) * (sum / n)) : 0;
            level_rng[run] = lmax - lmin;
            note(0x80, 72, 0);
            note(0xB0, 1, 0);
        }
        printf("  pitch sd (cents) off %.1f, vibrato %.1f, delayed %.1f, mod wheel %.1f; level range (dB) off %.2f, tremolo %.1f\n",
               pitch_sd[0], pitch_sd[2], pitch_sd[4], pitch_sd[5], level_rng[0], level_rng[3]);
        check("FLUTE held, no LFO: a steady pitch (< 3 cents) and level (< 1 dB)", pitch_sd[0] < 3 && level_rng[0] < 1);
        check("LFO vibrato: the pitch moves (> 20 cents sd)", pitch_sd[2] > 20);
        check("LFO tremolo: the level moves (> 6 dB)", level_rng[3] > 6);
        check("LFO delay 3 s: 0.3 s after the note on, under a quarter of the full vibrato", pitch_sd[4] < pitch_sd[2] / 4);
        check("mod wheel, LFO off: vibrato", pitch_sd[5] > 20);
        ork_set.lfo_on = 0;
        ork_set.lfo_vib = 40;
        ork_set.lfo_trem = 0;
    }

    /* ---- the LFO on the tempo: MIDI clock at 120 BPM (a tick every 918.75 samples), Start, 1/4 (24 ticks): on every
     * beat the LFO is back at its start; the clock's tempo read; then the clock stops and the internal tempo
     * (120 BPM, 1/4: 0.5 s a cycle) carries on */
    {
        extern uint32_t ork_test_lfo_phase(void);
        uint32_t blk_n, tick = 0, worst = 0;
        double next = 0;
        int32_t ph;
        ork_set.lfo_on = 1; ork_set.lfo_vib = 40; ork_set.lfo_sync = 1; ork_set.lfo_ticks = 24;
        ork_set.lfo_inc = (uint32_t)((((uint64_t)1 << 32) * 128u * 120u * 24u) / (44100ull * 60u * 24u));
        note(0xFA, 0, 0);
        for (blk_n = 0; blk_n < 44100u * 6u / 128u; blk_n++) {
            double start = blk_n * 128.0;
            int beat = 0;
            while (next < start) {                   /* the ticks due by this block */
                note(0xF8, 0, 0);
                if (++tick % 24u == 0u) beat = 1;
                next += 918.75;
            }
            orkest_render(blk);
            ph = (int32_t)ork_test_lfo_phase();      /* signed: a little before or after the beat */
            if (beat && blk_n > 44100u * 2u / 128u) {
                uint32_t a = (uint32_t)(ph < 0 ? -ph : ph);
                if (a > worst) worst = a;
            }
        }
        printf("  MIDI clock: %u.%u BPM read; LFO off its beat by at most %.1f %% of a cycle\n",
               ork_stat.clock_bpm10 / 10u, ork_stat.clock_bpm10 % 10u, worst / 42949672.96);
        check("MIDI clock 120 BPM: read as 120 (+-0.5)", ork_stat.clock && ork_stat.clock_bpm10 >= 1195 && ork_stat.clock_bpm10 <= 1205);
        check("... the LFO (1/4) back at its start on every beat (within 3 %)", worst < 0x07AE147Bu);
        {
            uint32_t p0, p1;
            render(700);                              /* no more ticks: the clock has stopped */
            p0 = ork_test_lfo_phase();
            render(500);
            p1 = ork_test_lfo_phase();
            {
                int32_t d = (int32_t)(p1 - p0);       /* 0.5 s at 120 BPM, 1/4: one whole cycle */
                check("clock gone: the internal tempo (120 BPM, 1/4 = 0.5 s a cycle) takes over", !ork_stat.clock &&
                      (d < 0 ? -d : d) < 0x0A3D70A4);
            }
        }
        ork_set.lfo_on = 0; ork_set.lfo_vib = 0; ork_set.lfo_sync = 0;
    }

    /* ---- velocity -> tone: a soft note duller than a hard one (high frequencies' share of its energy) */
    {
        double bright[2];
        unsigned q, b, k;
        static const uint8_t VEL[2] = {127, 40};
        use(patch_of("VIOLINS"));
        for (q = 0; q < 2u; q++) {
            double e = 0, d = 0;
            int32_t prev = 0;
            note(0x90, 67, VEL[q]);
            render(300);
            for (b = 0; b < 200u; b++) {
                orkest_render(blk);
                for (k = 0; k < FM1_BLOCK_FRAMES; k++) {
                    int32_t x = blk[2u * k];
                    e += (double)x * x; d += (double)(x - prev) * (x - prev); prev = x;
                }
            }
            note(0x80, 67, 0); render(2500);
            bright[q] = d / (e + 1);
        }
        printf("  VIOLINS G4: brightness (difference energy / energy) at 127 %.4f, at 40 %.4f\n", bright[0], bright[1]);
        check("velocity -> tone: velocity 40 clearly duller than 127 (brightness under 2/3)", bright[1] < bright[0] * 0.67);
    }

    /* ---- panning: the concert seating (violins left, celli right), spread 0: the middle */
    {
        static const char *const PN[2] = {"VIOLINS", "CELLI"};
        double side[2][2];
        unsigned q, sp, b, k;
        ork_set.reverb = 0;
        for (sp = 0; sp < 2u; sp++)
            for (q = 0; q < 2u; q++) {
                double el = 0, er = 0;
                ork_set.pan_spread = sp ? 0 : 100;
                use(patch_of(PN[q]));
                note(0x90, 60, 100);
                for (b = 0; b < 100u; b++) { orkest_render(blk); for (k = 0; k < 128u; k++) { el += (double)blk[2 * k] * blk[2 * k]; er += (double)blk[2 * k + 1] * blk[2 * k + 1]; } }
                note(0x80, 60, 0); render(2000);
                side[sp][q] = 10 * log10((el + 1) / (er + 1));
            }
        printf("  left over right (dB): VIOLINS %.1f, CELLI %.1f; spread 0: %.1f, %.1f\n", side[0][0], side[0][1], side[1][0], side[1][1]);
        check("seating: the violins left, the celli right", side[0][0] > 3 && side[0][1] < -3);
        check("... spread 0: both in the middle", fabs(side[1][0]) < 0.5 && fabs(side[1][1]) < 0.5);
        ork_set.pan_spread = 100;
    }

    /* ---- two layers on a key (ARP off): FLUTE + OBOE, two voices; layer 2's volume 0: one; half the volume: quieter */
    {
        unsigned most, b, w;
        double e[2] = {0, 0};
        use(patch_of("FLUTE"));
        ork_set.part[1].patch = (uint8_t)patch_of("OBOE");
        note(0x90, 72, 100); render(200); most = ork_stat.voices; note(0x80, 72, 0); render(2500);
        check("a key, two layers: both sound", most == 2u);
        ork_set.part[1].level = 0;
        note(0x90, 72, 100); render(200); most = ork_stat.voices; note(0x80, 72, 0); render(2500);
        check("... layer 2 at volume 0: one", most == 1u);
        ork_set.part[1].patch = 255; ork_set.part[1].level = 100;
        for (w = 0; w < 2u; w++) {
            ork_set.part[0].level = w ? 50 : 100;
            note(0x90, 72, 100);
            for (b = 0; b < 100u; b++) { orkest_render(blk); for (most = 0; most < 256u; most++) e[w] += (double)blk[most] * blk[most]; }
            note(0x80, 72, 0); render(2500);
        }
        printf("  layer 1 at volume 50: %.1f dB\n", 10 * log10(e[1] / e[0]));
        check("... layer 1 at volume 50: about 6 dB down", 10 * log10(e[1] / e[0]) < -5 && 10 * log10(e[1] / e[0]) > -7);
        ork_set.part[0].level = 100;
    }

    /* ---- the morph (SEQ): ONCE, attack 1 beat, decay 1 beat, linear, at 120 BPM: layer 1, layer 2 at 0.5 s, back at 1 s */
    {
        unsigned b, m[4];
        ork_set.arp_inc = (uint32_t)((65536ull * 24u * 128u * 120u) / (60u * 44100u));
        ork_set.morph_mode = 1; ork_set.morph_a = 3; ork_set.morph_d = 3; ork_set.morph_curve = 0;
        use(patch_of("FLUTE")); ork_set.part[1].patch = (uint8_t)patch_of("OBOE");
        note(0x90, 72, 100);
        for (b = 0; b < 1200u * 441u / 1280u; b++) {
            orkest_render(blk);
            if (b == 2u) m[0] = ork_stat.morph;
            if (b == 172u) m[1] = ork_stat.morph;      /* 0.5 s */
            if (b == 258u) m[2] = ork_stat.morph;      /* 0.75 s */
            if (b == 400u) m[3] = ork_stat.morph;      /* past 1 s */
        }
        note(0x80, 72, 0); render(2500);
        printf("  morph: %u at the key, %u at 0.5 s, %u at 0.75 s, %u past 1 s\n", m[0], m[1], m[2], m[3]);
        check("MORPH once, 1 beat up, 1 beat down: layer 1, layer 2, halfway back, layer 1",
              m[0] < 10u && m[1] > 245u && m[2] > 100u && m[2] < 155u && m[3] == 0u);
        ork_set.morph_mode = 0; ork_set.part[1].patch = 255;
    }
    {   /* the morph of the phrases: part 1 OSTINATO (C C Eb C G ...), part 2 OCTAVES (C4 C5); at the top only part 2's
         * notes; looping up and down over a bar each way: both phrases' notes */
        unsigned q1, q2, b, w, seen[2][128];
        for (q1 = 0; q1 < ORK_NPHRASE && strcmp(PHRASE_NAME[q1][0], "OSTINATO"); q1++) ;
        for (q2 = 0; q2 < ORK_NPHRASE && strcmp(PHRASE_NAME[q2][0], "OCTAVES"); q2++) ;
        memset(seen, 0, sizeof seen);
        use(patch_of("HARP"));
        ork_set.part[1].patch = (uint8_t)patch_of("PIZZICATO");
        ork_set.arp_on = 1; ork_set.arp_mode = 0; ork_set.part[0].phrase = (uint8_t)q1; ork_set.part[1].phrase = (uint8_t)q2;
        ork_set.part[1].oct = 0; ork_set.part[0].oct = 0;
        for (w = 0; w < 2u; w++) {
            ork_set.morph_mode = w ? 2 : 1; ork_set.morph_a = w ? 6 : 0; ork_set.morph_d = w ? 6 : 0;
            render(10);
            (void)ork_post(0, 0x90, 60, 100);
            for (b = 0; b < (w ? 4200u : 1500u) * 441u / 1280u; b++) {
                unsigned before = ork_stat.last_note;
                ork_stat.last_note = 255;
                orkest_render(blk);
                if (ork_stat.last_note != 255) seen[w][ork_stat.last_note] = 1; else ork_stat.last_note = (uint8_t)before;
            }
            (void)ork_post(0, 0x80, 60, 0); render(2500);
        }
        printf("  morph: at the top C4 %u C5 %u Eb4 %u G4 %u; looping: Eb4 %u G4 %u C5 %u\n", seen[0][60], seen[0][72],
               seen[0][63], seen[0][67], seen[1][63], seen[1][67], seen[1][72]);
        check("MORPH of the phrases: at the top part 2's notes only (C4 C5)", seen[0][60] && seen[0][72] && !seen[0][63] && !seen[0][67]);
        {
            unsigned n, other = 0;
            for (n = 0; n < 128u; n++) if (seen[1][n] && n != 60u && n != 63u && n != 67u) other++;
            check("... up and down: part 1's notes, and notes drawn towards part 2's", (seen[1][63] || seen[1][67]) && other);
        }
        ork_set.morph_mode = 0; ork_set.part[1].patch = 255; ork_set.part[0].phrase = 0; ork_set.arp_on = 0;
        render(100);
    }

    /* ---- polyphony: 16 notes, last note priority */
    {
        unsigned k;
        use(patch_of("FLUTE"));
        ork_set.release = 0;
        for (k = 0; k < 20u; k++) { note(0x90, (uint8_t)(50 + k), 100); render(20); }
        render(100);
        check("20 notes held (FLUTE, one layer): 12 sound, the oldest eight ended", ork_stat.voices == 12u);
        check("... the newest is among them", ork_stat.last_note == 69);
        for (k = 0; k < 20u; k++) note(0x80, (uint8_t)(50 + k), 0);
        render(2000);
        use(patch_of("FLUTE"));
    }

    /* ---- ARP: the phrases */
    {
        static uint8_t seq[64];
        unsigned n = 0, t, last = 255, firsts = 0;
        static const uint8_t PRELUDE[9] = {60, 60, 63, 60, 67, 60, 63, 60, 60};   /* OSTINATO from C4 */
        int same = 1;
        use(patch_of("HARP"));
        ork_set.arp_on = 1; ork_set.arp_phrase = 0; ork_set.arp_mode = 0; ork_set.arp_speed = 1; ork_set.arp_gate = 90;
        render(10);
        (void)ork_post(0, 0x90, 60, 100);           /* a key: OSTINATO from C4, 120 BPM (a 16th = 125 ms) */
        for (t = 0; t < 2100u * 441u / 1280u && n < sizeof seq; t++) {   /* block by block, 2.9 ms each */
            unsigned before = ork_stat.last_note;
            ork_stat.last_note = 255;                /* (each note on, repeated pitches too) */
            orkest_render(blk);
            if (ork_stat.last_note != 255) {
                last = ork_stat.last_note;
                if (n == 8u) firsts = t * 128u * 1000u / 44100u;   /* the 9th note: ms after the key */
                seq[n++] = (uint8_t)last;
            } else
                ork_stat.last_note = (uint8_t)before;
        }
        for (t = 0; t < 9u; t++) same &= seq[t] == PRELUDE[t];
        check("ARP, OSTINATO on C4: C C Eb C G C Eb C, again", same && n >= 9);
        printf("  OSTINATO: the 9th note %u ms after the key\n", firsts);
        check("... at 120 BPM: again 1 s after the start (+-6 ms)", firsts >= 994u && firsts <= 1006u);
        render(1000);
        check("... held: it loops (still playing after 2 s)", sounding() > 0);
        (void)ork_post(0, 0x80, 60, 0);
        render(2500);
        check("... HOLD, let go: it stops", sounding() == 0);
        {   /* MIDI clock at 150 BPM (a tick = 735 samples): the phrase follows it, the figure repeats after 0.8 s */
            double next = 0;
            unsigned blk_n, ninth = 0, cnt = 0;
            last = 255;
            for (blk_n = 0; blk_n < 44100u * 2u / 128u; blk_n++) {
                while (next < blk_n * 128.0) { note(0xF8, 0, 0); next += 735.0; }
                unsigned before = ork_stat.last_note;
                if (blk_n == 44100u / 2u / 128u) { (void)ork_post(0, 0x90, 60, 100); cnt = 0; }
                ork_stat.last_note = 255;
                orkest_render(blk);
                if (ork_stat.last_note == 255) ork_stat.last_note = (uint8_t)before;
                else if (blk_n >= 44100u / 2u / 128u && ++cnt == 9u) ninth = (blk_n - 44100u / 2u / 128u) * 128u * 1000u / 44100u;
            }
            (void)ork_post(0, 0x80, 60, 0);
            render(2500);
            printf("  MIDI clock 150 BPM: the 9th note %u ms after the key\n", ninth);
            check("ARP on MIDI clock 150 BPM: again after 0.8 s (+-10 ms)", ninth >= 790u && ninth <= 810u);
            render(800);                             /* the clock gone */
        }
        ork_set.arp_mode = 1;                        /* ONCE: a short press plays it all, then ends */
        for (t = 0; t < ORK_NPHRASE && strcmp(PHRASE_NAME[t][0], "LAMENT"); t++) ;
        ork_set.arp_phrase = (uint8_t)t;             /* LAMENT: C5 Ab4 G4, 1.25 s */
        render(10);
        (void)ork_post(0, 0x90, 62, 100); render(20); (void)ork_post(0, 0x80, 62, 0);
        render(700);
        check("ONCE, a short press: still playing at 0.7 s (LAMENT lasts 1.25 s)", sounding() > 0 && ork_stat.last_note == 69);
        render(4000);
        check("... then it ends by itself", sounding() == 0);
        {   /* a slide: SWOOP (C4, then G4 sliding in) glides on VIOLINS, not on FLUTE (it cannot) */
            extern unsigned ork_test_gliding(void);
            unsigned q, b, w, glid[2] = {0, 0};
            static const char *const SP[2] = {"VIOLINS", "FLUTE"};
            for (q = 0; q < ORK_NPHRASE && strcmp(PHRASE_NAME[q][0], "SWOOP"); q++) ;
            for (w = 0; w < 2u; w++) {
                use(patch_of(SP[w]));
                ork_set.arp_mode = 1; ork_set.arp_phrase = (uint8_t)q;
                render(10);
                (void)ork_post(0, 0x90, 60, 100); render(20); (void)ork_post(0, 0x80, 60, 0);
                for (b = 0; b < 900u * 441u / 1280u; b++) { orkest_render(blk); if (ork_test_gliding()) glid[w]++; }
                render(2500);
            }
            printf("  SWOOP: blocks gliding on VIOLINS %u, on FLUTE %u\n", glid[0], glid[1]);
            check("a slide: VIOLINS glide into the G (about 0.375 s), FLUTE does not", glid[0] > 100u && glid[0] < 140u && !glid[1]);
            ork_set.arp_mode = 0;
            use(patch_of("HARP"));
        }
        ork_set.arp_phrase = 0; render(10);
        ork_set.arp_mode = 2;                        /* LATCH: plays on after letting go, a new key takes over */
        (void)ork_post(0, 0x90, 60, 100); render(20); (void)ork_post(0, 0x80, 60, 0);
        render(3000);
        check("LATCH: plays on after letting go", sounding() > 0);
        (void)ork_post(0, 0x90, 67, 100); render(20); (void)ork_post(0, 0x80, 67, 0);
        render(30);
        check("... another key takes over: from its note", ork_stat.last_note == 67);
        ork_set.arp_on = 0;
        render(3000);
        check("ARP off: everything ends", sounding() == 0);
        {   /* SEQ scale C MAJOR: TRILL HALF from A4 (A Bb) stays on the white keys; off: Bb4 again */
            unsigned b, out = 0, sharp = 0;
            unsigned q;
            for (q = 0; q < ORK_NPHRASE && strcmp(PHRASE_NAME[q][0], "TRILL HALF"); q++) ;
            ork_set.arp_on = 1; ork_set.arp_mode = 0; ork_set.arp_phrase = (uint8_t)q; ork_set.scale = 1; ork_set.scale_root = 0;
            render(10);
            (void)ork_post(0, 0x90, 69, 100);
            for (b = 0; b < 1000u * 441u / 1280u; b++) { orkest_render(blk); if ((0x54Au >> (ork_stat.last_note % 12u)) & 1u) out++; }
            (void)ork_post(0, 0x80, 69, 0); render(2000);
            ork_set.scale = 0;
            render(10);
            (void)ork_post(0, 0x90, 69, 100);
            for (b = 0; b < 1000u * 441u / 1280u; b++) { orkest_render(blk); if (ork_stat.last_note == 70) sharp++; }
            (void)ork_post(0, 0x80, 69, 0); render(2000);
            check("SEQ C MAJOR: TRILL HALF stays in the scale (no black key)", out == 0);
            check("... scale off: as written (Bb4 played)", sharp > 0);
        }
        {   /* velocity in the phrases: GALLOP from a key at 100: soft sixteenths, accented eighths */
            unsigned b, lo = 255, hi = 0, q;
            for (q = 0; q < ORK_NPHRASE && strcmp(PHRASE_NAME[q][0], "GALLOP"); q++) ;
            ork_set.arp_on = 1; ork_set.arp_mode = 0; ork_set.arp_phrase = (uint8_t)q; ork_set.scale = 0;
            render(10);
            (void)ork_post(0, 0x90, 60, 100);
            for (b = 0; b < 1000u * 441u / 1280u; b++) {
                orkest_render(blk);
                if (ork_stat.last_vel < lo) lo = ork_stat.last_vel;
                if (ork_stat.last_vel > hi) hi = ork_stat.last_vel;
            }
            (void)ork_post(0, 0x80, 60, 0); render(2000);
            printf("  GALLOP velocities %u .. %u (key 100)\n", lo, hi);
            check("ARP velocity: GALLOP's notes soft and accented (spread > 30)", q < ORK_NPHRASE && hi - lo > 30u);
        }
        {   /* a velocity pattern apart from the phrase: ACCENT 3 (120 70 70) over OSTINATO (8 notes), on over its repeat */
            static const uint8_t WANT[10] = {120, 70, 70, 120, 70, 70, 120, 70, 70, 120};
            uint8_t got[10];
            unsigned b, nn, ok = 1;
            ork_set.arp_phrase = 0; ork_set.velo_pat = 3; ork_set.velo_depth = 100; ork_set.velo_human = 0;
            render(10);
            (void)ork_post(0, 0x90, 60, 100);
            for (b = 0, nn = 0; b < 1300u * 441u / 1280u && nn < 10u; b++) {
                unsigned before = ork_stat.last_note;
                ork_stat.last_note = 255;            /* (to see each note on, repeated pitches too) */
                orkest_render(blk);
                if (ork_stat.last_note != 255) got[nn++] = ork_stat.last_vel;
                else ork_stat.last_note = (uint8_t)before;
            }
            (void)ork_post(0, 0x80, 60, 0); render(1500);
            for (b = 0; b < 10u; b++) if (b >= nn || got[b] != WANT[b]) ok = 0;
            check("velocity pattern ACCENT 3 over an 8-note phrase: 120 70 70 runs on across the repeat", ok);
            {   /* humanize 100 % on FLAT: the velocities spread (+-45 of 100) */
                unsigned lo = 255, hi = 0;
                ork_set.velo_pat = 1; ork_set.velo_human = 100;
                render(10);
                (void)ork_post(0, 0x90, 60, 100);
                for (b = 0; b < 3000u * 441u / 1280u; b++) {
                    unsigned before = ork_stat.last_note;
                    ork_stat.last_note = 255;
                    orkest_render(blk);
                    if (ork_stat.last_note != 255) { if (ork_stat.last_vel < lo) lo = ork_stat.last_vel; if (ork_stat.last_vel > hi) hi = ork_stat.last_vel; }
                    else ork_stat.last_note = (uint8_t)before;
                }
                (void)ork_post(0, 0x80, 60, 0); render(1500);
                printf("  humanize 100 %%: velocities %u .. %u\n", lo, hi);
                check("humanize: the velocities spread wide (> 50)", hi - lo > 50u);
                ork_set.velo_human = 0;
            }
            {   /* a pattern with rests: HALF (110, 0) over the same phrase: every other note left out */
                unsigned q, ons = 0;
                for (q = 0; q < ORK_NVELO && strcmp(VELO_NAME[q], "HALF"); q++) ;
                ork_set.velo_pat = (uint8_t)q;
                render(10);
                (void)ork_post(0, 0x90, 60, 100);
                for (b = 0; b < 1000u * 441u / 1280u; b++) {
                    unsigned before = ork_stat.last_note;
                    ork_stat.last_note = 255;
                    orkest_render(blk);
                    if (ork_stat.last_note != 255) ons++; else ork_stat.last_note = (uint8_t)before;
                }
                (void)ork_post(0, 0x80, 60, 0); render(1500);
                check("velocity pattern HALF: 0 is a rest, 4 of the phrase's 8 notes in a second", q < ORK_NVELO && ons == 4u);
            }
            ork_set.velo_pat = 0;
        }
        {   /* mono: a second key takes over (last key priority): one phrase, from the new key */
            unsigned b, two = 0;
            use(patch_of("FLUTE"));
            ork_set.arp_on = 1; ork_set.arp_mode = 0; ork_set.arp_phrase = 0; ork_set.arp_gate = 50; ork_set.release = 0;
            ork_set.tail = 0;                        /* (no release samples: the voices counted are the notes) */
            render(10);
            (void)ork_post(0, 0x90, 60, 100); render(200); (void)ork_post(0, 0x90, 67, 100);
            for (b = 0; b < 600u * 441u / 1280u; b++) { orkest_render(blk); if (ork_stat.voices >= 2u) two++; }
            printf("  mono: blocks with 2+ voices %u, last note %u\n", two, ork_stat.last_note);
            check("ARP mono: a second key takes over, one phrase sounding", two < 20u && ork_stat.last_note >= 67);
            (void)ork_post(0, 0x80, 67, 0); (void)ork_post(0, 0x80, 60, 0); render(1500);
            check("... let go: silence", sounding() == 0);
            ork_set.arp_gate = 90; ork_set.tail = 64;
            use(patch_of("HARP"));
        }
        {   /* two parts: part 2 (FLUTE, TRILL, an octave up) with part 1 from the same key; part 2 off: part 1 alone */
            unsigned b, most[2] = {0, 0}, w;
            ork_set.arp_on = 1; ork_set.arp_mode = 0; ork_set.arp_phrase = 0; ork_set.arp_gate = 50;
            ork_set.tail = 0; ork_set.release = 0;
            ork_set.part[1].patch = (uint8_t)patch_of("FLUTE"); ork_set.part[1].gate = 50;
            ork_set.part[0].patch = (uint8_t)patch_of("VIOLINS");
            for (w = 0; w < 2u; w++) {
                if (w) ork_set.part[1].patch = 255;
                render(10);
                (void)ork_post(0, 0x90, 60, 100);
                for (b = 0; b < 800u * 441u / 1280u; b++) { orkest_render(blk); if (ork_stat.voices > most[w]) most[w] = ork_stat.voices; }
                (void)ork_post(0, 0x80, 60, 0); render(1500);
            }
            printf("  two parts: at most %u voices; part 2 off: %u\n", most[0], most[1]);
            check("ARP two parts: part 1 and part 2 sounding from one key", most[0] >= 2u);
            check("... part 2 OFF: part 1 alone", most[1] == 1u);
            check("... let go: silence", sounding() == 0);
            ork_set.arp_gate = 90; ork_set.part[1].gate = 90; ork_set.tail = 64;
        }
        ork_set.arp_on = 1; ork_set.arp_mode = 0;
        for (t = 0; t < ORK_NPHRASE; t++) {         /* every phrase, mono, two keys: sane, one voice, ends when let go */
            ork_set.arp_phrase = (uint8_t)t;
            unsigned most = 0, b;
            (void)ork_post(0, 0x90, 48, 100); render(300); (void)ork_post(0, 0x90, 72, 100);
            peak = 0;
            for (b = 0; b < 4000u * 441u / 1280u; b++) { orkest_render(blk); if (ork_stat.voices > most) most = ork_stat.voices; for (i = 0; i < 256u; i++) { int32_t a = blk[i] < 0 ? -blk[i] : blk[i]; if (a > peak) peak = a; } }
            (void)ork_post(0, 0x80, 48, 0); (void)ork_post(0, 0x80, 72, 0);
            render(3000);
            if (peak >= (1 << 23) || peak < (1 << 15) || sounding() != 0) { printf("  phrase %u: peak %d, voices %u, most %u\n", t, peak, ork_stat.voices, most); same = 0; }
        }
        check("every phrase, a second key taking over: a sane level, ends when let go", same);
        ork_set.arp_on = 0;
    }

    /* ---- every sound in turn, to listen to */
    if (argc > 1 && (wav = fopen(argv[1], "wb")) != 0) {
        static const uint8_t SCALE[] = {60, 62, 64, 65, 67, 69, 71, 72};
        int p;
        uint32_t v;
        uint16_t a = 1, ch = 2, ba = 4, bits = 16;
        uint32_t sr = FM1_SAMPLE_RATE, br = FM1_SAMPLE_RATE * 4u;
        fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0", 1, 20, wav);
        fwrite(&a, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&sr, 4, 1, wav); fwrite(&br, 4, 1, wav);
        fwrite(&ba, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data\0\0\0\0", 1, 8, wav);
        ork_set.reverb = 40;
        for (p = 0; p < ork_bank.np; p++) {
            use(p);
            note(0x90, 48, 90); note(0x90, 55, 90); note(0x90, 64, 90);
            render(1100);
            note(0x80, 48, 0); note(0x80, 55, 0); note(0x80, 64, 0);
            render(250);
            for (i = 0; i < sizeof SCALE; i++) { note(0x90, SCALE[i], 100); render(170); note(0x80, SCALE[i], 0); }
            render(900);
        }
        {   /* the LFO: STRINGS, then SOLO VIOLIN, with vibrato fading in */
            static const char *const LP[2] = {"STRINGS", "SOLO VIOLIN"};
            int q;
            ork_set.lfo_on = 1; ork_set.lfo_rate = 92; ork_set.lfo_vib = 45; ork_set.lfo_trem = 0; ork_set.lfo_delay = 17;
            ork_set.lfo_inc = LFO_INC[92];
            ork_set.lfo_dstep = 65536u / (17u * 3u * 44100u / (127u * 128u));
            for (q = 0; q < 2; q++) {
                use(patch_of(LP[q]));
                note(0x90, 60, 90); note(0x90, 64, 90); note(0x90, 67, 90);
                render(2500);
                note(0x80, 60, 0); note(0x80, 64, 0); note(0x80, 67, 0);
                render(1200);
            }
            ork_set.lfo_on = 0;
        }
        v = 36 + wav_frames * 4u;
        fseek(wav, 4, SEEK_SET); fwrite(&v, 4, 1, wav);
        v = wav_frames * 4u;
        fseek(wav, 40, SEEK_SET); fwrite(&v, 4, 1, wav);
        fclose(wav);
        printf("wrote %s (%u frames)\n", argv[1], wav_frames);
    }
    if (argc > 2 && (wav = fopen(argv[2], "wb")) != 0) {   /* the phrases, to listen to */
        static const char *const PP[6] = {"HARP", "STRINGS", "FLUTE", "VIOLINS", "OBOE", "PIZZICATO"};
        uint16_t a = 1, ch = 2, ba = 4, bits = 16;
        uint32_t sr = FM1_SAMPLE_RATE, br = FM1_SAMPLE_RATE * 4u, v, q;
        wav_frames = 0;
        fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0", 1, 20, wav);
        fwrite(&a, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&sr, 4, 1, wav); fwrite(&br, 4, 1, wav);
        fwrite(&ba, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data\0\0\0\0", 1, 8, wav);
        ork_set.reverb = 45; ork_set.arp_on = 1; ork_set.arp_mode = 2; ork_set.arp_speed = 1; ork_set.arp_gate = 90;
        for (q = 0; q < ORK_NPHRASE; q++) {         /* each at its written key, twice (LATCH, then ARP off) */
            unsigned key = PHRASE_REF[q] < 55u ? PHRASE_REF[q] + 12u : PHRASE_REF[q];
            const ork_phrase_t *ph = &PHRASE[q];
            use(patch_of(PP[q % 6u]));
            ork_set.arp_phrase = (uint8_t)q;
            render(10);
            (void)ork_post(0, 0x90, key, 100); render(20); (void)ork_post(0, 0x80, key, 0);
            render(2u * ph->len * 500u / 24u - 20u);
            ork_set.arp_on = 0; render(700); ork_set.arp_on = 1;
        }
        ork_set.arp_on = 0;
        v = 36 + wav_frames * 4u;
        fseek(wav, 4, SEEK_SET); fwrite(&v, 4, 1, wav);
        v = wav_frames * 4u;
        fseek(wav, 40, SEEK_SET); fwrite(&v, 4, 1, wav);
        fclose(wav);
        wav = 0;
        printf("wrote %s (%u frames)\n", argv[2], wav_frames);
    }
    printf(bad ? "orkest v14 test: %d FAILED\n" : "orkest v14 test passed\n", bad);
    return bad != 0;
}
