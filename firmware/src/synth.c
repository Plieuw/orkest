/* SPDX-License-Identifier: GPL-3.0-only */
/* Orkest: the orchestral sample player. Runs in the audio ISR only; fixed point throughout.
 *
 * The sounds come from tools/gen_orkest.py (build/gen/orkest_samples.h): VSCO 2 CE recordings (CC0) stored as
 * IMA ADPCM. A zone is one recording: its attack (the transient, played once) and, for sustained instruments, a
 * crossfaded loop (the sustain, repeated while the key is held); one-shot instruments (pizzicato, harp, timpani,
 * glockenspiel) play their excerpt to the end and ignore note off. A patch (KNOB 1) is one instrument or up to
 * ORK_MAXL layered, each layer with its own key range, transposition, level and place on the stage (pan).
 * After the voices: a small stereo room (Freeverb-style: 4 damped combs + 2 allpasses per side), KNOB 4.
 *
 *   synth_note_on(note, vel)    start a note (MIDI note number, velocity 1..127): one voice per sounding layer
 *   synth_note_off(note)        release the voices of that note (one-shots ring on)
 *   synth_all_off()             release all; synth_kill() silence at once
 *   synth_render(out, n)        n stereo frames, Q15, interleaved L R (written, not added) */
typedef struct {
    uint32_t off, n, ls, le;     /* ORK_DATA byte offset, samples, loop [ls, le) */
    uint32_t rate;               /* the zone's sample rate / 44100, Q16 */
    int16_t root16;              /* root pitch, 1/16 semitone */
    int16_t pred;                /* ADPCM state at ls */
    uint8_t idx, lo, hi, looped;
} ork_zone_t;
typedef struct {
    const char *name;
    uint8_t z0, nz, one;         /* zones; one = one-shot */
    uint16_t gain;               /* Q8 */
} ork_inst_t;
typedef struct {
    uint8_t inst, lo, hi;        /* instrument; keys it plays */
    int8_t tr;                   /* transposition, semitones */
    uint16_t gain;               /* Q8 */
    int8_t pan;                  /* -64 left .. +63 right */
} ork_layer_t;
typedef struct {
    const char *name;
    uint8_t attack, release, nl;
    ork_layer_t l[3];
} ork_patch_t;
#include "orkest_samples.h"
#if ORK_MAXL != 3
#error "tools/gen_orkest.py MAXL != ork_patch_t's layers"
#endif

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

#define ENV_MAX (1u << 30)
enum { ST_OFF, ST_ATTACK, ST_SUSTAIN, ST_RELEASE };
typedef struct {
    const ork_zone_t *z;
    uint32_t pos, frac, step;    /* next sample to decode; position between s0 and s1 (Q16); samples per frame Q16 */
    int32_t pred, s0, s1;        /* decoder predictor; the two samples interpolated between */
    uint32_t env, estep;         /* 0..ENV_MAX; the attack's or the release's step per frame */
    uint32_t age;                /* started at (voice clock): the oldest is stolen first */
    uint16_t gl, gr;             /* level per side, Q15 (velocity x layer x instrument x pan) */
    uint8_t idx, note, stage, one, ended;
} voice_t;
static voice_t voice[NVOICE];
static uint32_t voice_clock;

/* 0..127 -> frames for a full rise / fall: ~1 ms .. ~3 s, squared for finer control at the short end */
static uint32_t env_step(uint32_t v)
{
    uint32_t ms = 1u + (v * v * 3000u) / (127u * 127u);
    return ENV_MAX / (ms * (FS / 1000u));
}

/* the zone's samples per output frame (Q16) for MIDI note `note` */
static uint32_t zone_step(const ork_zone_t *z, int32_t note)
{
    int32_t d16 = clamp(note * 16 - z->root16, -24 * 16, 24 * 16);   /* +-2 octaves: bounded decode load, and a
                                                                       * one-shot never drags on for seconds */
    uint32_t u = (uint32_t)(d16 + 4 * 192), oct = u / 192u;          /* + 4 octaves: no negative division */
    uint64_t r = ((uint64_t)z->rate * ORK_POW2[u % 192u]) >> 30;      /* rate x 2^(frac / 192), Q16 */
    return (uint32_t)(oct >= 4u ? r << (oct - 4u) : r >> (4u - oct));
}

static const ork_zone_t *inst_zone(const ork_inst_t *in, int32_t note)
{
    uint32_t i;
    for (i = 0; i < in->nz; i++)
        if (note >= ORK_ZONES[in->z0 + i].lo && note <= ORK_ZONES[in->z0 + i].hi)
            return &ORK_ZONES[in->z0 + i];
    return &ORK_ZONES[in->z0];
}

