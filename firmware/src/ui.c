/* SPDX-License-Identifier: GPL-3.0-only */
/* Orkest UI (main loop only): one screen, the LEDs, the buttons and knobs, and the settings in flash.
 *
 *   KNOB 1 / PRESETS  the sound (patch)       OCT- / OCT+   octave -3..+3
 *   KNOB 2  attack                            SAVE          the settings into flash (restored at power-on)
 *   KNOB 3  release                           MASTER pot    volume
 *   KNOB 4  room (reverb)                     GLO           the second core on / off (after SAVE and a restart)
 *   Choosing a sound sets attack and release to that sound's own; turn KNOB 2 / 3 after to change them.
 *   SELECT, ALGORITHM and the other buttons: shown on the screen, free for your own use.
 *
 * Drawing: draw_text_line (gfx.c) renders one line into a strip and blits it; a line is redrawn only when its
 * text changed (ui_line). Fonts: AF_S, AF_M, AF_L (Inter Tight, tools/gen_aa_font.py); colours: the palette's
 * theme tokens T_BG, T_TEXT, T_THEME, T_MID, T_DIM ... (gfx.c). */
#define UI_VERSION FELUCCA_VERSION

static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* ------------------------------------------------------------ settings --- */
#define SET_MAGIC 0x4F524B31u      /* "ORK1": bump when the layout changes (older records are then ignored) */
typedef struct {
    uint32_t magic;
    int8_t octave;
    uint8_t patch, attack, release, reverb;
    uint8_t one_core;              /* 1 = the second core stays off (0 in records from before it: on) */
    uint8_t rsv[2];
} settings_t;
static uint8_t ui_one_core;        /* the setting (main.c reads it at power-on, before the audio) */

static void settings_boot(void)    /* power-on, before the audio */
{
    settings_t s;
    uint32_t f = irq_save();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    irq_restore(f);
    if (!flash_ok)
        return;
    if (st_load(OBJ_SETTINGS, &s, sizeof s) == (int)sizeof s && s.magic == SET_MAGIC) {
        sys.octave = (int8_t)clamp(s.octave, OCT_MIN, OCT_MAX);
        sys.patch = (uint8_t)(s.patch % ORK_NPATCH);
        sys.attack = (uint8_t)(s.attack & 127u);
        sys.release = (uint8_t)(s.release & 127u);
        sys.reverb = (uint8_t)(s.reverb & 127u);
        ui_one_core = s.one_core & 1u;
    }
}

static int settings_save(void)     /* 0 = saved */
{
    settings_t s;
    if (!flash_ok)
        return -1;
    memset(&s, 0, sizeof s);
    s.magic = SET_MAGIC;
    s.octave = sys.octave;
    s.patch = sys.patch;
    s.attack = sys.attack;
    s.release = sys.release;
    s.reverb = sys.reverb;
    s.one_core = ui_one_core;
    return st_save(OBJ_SETTINGS, &s, sizeof s);
}

/* ------------------------------------------------------------- screen --- */
enum { L_HEAD, L_NOTE, L_OCT, L_K1, L_K2, L_K3, L_K4, L_EVENT, L_STATUS, NLINES };
static const struct { uint8_t y; const aafont_t *f; uint8_t align; } LINE[NLINES] = {
    {6, &AF_M, 0}, {44, &AF_L, 1}, {82, &AF_M, 1},
    {112, &AF_M, 0}, {134, &AF_M, 0}, {156, &AF_M, 0}, {178, &AF_M, 0},
    {204, &AF_S, 1}, {222, &AF_S, 1},
};
static char ui_shown[NLINES][28];
static uint8_t ui_force = 1;       /* redraw everything (after the splash, the update screen) */
static char ui_event[24] = "PLAY THE KEYS";
static uint32_t ui_event_ms;

static void patch_select(int32_t to)                /* a sound: its own attack and release */
{
    const ork_patch_t *p;
    to = clamp(to, 0, ORK_NPATCH - 1);
    p = &ORK_PATCH[to];
    sys.patch = (uint8_t)to;
    sys.attack = p->attack;
    sys.release = p->release;
}

