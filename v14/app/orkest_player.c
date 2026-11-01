/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Orkest's sample player (audio ISR). A zone is one recording: its attack (the transient, played once) and, for
 * sustained instruments, a crossfaded loop (the sustain, repeated while the key is held); one-shots (pizzicato,
 * harp, timpani ...) play to the end and ignore note off. A patch is up to three instruments layered, each with its
 * own key range, transposition, level and place on the stage. After the voices: a small stereo room.
 *
 * FM1 Open rules: every function the ISR runs and every table it reads live in internal SRAM (FM1_AUDIO_CODE,
 * FM1_AUDIO_DATA, .bss); the samples are read from the mapped user flash, which the SDK never programs while the
 * ISR renders (board/audio.c: fm1_audio_suspend). Fixed point throughout. */
#include "orkest.h"
#include "fm1_block.h"

#define NVOICE 24u
#define FS FM1_SAMPLE_RATE
#define ENV_MAX (1u << 30)
enum { ST_OFF, ST_ATTACK, ST_SUSTAIN, ST_RELEASE };

static FM1_AUDIO_DATA const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static FM1_AUDIO_DATA const int8_t IMA_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
/* 2^(i/192), Q30: pitch ratios in 1/16 semitones within an octave */
static FM1_AUDIO_DATA const uint32_t POW2[192] = {
#include "orkest_pow2.inc"
};
/* envelope: frames for a full rise / fall, 1 ms .. 3 s (0..127, squared) -> step; computed at note on */

typedef struct {
    const ork_zone_t *z;
    uint32_t pos, frac, step;    /* next sample to decode; position between s0 and s1 (Q16); samples per frame Q16 */
    int32_t pred, s0, s1;        /* the decoder's predictor; the two samples interpolated between */
    uint32_t env, estep, age;    /* 0..ENV_MAX; per frame; started at */
    uint16_t gl, gr;             /* level per side, Q15 */
    uint8_t idx, note, stage, one, ended;
} voice_t;
static voice_t voice[NVOICE];
static uint32_t voice_clock;

struct ork_settings ork_set = {0, 8, 50, 40, 100};
struct ork_status ork_stat = {0, 0xFF, 0, 1};

/* ------------------------------------------------------------ events --- */
enum { QN = 64u };
static struct { volatile uint32_t w, r; uint32_t q[QN]; } lane[2];
static volatile uint8_t panic_req, stop_req = 1;

#ifndef FM1_HOST_TEST
#define BARRIER() __asm__ volatile("csync" ::: "memory")
#else
#define BARRIER() __asm__ volatile("" ::: "memory")
#endif

int ork_post(unsigned l, uint8_t status, uint8_t d1, uint8_t d2)
{
    uint32_t w;
    if (l > 1u)
        return -1;
    w = lane[l].w;
    if (w - lane[l].r >= QN)
        return -1;
    lane[l].q[w % QN] = (uint32_t)status | (uint32_t)d1 << 8 | (uint32_t)d2 << 16;
    BARRIER();
    lane[l].w = w + 1u;
    return 0;
}
void ork_panic(void) { panic_req = 1; }
void ork_player_stop(void)
{
    stop_req = 1;
    BARRIER();
}
void ork_player_start(void)
{
    ork_stat.idle = 0;
    BARRIER();
    stop_req = 0;
}

/* ------------------------------------------------------------ voices --- */
static inline __attribute__((always_inline)) int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static FM1_AUDIO_CODE uint32_t env_step(uint32_t v)
{
    uint32_t ms = 1u + (v * v * 3000u) / (127u * 127u);
    return ENV_MAX / (ms * (FS / 1000u));
}
/* samples per output frame (Q16) of a zone played at MIDI note `note`: +-2 octaves (bounded decode load) */
static FM1_AUDIO_CODE uint32_t zone_step(const ork_zone_t *z, int32_t note)
{
    int32_t d16 = clampi(note * 16 - z->root16, -24 * 16, 24 * 16);
    uint32_t u = (uint32_t)(d16 + 4 * 192), oct = u / 192u;
    uint64_t r = ((uint64_t)z->rate * POW2[u % 192u]) >> 30;
    return (uint32_t)(oct >= 4u ? r << (oct - 4u) : r >> (4u - oct));
}
/* the next sample of the voice's zone (IMA ADPCM, two a byte, the even one low); a loop jumps back to ls in the
 * decoder state stored for it; past the end of a one-shot: silence, ended */
