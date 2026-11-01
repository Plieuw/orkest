/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Orkest's panel (control task): one screen, the keys, the knobs, the LEDs and the settings.
 *
 *   KNOB 1 / PRESETS  the sound            OCT- / OCT+  octave -3..+3
 *   KNOB 2            attack               SAVE         sound, attack, release, room, octave into flash
 *   KNOB 3            release              MASTER       volume
 *   KNOB 4            room (reverb)
 *   LFO               the LFO menu (press again or HOME: back): K1 rate, K2 vibrato, K3 tremolo, K4 delay;
 *                     SELECT: FREE (K1 in Hz) or TEMPO (K1 a note value, 4 bars .. 1/32); ALGORITHM: the internal
 *                     tempo (40..300 BPM). MIDI clock coming in sets the tempo while it runs (Start: the downbeat).
 *                     The modulation is on while vibrato or tremolo is above 0 (LFO's LED lit, "LFO" on the screen)
 *   Choosing a sound sets attack and release to its own; turn KNOB 2 / 3 after to change them.
 *   MIDI CC 1 (mod wheel): vibrato, also with the LFO off.
 *   The keys play the sound and go out as MIDI (channel 1, USB / Bluetooth).
 *
 * The 5x7 font and its glyph tables are M-VAVE's (FM1 Open app/screen.c, GPL-3.0 with the SDK linking exception). */
#include "orkest.h"
#include "fm1_device.h"
#include "version.h"
#include <string.h>
#include "orkest_lfo.inc"
#define ORK_PHRASE_NAMES
#include "orkest_phrases.inc"
#define ORK_SCALE_MASKS
#define ORK_SCALE_NAMES
#include "orkest_scales.inc"
#define ORK_VELO_STEPS
#define ORK_VELO_NAMES
#include "orkest_velo.inc"
#define ORK_SEAT_DATA
#define ORK_SEAT_CODE
#include "orkest_seats.inc"

enum { BACK = 0x0000, TEXT = 0xef5d, ACCENT = 0x07ff, DIM = 0x738e, AMBER = 0xfd20 };
enum { B_OCTDN, B_OCTUP, B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC };
enum { E_SELECT, E_ALGO, E_K1, E_K2, E_K3, E_K4, E_PRESETS };
static const char *const BUTTON_NAME[14] = {"OCT-", "OCT+", "FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE",
                                            "ARP", "SEQ", "PLAY", "REC"};
#define KEY_BASE 53              /* key 0 at octave 0: F3 */
#define KEYS 27u
#define KEY_VEL 100u

/* ------------------------------------------------------------- font --- */
/* Five columns per glyph, bit 0 at the top. Digits followed by uppercase (M-VAVE, FM1 Open). */
static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
static const uint8_t punctuation[][6] = {
    {45,8,8,8,8,8}, {47,0x60,0x18,6,1,0}, {43,8,8,0x3e,8,8},
    {35,0x14,0x7f,0x14,0x7f,0x14}, {37,0x63,0x13,8,0x64,0x63},
    {62,0x41,0x22,0x14,8,0}, {63,2,1,0x51,9,6},
    {33,0,0,0x5f,0,0}, {34,0,7,0,7,0}, {36,0x24,0x2a,0x7f,0x2a,0x12},
    {38,0x36,0x49,0x55,0x22,0x50}, {39,0,5,3,0,0}, {40,0,0x1c,0x22,0x41,0},
    {41,0,0x41,0x22,0x1c,0}, {42,0x14,8,0x3e,8,0x14}, {44,0,0x50,0x30,0,0},
    {46,0,0x60,0x60,0,0}, {58,0,0x36,0x36,0,0}, {59,0,0x56,0x36,0,0},
    {60,8,0x14,0x22,0x41,0}, {61,0x14,0x14,0x14,0x14,0x14},
    {64,0x32,0x49,0x79,0x41,0x3e}, {91,0,0x7f,0x41,0x41,0},
    {92,1,6,0x18,0x60,0}, {93,0,0x41,0x41,0x7f,0}, {94,4,2,1,2,4},
    {95,0x40,0x40,0x40,0x40,0x40}, {96,0,1,2,4,0}, {123,0,8,0x36,0x41,0},
    {124,0,0,0x7f,0,0}, {125,0,0x41,0x36,8,0}, {126,8,4,8,0x10,8}
};
static const uint8_t *glyph(char c)
{
    static const uint8_t blank[5] = {0};
    unsigned i;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9')
        return font[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return font[10 + c - 'A'];
    for (i = 0; i < sizeof punctuation / sizeof punctuation[0]; i++)
        if ((uint8_t)c == punctuation[i][0])
            return punctuation[i] + 1;
    return blank;
}

/* ------------------------------------------------------------ lines --- */
/* The screen is a few text lines; a line is drawn into a full-width strip and sent when its text or colour
 * changed. Scale 1..3: 6 x 7 .. 18 x 21 px a character. */
enum { L_TITLE, L_SOUND, L_NOTE, L_K1, L_K2, L_K3, L_K4, L_EVENT, L_STATUS, NLINES };
static const struct { uint8_t y, scale, centre; } LINE[NLINES] = {
    {8, 2, 0}, {38, 3, 1}, {70, 2, 1}, {100, 2, 0}, {124, 2, 0}, {148, 2, 0}, {172, 2, 0}, {202, 1, 1}, {220, 1, 1},
};
#define STRIP_ROWS 28u        /* scale 4 (the splash): 4 x 7 rows */
static uint16_t strip[240u * STRIP_ROWS];
static char shown[NLINES][42];      /* scale 1: up to 40 characters a line */
static uint16_t shown_colour[NLINES];
static uint8_t force_draw = 1;

static void draw_text(unsigned y, unsigned scale, unsigned centre, const char *s, uint16_t colour)
{
    unsigned n = (unsigned)strlen(s), w, h, x0, i, row, col;
    if (!scale || 7u * scale > STRIP_ROWS)
        return;
    w = 6u * scale;
    h = 7u * scale;
    if (n > 240u / w)
        n = 240u / w;
    x0 = centre ? (240u - n * w) / 2u : 8u;
    for (i = 0; i < 240u * h; i++)
        strip[i] = BACK;
    for (i = 0; i < n; i++) {
        const uint8_t *g = glyph(s[i]);
        for (row = 0; row < h; row++)
            for (col = 0; col < 5u * scale; col++)
                if (g[col / scale] & (1u << (row / scale)))
                    strip[row * 240u + x0 + i * w + col] = colour;
    }
    (void)fm1_screen_blit(0, y, 240, h, strip, 240u * h);
}
static void line(unsigned l, const char *s, uint16_t colour)
{
    if (!force_draw && shown_colour[l] == colour && !strcmp(shown[l], s))
        return;
    strncpy(shown[l], s, sizeof shown[l] - 1u);
    shown[l][sizeof shown[l] - 1u] = 0;
    shown_colour[l] = colour;
    draw_text(LINE[l].y, LINE[l].scale, LINE[l].centre, shown[l], colour);
}

/* small formatting without printf */
static char *put_int(char *p, int v)
{
    char t[12];
    unsigned n = 0, u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0)
        *p++ = '-';
    do
        t[n++] = (char)('0' + u % 10u);
    while (u /= 10u);
    while (n)
        *p++ = t[--n];
    *p = 0;
    return p;
}
static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}
/* a knob row: label left, value right, 19 characters at scale 2 */
static void knob_row(unsigned l, const char *label, const char *value, uint16_t colour)
{
    char b[24];
    unsigned nl = (unsigned)strlen(label), nv = (unsigned)strlen(value), i;
    memset(b, ' ', 19);
    b[19] = 0;
    memcpy(b, label, nl > 19u ? 19u : nl);
    if (nv <= 19u)
        for (i = 0; i < nv; i++)
            b[19u - nv + i] = value[i];
    line(l, b, colour);
}