static void ui_line(uint32_t i, const char *s, uint16_t c)
{
    if (!ui_force && str_eq(ui_shown[i], s))
        return;
    str_cpy(ui_shown[i], s, sizeof ui_shown[i]);
    draw_text_line(LINE[i].align ? 0u : 16u, LINE[i].y, LINE[i].align ? 240u : 208u, LINE[i].f, s, c, T_BG,
                   LINE[i].align);
}

static void ui_say(const char *s)                    /* the event line (the last button, knob, message) */
{
    str_cpy(ui_event, s, sizeof ui_event);
    ui_event_ms = fm1_ms;
}

static void ui_cat(char *d, uint32_t n, const char *s)
{
    uint32_t i = str_len(d);
    while (*s && i + 1u < n)
        d[i++] = *s++;
    d[i] = 0;
}

/* a knob row: its label left, its value right, one strip */
static void ui_kv(uint32_t i, const char *k, const char *v, uint16_t c)
{
    char sig[28];
    const aafont_t *f = LINE[i].f;
    str_cpy(sig, k, sizeof sig);
    ui_cat(sig, sizeof sig, "=");
    ui_cat(sig, sizeof sig, v);
    if (!ui_force && str_eq(ui_shown[i], sig))
        return;
    str_cpy(ui_shown[i], sig, sizeof ui_shown[i]);
    cv_begin(208, f->h, T_BG);
    cv_text(0, 0, f, k, T_MID);
    cv_text_r(208, 0, f, v, c, T_BG);
    cv_blit(16, LINE[i].y);
    lcd_sync();
}


static void ui_draw(void)
{
    char b[28], v[12];
    uint32_t note = sys.last_note;
    if (ui_force)
        lcd_fill(0, 0, 240, 240, T_BG);
    str_cpy(b, "ORKEST  ", sizeof b);
    ui_cat(b, sizeof b, UI_VERSION);
    if (usb.config && !usb.suspended)
        ui_cat(b, sizeof b, "  USB");
    ui_line(L_HEAD, b, T_MID);
    ui_line(L_NOTE, ORK_PATCH[sys.patch % ORK_NPATCH].name, T_THEME);
    if (note < 128u) {
        str_cpy(b, NOTE_NAME[note % 12u], sizeof b);
        fmt_int(v, (int32_t)note / 12 - 1);
        ui_cat(b, sizeof b, v);
    } else
        str_cpy(b, "-", sizeof b);
    ui_cat(b, sizeof b, "   OCTAVE ");
    fmt_int(v, sys.octave);
    if (sys.octave > 0)
        ui_cat(b, sizeof b, "+");
    ui_cat(b, sizeof b, v);
    ui_line(L_OCT, b, T_TEXT);
    fmt_int(v, sys.patch + 1);
    ui_cat(v, sizeof v, " / ");
    {
        char t[6];
        fmt_int(t, ORK_NPATCH);
        ui_cat(v, sizeof v, t);
    }
    ui_kv(L_K1, "KNOB 1  SOUND", v, T_TEXT);
    fmt_int(v, sys.attack);
    ui_kv(L_K2, "KNOB 2  ATTACK", v, T_TEXT);
    fmt_int(v, sys.release);
    ui_kv(L_K3, "KNOB 3  RELEASE", v, T_TEXT);
    fmt_int(v, sys.reverb);
    ui_kv(L_K4, "KNOB 4  ROOM", v, T_TEXT);
    ui_line(L_EVENT, fm1_ms - ui_event_ms < 3000u || ui_force ? ui_event : "", T_THEME);
    str_cpy(b, "CPU ", sizeof b);
    fmt_int(v, (int32_t)(sys.cpu_q8 * 100u / 256u));
    ui_cat(b, sizeof b, v);
    ui_cat(b, sizeof b, "%  VOICES ");
    fmt_int(v, sys.voices);
    ui_cat(b, sizeof b, v);
    ui_cat(b, sizeof b, synth_cores() > 1u ? "  2 CORES" : "  1 CORE");
    ui_line(L_STATUS, b, T_MID);
    ui_force = 0;
}

static void draw_splash(void)
{
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 96, 240, &AF_L, "ORKEST", T_THEME, 1);
    draw_text_box(0, 136, 240, &AF_S, UI_VERSION, T_MID, 1);
}