static inline __attribute__((always_inline)) int32_t adpcm_next(voice_t *v)
{
    const ork_zone_t *z = v->z;
    uint32_t pos = v->pos, b, code;
    int32_t step, vd, s;
    if (pos >= z->n) {
        v->ended = 1;
        return 0;
    }
    b = ork_bank.data[z->off + (pos >> 1)];
    code = (pos & 1u) ? (b >> 4) : (b & 15u);
    step = IMA_STEP[v->idx];
    vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    s = clampi(v->pred + ((code & 8u) ? -vd : vd), -32768, 32767);
    v->pred = s;
    v->idx = (uint8_t)clampi((int32_t)v->idx + IMA_IDX[code & 7u], 0, 88);
    if (++pos == z->le && z->looped) {
        pos = z->ls;
        v->pred = z->pred;
        v->idx = z->idx;
    }
    v->pos = pos;
    return s;
}
static FM1_AUDIO_CODE voice_t *voice_alloc(void)
{
    uint32_t i;
    voice_t *rel = 0, *old = &voice[0];
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_OFF)
            return &voice[i];
    for (i = 0; i < NVOICE; i++) {              /* steal the oldest released voice, else the oldest */
        if (voice[i].stage == ST_RELEASE && (!rel || voice[i].age < rel->age))
            rel = &voice[i];
        if (voice[i].age < old->age)
            old = &voice[i];
    }
    return rel ? rel : old;
}
static FM1_AUDIO_CODE void note_on(uint32_t note, uint32_t vel)
{
    const ork_patch_t *p;
    uint32_t i, l, vq = vel * vel * 2u;         /* velocity curve, Q15: 127 -> 32258, 100 -> 20000 */
    if (!ork_bank.np)
        return;
    p = &ork_bank.patch[ork_set.patch % ork_bank.np];
    for (i = 0; i < NVOICE; i++)                /* the same note again: the old one fades as the new one starts */
        if (voice[i].note == note && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(ork_set.release);
        }
    for (l = 0; l < p->nl && l < 3u; l++) {
        const ork_layer_t *ly = &p->l[l];
        const ork_inst_t *in;
        const ork_zone_t *z;
        int32_t n2 = (int32_t)note + ly->tr;
        uint32_t g, pan, k;
        voice_t *v;
        if (ly->inst >= ork_bank.ni || note < ly->lo || note > ly->hi || n2 < 0 || n2 > 127)
            continue;
        in = &ork_bank.inst[ly->inst];
        if (!in->nz || (uint32_t)in->z0 + in->nz > ork_bank.nz)
            continue;
        z = &ork_bank.zone[in->z0];
        for (k = 0; k < in->nz; k++)
            if (n2 >= ork_bank.zone[in->z0 + k].lo && n2 <= ork_bank.zone[in->z0 + k].hi) {
                z = &ork_bank.zone[in->z0 + k];
                break;
            }
        v = voice_alloc();
        v->z = z;
        v->step = zone_step(z, n2);
        v->pos = v->frac = 0;
        v->pred = 0;
        v->idx = 0;
        v->s0 = 0;
        v->ended = 0;
        v->s1 = adpcm_next(v);                  /* the interpolator starts between 0 and the first sample */
        v->env = 0;
        v->estep = env_step(in->one ? 0u : ork_set.attack);
        v->stage = ST_ATTACK;
        v->one = in->one;
        v->note = (uint8_t)note;
        v->age = ++voice_clock;
        g = (((vq * ly->gain) >> 8) * in->gain) >> 8;
        if (g > 32767u)
            g = 32767u;
        pan = (uint32_t)(ly->pan + 64);          /* 0..127, 64 = centre: full on the near side */
        v->gl = (uint16_t)(pan <= 64u ? g : g * (128u - pan) / 64u);
        v->gr = (uint16_t)(pan >= 64u ? g : g * pan / 64u);
    }
    ork_stat.last_note = (uint8_t)note;
    ork_stat.last_vel = (uint8_t)vel;
}
static FM1_AUDIO_CODE void note_off(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].note == note && !voice[i].one && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(ork_set.release);
        }
}
static FM1_AUDIO_CODE void all_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(voice[i].one ? 20u : ork_set.release);
        }
}

/* note events, held counts: a note held by several sources (a key and MIDI) ends when the last lets go */
static uint8_t held[128];
static FM1_AUDIO_CODE void events(void)
{
    uint32_t l;
    if (panic_req) {
        panic_req = 0;
        for (l = 0; l < 128u; l++)
            held[l] = 0;
        all_off();
    }
    for (l = 0; l < 2u; l++)
        while (lane[l].r != lane[l].w) {
            uint32_t e, st, d1, d2;
            BARRIER();
            e = lane[l].q[lane[l].r % QN];
            BARRIER();
            lane[l].r++;
            st = e & 0xF0u;
            d1 = (e >> 8) & 0x7Fu;
            d2 = (e >> 16) & 0x7Fu;
            if (st == 0x90u && d2) {
                if (held[d1] < 255u)
                    held[d1]++;
                note_on(d1, d2);
            } else if (st == 0x80u || st == 0x90u) {
                if (held[d1] && --held[d1] == 0u)
                    note_off(d1);
            } else if (st == 0xB0u && (d1 == 123u || d1 == 120u)) {
                for (d1 = 0; d1 < 128u; d1++)
                    held[d1] = 0;
                all_off();
            }
        }
}

/* ------------------------------------------------------------- room --- */
/* Freeverb-style: per side 4 lowpass-feedback combs in parallel, then 2 allpasses in series; int16 lines, Q15
 * coefficients. The right side's lines are 23 samples longer (width). */