static inline int32_t adpcm_next(voice_t *v);

static voice_t *voice_alloc(void)
{
    uint32_t i;
    voice_t *rel = 0, *old = &voice[0];
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_OFF)
            return &voice[i];
    for (i = 0; i < NVOICE; i++) {                  /* steal: the oldest released voice, else the oldest */
        if (voice[i].stage == ST_RELEASE && (!rel || voice[i].age < rel->age))
            rel = &voice[i];
        if (voice[i].age < old->age)
            old = &voice[i];
    }
    return rel ? rel : old;
}

static void synth_note_on(uint32_t note, uint32_t vel)
{
    const ork_patch_t *p = &ORK_PATCH[sys.patch % ORK_NPATCH];
    uint32_t i, l, vq = vel * vel * 2u;             /* velocity curve, Q15: 127 -> 32258, 100 -> 20000 */
    for (i = 0; i < NVOICE; i++)                    /* the same note again: the old one fades as the new one starts */
        if (voice[i].note == note && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(sys.release);
        }
    for (l = 0; l < p->nl; l++) {
        const ork_layer_t *ly = &p->l[l];
        const ork_inst_t *in = &ORK_INST[ly->inst];
        int32_t n2 = (int32_t)note + ly->tr;
        uint32_t g, pan;
        voice_t *v;
        if (note < ly->lo || note > ly->hi || n2 < 0 || n2 > 127)
            continue;
        v = voice_alloc();
        v->z = inst_zone(in, n2);
        v->step = zone_step(v->z, n2);
        v->pos = 0;
        v->frac = 0;
        v->pred = 0;
        v->idx = 0;
        v->s0 = 0;
        v->ended = 0;
        v->s1 = adpcm_next(v);                     /* the interpolator starts between 0 and the first sample */
        v->env = 0;
        v->estep = env_step(in->one ? 0u : sys.attack);   /* a one-shot keeps its own attack */
        v->stage = ST_ATTACK;
        v->one = in->one;
        v->note = (uint8_t)note;
        v->age = ++voice_clock;
        g = (((vq * ly->gain) >> 8) * in->gain) >> 8;     /* Q15 */
        if (g > 32767u)
            g = 32767u;
        pan = (uint32_t)(ly->pan + 64);                    /* 0..127, 64 = centre: full on the near side */
        v->gl = (uint16_t)(pan <= 64u ? g : g * (128u - pan) / 64u);
        v->gr = (uint16_t)(pan >= 64u ? g : g * pan / 64u);
    }
    sys.last_note = (uint8_t)note;
    sys.last_vel = (uint8_t)vel;
}

static void synth_note_off(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].note == note && !voice[i].one && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(sys.release);
        }
}

static void synth_all_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(voice[i].one ? 20u : sys.release);
        }
}

static void synth_kill(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        voice[i].stage = ST_OFF;
}

/* the next sample of the voice's zone (IMA ADPCM, two samples a byte, the even one in the low nibble); a loop
 * jumps back to ls with the decoder state stored for it; past the end of a one-shot: silence, ended */
static inline int32_t adpcm_next(voice_t *v)
{
    const ork_zone_t *z = v->z;
    uint32_t pos = v->pos, b, code;
    int32_t step, vd, s;
    if (pos >= z->n) {
        v->ended = 1;
        return 0;
    }
    b = ORK_DATA[z->off + (pos >> 1)];
    code = (pos & 1u) ? (b >> 4) : (b & 15u);
    step = IMA_STEP[v->idx];
    vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    s = clamp(v->pred + ((code & 8u) ? -vd : vd), -32768, 32767);
    v->pred = s;
    v->idx = (uint8_t)clamp((int32_t)v->idx + IMA_IDX[code & 7u], 0, 88);
    if (++pos == z->le && z->looped) {           /* the loop: back to ls in the state the encoder stored for it */
        pos = z->ls;
        v->pred = z->pred;
        v->idx = z->idx;
    }
    v->pos = pos;
    return s;
}

/* ------------------------------------------------------------- room --- */
/* Freeverb-style: per side 4 lowpass-feedback combs in parallel, then 2 allpasses in series; int16 lines in the
 * pool (zero-initialised), Q15 coefficients. The right side's lines are 23 samples longer (stereo width). */
