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
#define SET_MAGIC 0x3853524FU    /* "ORS8" (earlier records: what they hold, the rest the defaults) */
#define SET_MAGIC_7 0x3753524FU  /* "ORS7": up to the part mask, its check where part_mask is */
#define SET_MAGIC_6 0x3653524FU  /* "ORS6": up to the parts, its check where part_patch is */
#define SET_MAGIC_5 0x3553524FU  /* "ORS5": up to the velocity fields, its check where velo_depth is */
#define SET_MAGIC_4 0x3453524FU  /* "ORS4": up to the ARP fields, its check where they start */
#define SET_MAGIC_3 0x3353524FU  /* "ORS3": the same, transient and tail zero */
struct settings {
    uint32_t magic;
    uint8_t patch, attack, release, reverb;
    int8_t octave;
    uint8_t lfo_on, lfo_rate, lfo_vib, lfo_trem, lfo_delay, lfo_sync, lfo_div;
    uint16_t bpm;
    uint8_t transient, tail;
    uint8_t arp_on, arp_phrase, arp_speed, arp_gate, arp_mode, scale, scale_root;
    uint8_t velo_pat, velo_depth, velo_human, velo_reset, parts_on;   /* parts_on: 0 in ORS6 (its pad) */
    uint8_t part_patch[4], part_phrase[4], part_velo[4];
    int8_t part_oct[4];
    uint8_t part_mask, pad[3];
    uint32_t check;
};
/* the LFO's note values in TEMPO: MIDI clock ticks a cycle (24 a quarter note) */
static const struct { uint16_t ticks; char name[6]; } DIV[] = {
    {384, "4 BAR"}, {192, "2 BAR"}, {96, "1 BAR"}, {48, "1/2"}, {36, "1/4."}, {24, "1/4"}, {18, "1/8."},
    {16, "1/4T"}, {12, "1/8"}, {8, "1/8T"}, {6, "1/16"}, {4, "1/16T"}, {3, "1/32"}};
