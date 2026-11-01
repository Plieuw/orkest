/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Orkest's sound bank in the FM1's user flash (control task).
 *
 * The bank starts at offset 0 of the user partition (fm1_flash_*: the SDK finds the partition); the last sector
 * holds the settings (orkest_ui.c). Its layout: v14/tools/gen_bank.py. A bank is used only when its header is
 * whole, its commit word is written and its CRC matches: an interrupted upload leaves no half bank in play.
 *
 * Upload, SysEx (manufacturer 7D, non-commercial), one message at a time, each answered:
 *   F0 7D 4F 4B 10 F7                                  INFO  -> 11 state, total(5), flash(5), name(16), version
 *   F0 7D 4F 4B 20 total(5) F7                         BEGIN -> 7E 20 0(5)       (the player stops)
 *   F0 7D 4F 4B 21 offset(5) count(2) data sum F7      DATA  -> 7E 21 next(5)   or 7D 21 error expected(5)
 *   F0 7D 4F 4B 22 F7                                  END   -> 7E 22 total(5)  or 7D 22 error 0(5)
 * numbers 7 bits a byte, low first; data: 8 bytes per 7 (the first holds the others' top bits), count <= 512,
 * sum = the data bytes XOR, & 7F. Sectors are erased as the data reaches them. The host sends DATA in order. */
#include "orkest.h"
#include "fm1_device.h"
#include "version.h"
#include <string.h>

struct ork_bank ork_bank;
static unsigned bank_state = ORK_BANK_NONE;
unsigned ork_bank_state(void) { return bank_state; }

static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i, k;
    if (!crc_table[1])
        for (i = 0; i < 256u; i++) {
            uint32_t c = i;
            for (k = 0; k < 8u; k++)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

int ork_bank_bind(const uint8_t *image, uint32_t bytes)
{
    struct ork_bank_header h;
    uint32_t i, k;
    if (!image || bytes < sizeof h)
        return ORK_BANK_NONE;
    memcpy(&h, image, sizeof h);
    if (h.magic != ORK_MAGIC)
        return ORK_BANK_NONE;
    if (h.version != 1 || h.header_bytes != sizeof h || h.total > bytes || h.total < sizeof h ||
        h.commit != ORK_COMMIT || h.maxl != 3 || !h.nzones || h.nzones > ORK_MAX_ZONES || !h.ninst ||
        h.ninst > ORK_MAX_INST || !h.npatch || h.npatch > ORK_MAX_PATCH ||
        h.zones_off < sizeof h || h.zones_off + h.nzones * sizeof(ork_zone_t) > h.total ||
        h.inst_off + h.ninst * sizeof(ork_inst_t) > h.total || h.patch_off + h.npatch * sizeof(ork_patch_t) > h.total ||
        h.data_off > h.total || h.data_bytes > h.total - h.data_off)
        return ORK_BANK_BAD;
    if (crc32_update(0, image + sizeof h, h.total - sizeof h) != h.crc)
        return ORK_BANK_CRC;
    memcpy(ork_bank.zone, image + h.zones_off, h.nzones * sizeof(ork_zone_t));
    memcpy(ork_bank.inst, image + h.inst_off, h.ninst * sizeof(ork_inst_t));
    memcpy(ork_bank.patch, image + h.patch_off, h.npatch * sizeof(ork_patch_t));
    for (i = 0; i < h.nzones; i++) {                /* every zone inside the data, its loop inside the zone */
        const ork_zone_t *z = &ork_bank.zone[i];
        if (z->off > h.data_bytes || z->n > 2u * (h.data_bytes - z->off) || !z->n || z->idx > 88u || !z->rate ||
            z->rate > 4u << 16 || z->lo > z->hi || (z->looped && (z->ls >= z->le || z->le > z->n)))
            return ORK_BANK_BAD;
    }
    for (i = 0; i < h.ninst; i++)
        if (!ork_bank.inst[i].nz || (uint32_t)ork_bank.inst[i].z0 + ork_bank.inst[i].nz > h.nzones)
            return ORK_BANK_BAD;
    for (i = 0; i < h.npatch; i++) {
        if (!ork_bank.patch[i].nl || ork_bank.patch[i].nl > 3u)
            return ORK_BANK_BAD;
        for (k = 0; k < ork_bank.patch[i].nl; k++)
            if (ork_bank.patch[i].l[k].inst >= h.ninst)
                return ORK_BANK_BAD;
        ork_bank.patch[i].name[13] = 0;
    }
    for (i = 0; i < h.ninst; i++)
        ork_bank.inst[i].name[13] = 0;
    ork_bank.data = image + h.data_off;
    ork_bank.data_bytes = h.data_bytes;
    ork_bank.nz = h.nzones;
    ork_bank.ni = h.ninst;
    ork_bank.np = h.npatch;
    memcpy(ork_bank.name, h.name, sizeof ork_bank.name);
    ork_bank.name[15] = 0;
    return ORK_BANK_OK;
}

/* the player off the bank: request, then wait for the audio ISR to confirm (bounded: no audio, no wait) */
static void player_halt(void)
{
    uint32_t t0 = fm1_millis();
    ork_player_stop();
    while (!ork_stat.idle && fm1_millis() - t0 < 50u)
        ;
    ork_stat.idle = 1;
}

static uint32_t bank_limit(void)                    /* the user partition less the settings sector */
{
    uint32_t size = fm1_flash_size();
    return size > FM1_FLASH_SECTOR_BYTES ? size - FM1_FLASH_SECTOR_BYTES : 0;
}

void ork_bank_boot(void)
{
    struct ork_bank_header h;
    uint32_t limit = bank_limit();
    const uint8_t *m;
    player_halt();
    bank_state = ORK_BANK_NONE;
    if (!limit || fm1_flash_read(0, &h, sizeof h) != FM1_FLASH_OK || h.magic != ORK_MAGIC)
        return;
    if (h.total > limit || h.total < sizeof h || !(m = fm1_flash_map(0, h.total))) {
        bank_state = ORK_BANK_BAD;
        return;
    }
    bank_state = (unsigned)ork_bank_bind(m, h.total);
    if (bank_state == ORK_BANK_OK)
        ork_player_start();
}

/* ------------------------------------------------------------ upload --- */
enum { MSG_MAX = 1024u, CHUNK_MAX = 512u };
static uint8_t mbox[MSG_MAX];
static volatile uint32_t mbox_n;
static volatile uint8_t mbox_full, mbox_src;
static uint32_t up_total, up_next, up_erased;
static uint8_t up_active;
static uint8_t chunk[CHUNK_MAX];

void ork_bank_sysex(const uint8_t *m, size_t n, unsigned source)
{
    if (!m || n < 6u || n > MSG_MAX || m[1] != 0x7Du || m[2] != 0x4Fu || m[3] != 0x4Bu || mbox_full)
        return;                                     /* not ours, or one still waiting: the host retries */
    memcpy(mbox, m, n);
    mbox_n = (uint32_t)n;
    mbox_src = (uint8_t)source;
    __asm__ volatile("" ::: "memory");
    mbox_full = 1;
}
uint32_t ork_bank_progress(void) { return up_active ? up_next : 0u; }

static uint32_t get7(const uint8_t *p, unsigned bytes)
{
    uint32_t v = 0;
    while (bytes--)
        v = v << 7 | (p[bytes] & 0x7Fu);
    return v;
}
static unsigned put7(uint8_t *p, uint32_t v, unsigned bytes)
{
    unsigned i;
    for (i = 0; i < bytes; i++, v >>= 7)
        p[i] = (uint8_t)(v & 0x7Fu);
    return bytes;
}
static void reply(const uint8_t *body, unsigned n)
{
    uint8_t m[64];
    uint32_t t0 = fm1_millis();
    unsigned port = mbox_src <= FM1_MIDI_BLE ? mbox_src : FM1_MIDI_USB;
    m[0] = 0xF0;
    m[1] = 0x7D;
    m[2] = 0x4F;
    m[3] = 0x4B;
    memcpy(m + 4, body, n);
    m[4 + n] = 0xF7;
    while (fm1_midi_send_port(port, m, n + 5u) == -2 && fm1_millis() - t0 < 50u)
        ;
}
static void ack(uint8_t cmd, uint32_t v)
{
    uint8_t b[7] = {0x7E, cmd};
    reply(b, 2u + put7(b + 2, v, 5));
}
static void nak(uint8_t cmd, uint8_t err, uint32_t v)
{
    uint8_t b[8] = {0x7D, cmd, err};
    reply(b, 3u + put7(b + 3, v, 5));
}

static void info(void)
{
    uint8_t b[60];
    unsigned n = 0, i;
    struct ork_bank_header h;
    const char *v = FM1_RELEASE_VERSION;
    b[n++] = 0x11;
    b[n++] = (uint8_t)bank_state;
    if (fm1_flash_read(0, &h, sizeof h) != FM1_FLASH_OK || h.magic != ORK_MAGIC)
        h.total = 0;
    n += put7(b + n, bank_state == ORK_BANK_OK ? h.total : 0u, 5);
    n += put7(b + n, bank_limit(), 5);
    for (i = 0; i < 16u; i++)
        b[n++] = bank_state == ORK_BANK_OK ? (uint8_t)(ork_bank.name[i] & 0x7F) : 0;
    while (*v && n < sizeof b)
        b[n++] = (uint8_t)(*v++ & 0x7F);
    reply(b, n);
}

static void handle(const uint8_t *m, uint32_t n)
{
    uint8_t cmd = m[4];
    if (m[n - 1] != 0xF7)
        return;
    switch (cmd) {
    case 0x10:
        info();
        break;
    case 0x20: {                                    /* BEGIN */
        uint32_t total;
        if (n != 11u) {
            nak(cmd, 1, 0);
            break;
        }
        total = get7(m + 5, 5);
        if (total < sizeof(struct ork_bank_header) || total > bank_limit()) {
            nak(cmd, 2, bank_limit());
            break;
        }
        player_halt();
        bank_state = ORK_BANK_LOADING;
        up_total = total;
        up_next = up_erased = 0;
        up_active = 1;
        ack(cmd, 0);
        break;
    }
    case 0x21: {                                    /* DATA */
        uint32_t off, cnt, i, groups, sum = 0;
        const uint8_t *p;
        if (!up_active || n < 14u) {
            nak(cmd, 3, up_next);
            break;
        }
        off = get7(m + 5, 5);
        cnt = get7(m + 10, 2);
        groups = (cnt + 6u) / 7u;
        if (!cnt || cnt > CHUNK_MAX || n != 12u + groups * 8u + 2u || off != up_next || off + cnt > up_total) {
            nak(cmd, 4, up_next);
            break;
        }
        p = m + 12;
        for (i = 0; i < cnt; i++) {                 /* 8 bytes per 7: the first holds the others' top bits */
            uint32_t g = i / 7u, j = i % 7u;
            chunk[i] = (uint8_t)(p[g * 8u + 1u + j] | ((p[g * 8u] >> j) & 1u) << 7);
            sum ^= chunk[i];
        }
        if ((sum & 0x7Fu) != m[n - 2]) {
            nak(cmd, 5, up_next);
            break;
        }
        while (up_erased < off + cnt) {             /* the sectors this chunk reaches */
            if (fm1_flash_erase(up_erased) != FM1_FLASH_OK) {
                nak(cmd, 6, up_next);
                return;
            }
            up_erased += FM1_FLASH_SECTOR_BYTES;
        }
        if (fm1_flash_write(off, chunk, cnt) != FM1_FLASH_OK) {
            nak(cmd, 7, up_next);
            break;
        }
        up_next = off + cnt;
        ack(cmd, up_next);
        break;
    }
    case 0x22: {                                    /* END: check it all, then commit */
        const uint8_t *map;
        struct ork_bank_header h;
        static const uint32_t commit = ORK_COMMIT;
        int r;
        if (!up_active || up_next != up_total) {
            nak(cmd, 8, up_next);
            break;
        }
        up_active = 0;
        if (fm1_flash_read(0, &h, sizeof h) != FM1_FLASH_OK || h.magic != ORK_MAGIC || h.total != up_total ||
            h.commit != 0xFFFFFFFFu || !(map = fm1_flash_map(0, up_total)) ||
            crc32_update(0, map + sizeof h, up_total - sizeof h) != h.crc) {
            bank_state = ORK_BANK_CRC;
            nak(cmd, 9, 0);
            break;
        }
        if (fm1_flash_write(offsetof(struct ork_bank_header, commit), &commit, sizeof commit) != FM1_FLASH_OK) {
            bank_state = ORK_BANK_BAD;
            nak(cmd, 10, 0);
            break;
        }
        (void)fm1_flash_invalidate(0, up_total);
        r = ork_bank_bind(fm1_flash_map(0, up_total), up_total);
        bank_state = (unsigned)r;
        if (r != ORK_BANK_OK) {
            nak(cmd, (uint8_t)(16 + r), 0);
            break;
        }
        ork_player_start();
        ack(cmd, up_total);
        break;
    }
    default:
        break;
    }
}

void ork_bank_poll(void)
{
    if (!mbox_full)
        return;
    __asm__ volatile("" ::: "memory");
    handle(mbox, mbox_n);
    mbox_full = 0;
}