#define RV_NC 4
#define RV_NA 2
static const uint16_t RV_COMB[RV_NC] = {1116, 1188, 1277, 1356};
static const uint16_t RV_AP[RV_NA] = {556, 441};
#define RV_SPREAD 23
#define RV_CMAX (1356 + RV_SPREAD)
#define RV_AMAX (556 + RV_SPREAD)
static int16_t rv_cbuf[2][RV_NC][RV_CMAX] __attribute__((section(".pool")));
static int16_t rv_abuf[2][RV_NA][RV_AMAX] __attribute__((section(".pool")));
static uint16_t rv_ci[2][RV_NC], rv_ai[2][RV_NA];
static int32_t rv_lp[2][RV_NC];
#define RV_FB 27525              /* 0.84: the room's size (decay ~1.6 s) */
#define RV_DAMP 9830             /* 0.30: high frequencies decay faster */

static int32_t rv_side(uint32_t c, int32_t in)
{
    uint32_t k;
    int32_t acc = 0;
    for (k = 0; k < RV_NC; k++) {
        uint32_t len = RV_COMB[k] + (c ? RV_SPREAD : 0u), i = rv_ci[c][k];
        int32_t y = rv_cbuf[c][k][i];
        rv_lp[c][k] += ((y - rv_lp[c][k]) * (32768 - RV_DAMP)) >> 15;
        rv_cbuf[c][k][i] = (int16_t)clamp(in + ((rv_lp[c][k] * RV_FB) >> 15), -32768, 32767);
        rv_ci[c][k] = (uint16_t)(i + 1u == len ? 0u : i + 1u);
        acc += y;
    }
    for (k = 0; k < RV_NA; k++) {
        uint32_t len = RV_AP[k] + (c ? RV_SPREAD : 0u), i = rv_ai[c][k];
        int32_t b = rv_abuf[c][k][i];
        rv_abuf[c][k][i] = (int16_t)clamp(acc + (b >> 1), -32768, 32767);
        acc = b - acc;
        rv_ai[c][k] = (uint16_t)(i + 1u == len ? 0u : i + 1u);
    }
    return acc;
}

/* ----------------------------------------------------------- render --- */
static int32_t soft_clip(int32_t x)              /* Q15 in +-1.5 -> +-1: x - 4/27 x^3 */
{
    int64_t c;
    x = clamp(x, -49151, 49151);
    c = ((int64_t)x * x >> 15) * x >> 15;
    return x - (int32_t)(c * 4 / 27);
}

static void synth_render(int32_t *out, uint32_t n)
{
    uint32_t i, k, busy = 0;
    int32_t m = (int32_t)sys.master_q12, wet = (int32_t)sys.reverb * 258;   /* Q15 */
    for (i = 0; i < 2u * n; i++)
        out[i] = 0;
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &voice[k];
        if (v->stage == ST_OFF)
            continue;
        busy++;
        for (i = 0; i < n; i++) {
            int32_t s, g;
            switch (v->stage) {
            case ST_ATTACK:
                if (ENV_MAX - v->env <= v->estep) {
                    v->env = ENV_MAX;
                    v->stage = ST_SUSTAIN;
                } else
                    v->env += v->estep;
                break;
            case ST_RELEASE:
                if (v->env <= v->estep) {
                    v->env = 0;
                    v->stage = ST_OFF;
                } else
                    v->env -= v->estep;
                break;
            }
            v->frac += v->step;
            while (v->frac >= 65536u) {
                v->frac -= 65536u;
                v->s0 = v->s1;
                v->s1 = adpcm_next(v);
            }
            s = v->s0 + (((v->s1 - v->s0) * (int32_t)(v->frac >> 1)) >> 15);
            g = (int32_t)(v->env >> 15);         /* Q15 */
            s = (s * g) >> 15;
            out[2u * i] += (s * v->gl) >> 16;    /* each voice at -6 dB (the samples' sustain sits ~-14 dBFS):
                                                  * big chords reach the soft clip, MASTER sets the rest */
            out[2u * i + 1u] += (s * v->gr) >> 16;
            if (v->stage == ST_OFF)
                break;
        }
        if (v->ended && v->s0 == 0 && v->s1 == 0)
            v->stage = ST_OFF;                   /* a one-shot played to its end */
    }
    for (i = 0; i < n; i++) {
        int32_t l = out[2u * i], r = out[2u * i + 1u];
        if (wet) {
            int32_t in = (l + r) >> 4;            /* the room's input: mono, -24 dB (headroom in the combs) */
            l += (rv_side(0, in) * wet) >> 15;
            r += (rv_side(1, in) * wet) >> 15;
        }
        out[2u * i] = clamp(soft_clip(l) * m >> 12, -32767, 32767);
        out[2u * i + 1u] = clamp(soft_clip(r) * m >> 12, -32767, 32767);
    }
    sys.voices = (uint8_t)busy;
}