#define NDIV (sizeof DIV / sizeof DIV[0])
static uint8_t lfo_div = 8;        /* 1/8 */
static uint16_t bpm = 120;
static void lfo_apply(void)        /* the player's per-block steps for rate and delay */
{
    uint32_t blocks = (uint32_t)ork_set.lfo_delay * 3u * 44100u / (127u * 128u);
    if (lfo_div >= NDIV)
        lfo_div = 8;
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
static void settings_load(void)
{
    struct settings s;
    uint32_t old;
    if (!fm1_flash_size() || fm1_flash_read(set_offset(), &s, sizeof s) != FM1_FLASH_OK)
        return;
    {   /* each record its check after what it holds */
        uint32_t at = s.magic == SET_MAGIC ? offsetof(struct settings, check)
            : s.magic == SET_MAGIC_7 ? offsetof(struct settings, part_mask)
            : s.magic == SET_MAGIC_6 ? offsetof(struct settings, part_patch)
            : s.magic == SET_MAGIC_5 ? offsetof(struct settings, velo_depth)
            : s.magic == SET_MAGIC_4 || s.magic == SET_MAGIC_3 ? offsetof(struct settings, arp_on) : 0u;
        if (!at)
            return;
        memcpy(&old, (const uint8_t *)&s + at, 4);
        if (old != set_check(&s, at))
            return;
    }
    if (s.magic != SET_MAGIC)                       /* from before the part mask: every part */
        s.part_mask = 15;
    if (s.magic != SET_MAGIC && s.magic != SET_MAGIC_7) {   /* from before the parts: as the player starts */
        unsigned k;
        s.parts_on = 0;
        for (k = 0; k < 4u; k++) {
            s.part_patch[k] = ork_set.part_patch[k];
            s.part_phrase[k] = ork_set.part_phrase[k];
            s.part_velo[k] = ork_set.part_velo[k];
            s.part_oct[k] = ork_set.part_oct[k];
        }
    }
    if (s.magic != SET_MAGIC && s.magic != SET_MAGIC_7 && s.magic != SET_MAGIC_6) {   /* from before the velocity patterns */
        s.velo_pat = s.velo_human = s.velo_reset = 0;
        s.velo_depth = 100;
    }
    if (s.magic != SET_MAGIC && s.magic != SET_MAGIC_7 && s.magic != SET_MAGIC_6 && s.magic != SET_MAGIC_5)   /* from before the scale */
        s.scale = s.scale_root = 0;
    if (s.magic == SET_MAGIC_3)                     /* from before the ENV menu: the parts as recorded */
        s.transient = s.tail = 64;
    if (s.magic == SET_MAGIC_4 || s.magic == SET_MAGIC_3) {   /* from before the ARP menu: its defaults */
        s.arp_on = s.arp_phrase = s.arp_mode = 0;
        s.arp_speed = 1;
        s.arp_gate = 90;
    }
    ork_set.arp_on = s.arp_on & 1u;
    ork_set.arp_phrase = s.arp_phrase < ORK_NPHRASE ? s.arp_phrase : 0u;
    ork_set.arp_speed = s.arp_speed <= 2u ? s.arp_speed : 1u;
    ork_set.arp_gate = s.arp_gate >= 10u && s.arp_gate <= 100u ? s.arp_gate : 90u;
    ork_set.arp_mode = s.arp_mode <= 2u ? s.arp_mode : 0u;
    ork_set.scale = s.scale < ORK_NSCALE ? s.scale : 0u;
    ork_set.scale_root = s.scale_root % 12u;
    ork_set.velo_pat = s.velo_pat < ORK_NVELO ? s.velo_pat : 0u;
    ork_set.velo_depth = s.velo_depth <= 100u ? s.velo_depth : 100u;
    ork_set.velo_human = s.velo_human <= 100u ? s.velo_human : 0u;
    ork_set.velo_reset = s.velo_reset & 1u;
    ork_set.parts_on = s.parts_on & 1u;
    ork_set.part_mask = s.part_mask & 15u;
    {
        unsigned k;
        for (k = 0; k < 4u; k++) {
            ork_set.part_patch[k] = s.part_patch[k] <= ORK_MAX_PATCH ? s.part_patch[k] : 0u;
            ork_set.part_phrase[k] = s.part_phrase[k] < ORK_NPHRASE ? s.part_phrase[k] : 0u;
            ork_set.part_velo[k] = s.part_velo[k] < ORK_NVELO ? s.part_velo[k] : 0u;
            ork_set.part_oct[k] = (int8_t)(s.part_oct[k] < -2 ? -2 : s.part_oct[k] > 2 ? 2 : s.part_oct[k]);
        }
    }
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
    ork_set.transient = s.transient & 127u;
    ork_set.tail = s.tail & 127u;
}
static int settings_save(void)
{
    struct settings s;
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
    s.arp_phrase = ork_set.arp_phrase;
    s.arp_speed = ork_set.arp_speed;
    s.arp_gate = ork_set.arp_gate;
    s.arp_mode = ork_set.arp_mode;
    s.scale = ork_set.scale;
    s.scale_root = ork_set.scale_root;
    s.velo_pat = ork_set.velo_pat;
    s.velo_depth = ork_set.velo_depth;
    s.velo_human = ork_set.velo_human;
    s.velo_reset = ork_set.velo_reset;
    s.parts_on = ork_set.parts_on;
    s.part_mask = ork_set.part_mask;
    {
        unsigned k;
        for (k = 0; k < 4u; k++) {
            s.part_patch[k] = ork_set.part_patch[k];
            s.part_phrase[k] = ork_set.part_phrase[k];
            s.part_velo[k] = ork_set.part_velo[k];
            s.part_oct[k] = ork_set.part_oct[k];
        }
    }
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
static uint8_t arp_menu;           /* the ARP menu is open: the knobs choose and shape the phrase */
static uint8_t scale_menu;         /* 1 the SCL menu: the ARP's scale; 2 the SEQ menu: PARTS (four sounds, each its phrase) */
static uint8_t part_field;         /* PARTS: what K1..K4 set: 0 sound, 1 phrase, 2 velocity, 3 octave */
static uint8_t scale_dirty;        /* changed, not yet stored: when the menu closes, or 5 s after the last turn */
static uint32_t scale_turned;
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
    if (ork_set.parts_on)
        want |= 1u << 31;
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
    if (diff & (1u << 31))
        fm1_led_set(B_SEQ, ork_set.parts_on);
    led_shown = want;
}

static const char *arp_hint(void)   /* on / off, the tempo the phrases follow, the way on (40 characters at most) */
{
    static char b[48];
    unsigned t10 = ork_stat.clock ? ork_stat.clock_bpm10 : bpm * 10u;
    char *p = put_str(b, ork_set.arp_on ? "ON  " : "OFF  ");
    p = put_str(p, ork_stat.clock ? "MIDI " : "");
    p = put_int(p, (int)((t10 + 5u) / 10u));
    p = put_str(p, ork_stat.clock ? "" : " BPM");
    put_str(p, arp_menu == 3u ? "  SELECT PARTS  ARP BACK" : arp_menu == 2u ? "  SELECT ON OFF  ARP PARTS"
               : "  SELECT ON OFF  ARP VELO");
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
    } else if (arp_menu == 3u) {                    /* the ARP menu's third page: which parts play */
        static const uint8_t ROW[4] = {L_K1, L_K2, L_K3, L_K4};
        unsigned k;
        line(L_SOUND, ork_set.parts_on ? "PARTS ON" : "PARTS OFF", ork_set.parts_on ? ACCENT : DIM);
        p = b;
        for (k = 0; k < 4u; k++) {                  /* the combination: 1 2 - 4 */
            if (k)
                *p++ = ' ';
            *p++ = (ork_set.part_mask >> k & 1u) && ork_set.part_patch[k] ? (char)('1' + k) : '-';
        }
        *p = 0;
        line(L_NOTE, ork_set.parts_on ? b : "ARP PLAYS ITS PHRASE", ork_set.parts_on ? TEXT : DIM);
        for (k = 0; k < 4u; k++) {
            unsigned pp = ork_set.part_patch[k], on = ork_set.part_mask >> k & 1u;
            p = put_int(v, (int)k + 1);
            p = put_str(p, " ");
            put_str(p, !pp ? "NO SOUND" : pp <= ork_bank.np ? ork_bank.patch[pp - 1u].name : "?");
            knob_row(ROW[k], v, on ? "ON" : "OFF", on && pp ? TEXT : DIM);
        }
    } else if (arp_menu == 2u) {                    /* the ARP menu's second page: velocity, apart from the phrase */
        unsigned vp = ork_set.velo_pat % ORK_NVELO, k, n = VELO[vp].n;
        line(L_SOUND, "VELOCITY", ork_set.arp_on ? ACCENT : DIM);
        p = b;
        if (!vp)
            p = put_str(p, "AS THE PHRASE");
        else
            for (k = 0; k < n && k < 16u; k++) {   /* its steps, 1 soft .. 9 loud */
                int lv = ((int)VELO_STEP[VELO[vp].first + k] - 20) * 8 / 115 + 1;
                *p++ = (char)('0' + (lv < 1 ? 1 : lv > 9 ? 9 : lv));
            }
        *p = 0;
        line(L_NOTE, b, TEXT);
        knob_row(L_K1, "K1 PATTERN", VELO_NAME[vp], TEXT);
        put_str(put_int(v, ork_set.velo_depth), " %");
        knob_row(L_K2, "K2 DEPTH", v, TEXT);
        put_str(put_int(v, ork_set.velo_human), " %");
        knob_row(L_K3, "K3 HUMANIZE", v, TEXT);
        knob_row(L_K4, "K4 STEPS", ork_set.velo_reset ? "LOOP" : "FREE", TEXT);
    } else if (arp_menu) {                          /* the ARP menu: the phrase a key plays */
        static const char *const SPEED[3] = {"1/2 X", "1 X", "2 X"}, *const MODE[3] = {"HOLD", "ONCE", "LATCH"};
        unsigned ph = ork_set.arp_phrase % ORK_NPHRASE;
        line(L_SOUND, PHRASE_NAME[ph][0], ork_set.arp_on ? ACCENT : DIM);
        line(L_NOTE, PHRASE_NAME[ph][1], TEXT);
        p = put_int(v, (int)ph + 1);
        p = put_str(p, "/");
        put_int(p, ORK_NPHRASE);
        knob_row(L_K1, "K1 PHRASE", v, TEXT);
        knob_row(L_K2, "K2 SPEED", SPEED[ork_set.arp_speed % 3u], TEXT);
        put_str(put_int(v, ork_set.arp_gate), " %");
        knob_row(L_K3, "K3 GATE", v, TEXT);
        knob_row(L_K4, "K4 MODE", MODE[ork_set.arp_mode % 3u], TEXT);
    } else if (scale_menu == 2u) {                  /* the SEQ menu's PARTS: four sounds, each its phrase */
        static const char *const FIELD[4] = {"SOUND", "PHRASE", "VELOCITY", "OCTAVE"};
        static const uint8_t ROW[4] = {L_K1, L_K2, L_K3, L_K4};
        static const char *const KN[4] = {"1", "2", "3", "4"};
        unsigned k;
        line(L_SOUND, ork_set.parts_on ? "PARTS ON" : "PARTS OFF", ork_set.parts_on ? ACCENT : DIM);
        p = put_str(b, "< ");
        p = put_str(p, FIELD[part_field & 3u]);
        put_str(p, " >");
        line(L_NOTE, b, TEXT);
        for (k = 0; k < 4u; k++) {
            const char *val;
            unsigned pp = ork_set.part_patch[k];
            switch (part_field & 3u) {
            case 0: val = !pp ? "OFF" : pp <= ork_bank.np ? ork_bank.patch[pp - 1u].name : "?"; break;
            case 1: val = PHRASE_NAME[ork_set.part_phrase[k] % ORK_NPHRASE][0]; break;
            case 2: val = VELO_NAME[ork_set.part_velo[k] % ORK_NVELO]; break;
            default:
                p = v;
                if (ork_set.part_oct[k] > 0)
                    p = put_str(p, "+");
                put_int(p, ork_set.part_oct[k]);
                val = v;
            }
            knob_row(ROW[k], KN[k], val, pp ? TEXT : DIM);
        }
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
         : env_menu ? "ENV OR HOME BACK" : arp_menu ? arp_hint() : scale_menu == 2u ? "SELECT ON OFF  ALGO FIELD  SEQ BACK"
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
        else if (k == B_LFO || k == B_ENV || k == B_ARP || k == B_SEQ || k == B_SCL ||
                 (k == B_HOME && (lfo_menu || env_menu || arp_menu || scale_menu))) {
            lfo_menu = k == B_LFO ? !lfo_menu : 0;
            env_menu = k == B_ENV ? !env_menu : 0;
            arp_menu = k == B_ARP ? (uint8_t)((arp_menu + 1u) % 4u) : 0;   /* ARP: phrase, velocity, parts, closed */
            if (k == B_ARP && arp_menu == 1u)
                ork_set.arp_on = 1;                 /* opening the ARP menu turns ARP on (SELECT turns it off) */
            scale_menu = k == B_SCL ? (scale_menu == 1u ? 0u : 1u)       /* SCL: the scale */
                       : k == B_SEQ ? (scale_menu == 2u ? 0u : 2u) : 0u;   /* SEQ: the parts */
            if (scale_menu != 1u && scale_dirty) {
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
    } else if (arp_menu == 3u) {                    /* parts: SELECT PARTS on/off, K1..K4 each part on/off */
        int e[4] = {c.encoders[E_K1], c.encoders[E_K2], c.encoders[E_K3], c.encoders[E_K4]};
        if (c.encoders[E_SELECT])
            ork_set.parts_on = c.encoders[E_SELECT] > 0;
        for (k = 0; k < 4u; k++)
            if (e[k])
                ork_set.part_mask = (uint8_t)(e[k] > 0 ? ork_set.part_mask | 1u << k : ork_set.part_mask & ~(1u << k));
    } else if (arp_menu == 2u) {
        int v;
        if (c.encoders[E_SELECT])                   /* SELECT: on / off, as on the first page */
            ork_set.arp_on = c.encoders[E_SELECT] > 0;
        if (c.encoders[E_ALGO]) {
            v = (int)bpm + c.encoders[E_ALGO];
            bpm = (uint16_t)(v < 40 ? 40 : v > 300 ? 300 : v);
            lfo_apply();
        }
        if (c.encoders[E_K1] || c.encoders[E_PRESETS]) {
            v = (int)ork_set.velo_pat + c.encoders[E_K1] + c.encoders[E_PRESETS];
            ork_set.velo_pat = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NVELO ? (int)ORK_NVELO - 1 : v);
        }
        if (c.encoders[E_K2]) {
            v = (int)ork_set.velo_depth + c.encoders[E_K2] * 5;
            ork_set.velo_depth = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
        }
        if (c.encoders[E_K3]) {
            v = (int)ork_set.velo_human + c.encoders[E_K3] * 5;
            ork_set.velo_human = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
        }
        if (c.encoders[E_K4])
            ork_set.velo_reset = c.encoders[E_K4] > 0;
    } else if (arp_menu) {
        int v;
        if (c.encoders[E_SELECT])                   /* SELECT: on / off */
            ork_set.arp_on = c.encoders[E_SELECT] > 0;
        if (c.encoders[E_ALGO]) {                   /* ALGORITHM: the internal tempo (shared with the LFO) */
            v = (int)bpm + c.encoders[E_ALGO];
            bpm = (uint16_t)(v < 40 ? 40 : v > 300 ? 300 : v);
            lfo_apply();
        }
        if (c.encoders[E_K1] || c.encoders[E_PRESETS]) {
            v = (int)ork_set.arp_phrase + c.encoders[E_K1] + c.encoders[E_PRESETS];
            ork_set.arp_phrase = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NPHRASE ? (int)ORK_NPHRASE - 1 : v);
        }
        if (c.encoders[E_K2]) {
            v = (int)ork_set.arp_speed + (c.encoders[E_K2] > 0 ? 1 : -1);
            ork_set.arp_speed = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
        }
        if (c.encoders[E_K3]) {
            v = (int)ork_set.arp_gate + c.encoders[E_K3];
            ork_set.arp_gate = (uint8_t)(v < 10 ? 10 : v > 100 ? 100 : v);
        }
        if (c.encoders[E_K4]) {
            v = (int)ork_set.arp_mode + (c.encoders[E_K4] > 0 ? 1 : -1);
            ork_set.arp_mode = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
        }
    } else if (scale_menu == 2u) {                  /* PARTS: SELECT on/off, ALGO the field, K1..K4 the parts */
        int v, e[4] = {c.encoders[E_K1], c.encoders[E_K2], c.encoders[E_K3], c.encoders[E_K4]};
        if (c.encoders[E_SELECT])
            ork_set.parts_on = c.encoders[E_SELECT] > 0;
        if (c.encoders[E_ALGO]) {
            v = ((int)part_field + (c.encoders[E_ALGO] > 0 ? 1 : -1)) & 3;
            part_field = (uint8_t)v;
        }
        for (k = 0; k < 4u; k++) {
            if (!e[k])
                continue;
            switch (part_field & 3u) {
            case 0:
                v = (int)ork_set.part_patch[k] + e[k];
                ork_set.part_patch[k] = (uint8_t)(v < 0 ? 0 : v > (int)ork_bank.np ? (int)ork_bank.np : v);
                break;
            case 1:
                v = (int)ork_set.part_phrase[k] + e[k];
                ork_set.part_phrase[k] = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NPHRASE ? (int)ORK_NPHRASE - 1 : v);
                break;
            case 2:
                v = (int)ork_set.part_velo[k] + e[k];
                ork_set.part_velo[k] = (uint8_t)(v < 0 ? 0 : v >= (int)ORK_NVELO ? (int)ORK_NVELO - 1 : v);
                break;
            default:
                v = (int)ork_set.part_oct[k] + (e[k] > 0 ? 1 : -1);
                ork_set.part_oct[k] = (int8_t)(v < -2 ? -2 : v > 2 ? 2 : v);
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
    if (!lfo_menu && !env_menu && !arp_menu && !scale_menu && (c.encoders[E_SELECT] || c.encoders[E_ALGO]))
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