/* --------------------------------------------------------- settings --- */
#define SET_MAGIC 0x3953524FU    /* "ORS9" (earlier records: what they hold, the rest the defaults) */
/* earlier records: their magic, where their check is (what they hold before it) */
static const struct { uint32_t magic; uint8_t check; } OLD_SET[] = {
    {0x3853524FU, 52}, {0x3753524FU, 48}, {0x3653524FU, 32}, {0x3553524FU, 28}, {0x3453524FU, 20}, {0x3353524FU, 20}};
struct settings {
    uint32_t magic;
    uint8_t patch, attack, release, reverb;
    int8_t octave;
    uint8_t lfo_on, lfo_rate, lfo_vib, lfo_trem, lfo_delay, lfo_sync, lfo_div;
    uint16_t bpm;
    uint8_t transient, tail;                  /* from ORS4 */
    uint8_t arp_on, p0_phrase, p0_speed, p0_gate, arp_mode, scale, scale_root;   /* from ORS5 (part 1's phrase) */
    uint8_t p0_velo, p0_depth, p0_human;      /* from ORS6 */
    uint8_t unused, p0_patch;                 /* from here ORS9 */
    int8_t p0_oct;
    uint8_t p1_patch, p1_phrase, p1_speed, p1_gate;
    int8_t p1_oct;
    uint8_t p1_velo, p1_depth, p1_human;
    uint8_t pan;                              /* bit 7 pan_mode, bits 0..6 pan_spread */
    uint8_t pad[2];
    uint32_t check;
};
/* the LFO's note values in TEMPO: MIDI clock ticks a cycle (24 a quarter note) */
static const struct { uint16_t ticks; char name[6]; } DIV[] = {
    {384, "4 BAR"}, {192, "2 BAR"}, {96, "1 BAR"}, {48, "1/2"}, {36, "1/4."}, {24, "1/4"}, {18, "1/8."},
    {16, "1/4T"}, {12, "1/8"}, {8, "1/8T"}, {6, "1/16"}, {4, "1/16T"}, {3, "1/32"}};