#define RV_NC 4
#define RV_NA 2
#define RV_SPREAD 23
static FM1_AUDIO_DATA const uint16_t RV_COMB[RV_NC] = {1116, 1188, 1277, 1356};
static FM1_AUDIO_DATA const uint16_t RV_AP[RV_NA] = {556, 441};
static int16_t rv_cbuf[2][RV_NC][1356 + RV_SPREAD];
static int16_t rv_abuf[2][RV_NA][556 + RV_SPREAD];
static uint16_t rv_ci[2][RV_NC], rv_ai[2][RV_NA];
static int32_t rv_lp[2][RV_NC];
#define RV_FB 27525              /* 0.84: decay ~1.6 s */
#define RV_DAMP 9830             /* 0.30 */
static inline __attribute__((always_inline)) int32_t rv_side(uint32_t c, int32_t in)
{
    uint32_t k;
    int32_t acc = 0;
    for (k = 0; k < RV_NC; k++) {
        uint32_t len = RV_COMB[k] + (c ? RV_SPREAD : 0u), i = rv_ci[c][k];
        int32_t y = rv_cbuf[c][k][i];
        rv_lp[c][k] += ((y - rv_lp[c][k]) * (32768 - RV_DAMP)) >> 15;
        rv_cbuf[c][k][i] = (int16_t)clampi(in + ((rv_lp[c][k] * RV_FB) >> 15), -32768, 32767);
        rv_ci[c][k] = (uint16_t)(i + 1u == len ? 0u : i + 1u);
        acc += y;
    }
    for (k = 0; k < RV_NA; k++) {
        uint32_t len = RV_AP[k] + (c ? RV_SPREAD : 0u), i = rv_ai[c][k];
        int32_t b = rv_abuf[c][k][i];
        rv_abuf[c][k][i] = (int16_t)clampi(acc + (b >> 1), -32768, 32767);
        acc = b - acc;
        rv_ai[c][k] = (uint16_t)(i + 1u == len ? 0u : i + 1u);
    }
    return acc;
}
/* Q15 in +-1.5 -> +-1, smooth: x - 4/27 x^3 */
static inline __attribute__((always_inline)) int32_t soft_clip(int32_t x)
{
    int64_t c;
    x = clampi(x, -49151, 49151);
    c = ((int64_t)x * x >> 15) * x >> 15;
    return x - (int32_t)(c * 4 / 27);
}

/* ----------------------------------------------------------- render --- */
FM1_AUDIO_CODE unsigned orkest_output_level(void) { return ork_set.master; }

FM1_AUDIO_CODE int orkest_render(int32_t *out)
{
    uint32_t i, k, busy = 0;
    int32_t wet = (int32_t)ork_set.reverb * 258;   /* Q15 */
    if (!out)
        return -1;
    for (i = 0; i < 2u * FM1_BLOCK_FRAMES; i++)
        out[i] = 0;
    if (stop_req) {                                 /* no bank (or being rewritten): silence, every voice gone */
        for (k = 0; k < NVOICE; k++)
            voice[k].stage = ST_OFF;
        for (k = 0; k < 2u; k++)
            lane[k].r = lane[k].w;
        for (k = 0; k < 128u; k++)
            held[k] = 0;
        ork_stat.voices = 0;
        ork_stat.idle = 1;
        return 0;
    }
    events();
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &voice[k];
        if (v->stage == ST_OFF)
            continue;
        busy++;
        for (i = 0; i < FM1_BLOCK_FRAMES; i++) {
            int32_t s;
            if (v->stage == ST_ATTACK) {
                if (ENV_MAX - v->env <= v->estep) {
                    v->env = ENV_MAX;
                    v->stage = ST_SUSTAIN;
                } else
                    v->env += v->estep;
            } else if (v->stage == ST_RELEASE) {
                if (v->env <= v->estep) {
                    v->env = 0;
                    v->stage = ST_OFF;
                } else
                    v->env -= v->estep;
            }
            v->frac += v->step;
            while (v->frac >= 65536u) {
                v->frac -= 65536u;
                v->s0 = v->s1;
                v->s1 = adpcm_next(v);
            }
            s = v->s0 + (((v->s1 - v->s0) * (int32_t)(v->frac >> 1)) >> 15);
            s = (s * (int32_t)(v->env >> 15)) >> 15;
            out[2u * i] += (s * v->gl) >> 16;       /* each voice at -6 dB: big chords reach the soft clip */
            out[2u * i + 1u] += (s * v->gr) >> 16;
            if (v->stage == ST_OFF)
                break;
        }
        if (v->ended && v->s0 == 0 && v->s1 == 0)
            v->stage = ST_OFF;                       /* a one-shot played to its end */
    }
    for (i = 0; i < FM1_BLOCK_FRAMES; i++) {
        int32_t l = out[2u * i], r = out[2u * i + 1u];
        if (wet) {
            int32_t in = (l + r) >> 4;               /* the room's input: mono, -24 dB */
            l += (rv_side(0, in) * wet) >> 15;
            r += (rv_side(1, in) * wet) >> 15;
        }
        out[2u * i] = soft_clip(l) * 256;            /* Q15 -> Q23 */
        out[2u * i + 1u] = soft_clip(r) * 256;
    }
    ork_stat.voices = (uint8_t)busy;
    return 0;
}
