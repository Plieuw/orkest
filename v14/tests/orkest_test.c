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
