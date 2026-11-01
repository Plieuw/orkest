/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host test of Orkest on FM1 Open: the player (app/orkest_player.c) and the bank (app/orkest_bank.c), the same
 * sources as the firmware, against a simulated user flash and MIDI output.
 *   - the bank: binding, its checks (damaged, uncommitted, not a bank), the decoder against the generator's
 *   - the upload: INFO, BEGIN, DATA in 512-byte chunks, END, as the installer sends them; the flash afterwards;
 *     errors (wrong order, bad checksum, END too early) answered with NAK and nothing committed
 *   - the player: every sound (level, loops hold, one-shots end, release ends), layers, 24 voices at most,
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
static void render(uint32_t ms)
{
    uint32_t b = ms * FM1_SAMPLE_RATE / 1000u / FM1_BLOCK_FRAMES + 1u, i;
    while (b--) {
        orkest_render(blk);
        for (i = 0; i < 2u * FM1_BLOCK_FRAMES; i++) {
            int32_t a = blk[i] < 0 ? -blk[i] : blk[i];
            if (a > peak) peak = a;
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
static void use(int p) { ork_set.patch = (uint8_t)p; ork_set.attack = ork_bank.patch[p].attack; ork_set.release = ork_bank.patch[p].release; }
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

    /* ---- the decoder: every zone as the generator decodes it (a loop: one more pass, from its stored state) */
    ork_bank_bind(image, sizeof image);
    {
        int same = 1;
        for (i = 0; i < ork_bank.nz; i++) {
            const ork_zone_t *z = &ork_bank.zone[i];
            extern long long ork_test_decode(unsigned zone, unsigned samples);
            long long sum = ork_test_decode(i, z->n + (z->looped ? z->le - z->ls : 0u));
            if (sum != ORK_BANK_CHECK[i]) { printf("  zone %u: %lld, the generator's %lld\n", i, sum, ORK_BANK_CHECK[i]); same = 0; }
        }
        check("decoder: every zone decodes as the generator's (loops too)", same);
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
              !memcmp(r + 12, "ORKEST 1", 8));
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
    check("40 notes on TUTTI: 24 voices, the output within 24 bits", ork_stat.voices == 24 && peak < (1 << 23));
    note(0xB0, 123, 0);
    render(4000);
    check("... all released", sounding() == 0);
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
    printf(bad ? "orkest v14 test: %d FAILED\n" : "orkest v14 test passed\n", bad);
    return bad != 0;
}
