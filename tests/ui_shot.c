/* SPDX-License-Identifier: GPL-3.0-only */
/* The screen without the hardware: firmware/src/ui.c and gfx.c (the real drawing code) against a 240 x 240
 * framebuffer instead of the LCD, a few states drawn and saved as raw RGB565 (tests/ui_shot.py makes PNGs).
 * Also checks that no line is drawn outside the screen.
 *   tests/ui_shot OUT_DIR      (run_tests.sh: PNGs in build/host/ui) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp

/* ------------------------------------------------------------ the LCD --- */
static uint16_t fb[240 * 240];
static int off_screen;
static void lcd_sync(void) {}
static void lcd_wake_now(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    if (x + w > 240u || y + h > 240u)
        off_screen++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = (uint16_t)(px[j * w + i] >> 8 | px[j * w + i] << 8);   /* (byte-swapped) */
}
#include "../firmware/src/gfx.c"

/* ------------------------------------------------- what ui.c reads --- */
#define FELUCCA_VERSION "v0.1"
#include "../firmware/src/core.h"
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {      /* as hal/fm1_input.h */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static struct { volatile uint32_t notes, buttons; } fm1_in;
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL];
static int fm1_led_anim_on(void) { return 0; }
static int32_t enc_steps[7];
static int32_t fm1_enc_take(uint32_t e) { int32_t s = enc_steps[e]; enc_steps[e] = 0; return s; }
static uint32_t edges;
static uint32_t fm1_input_edges(uint32_t *released) { uint32_t p = edges; (void)released; edges = 0; return p; }
static struct { uint8_t config, suspended; } usb = {1, 0};
static uint8_t flash_ok = 1;
enum { OBJ_SETTINGS };
static uint8_t flash_store[64];
static int saved;
static int st_load(uint32_t obj, void *dst, uint32_t max) { (void)obj; if (!saved) return -1; memcpy(dst, flash_store, max); return (int)max; }
static int st_save(uint32_t obj, const void *src, uint32_t len) { (void)obj; memcpy(flash_store, src, len); saved = 1; return 0; }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec(void) { return 0x856014u; }
#define FL_FAR(f) fl_jedec
#include "../firmware/src/panel.c"
enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_COUNT };
static const char *const WAVE_NAME[W_COUNT] = {"SINE", "TRI", "SAW", "SQUARE"};
#include "../firmware/src/ui.c"

static int shot(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.rgb565", dir, name);
    if (!(f = fopen(path, "wb")))
        return 1;
    fwrite(fb, 2, 240 * 240, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int bad = 0;
    palette_set(UI_GREY_INDEX);

    draw_splash();
    bad |= shot(dir, "splash");

    ui_force = 1;
    fm1_ms = 0;
    ui_draw();
    bad |= shot(dir, "home");

    sys.last_note = 61;                  /* C#4, octave +2, a knob turned, a button, the widest values */
    sys.octave = 2;
    sys.wave = W_SQUARE;
    sys.attack = 127;
    sys.release = 127;
    sys.cpu_q8 = 255;
    sys.voices = 8;
    k4_count = -1234;
    enc_steps[panel.enc[EN_PRESET]] = -12;
    ui_input();
    ui_draw();
    bad |= shot(dir, "playing");

    edges = btn_bit(B_SAVE);
    ui_input();
    ui_draw();
    bad |= shot(dir, "saved");

    sys.octave = -3;
    sys.last_note = 0;
    edges = btn_bit(B_ARP);
    ui_input();
    ui_draw();
    bad |= shot(dir, "low");

    {   /* settings: SAVE wrote them, a power-on reads them back */
        sys.octave = 0; sys.wave = 0; sys.attack = 0; sys.release = 0;
        settings_boot();
        if (sys.octave != 2 || sys.wave != W_SQUARE || sys.attack != 127 || sys.release != 127) {
            printf("settings round trip FAIL\n");
            bad = 1;
        }
    }
    if (off_screen) {
        printf("%d strips drawn outside the screen FAIL\n", off_screen);
        bad = 1;
    }
    printf(bad ? "ui shots: FAILED\n" : "ui shots: ok (%s)\n", dir);
    return bad;
}
