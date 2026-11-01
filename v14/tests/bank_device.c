/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* tests/upload_test.mjs's FM1: app/orkest_bank.c with a simulated user flash, its MIDI over stdin / stdout
 * (one SysEx message a line, hex). DROP=n: the n-th reply is lost (the host must recover). At EOF: the flash's
 * first bytes to build/host/flash.bin, the bank's state on stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orkest.h"
#include "fm1_device.h"
#define FLASH_BYTES 536576u
static uint8_t flash[FLASH_BYTES];
static uint32_t ms;
static int drop_at, sent;
uint32_t fm1_millis(void) { return ++ms; }
uint32_t fm1_flash_size(void) { return FLASH_BYTES; }
int fm1_flash_read(uint32_t o, void *d, size_t n) { memcpy(d, flash + o, n); return 0; }
int fm1_flash_erase(uint32_t o) { memset(flash + o, 0xFF, 4096); return 0; }
int fm1_flash_write(uint32_t o, const void *d, size_t n)
{
    size_t i; const uint8_t *s = d;
    for (i = 0; i < n; i++) { if ((flash[o + i] & s[i]) != s[i]) return FM1_FLASH_NEEDS_ERASE; flash[o + i] &= s[i]; }
    return 0;
}
const uint8_t *fm1_flash_map(uint32_t o, size_t n) { (void)n; return flash + o; }
int fm1_flash_invalidate(uint32_t o, size_t n) { (void)o; (void)n; return 0; }
unsigned fm1_midi_input_source(void) { return FM1_MIDI_USB; }
int fm1_midi_send_port(unsigned port, const uint8_t *m, size_t n)
{
    size_t i;
    (void)port;
    if (++sent == drop_at) return 0;
    for (i = 0; i < n; i++) printf("%02x", m[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}
volatile uint16_t orkest_load_q8;
int main(void)
{
    static char line[4096];
    static uint8_t m[2048];
    FILE *f;
    drop_at = getenv("DROP") ? atoi(getenv("DROP")) : 0;
    memset(flash, 0xFF, sizeof flash);
    ork_bank_boot();
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line) / 2, i;
        for (i = 0; i < n; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); m[i] = (uint8_t)v; }
        ork_bank_sysex(m, n, FM1_MIDI_USB);
        ork_bank_poll();
    }
    f = fopen("build/host/flash.bin", "wb");
    fwrite(flash, 1, FLASH_BYTES, f);
    fclose(f);
    fprintf(stderr, "state %u patches %u\n", ork_bank_state(), ork_bank.np);
    return 0;
}