/* -------------------------------------------------------------- LEDs --- */
/* The picture is built off-line and written one byte per column: clearing and relighting would let the
 * 10 kHz scan catch the dark gap and flicker. Every LED glows dim (fm1_led_dim), lit while pressed. */
static void led_on(uint8_t *nl, uint32_t id)            /* matrix id: button 0..13, note key 14..40 */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                nl[p] |= (uint8_t)(1u << r);
}

static void ui_leds(void)
{
    uint8_t nl[FM1_NCOL];
    uint32_t i, notes = fm1_in.notes, btns = fm1_in.buttons;
    if (fm1_led_anim_on())                               /* the power-on sweep still runs (hal/fm1_led_anim.h) */
        return;
    memset(nl, 0, sizeof nl);
    for (i = 0; i < NKEYS; i++)
        if ((notes >> i) & 1u)
            led_on(nl, 14u + i);
    for (i = 0; i < 14u; i++)
        if ((btns >> i) & 1u)
            led_on(nl, i);
    if (sys.octave < 0)
        led_on(nl, panel.btn[B_OCTDN]);
    if (sys.octave > 0)
        led_on(nl, panel.btn[B_OCTUP]);
    for (i = 0; i < FM1_NCOL; i++)
        fm1_led[i] = nl[i];
}

static void ui_leds_init(void)                         /* the idle glow under every LED */
{
    uint8_t nl[FM1_NCOL];
    uint32_t i;
    memset(nl, 0, sizeof nl);
    for (i = 0; i < 14u + NKEYS; i++)
        led_on(nl, i);
    for (i = 0; i < FM1_NCOL; i++)
        fm1_led_dim[i] = nl[i];
}

/* ------------------------------------------------------------- input --- */
static void ui_input(void)
{
    uint32_t pressed = fm1_input_edges(0), b;
    int32_t s;
    char t[24], v[12];
    for (b = 0; b < NB; b++) {
        if (!(pressed & btn_bit(b)))
            continue;
        switch (b) {
        case B_OCTDN:
        case B_OCTUP:
            if ((fm1_in.buttons & (btn_bit(B_OCTDN) | btn_bit(B_OCTUP))) == (btn_bit(B_OCTDN) | btn_bit(B_OCTUP)))
                break;                                  /* both: the UBOOT combo (main.c), not an octave */
            sys.octave = (int8_t)clamp(sys.octave + (b == B_OCTUP ? 1 : -1), OCT_MIN, OCT_MAX);
            break;
        case B_SAVE:
            ui_say(settings_save() == 0 ? "SAVED" : flash_ok ? "SAVE FAILED" : "NO FLASH");
            break;
        case B_GLO:                                     /* takes effect at the next power-on (it starts at boot) */
            ui_one_core ^= 1u;
            ui_say(ui_one_core ? "CORE 2 OFF: SAVE+REBOOT" : "CORE 2 ON: SAVE+REBOOT");
            break;
        default:
            str_cpy(t, "BUTTON ", sizeof t);
            ui_cat(t, sizeof t, B_NAME[b]);
            ui_say(t);
            break;
        }
    }
    if ((s = panel_enc(EN_K1)) != 0)
        patch_select(sys.patch + s);
    if ((s = panel_enc(EN_PRESET)) != 0)
        patch_select(sys.patch + s);
    if ((s = panel_enc(EN_K2)) != 0)
        sys.attack = (uint8_t)clamp(sys.attack + s, 0, 127);
    if ((s = panel_enc(EN_K3)) != 0)
        sys.release = (uint8_t)clamp(sys.release + s, 0, 127);
    if ((s = panel_enc(EN_K4)) != 0)
        sys.reverb = (uint8_t)clamp(sys.reverb + s, 0, 127);
    for (b = EN_SELECT; b <= EN_ALGO; b++)
        if ((s = panel_enc(b)) != 0) {
            str_cpy(t, E_NAME[b], sizeof t);
            ui_cat(t, sizeof t, s > 0 ? " +" : " ");
            fmt_int(v, s);
            ui_cat(t, sizeof t, v);
            ui_say(t);
        }
}