#define NDIV (sizeof DIV / sizeof DIV[0])
static uint8_t lfo_div = 9;        /* 1/8T: at 120 BPM 6 Hz, a natural vibrato */
static uint16_t bpm = 120;
static void lfo_apply(void)        /* the player's per-block steps for rate and delay */
{
    uint32_t blocks = (uint32_t)ork_set.lfo_delay * 3u * 44100u / (127u * 128u);
    if (lfo_div >= NDIV)
        lfo_div = 9;
    if (bpm < 40u || bpm > 300u)
        bpm = 120;
    ork_set.lfo_ticks = DIV[lfo_div].ticks;
    ork_set.lfo_inc = ork_set.lfo_sync          /* the internal tempo: 2^32 per cycle of `ticks` clock ticks */
        ? (uint32_t)((((uint64_t)1 << 32) * 128u * bpm * 24u) / (44100ull * 60u * DIV[lfo_div].ticks))
        : LFO_INC[ork_set.lfo_rate & 127u];
    ork_set.lfo_dstep = blocks ? 65536u / blocks : 65536u;
    ork_set.arp_inc = (uint32_t)((65536ull * 24u * 128u * bpm) / (60u * 44100u));   /* the phrases' ticks a block */
    ork_set.lfo_on = ork_set.lfo_vib || ork_set.lfo_trem;   /* on while it has something to do */
}
static int8_t octave;
static uint32_t set_check(const struct settings *s, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static uint32_t set_offset(void)
{
    uint32_t size = fm1_flash_size();
    return size >= FM1_FLASH_SECTOR_BYTES ? size - FM1_FLASH_SECTOR_BYTES : 0;
}
static void part_load(unsigned p, uint8_t patch, uint8_t phrase, uint8_t speed, uint8_t gate, int8_t oct,
                      uint8_t velo, uint8_t depth, uint8_t human)
{
    struct ork_part *pt = (struct ork_part *)&ork_set.part[p];
    pt->patch = patch < ORK_MAX_PATCH || (p && patch == 255u) ? patch : 0u;
    pt->phrase = phrase < ORK_NPHRASE ? phrase : 0u;
    pt->speed = speed <= 2u ? speed : 1u;
    pt->gate = gate >= 10u && gate <= 100u ? gate : 90u;
    pt->oct = (int8_t)(oct < -2 ? -2 : oct > 2 ? 2 : oct);
    pt->velo = velo < ORK_NVELO ? velo : 0u;
    pt->depth = depth <= 100u ? depth : 100u;
    pt->human = human <= 100u ? human : 0u;
}
static void settings_load(void)
{
    struct settings s;
    uint32_t old, at = 0, ver = 9, i;          /* ver: the record's ORS number */
    if (!fm1_flash_size() || fm1_flash_read(set_offset(), &s, sizeof s) != FM1_FLASH_OK)
        return;
    if (s.magic == SET_MAGIC)
        at = offsetof(struct settings, check);
    for (i = 0; !at && i < sizeof OLD_SET / sizeof OLD_SET[0]; i++)
        if (s.magic == OLD_SET[i].magic) {
            at = OLD_SET[i].check;
            ver = 8u - i;
        }
    if (!at)
        return;
    memcpy(&old, (const uint8_t *)&s + at, 4);
    if (old != set_check(&s, at))
        return;
    ork_set.patch = s.patch;
    ork_set.attack = s.attack & 127u;
    ork_set.release = s.release & 127u;
    ork_set.reverb = s.reverb & 127u;
    octave = s.octave < -3 ? -3 : s.octave > 3 ? 3 : s.octave;
    ork_set.lfo_on = s.lfo_on & 1u;
    ork_set.lfo_rate = s.lfo_rate & 127u;
    ork_set.lfo_vib = s.lfo_vib & 127u;
    ork_set.lfo_trem = s.lfo_trem & 127u;
    ork_set.lfo_delay = s.lfo_delay & 127u;
    ork_set.lfo_sync = s.lfo_sync & 1u;
    lfo_div = s.lfo_div;
    bpm = s.bpm;
    ork_set.transient = ver >= 4u ? s.transient & 127u : 64u;
    ork_set.tail = ver >= 4u ? s.tail & 127u : 64u;
    if (ver >= 5u) {
        ork_set.arp_on = s.arp_on & 1u;
        ork_set.arp_mode = s.arp_mode <= 2u ? s.arp_mode : 0u;
        ork_set.scale = s.scale < ORK_NSCALE ? s.scale : 0u;
        ork_set.scale_root = s.scale_root % 12u;
        part_load(0, ver >= 9u ? s.p0_patch : s.patch, s.p0_phrase, s.p0_speed, s.p0_gate, ver >= 9u ? s.p0_oct : 0,
                  ver >= 6u ? s.p0_velo : 0u, ver >= 6u ? s.p0_depth : 100u, ver >= 6u ? s.p0_human : 0u);
    }
    if (ver >= 9u) {
        part_load(1, s.p1_patch, s.p1_phrase, s.p1_speed, s.p1_gate, s.p1_oct, s.p1_velo, s.p1_depth, s.p1_human);
        ork_set.pan_mode = s.pan >> 7;
        ork_set.pan_spread = s.pan & 127u;
    }
}
static int settings_save(void)
{
    struct settings s;
    const struct ork_part *p0 = (const struct ork_part *)&ork_set.part[0], *p1 = (const struct ork_part *)&ork_set.part[1];
    memset(&s, 0, sizeof s);
    s.magic = SET_MAGIC;
    s.patch = ork_set.patch;
    s.attack = ork_set.attack;
    s.release = ork_set.release;
    s.reverb = ork_set.reverb;
    s.octave = octave;
    s.lfo_on = ork_set.lfo_on;
    s.lfo_rate = ork_set.lfo_rate;
    s.lfo_vib = ork_set.lfo_vib;
    s.lfo_trem = ork_set.lfo_trem;
    s.lfo_delay = ork_set.lfo_delay;
    s.lfo_sync = ork_set.lfo_sync;
    s.lfo_div = lfo_div;
    s.bpm = bpm;
    s.transient = ork_set.transient;
    s.tail = ork_set.tail;
    s.arp_on = ork_set.arp_on;
    s.arp_mode = ork_set.arp_mode;
    s.scale = ork_set.scale;
    s.scale_root = ork_set.scale_root;
    s.p0_patch = p0->patch, s.p0_phrase = p0->phrase, s.p0_speed = p0->speed, s.p0_gate = p0->gate, s.p0_oct = p0->oct;
    s.p0_velo = p0->velo, s.p0_depth = p0->depth, s.p0_human = p0->human;
    s.p1_patch = p1->patch, s.p1_phrase = p1->phrase, s.p1_speed = p1->speed, s.p1_gate = p1->gate, s.p1_oct = p1->oct;
    s.p1_velo = p1->velo, s.p1_depth = p1->depth, s.p1_human = p1->human;
    s.pan = (uint8_t)((ork_set.pan_mode ? 128u : 0u) | (ork_set.pan_spread & 127u));
    s.check = set_check(&s, offsetof(struct settings, check));
    if (!fm1_flash_size() || fm1_flash_erase(set_offset()) != FM1_FLASH_OK)
        return -1;
    return fm1_flash_write(set_offset(), &s, sizeof s) == FM1_FLASH_OK ? 0 : -1;
}
/* the scale and its root are kept without SAVE: written into the stored record (the rest of it as it was saved), or
 * a whole record of the current settings when there is none yet */
static int scale_store(void)
{
    struct settings s;
    if (!fm1_flash_size())
        return -1;
    if (fm1_flash_read(set_offset(), &s, sizeof s) != FM1_FLASH_OK || s.magic != SET_MAGIC ||
        s.check != set_check(&s, offsetof(struct settings, check)))
        return settings_save();
    if (s.scale == ork_set.scale && s.scale_root == ork_set.scale_root)
        return 0;
    s.scale = ork_set.scale;
    s.scale_root = ork_set.scale_root;
    s.check = set_check(&s, offsetof(struct settings, check));
    if (fm1_flash_erase(set_offset()) != FM1_FLASH_OK)
        return -1;
    return fm1_flash_write(set_offset(), &s, sizeof s) == FM1_FLASH_OK ? 0 : -1;
}

/* ------------------------------------------------------------ state --- */
static uint32_t prev_keys, blocked_keys, prev_buttons, led_shown, event_until, splash_until, frame_ms;
static uint8_t lfo_menu;           /* the LFO menu is open: the knobs set the LFO */
static uint8_t arp_menu;           /* the ARP menu's page: 1 part 1, 2 its velocity, 3 part 2, 4 its velocity (0 closed) */
static uint8_t scale_menu;         /* the SCL menu is open: the ARP's scale */
static uint8_t scale_dirty;        /* changed, not yet stored: when the menu closes, or 5 s after the last turn */
static uint32_t scale_turned;
static uint8_t pan_menu;           /* the GLO menu is open: panning */
static uint8_t env_menu;           /* the ENV menu is open: the knobs set the envelope and the parts' levels */
static uint8_t key_note[KEYS];
static char event[40];
static unsigned master_adc = 0xFFFF;

static void say(const char *s, uint32_t now)
{
    strncpy(event, s, sizeof event - 1u);
    event[sizeof event - 1u] = 0;
    event_until = now + 2500u;
}

static void patch_select(int to)
{
    const ork_patch_t *p;
    if (!ork_bank.np)
        return;
    to = to < 0 ? 0 : to >= (int)ork_bank.np ? (int)ork_bank.np - 1 : to;
    p = &ork_bank.patch[to];
    ork_set.patch = (uint8_t)to;
    ork_set.attack = p->attack;
    ork_set.release = p->release;
}

static void key_event(uint8_t status, uint8_t note, uint8_t vel)
{
    uint8_t m[3] = {status, note, vel};
    (void)ork_post(0, status, note, vel);
    (void)fm1_midi_send(m, 3);                      /* to the computer / phone: channel 1 */
}

static void leds(void)
{
    uint32_t want = 0, diff, i;                     /* bit k: note key k; bits 27, 28: OCT- / OCT+; 29: LFO */
    want = prev_keys & ((1u << KEYS) - 1u);
    if (ork_set.lfo_on)
        want |= 1u << 29;
    if (ork_set.arp_on)
        want |= 1u << 30;

    if (octave < 0)
        want |= 1u << 27;
    if (octave > 0)
        want |= 1u << 28;
    diff = want ^ led_shown;
    for (i = 0; i < KEYS; i++)
        if (diff & (1u << i))
            fm1_led_set(14u + i, (int)((want >> i) & 1u));
    if (diff & (1u << 27))
        fm1_led_set(B_OCTDN, octave < 0);
    if (diff & (1u << 28))
        fm1_led_set(B_OCTUP, octave > 0);
    if (diff & (1u << 29))
        fm1_led_set(B_LFO, ork_set.lfo_on);
    if (diff & (1u << 30))
        fm1_led_set(B_ARP, ork_set.arp_on);

    led_shown = want;
}

static const char *arp_hint(void)   /* on / off, the tempo, the mode (PRESETS) (40 characters at most) */
{
    static char b[48];
    static const char *const MODE[3] = {"HOLD", "ONCE", "LATCH"};
    unsigned t10 = ork_stat.clock ? ork_stat.clock_bpm10 : bpm * 10u;
    char *p = put_str(b, ork_set.arp_on ? "ON  " : "OFF  ");
    p = put_str(p, ork_stat.clock ? "MIDI " : "");
    p = put_int(p, (int)((t10 + 5u) / 10u));
    p = put_str(p, ork_stat.clock ? "  " : " BPM  ");
    p = put_str(p, MODE[ork_set.arp_mode % 3u]);
    put_str(p, "  PRESETS MODE");
    return b;
}
static void draw(uint32_t now)
{
    char b[40], v[20], *p;
    unsigned st = ork_bank_state(), note = ork_stat.last_note;
    static const char *const NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    p = put_str(b, "ORKEST ");
#ifdef ORKEST_BUILD
    p = put_str(p, ORKEST_BUILD);
#else
    p = put_str(p, FM1_RELEASE_VERSION);
#endif
    if (fm1_device_usb_connected())
        put_str(p, "  USB");
    line(L_TITLE, b, DIM);
    if (st == ORK_BANK_LOADING) {
        line(L_SOUND, "LOADING", AMBER);
        p = put_int(b, (int)(ork_bank_progress() / 1024u));
        put_str(p, " KB RECEIVED");
        line(L_NOTE, b, TEXT);
        knob_row(L_K1, "", "", TEXT);
        knob_row(L_K2, "KEEP THE USB", "", DIM);
        knob_row(L_K3, "CABLE CONNECTED", "", DIM);
        knob_row(L_K4, "", "", TEXT);
    } else if (lfo_menu) {                          /* the LFO menu: its settings on the knobs */
        unsigned d = (unsigned)ork_set.lfo_delay * 30u / 127u, r = LFO_DHZ[ork_set.lfo_rate & 127u];
        line(L_SOUND, ork_set.lfo_on ? "LFO ON" : "LFO OFF", ork_set.lfo_on ? ACCENT : DIM);
        if (!ork_set.lfo_sync)
            line(L_NOTE, "FREE", TEXT);
        else {
            unsigned t10 = ork_stat.clock ? ork_stat.clock_bpm10 : bpm * 10u;
            p = put_str(b, ork_stat.clock ? "MIDI " : "TEMPO ");
            p = put_int(p, (int)((t10 + 5u) / 10u));
            put_str(p, " BPM");
            line(L_NOTE, b, ork_stat.clock ? AMBER : TEXT);
        }
        if (ork_set.lfo_sync)
            put_str(v, DIV[lfo_div].name);
        else {
            p = put_int(v, (int)(r / 10u));
            p = put_str(p, ".");
            p = put_int(p, (int)(r % 10u));
            put_str(p, " HZ");
        }
        knob_row(L_K1, ork_set.lfo_sync ? "K1 NOTE" : "K1 RATE", v, TEXT);
        put_int(v, ork_set.lfo_vib);
        knob_row(L_K2, "K2 VIBRATO", v, TEXT);
        put_int(v, ork_set.lfo_trem);
        knob_row(L_K3, "K3 TREMOLO", v, TEXT);
        p = put_int(v, (int)(d / 10u));
        p = put_str(p, ".");
        p = put_int(p, (int)(d % 10u));
        put_str(p, " S");
        knob_row(L_K4, "K4 DELAY", v, TEXT);
    } else if (arp_menu == 2u || arp_menu == 4u) {   /* a part's velocity page */
        const struct ork_part *pt = (const struct ork_part *)&ork_set.part[arp_menu / 3u];
        unsigned vp = pt->velo % ORK_NVELO, k, n = VELO[vp].n;
        line(L_SOUND, arp_menu == 2u ? "VELOCITY 1" : "VELOCITY 2", ork_set.arp_on ? ACCENT : DIM);
        p = b;
        if (!vp)
            p = put_str(p, "AS THE PHRASE");
        else
            for (k = 0; k < n && k < 16u; k++) {   /* its steps, 1 soft .. 9 loud, - a rest */
                int st = VELO_STEP[VELO[vp].first + k], lv = (st - 20) * 8 / 115 + 1;
                *p++ = st ? (char)('0' + (lv < 1 ? 1 : lv > 9 ? 9 : lv)) : '-';
            }
        *p = 0;
        line(L_NOTE, b, TEXT);
        knob_row(L_K1, "K1", VELO_NAME[vp], TEXT);
        put_str(put_int(v, pt->depth), " %");
        knob_row(L_K2, "K2 DEPTH", v, TEXT);
        put_str(put_int(v, pt->human), " %");
        knob_row(L_K3, "K3 HUMANIZE", v, TEXT);
        put_str(put_int(v, pt->gate), " %");
        knob_row(L_K4, "K4 GATE", v, TEXT);
    } else if (arp_menu) {                          /* a part's page: its sound, phrase, speed, octave */
        static const char *const SPEED[3] = {"1/2 X", "1 X", "2 X"};
        unsigned pi = arp_menu / 3u;
        const struct ork_part *pt = (const struct ork_part *)&ork_set.part[pi];
        int off = pt->patch >= ork_bank.np;
        unsigned ph = pt->phrase % ORK_NPHRASE;
        line(L_SOUND, off ? "PART 2 OFF" : PHRASE_NAME[ph][0], ork_set.arp_on && !off ? ACCENT : DIM);
        line(L_NOTE, pi ? "PART 2" : "PART 1", TEXT);
        knob_row(L_K1, "K1", off ? "OFF" : ork_bank.patch[pt->patch].name, TEXT);
        p = put_int(v, (int)ph + 1);
        p = put_str(p, "/");
        put_int(p, ORK_NPHRASE);
        knob_row(L_K2, "K2 PHRASE", v, TEXT);
        knob_row(L_K3, "K3 SPEED", SPEED[pt->speed % 3u], TEXT);
        p = v;
        if (pt->oct > 0)
            p = put_str(p, "+");
        put_int(p, pt->oct);
        knob_row(L_K4, "K4 OCTAVE", v, TEXT);
    } else if (scale_menu) {                        /* the SEQ menu: the scale ARP keeps to */
        static const char *const NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        unsigned sc = ork_set.scale % ORK_NSCALE, rt = ork_set.scale_root % 12u, k;
        line(L_SOUND, sc ? SCALE_NAME[sc] : "NO SCALE", sc ? ACCENT : DIM);
        p = b;
        *p = 0;
        for (k = 0; sc && k < 12u; k++)             /* its notes, from the root */
            if (SCALE_MASK[sc] >> k & 1u) {
                if (p != b)
                    p = put_str(p, " ");
                p = put_str(p, NOTE[(rt + k) % 12u]);
            }
        line(L_NOTE, sc ? b : "ARP PLAYS AS WRITTEN", sc ? TEXT : DIM);
        knob_row(L_K1, "K1 ROOT", NOTE[rt], TEXT);
        knob_row(L_K2, "K2 SCALE", SCALE_NAME[sc], TEXT);
        knob_row(L_K3, "", "", TEXT);
        knob_row(L_K4, "", "", TEXT);
    } else if (pan_menu) {                          /* the GLO menu: panning, the sound's layers where they sit */
        static const uint8_t ROW[2] = {L_K3, L_K4};
        const ork_patch_t *pt = ork_bank.np ? &ork_bank.patch[ork_set.patch % ork_bank.np] : 0;
        unsigned k;
        line(L_SOUND, "PANNING", ACCENT);
        line(L_NOTE, ork_set.pan_mode ? "AS THE SOUND" : "CONCERT SEATING", TEXT);
        put_str(put_int(v, ork_set.pan_spread), " %");
        knob_row(L_K1, "K1 SPREAD", v, TEXT);
        knob_row(L_K2, "K2 SEATS", ork_set.pan_mode ? "SOUND" : "CONCERT", TEXT);
        for (k = 0; k < 2u; k++) {                  /* the current sound's first two layers */
            if (pt && k < pt->nl && pt->l[k].inst < ork_bank.ni) {
                const char *nm = ork_bank.inst[pt->l[k].inst].name;
                int32_t pn = (ork_set.pan_mode ? pt->l[k].pan : seat_pan(nm)) * (int32_t)ork_set.pan_spread / 100;
                pn = pn < -64 ? -64 : pn > 63 ? 63 : pn;
                p = v;
                if (pn)
                    p = put_str(p, pn < 0 ? "L" : "R");
                put_int(p, pn < 0 ? -pn * 100 / 64 : pn * 100 / 63);
                if (!pn)
                    put_str(v, "C");
                strncpy(b, nm, 12);
                b[12] = 0;
                knob_row(ROW[k], b, v, DIM);
            } else
                knob_row(ROW[k], "", "", DIM);
        }
    } else if (env_menu) {                          /* the ENV menu: the envelope and the three parts */
        line(L_SOUND, "ENVELOPE", ACCENT);
        line(L_NOTE, "64 AS RECORDED", DIM);
        put_int(v, ork_set.attack);
        knob_row(L_K1, "K1 ATTACK", v, TEXT);
        put_int(v, ork_set.release);
        knob_row(L_K2, "K2 RELEASE", v, TEXT);
        put_int(v, ork_set.transient);
        knob_row(L_K3, "K3 TRANSIENT", v, TEXT);
        put_int(v, ork_set.tail);
        knob_row(L_K4, "K4 TAIL", v, TEXT);
    } else if (st != ORK_BANK_OK) {
        line(L_SOUND, st == ORK_BANK_NONE ? "NO SOUNDS" : "BANK DAMAGED", AMBER);
        line(L_NOTE, "", TEXT);
        knob_row(L_K1, "INSTALL THE", "", DIM);
        knob_row(L_K2, "ORCHESTRA BANK", "", DIM);
        knob_row(L_K3, "FROM THE ORKEST", "", DIM);
        knob_row(L_K4, "WEB PAGE", "", DIM);
    } else {
        line(L_SOUND, ork_bank.patch[ork_set.patch % ork_bank.np].name, ACCENT);
        p = b;
        if (note < 128u) {
            p = put_str(p, NOTE[note % 12u]);
            p = put_int(p, (int)note / 12 - 1);
        } else
            p = put_str(p, "-");
        p = put_str(p, "   OCT ");
        if (octave > 0)
            p = put_str(p, "+");
        p = put_int(p, octave);
        if (ork_set.lfo_on)
            p = put_str(p, "   LFO");
        if (ork_set.arp_on)
            put_str(p, "   ARP");
        line(L_NOTE, b, TEXT);
        p = put_int(v, ork_set.patch + 1);
        p = put_str(p, "/");
        put_int(p, ork_bank.np);
        knob_row(L_K1, "K1 SOUND", v, TEXT);
        put_int(v, ork_set.attack);
        knob_row(L_K2, "K2 ATTACK", v, TEXT);
        put_int(v, ork_set.release);
        knob_row(L_K3, "K3 RELEASE", v, TEXT);
        put_int(v, ork_set.reverb);
        knob_row(L_K4, "K4 ROOM", v, TEXT);
    }
    line(L_EVENT, (int32_t)(now - event_until) < 0 ? event : lfo_menu ? "SELECT SYNC  ALGO BPM  LFO BACK"
         : pan_menu ? "GLO OR HOME BACK" : env_menu ? "ENV OR HOME BACK" : arp_menu ? arp_hint()
         : scale_menu ? "THE ARP KEEPS TO IT   SCL BACK" : "",
         (int32_t)(now - event_until) < 0 ? AMBER : DIM);
    p = put_str(b, "VOICES ");
    p = put_int(p, ork_stat.voices);
    p = put_str(p, "   LOAD ");
    p = put_int(p, (int)(orkest_load_q8 * 100u / 256u));
    put_str(p, "%");
    line(L_STATUS, b, DIM);
    force_draw = 0;
}

/* ---------------------------------------------------- SDK callbacks --- */
void groove_screen_init(void)
{
    unsigned k;
    fm1_controls_init();
    for (k = 0; k < 41u; k++)
        fm1_led_set(k, 0);
    fm1_screen_init();
    fm1_screen_fill(0, 0, 240, 240, BACK);
    settings_load();
    lfo_apply();
    ork_bank_boot();
    if (ork_bank_state() == ORK_BANK_OK && ork_set.patch >= ork_bank.np)
        patch_select(0);
    draw_text(96, 4, 1, "ORKEST", ACCENT);
#ifdef ORKEST_BUILD
    draw_text(136, 1, 1, ORKEST_BUILD, DIM);
#else
    draw_text(136, 1, 1, FM1_RELEASE_VERSION, DIM);
#endif
    splash_until = fm1_millis() + 700u;
    for (k = 0; k < KEYS; k++)
        key_note[k] = 0xFF;
    prev_keys = prev_buttons = led_shown = 0;
    blocked_keys = 0xFFFFFFFFu;                     /* keys held at power-on stay silent until let go */
    force_draw = 1;
}

void groove_screen_control(void)
{
    struct fm1_controls c;
    uint32_t now = fm1_millis(), pressed, k;
    int d;
    fm1_controls_read(&c);
    ork_bank_poll();
    /* the keys: each remembers the note it started (the octave may change while it is held) */
    blocked_keys &= c.keys;
    for (k = 0; k < KEYS; k++) {
        uint32_t bit = 1u << k;
        if ((c.keys & bit) && !(prev_keys & bit) && !(blocked_keys & bit)) {
            int n = KEY_BASE + (int)k + 12 * octave;
            if (n >= 0 && n <= 127 && ork_bank_state() == ORK_BANK_OK) {
                key_note[k] = (uint8_t)n;
                key_event(0x90, (uint8_t)n, KEY_VEL);
            }
        } else if (!(c.keys & bit) && (prev_keys & bit) && key_note[k] != 0xFF) {
            key_event(0x80, key_note[k], 0);
            key_note[k] = 0xFF;
        }
    }
    prev_keys = c.keys;
    /* the buttons */
    pressed = c.buttons & ~prev_buttons;
    prev_buttons = c.buttons;
    for (k = 0; k < 14u; k++) {
        if (!(pressed & (1u << k)))
            continue;
        if (k == B_OCTDN || k == B_OCTUP) {
            if ((c.buttons & 3u) == 3u)
                continue;
            octave = (int8_t)(octave + (k == B_OCTUP ? 1 : -1));
            octave = octave < -3 ? -3 : octave > 3 ? 3 : octave;
        } else if (k == B_SAVE)
            say(settings_save() == 0 ? "SAVED" : "SAVE FAILED", now);
        else if (k == B_LFO || k == B_ENV || k == B_ARP || k == B_SCL || k == B_GLO ||
                 (k == B_HOME && (lfo_menu || env_menu || arp_menu || scale_menu || pan_menu))) {
            pan_menu = k == B_GLO ? !pan_menu : 0;
            lfo_menu = k == B_LFO ? !lfo_menu : 0;
            env_menu = k == B_ENV ? !env_menu : 0;
            arp_menu = k == B_ARP ? (uint8_t)((arp_menu + 1u) % 5u) : 0;   /* ARP: part 1, its velocity, part 2, its velocity, closed */
            if (k == B_ARP && arp_menu == 1u)
                ork_set.arp_on = 1;                 /* opening the ARP menu turns ARP on (SELECT turns it off) */
            scale_menu = k == B_SCL ? !scale_menu : 0;   /* SCL: the scale */
            if (!scale_menu && scale_dirty) {
                scale_dirty = 0;
                (void)scale_store();
            }
            force_draw = 1;
        }
        else {
            char b[24];
            put_str(put_str(b, "BUTTON "), BUTTON_NAME[k]);
            say(b, now);
        }
    }
    /* the knobs: in the LFO menu, the LFO's settings */
    if (lfo_menu) {
        int e[4] = {c.encoders[E_K1], c.encoders[E_K2], c.encoders[E_K3], c.encoders[E_K4]};
        if (c.encoders[E_SELECT])                   /* SELECT: FREE / TEMPO */
            ork_set.lfo_sync = c.encoders[E_SELECT] > 0;
        if (c.encoders[E_ALGO]) {                   /* ALGORITHM: the internal tempo */
            int v = (int)bpm + c.encoders[E_ALGO];
            bpm = (uint16_t)(v < 40 ? 40 : v > 300 ? 300 : v);
        }
        if (ork_set.lfo_sync && e[0]) {             /* K1 in TEMPO: the note value (clockwise: faster) */
            int v = (int)lfo_div + e[0];
            lfo_div = (uint8_t)(v < 0 ? 0 : v >= (int)NDIV ? (int)NDIV - 1 : v);
            e[0] = 0;
        }
        volatile uint8_t *t[4] = {&ork_set.lfo_rate, &ork_set.lfo_vib, &ork_set.lfo_trem, &ork_set.lfo_delay};
        for (k = 0; k < 4u; k++)
            if (e[k]) {
                int v = *t[k] + e[k];
                *t[k] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
            }
        lfo_apply();
    } else if (arp_menu) {                          /* the ARP pages: SELECT on/off, ALGO tempo, PRESETS mode */
        struct ork_part *pt = (struct ork_part *)&ork_set.part[arp_menu >= 3u];
        int v, e[4] = {c.encoders[E_K1], c.encoders[E_K2], c.encoders[E_K3], c.encoders[E_K4]};
        if (c.encoders[E_SELECT])
            ork_set.arp_on = c.encoders[E_SELECT] > 0;
        if (c.encoders[E_ALGO]) {
            v = (int)bpm + c.encoders[E_ALGO];
            bpm = (uint16_t)(v < 40 ? 40 : v > 300 ? 300 : v);
            lfo_apply();
        }
        if (c.encoders[E_PRESETS]) {
            v = (int)ork_set.arp_mode + (c.encoders[E_PRESETS] > 0 ? 1 : -1);
            ork_set.arp_mode = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
        }
        if (arp_menu == 2u || arp_menu == 4u) {     /* velocity: pattern, depth, humanize, gate */
            if (e[0]) {
                v = (int)pt->velo + e[0];
                pt->velo = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NVELO ? (int)ORK_NVELO - 1 : v);
            }
            if (e[1]) {
                v = (int)pt->depth + e[1] * 5;
                pt->depth = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
            }
            if (e[2]) {
                v = (int)pt->human + e[2] * 5;
                pt->human = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
            }
            if (e[3]) {
                v = (int)pt->gate + e[3];
                pt->gate = (uint8_t)(v < 10 ? 10 : v > 100 ? 100 : v);
            }
        } else {                                    /* the part: sound (part 2 also OFF), phrase, speed, octave */
            if (e[0]) {
                int lo = arp_menu >= 3u ? -1 : 0, cur = pt->patch >= ork_bank.np ? -1 : (int)pt->patch;
                v = cur + e[0];
                v = v < lo ? lo : v >= (int)ork_bank.np ? (int)ork_bank.np - 1 : v;
                pt->patch = (uint8_t)(v < 0 ? 255 : v);
            }
            if (e[1]) {
                v = (int)pt->phrase + e[1];
                pt->phrase = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NPHRASE ? (int)ORK_NPHRASE - 1 : v);
            }
            if (e[2]) {
                v = (int)pt->speed + (e[2] > 0 ? 1 : -1);
                pt->speed = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
            }
            if (e[3]) {
                v = (int)pt->oct + (e[3] > 0 ? 1 : -1);
                pt->oct = (int8_t)(v < -2 ? -2 : v > 2 ? 2 : v);
            }
        }
    } else if (scale_menu) {
        int v;
        if (c.encoders[E_K1] || c.encoders[E_K2] || c.encoders[E_PRESETS]) {
            scale_dirty = 1;
            scale_turned = now;
        }
        if (c.encoders[E_K1]) {                     /* K1: the root, round the octave */
            v = ((int)ork_set.scale_root + c.encoders[E_K1]) % 12;
            ork_set.scale_root = (uint8_t)(v < 0 ? v + 12 : v);
        }
        if (c.encoders[E_K2] || c.encoders[E_PRESETS]) {
            v = (int)ork_set.scale + c.encoders[E_K2] + c.encoders[E_PRESETS];
            ork_set.scale = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NSCALE ? (int)ORK_NSCALE - 1 : v);
        }
    } else if (pan_menu) {                          /* K1 the spread, K2 concert seating / the sound's own */
        int v;
        if (c.encoders[E_K1]) {
            v = (int)ork_set.pan_spread + c.encoders[E_K1] * 5;
            ork_set.pan_spread = (uint8_t)(v < 0 ? 0 : v > 125 ? 125 : v);
        }
        if (c.encoders[E_K2])
            ork_set.pan_mode = c.encoders[E_K2] > 0;
    } else if (env_menu) {
        volatile uint8_t *t[4] = {&ork_set.attack, &ork_set.release, &ork_set.transient, &ork_set.tail};
        const int e[4] = {c.encoders[E_K1], c.encoders[E_K2], c.encoders[E_K3], c.encoders[E_K4]};
        for (k = 0; k < 4u; k++)
            if (e[k]) {
                int v = *t[k] + e[k];
                *t[k] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
            }
    } else if (ork_bank_state() == ORK_BANK_OK) {
        if ((d = c.encoders[E_K1] + c.encoders[E_PRESETS]) != 0)
            patch_select(ork_set.patch + d);
        if ((d = c.encoders[E_K2]) != 0)
            ork_set.attack = (uint8_t)(ork_set.attack + d < 0 ? 0 : ork_set.attack + d > 127 ? 127 : ork_set.attack + d);
        if ((d = c.encoders[E_K3]) != 0)
            ork_set.release = (uint8_t)(ork_set.release + d < 0 ? 0 : ork_set.release + d > 127 ? 127 : ork_set.release + d);
        if ((d = c.encoders[E_K4]) != 0)
            ork_set.reverb = (uint8_t)(ork_set.reverb + d < 0 ? 0 : ork_set.reverb + d > 127 ? 127 : ork_set.reverb + d);
    }
    if (!lfo_menu && !env_menu && !arp_menu && !scale_menu && !pan_menu && (c.encoders[E_SELECT] || c.encoders[E_ALGO]))
        say(c.encoders[E_SELECT] ? "SELECT" : "ALGORITHM", now);
    if (scale_dirty && now - scale_turned >= 5000u) {   /* the scale kept, also with the menu left open */
        scale_dirty = 0;
        (void)scale_store();
    }
    /* MASTER: 0..1023 -> 0..127, with a little hysteresis against ADC noise */
    if (c.knob <= 1023u && (master_adc > 1023u || c.knob + 4u < master_adc || c.knob > master_adc + 4u)) {
        master_adc = c.knob;
        ork_set.master = (uint8_t)(master_adc * 127u / 1023u);
    }
    leds();
    {   /* SCL: lit while a scale is chosen */
        static int8_t scl_shown = -1;
        if (scl_shown != (ork_set.scale != 0u)) {
            scl_shown = (int8_t)(ork_set.scale != 0u);
            fm1_led_set(B_SCL, scl_shown);
        }
    }
    if ((int32_t)(now - splash_until) < 0)
        return;
    if (splash_until) {                             /* the splash is over: the whole screen once */
        splash_until = 0;
        fm1_screen_fill(0, 0, 240, 240, BACK);
        force_draw = 1;
    }
    if (force_draw || now - frame_ms >= 40u) {
        frame_ms = now;
        draw(now);
    }
}
