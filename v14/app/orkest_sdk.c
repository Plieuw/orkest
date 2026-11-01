/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* What the FM1 SDK calls in an APP, for Orkest.
 *
 * MIDI in (DIN/TRS, USB, Bluetooth; the SDK's routing task): notes and all-notes-off on every channel go to the
 * orchestra (lane 1 of orkest_player.c); the SysEx of the bank upload goes to orkest_bank.c.
 * The rest are FM1 Open's instrument hooks (projects, the DX7 library, the sampler bank, the sequencer's MIDI
 * output, its transfers): Orkest renders no SDK instrument, so they have nothing to do. They stay defined because
 * the SDK's startup and control task call them. */
#include "orkest.h"
#include "instrument.h"
#include "fm_patch.h"
#include <string.h>

void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n)
{
    unsigned st;
    (void)source;
    if (!m || !n || n > 3u)
        return;
    st = m[0] & 0xF0u;
    if (m[0] == 0xFF)                                /* MIDI reset */
        ork_panic();
    else if ((st == 0x90u || st == 0x80u) && n == 3u)
        (void)ork_post(1, (uint8_t)st, m[1], m[2]);
    else if (st == 0xB0u && n == 3u && (m[1] == 120u || m[1] == 123u))
        (void)ork_post(1, 0xB0, m[1], 0);
}
void fm1_app_sysex(const uint8_t *m, size_t n) { ork_bank_sysex(m, n, fm1_midi_input_source()); }

/* the SDK's startup: the instrument's data */
void groove_project_init(void) {}
void groove_project_poll(void) {}
void groove_project_metadata(const struct groove_project *p) { (void)p; }
void groove_project_name(char name[13]) { memcpy(name, "ORKEST      ", 13); name[12] = 0; }
struct groove_project *groove_project_scratch(void)
{
    static struct groove_project scratch;
    return &scratch;
}
void groove_samples_init(void) {}
void groove_samples_poll(void) {}
void groove_transfer_init(void) {}
void groove_transfer_poll(void) {}
void groove_fm_library_init(void) {}
FM1_DSP_CODE void groove_fm_program(uint8_t patch[156], unsigned program)
{
    unsigned i;
    (void)program;
    for (i = 0; i < 156u; i++)
        patch[i] = 0;
}

/* the sequencer's MIDI output and clock (the SDK instrument would call these while it renders) */
void groove_midi_init(void) {}
void groove_midi_clock_reset(void) {}
void groove_midi_poll(void) {}
void groove_midi_panic(void) { ork_panic(); }
FM1_AUDIO_CODE unsigned groove_midi_mode(unsigned track) { (void)track; return 0; }
FM1_AUDIO_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity)
{
    (void)track; (void)note; (void)velocity;
}
FM1_AUDIO_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_AUDIO_CODE void groove_midi_transport(unsigned owner, uint8_t status) { (void)owner; (void)status; }
FM1_AUDIO_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner; (void)tempo; }
