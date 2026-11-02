/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Orkest's sample player (audio ISR). Every note plays in three independent parts (bank format 2,
 * v14/tools/gen_bank.py), each a stream of its own:
 *   TRANSIENT  the recording's start minus its sustain: once, at ork_set.transient (64 = as recorded)
 *   SUSTAIN    a steady loop in phase with the recording, swelling in over [w_t0, w_t1); with the transient turned
 *              down it takes over the start too (0: the sustain alone from the first sample)
 *   RELEASE    the instrument's recorded release, from note off, at ork_set.tail, handed over from the sustain with
 *              equal power over the release time
 * A patch layer may take its transients from another instrument (MARCATO: the spiccato's bite on the violins).
 * One-shots (pizzicato, harp, timpani ...) are a transient alone, at full level. Patches layer up to three
 * instruments, each with its own key range, transposition, level and place on the stage. Then the LFO (vibrato,
 * tremolo; free or on the tempo, MIDI clock in) and a small stereo room.
 *
 * FM1 Open rules: every function the ISR runs and every table it reads live in internal SRAM (FM1_AUDIO_CODE,
 * FM1_AUDIO_DATA, .bss); the samples are read from the mapped user flash, which the SDK never programs while the
 * ISR renders (board/audio.c: fm1_audio_suspend). Fixed point throughout. */
#include "orkest.h"
#include "fm1_block.h"

#define NVOICE 12u
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

typedef struct {                 /* one part of a note: a wave of the bank, decoded as it plays */
    uint32_t off, n, ls, le;     /* the wave: data offset, samples, loop [ls, le) (le 0: played once) */
    uint32_t pos, frac, step;    /* next sample; position between s0 and s1 (Q16); samples per frame (Q16) */
    int32_t pred, s0, s1;        /* the decoder's predictor; the two samples interpolated between */
    int16_t lpred;               /* the decoder's state at ls */
    uint8_t lidx, idx, on, ended;
} stream_t;
typedef struct {
    stream_t t, l, r;            /* transient, sustain, release */
    uint32_t env, estep, age;    /* 0..ENV_MAX; per frame; started at */
    uint32_t w_t0, w_t1, lpos;   /* the sustain's swell (source samples) and how far the sustain has played */
    uint16_t gl, gr;             /* level per side, Q15 */
    uint16_t lfo_amt, trem;      /* the LFO's share (fades in), the tremolo gain at the end of the last block */
    uint16_t tg;                 /* the transient's level, Q14 (16384 = as recorded) */
    uint16_t rg;                 /* the release's level, Q14 */
    uint16_t w_last;             /* the sustain's level at the end of the last block, Q14 */
    uint8_t note, n2, stage, one; /* the key; the note it plays (with the layer's transposition) */
    uint8_t wk;                  /* the transient knob's share of the swell, Q8 (255: none) */
    uint16_t lp_a;               /* velocity -> tone: its lowpass coefficient, Q15 (32767: open) */
    uint8_t src;                 /* who started it: 0 a key (or MIDI), else ARP's runner + 1 */
    uint8_t vf, tf;              /* the instrument's share of the LFO's vibrato and tremolo, Q7 (128 = all) */
    uint8_t layer;               /* the layer it plays: 0, 1 (the morph's gain) */
    uint32_t nid;                /* the note it belongs to (its layers share it): polyphony counts notes */
    uint32_t g_t, g_l;           /* a slide: the transient's and the sustain's steps it starts from */
    uint16_t g_n, g_total;       /* the slide's blocks to go, of */
    int32_t lp1, lp2;            /* the lowpass's two poles */
} voice_t;
/* velocity -> tone: soft notes duller, hard ones brighter (orkest_tone.inc: 2 poles, 600 Hz .. open) */
static FM1_AUDIO_DATA const uint16_t TONE[128] = {
#include "orkest_tone.inc"
};
static voice_t voice[NVOICE];
static uint32_t voice_clock;

struct ork_settings ork_set = {0, 8, 3, 40, 100, 1, 92, 40, 12, 8, 0, 0, 1, 8, 64, 64, 0, 0, 0, 0,
                               {{0, 0, 1, 90, 0, 0, 100, 0, 100}, {255, 39, 1, 90, 1, 0, 100, 0, 100}}, 0, 100, 0, 7, 50, 3, 100, 9131};
struct ork_status ork_stat = {0, 0xFF, 0, 1, 0, 0, 0};

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
/* samples per output frame (Q16) of a wave at `rate` (Q16 of 44100) and root `root16` played at MIDI note `note`:
 * +-2 octaves (bounded decode load) */
static FM1_AUDIO_CODE uint32_t pitch_step(uint32_t rate, int32_t root16, int32_t note)
{
    int32_t d16 = clampi(note * 16 - root16, -24 * 16, 24 * 16);
    uint32_t u = (uint32_t)(d16 + 4 * 192), oct = u / 192u;
    uint64_t r = ((uint64_t)rate * POW2[u % 192u]) >> 30;
    return (uint32_t)(oct >= 4u ? r << (oct - 4u) : r >> (4u - oct));
}
/* the next sample of a stream (IMA ADPCM, two a byte, the even one low); a loop jumps back to ls in the decoder state
 * stored for it; past the end of a wave played once: silence, ended */
static inline __attribute__((always_inline)) int32_t adpcm_next(stream_t *v)
{
    uint32_t pos = v->pos, b, code;
    int32_t step, vd, s;
    if (pos >= v->n) {
        v->ended = 1;
        return 0;
    }
    b = ork_bank.data[v->off + (pos >> 1)];
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
    if (++pos == v->le && v->le) {
        pos = v->ls;
        v->pred = v->lpred;
        v->idx = v->lidx;
    }
    v->pos = pos;
    return s;
}
/* start a stream on a wave: from `start` in the decoder state given (a loop: its stored state at ls) */
static FM1_AUDIO_CODE void stream_start(stream_t *v, uint32_t off, uint32_t n, uint32_t ls, uint32_t le, int32_t pred,
                                        uint32_t idx, uint32_t step)
{
    v->off = off;
    v->n = n;
    v->ls = ls;
    v->le = le;
    v->lpred = (int16_t)pred;
    v->lidx = (uint8_t)idx;
    v->pos = ls;
    v->pred = pred;
    v->idx = (uint8_t)idx;
    v->frac = 0;
    v->step = step;
    v->ended = 0;
    v->on = 1;
    v->s0 = 0;
    v->s1 = adpcm_next(v);                      /* the interpolator starts between 0 and the first sample */
}
/* the next output sample of a stream at `step` (the LFO's) */
static inline __attribute__((always_inline)) int32_t stream_sample(stream_t *v, uint32_t step)
{
    v->frac += step;
    while (v->frac >= 65536u) {
        v->frac -= 65536u;
        v->s0 = v->s1;
        v->s1 = adpcm_next(v);
    }
    return v->s0 + (((v->s1 - v->s0) * (int32_t)(v->frac >> 1)) >> 15);
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
static FM1_AUDIO_CODE const ork_zone_t *zone_of(const ork_inst_t *in, int32_t note)
{
    uint32_t k;
    for (k = 0; k < in->nz; k++)
        if (note >= ork_bank.zone[in->z0 + k].lo && note <= ork_bank.zone[in->z0 + k].hi)
            return &ork_bank.zone[in->z0 + k];
    return &ork_bank.zone[in->z0];
}
static uint8_t layer_inst[NVOICE];               /* the instrument a voice plays (its release) */
#define ORK_SEAT_DATA FM1_AUDIO_DATA  /* (the audio ISR: in SRAM) */
#define ORK_SEAT_CODE FM1_AUDIO_CODE
#include "orkest_seats.inc"
/* the LFO where the instrument has it of itself: vibrato (bowed strings most, the solo violin more, flute and oboe,
 * a little in bassoon and brass, none in clarinet, plucked, struck or short notes) and tremolo (the flute's breath,
 * a little in oboe, bassoon, trumpet and trombone); Q7, 128 = the LFO's whole depth */
static FM1_AUDIO_DATA const struct { char name[8]; uint8_t vib, trem; } LFO_INST[] = {
    {"VIOLINS", 128, 0}, {"VIOLAS", 128, 0}, {"CELLI", 128, 0}, {"BASSES", 90, 0}, {"SOLO", 154, 0}, {"TREMOLO", 40, 0},
    {"FLUTE", 100, 128}, {"OBOE", 90, 64}, {"BASSOON", 50, 40}, {"HORN", 26, 0}, {"TRUMPET", 50, 26},
    {"TROMBON", 50, 26}, {"TUBA", 26, 0}};
static FM1_AUDIO_CODE uint32_t lfo_share(const char *name, int trem)
{
    uint32_t i, k;
    for (i = 0; i < sizeof LFO_INST / sizeof LFO_INST[0]; i++) {
        for (k = 0; k < 7u && LFO_INST[i].name[k] && name[k] == LFO_INST[i].name[k]; k++)
            ;
        if (k == 7u || !LFO_INST[i].name[k])
            return trem ? LFO_INST[i].trem : LFO_INST[i].vib;
    }
    return 0;                                    /* (none: clarinet, harp, pizzicato, spiccato, timpani, glockenspiel) */
}
/* slides only where the instrument can: bowed strings (not pizzicato or spiccato) and the trombone */
static FM1_AUDIO_DATA const char SLIDE_INST[7][8] = {"VIOLINS", "VIOLAS", "CELLI", "BASSES", "SOLO", "TREMOLO", "TROMBON"};
static FM1_AUDIO_CODE int can_slide(const char *name)
{
    uint32_t i, k;
    for (i = 0; i < 7u; i++) {
        for (k = 0; k < 7u && SLIDE_INST[i][k] && name[k] == SLIDE_INST[i][k]; k++)
            ;
        if (k == 7u || !SLIDE_INST[i][k])
            return 1;
    }
    return 0;
}
static uint32_t layer_nid;                       /* the second layer of a key: its first layer's note (not ended) */
#define NPOLY 12                                 /* notes at once (as many as voices); a new one past it ends the oldest */
static uint32_t note_clock;
static FM1_AUDIO_CODE void release_voice(voice_t *v, uint32_t time, int tail);
static FM1_AUDIO_CODE void poly_limit(void)
{
    uint32_t i, j, n = 0, oldest = 0xFFFFFFFFu;
    for (i = 0; i < NVOICE; i++) {               /* the notes playing (held, or a one-shot still sounding) */
        const voice_t *v = &voice[i];
        int first = 1;
        if (v->stage != ST_ATTACK && v->stage != ST_SUSTAIN)
            continue;
        for (j = 0; j < i && first; j++)
            if ((voice[j].stage == ST_ATTACK || voice[j].stage == ST_SUSTAIN) && voice[j].nid == v->nid)
                first = 0;
        if (first) {
            n++;
            if (v->nid < oldest)
                oldest = v->nid;
        }
    }
    if (n < NPOLY)
        return;
    for (i = 0; i < NVOICE; i++)                 /* last note priority: the oldest note gives way, quickly */
        if (voice[i].nid == oldest && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN))
            release_voice(&voice[i], 8u, 0);
}
/* from: a slide from that note (its pitch gliding to `note` over `glide` blocks), or -1 */
static uint8_t cur_layer;                        /* the layer the next note_on_p plays (the morph's gain) */
static FM1_AUDIO_CODE void note_on_p(uint32_t patch, uint32_t note, uint32_t vel, uint32_t src, int32_t from,
                                     uint32_t glide, uint32_t level)
{
    const ork_patch_t *p;
    uint32_t i, l, vq = vel * vel * 2u;         /* velocity curve, Q15: 127 -> 32258, 100 -> 20000 */
    if (!ork_bank.np)
        return;
    p = &ork_bank.patch[patch % ork_bank.np];
    poly_limit();
    ++note_clock;
    for (i = 0; i < NVOICE; i++)                /* the same note again: the old one fades as the new one starts */
        if (voice[i].note == note && voice[i].src == src && voice[i].nid != layer_nid &&
            (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)) {
            voice[i].stage = ST_RELEASE;
            voice[i].estep = env_step(ork_set.release);
        }
    for (l = 0; l < p->nl && l < 3u; l++) {
        const ork_layer_t *ly = &p->l[l];
        const ork_inst_t *in, *tin;
        const ork_zone_t *z, *zt;
        int32_t n2 = (int32_t)note + ly->tr;
        uint32_t g, pan, k = ork_set.transient;
        voice_t *v;
        if (ly->inst >= ork_bank.ni || note < ly->lo || note > ly->hi || n2 < 0 || n2 > 127)
            continue;
        in = &ork_bank.inst[ly->inst];
        tin = ly->tinst < ork_bank.ni ? &ork_bank.inst[ly->tinst] : in;
        if (!in->nz || !tin->nz)
            continue;
        z = zone_of(in, n2);
        zt = zone_of(tin, n2);
        v = voice_alloc();
        layer_inst[v - voice] = ly->inst;
        v->r.on = 0;
        v->l.on = 0;
        stream_start(&v->t, zt->t_off, zt->t_n, 0, 0, 0, 0, pitch_step(zt->rate, zt->root16, n2));
        if (z->flags & 1u)
            stream_start(&v->l, z->l_off, z->l_n, z->l_ls, z->l_n, z->l_pred, z->l_idx, pitch_step(z->rate, z->root16, n2));
        v->vf = (uint8_t)lfo_share(in->name, 0);
        v->tf = (uint8_t)lfo_share(in->name, 1);
        v->g_n = 0;
        if (from >= 0 && glide && can_slide(in->name)) {   /* a slide (where it can): from the old note's pitch */
            int32_t f2 = from + ly->tr;
            f2 = f2 < 0 ? 0 : f2 > 127 ? 127 : f2;
            v->g_t = pitch_step(zt->rate, zt->root16, f2);
            v->g_l = pitch_step(z->rate, z->root16, f2);
            v->g_n = v->g_total = (uint16_t)(glide > 200u ? 200u : glide);
        }
        v->w_t0 = z->w_t0;
        v->w_t1 = z->w_t1;
        v->lpos = 0;
        v->one = !(z->flags & 1u) || in->one;
        /* the transient's level and its share of the swell: one-shots are a transient alone, always whole */
        if (v->one || k > 127u)
            k = 64;
        v->tg = (uint16_t)(k * 256u);
        v->wk = (uint8_t)(k >= 64u ? 255u : k * 4u);   /* below 64 the sustain takes over the start too */
        v->w_last = 0xFFFFu;                    /* (none yet: the first block starts where it is) */
        v->rg = 0;
        v->env = 0;
        v->lfo_amt = 0;
        v->trem = 32767;
        v->estep = env_step(v->one ? 0u : ork_set.attack);
        v->stage = ST_ATTACK;
        v->note = (uint8_t)note;
        v->src = (uint8_t)src;
        v->layer = cur_layer;
        v->nid = note_clock;
        v->n2 = (uint8_t)n2;
        v->age = ++voice_clock;
        g = (((((vq * ly->gain) >> 8) * in->gain) >> 8) * level) / 100u;   /* the layer's volume */
        if (g > 32767u)
            g = 32767u;
        {   /* the place: the concert seating or the sound's own, times the spread; 0..127, 64 = centre */
            int32_t pn = (ork_set.pan_mode ? ly->pan : seat_pan(in->name)) * (int32_t)ork_set.pan_spread / 100;
            pan = (uint32_t)((pn < -64 ? -64 : pn > 63 ? 63 : pn) + 64);   /* full on the near side */
        }
        {   /* brightness: the velocity, half an octave of cutoff more per octave up */
            int32_t bi = (int32_t)vel + (n2 - 60);
            v->lp_a = TONE[bi < 0 ? 0 : bi > 127 ? 127 : bi];
            v->lp1 = v->lp2 = 0;
        }
        v->gl = (uint16_t)(pan <= 64u ? g : g * (128u - pan) / 64u);
        v->gr = (uint16_t)(pan >= 64u ? g : g * pan / 64u);
    }
    ork_stat.last_note = (uint8_t)note;
    ork_stat.last_vel = (uint8_t)vel;
}
/* a key without ARP: both layers (the second when it has a sound), each at its volume */
static FM1_AUDIO_CODE void note_on(uint32_t note, uint32_t vel)
{
    uint32_t p;
    layer_nid = 0;
    for (p = 0; p < 2u; p++)
        if (ork_set.part[p].patch < ork_bank.np && ork_set.part[p].level) {
            cur_layer = (uint8_t)p;
            note_on_p(ork_set.part[p].patch, note, vel, 0, -1, 0, ork_set.part[p].level);
            layer_nid = note_clock;              /* (the layer just started: the second keeps it) */
        }
    layer_nid = 0;
}
/* note off: the sustain releases over the release time; the instrument's release sample (TAIL) takes over */
static FM1_AUDIO_CODE void release_voice(voice_t *v, uint32_t time, int tail)
{
    const ork_inst_t *in = &ork_bank.inst[layer_inst[v - voice] % (ork_bank.ni ? ork_bank.ni : 1u)];
    v->stage = ST_RELEASE;
    v->estep = env_step(time);
    if (tail && ork_set.tail && in->has_rel && !v->one) {
        stream_start(&v->r, in->rel_off, in->rel_n, 0, 0, 0, 0,
                     pitch_step(in->rel_rate, in->rel_root16, v->n2));
        v->rg = (uint16_t)((ork_set.tail * 256u * in->rel_gain) >> 8);
    }
}
static FM1_AUDIO_CODE void note_off_src(uint32_t note, uint32_t src)   /* the note as `src` started it */
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].note == note && voice[i].src == src && !voice[i].one &&
            (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN))
            release_voice(&voice[i], ork_set.release, 1);
}
static FM1_AUDIO_CODE void note_off(uint32_t note) { note_off_src(note, 0); }
static FM1_AUDIO_CODE void all_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)
            release_voice(&voice[i], voice[i].one ? 20u : ork_set.release, 0);
}

/* note events, held counts: a note held by several sources (a key and MIDI) ends when the last lets go */
static uint8_t held[128];
static uint8_t modwheel;                         /* MIDI CC 1: vibrato, also with the LFO off */
static uint32_t lfo_phase;
/* MIDI clock: ticks since Start, the samples since the last tick, its length (samples, Q8: measured over a beat of
 * 24 ticks, smoothed), how long since a tick came (blocks) */
static uint32_t clk_ticks, clk_since, clk_total, clk_mark, clk_len_q8, clk_age = 0xFFFFu;
static uint8_t clk_count;
#define CLK_TIMEOUT 172u                         /* blocks: 0.5 s without a tick, the clock has stopped */
static FM1_AUDIO_CODE void clock_tick(void)
{
    clk_ticks++;
    clk_since = 0;
    if (clk_age >= CLK_TIMEOUT) {                /* the clock (re)starts: measure afresh */
        clk_count = 0;
        clk_mark = clk_total;
        clk_len_q8 = 0;
    } else if (++clk_count == 24u) {             /* a beat: its length over 24 ticks, smoothed (1/2 new) */
        uint32_t len = (clk_total - clk_mark) * 256u / 24u;
        clk_len_q8 = clk_len_q8 ? (clk_len_q8 + len) / 2u : len;
        clk_count = 0;
        clk_mark = clk_total;
    }
    clk_age = 0;
}
/* ------------------------------------------------------------- morph --- */
/* SEQ MORPH: an AD envelope on the tempo (from each key) that morphs part 1's phrase into part 2's and back. Both
 * phrases run on (each its speed); each note either keeps to its own phrase or gives way, by a threshold of its own
 * against the envelope (so more and more of part 2's notes come in as it rises), and its pitch is drawn towards where
 * the other phrase is by as much, kept in the key (the scale, or natural minor on the key) */
static FM1_AUDIO_DATA const uint16_t MORPH_TICKS[ORK_NMORPH] = ORK_MORPH_TICKS_INIT;
static uint32_t morph_pos;                       /* ticks (Q16) since the key */
static int32_t morph_x;                          /* the envelope: 0 part 1 .. 32767 part 2 */
static FM1_AUDIO_CODE void morph_start(void) { morph_pos = 0; }
static FM1_AUDIO_CODE void morph_block(uint32_t beat_inc)
{
    uint32_t len, top, t;
    int32_t x;
    if (!ork_set.morph_mode) {
        morph_x = 0;
        ork_stat.morph = 0;
        return;
    }
    len = (uint32_t)MORPH_TICKS[ork_set.morph_len % ORK_NMORPH] << 16;
    top = (uint32_t)(((uint64_t)len * (ork_set.morph_skew > 100u ? 100u : ork_set.morph_skew)) / 100u);
    if (ork_set.morph_mode == 2u && morph_pos >= len)
        morph_pos -= len;                        /* looping */
    t = morph_pos;
    if (t >= len)
        x = 0;                                   /* once: back at part 1 */
    else if (t < top)
        x = (int32_t)(((uint64_t)t << 15) / top);   /* up to the top (skew: where it is) */
    else
        x = 32767 - (int32_t)(((uint64_t)(t - top) << 15) / (len - top));   /* and down */
    x = x < 0 ? 0 : x > 32767 ? 32767 : x;
    switch (ork_set.morph_curve & 3u) {          /* the curve */
    case 1: x = (int32_t)((int64_t)x * x >> 15); break;                                   /* slow start */
    case 2: x = 32767 - (int32_t)((int64_t)(32767 - x) * (32767 - x) >> 15); break;       /* fast start */
    case 3: x = (int32_t)(((int64_t)x * x >> 15) * (98304 - 2 * x) >> 15); break;          /* S: 3x^2 - 2x^3 */
    default: break;
    }
    x = x * (int32_t)(ork_set.morph_height > 100u ? 100u : ork_set.morph_height) / 100;   /* the height */
    morph_x = x < 0 ? 0 : x > 32767 ? 32767 : x;
    ork_stat.morph = (uint8_t)(morph_x >> 7);
    morph_pos += beat_inc;
}
static FM1_AUDIO_CODE uint32_t morph_thresh(uint32_t idx, uint32_t salt)   /* a note's own threshold, 1..32767 */
{
    uint32_t h = (idx + 1u) * 2654435761u ^ salt;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return ((h >> 8) & 32766u) + 1u;
}

/* ------------------------------------------------------------ phrases --- */
/* ARP: monophonic, last key priority: the newest key plays the chosen phrase from its note (or, with PARTS, each part
 * chosen in the ARP menu its own phrase); each phrase is one line, one note at a time. Positions in MIDI clock ticks, Q16; a phrase's notes start on the block they fall in
 * (<= 2.9 ms late). */
#define ORK_PHRASE_EVENTS
#include "orkest_phrases.inc"
#define ORK_SCALE_MASKS
#include "orkest_scales.inc"
/* the note in the scale nearest to n (the lower one at a tie); every note when the scale is off */
static FM1_AUDIO_CODE int32_t scale_snap(int32_t n)
{
    uint32_t mask = SCALE_MASK[ork_set.scale < ORK_NSCALE ? ork_set.scale : 0u], root = ork_set.scale_root % 12u, d;
    for (d = 0; d < 7u; d++) {
        if (n - (int32_t)d >= 0 && (mask >> ((uint32_t)(n - (int32_t)d + 120 - (int32_t)root) % 12u) & 1u))
            return n - (int32_t)d;
        if (mask >> ((uint32_t)(n + (int32_t)d + 120 - (int32_t)root) % 12u) & 1u)
            return n + (int32_t)d;
    }
    return n;
}
/* the scale chosen (SCL), else natural minor on the key: where a morphed note lands */
static FM1_AUDIO_CODE int32_t key_snap(int32_t n, int32_t key)
{
    uint32_t d, root = (uint32_t)key % 12u;
    if (ork_set.scale)
        return scale_snap(n);
    for (d = 0; d < 7u; d++) {
        if (n - (int32_t)d >= 0 && (0x5ADu >> ((uint32_t)(n - (int32_t)d + 120 - (int32_t)root) % 12u) & 1u))
            return n - (int32_t)d;
        if (0x5ADu >> ((uint32_t)(n + (int32_t)d + 120 - (int32_t)root) % 12u) & 1u)
            return n + (int32_t)d;
    }
    return n;
}
#define ORK_VELO_STEPS
#include "orkest_velo.inc"
_Static_assert(sizeof VELO_STEP == 185, "velocity patterns: VELO's offsets");
static uint32_t velo_rng = 0x2545F491u;
/* the velocity of the phrase's next note: the key's, shaped by the phrase or the part's pattern (its next step);
 * 0: the pattern rests there (no note) */
static FM1_AUDIO_CODE uint32_t phrase_vel(uint32_t key_vel, uint32_t own, const struct ork_part *pt, uint16_t *step)
{
    uint32_t pat = pt->velo < ORK_NVELO ? pt->velo : 0u;
    int32_t v = (int32_t)own;
    if (pat) {
        v = VELO_STEP[VELO[pat].first + *step % VELO[pat].n];
        *step = (uint16_t)((*step + 1u) % (VELO[pat].n * 16u));   /* (a multiple of every length: no jump) */
        if (!v)
            return 0;
    }
    v = 100 + (v - 100) * (int32_t)(pt->depth > 150u ? 150u : pt->depth) / 100;   /* (past 100 %: more contrast) */
    if (pt->human) {                             /* humanize: +- up to 45 */
        int32_t span = (int32_t)(pt->human > 100u ? 100u : pt->human) * 45 / 100;
        velo_rng = velo_rng * 1664525u + 1013904223u;
        v += (int32_t)((velo_rng >> 16) % (uint32_t)(2 * span + 1)) - span;
    }
    v = (int32_t)key_vel * v / 100;
    return v < 1 ? 1u : v > 127 ? 127u : (uint32_t)v;
}
#define NRUN 2                                   /* one key at a time, its two parts */
#define RUN_NOTES 1
typedef struct {
    uint8_t key, vel, on;
    uint8_t patch, phrase;                       /* what it plays (taken at the key) */
    int8_t shift;                                /* its octave, semitones */
    int16_t prev;                                /* the note it played last (a slide's start), -1 none */
    int8_t cur;                                  /* MORPH: its phrase's last note (interval), where it is now */
    uint8_t curv;                                /* (cur set) */
    uint16_t idx;                                /* the next note in the phrase */
    uint16_t vstep;                              /* the velocity pattern's next step */
    uint32_t pos;                                /* ticks into the phrase, Q16 */
    uint8_t nn, note[RUN_NOTES];                 /* its note sounding, the ticks (Q16) it has left */
    int32_t left[RUN_NOTES];
} run_t;
static run_t run[NRUN];
static FM1_AUDIO_CODE void run_quiet(run_t *r)  /* its notes off */
{
    while (r->nn) {
        r->nn--;
        note_off_src(r->note[r->nn], (uint32_t)(r - run) + 1u);
    }
}
static FM1_AUDIO_CODE void run_end(run_t *r)
{
    run_quiet(r);
    r->on = 0;
}
/* a key (the newest wins: what played ends): part 1, and part 2 when it has a sound */
static FM1_AUDIO_CODE void runs_start(uint32_t key, uint32_t vel)
{
    uint32_t p;
    for (p = 0; p < NRUN; p++) {
        const struct ork_part *pt = &ork_set.part[p];
        run_t *r = &run[p];
        run_end(r);
        if (pt->patch >= ork_bank.np || pt->phrase >= ORK_NPHRASE)
            continue;                            /* (part 2 off) */
        r->key = (uint8_t)key;
        r->vel = (uint8_t)vel;
        r->on = 1;
        r->idx = 0;
        r->pos = 0;
        r->vstep = 0;
        r->prev = -1;
        r->curv = 0;
        r->patch = pt->patch;
        r->phrase = pt->phrase;
        r->shift = (int8_t)(12 * (pt->oct < -2 ? -2 : pt->oct > 2 ? 2 : pt->oct));
    }
}
static FM1_AUDIO_CODE void run_release(uint32_t key)   /* its key let go: HOLD stops, ONCE and LATCH play on */
{
    uint32_t i;
    for (i = 0; i < NRUN; i++)
        if (run[i].on && run[i].key == key && ork_set.arp_mode == 0u)
            run_end(&run[i]);
}
static uint8_t arp_was, phrase_was[NRUN] = {255, 255}, patch_was[NRUN] = {255, 255};
static FM1_AUDIO_CODE void phrases_sync(void)   /* before the block's events: ARP off, or a part changed, it ends */
{
    uint32_t i;
    if (ork_set.arp_on != arp_was) {             /* ARP on or off: what sounds ends (keys held go silent) */
        arp_was = ork_set.arp_on;
        for (i = 0; i < 128u; i++)
            held[i] = 0;
        for (i = 0; i < NRUN; i++)
            run[i].on = run[i].nn = 0;
        all_off();
    }
    for (i = 0; i < NRUN; i++)                   /* another phrase or sound for a part: it stops (the next key) */
        if (ork_set.part[i].phrase != phrase_was[i] || ork_set.part[i].patch != patch_was[i]) {
            phrase_was[i] = ork_set.part[i].phrase;
            patch_was[i] = ork_set.part[i].patch;
            if (run[i].on)
                run_end(&run[i]);
        }
}
static FM1_AUDIO_CODE int32_t speed_log2(uint32_t sp) { return sp == 0u ? -1 : sp == 2u ? 1 : sp == 3u ? -2 : 0; }
/* a note of a part: its pitch, velocity (0: a rest), the patch and the runner whose slot sounds it */
static FM1_AUDIO_CODE void phrase_note(run_t *slot, uint32_t src, uint32_t layer, const struct ork_part *pt, run_t *own,
                                       const ork_pev_t *e, int32_t n, uint32_t inc, int32_t lshift)
{
    int32_t gate = pt->gate < 10u ? 10 : pt->gate > 100u ? 100 : (int32_t)pt->gate, left;
    uint32_t vel, gl;
    if (n < 0 || n > 127)
        return;
    vel = phrase_vel(own->vel, e->vel, pt, &own->vstep);
    if (!vel)
        return;                                  /* the velocity pattern rests: no note (the last one plays on) */
    run_quiet(slot);                             /* one note at a time */
    gl = e->slide && slot->prev >= 0 && inc ? ((uint32_t)e->len << 15) / inc : 0u;   /* a slide: half the note */
    cur_layer = (uint8_t)layer;
    note_on_p(own->patch, (uint32_t)n, vel, src, e->slide ? slot->prev : -1, gl < 4u && gl ? 4u : gl,
              ork_set.morph_mode ? 100u : pt->level);
    slot->prev = (int16_t)n;
    slot->note[slot->nn] = (uint8_t)n;
    if (pt->human) {                             /* humanize the length too: +- up to 35 points of the gate */
        int32_t span = (int32_t)(pt->human > 100u ? 100u : pt->human) * 35 / 100;
        velo_rng = velo_rng * 1664525u + 1013904223u;
        gate += (int32_t)((velo_rng >> 16) % (uint32_t)(2 * span + 1)) - span;
        gate = gate < 5 ? 5 : gate > 100 ? 100 : gate;
    }
    left = (int32_t)(((uint32_t)e->len * (uint32_t)gate << 16) / 100u);   /* in the slot's ticks */
    slot->left[slot->nn] = lshift >= 0 ? left << lshift : left >> -lshift;
    slot->nn++;
}
static FM1_AUDIO_CODE void phrases(void)
{
    uint32_t i, k, beat_inc, inc[NRUN], oni[NRUN];
    const ork_pev_t *on[NRUN] = {0, 0};
    int morph;
    if (!ork_set.arp_on)
        return;
    /* ticks this block: from MIDI clock while it runs (2^31 / its tick length in samples Q8), else the tempo */
    beat_inc = ork_stat.clock && clk_len_q8 ? 2147483648u / clk_len_q8 : ork_set.arp_inc;
    morph = ork_set.morph_mode && run[0].on && run[1].on;
    for (i = 0; i < NRUN; i++) {
        run_t *r = &run[i];
        const struct ork_part *pt = &ork_set.part[i];
        const ork_phrase_t *ph;
        int32_t sh = speed_log2(pt->speed);
        inc[i] = sh >= 0 ? beat_inc << sh : beat_inc >> -sh;
        if (!r->on)
            continue;
        if (r->phrase >= ORK_NPHRASE) {
            run_end(r);
            continue;
        }
        ph = &PHRASE[r->phrase];
        for (k = 0; k < r->nn;)                  /* notes whose time is up */
            if (r->left[k] <= 0) {
                note_off_src(r->note[k], i + 1u);
                r->nn--;
                r->note[k] = r->note[r->nn];
                r->left[k] = r->left[r->nn];
            } else
                k++;
        for (;;) {
            if (r->idx >= ph->n && r->pos >= (uint32_t)ph->len << 16) {   /* the end of the phrase */
                if (ork_set.arp_mode == 1u) {
                    if (!r->nn && !(morph && i == 0u && run[1].on))
                        r->on = 0;
                    break;
                }
                r->pos -= (uint32_t)ph->len << 16;
                r->idx = 0;
            }
            if (r->idx >= ph->n || ((uint32_t)PEV[ph->first + r->idx].at << 16) > r->pos)
                break;
            {
                const ork_pev_t *e = &PEV[ph->first + r->idx];
                oni[i] = r->idx++;
                if (morph) {                     /* (MORPH: the onsets weighed below) */
                    on[i] = e;
                    r->cur = (int8_t)(e->iv + r->shift);
                    r->curv = 1;
                } else
                    phrase_note(r, i + 1u, i, pt, r, e, scale_snap((int32_t)r->key + r->shift + e->iv), inc[i], 0);
            }
        }
        if (!r->on)
            continue;
        r->pos += inc[i];
        for (k = 0; k < r->nn; k++)
            r->left[k] -= (int32_t)inc[i];
    }
    if (morph) {   /* a note keeps to its phrase while the envelope is under its threshold, then gives way */
        int c = -1;
        int a_ok = on[0] && morph_x < (int32_t)morph_thresh(oni[0], 0x5A5Au);
        int b_ok = on[1] && morph_x >= (int32_t)morph_thresh(oni[1], 0xC3C3u);
        c = a_ok && b_ok ? (morph_x >= 16384 ? 1 : 0) : a_ok ? 0 : b_ok ? 1 : -1;
        if (c >= 0) {
            const ork_pev_t *e = on[c];
            int o = 1 - c;
            int32_t mine = e->iv + run[c].shift, other = run[o].curv ? run[o].cur : mine;
            int32_t w = c ? 32767 - morph_x : morph_x;   /* how far towards the other phrase */
            int32_t iv = mine + (((other - mine) * w + (w >= 0 ? 16384 : -16384)) >> 15);
            int32_t key = (int32_t)run[0].key;
            phrase_note(&run[0], 1u, (uint32_t)c, &ork_set.part[c], &run[c], e, key_snap(key + iv, key), inc[c],
                        speed_log2(ork_set.part[0].speed) - speed_log2(ork_set.part[c].speed));
        }
    }
}

static FM1_AUDIO_CODE void events(void)
{
    uint32_t l;
    if (panic_req) {
        panic_req = 0;
        for (l = 0; l < 128u; l++)
            held[l] = 0;
        for (l = 0; l < NRUN; l++)
            run[l].on = run[l].nn = 0;
        all_off();
    }
    for (l = 0; l < 2u; l++)
        while (lane[l].r != lane[l].w) {
            uint32_t e, st, d1, d2;
            BARRIER();
            e = lane[l].q[lane[l].r % QN];
            BARRIER();
            lane[l].r++;
            if ((e & 0xFFu) >= 0xF8u) {             /* real time: clock, start */
                if ((e & 0xFFu) == 0xF8u)
                    clock_tick();
                else if ((e & 0xFFu) == 0xFAu) {    /* Start: the LFO from the downbeat */
                    clk_ticks = 0;
                    clk_since = 0;
                    lfo_phase = 0;
                }
                continue;
            }
            st = e & 0xF0u;
            d1 = (e >> 8) & 0x7Fu;
            d2 = (e >> 16) & 0x7Fu;
            if (st == 0x90u && d2) {
                if (held[d1] < 255u)
                    held[d1]++;
                morph_start();                      /* each key starts the morph */
                if (ork_set.arp_on)                 /* ARP: the key plays its phrase */
                    runs_start(d1, d2);
                else
                    note_on(d1, d2);
            } else if (st == 0x80u || st == 0x90u) {
                if (held[d1] && --held[d1] == 0u) {
                    if (ork_set.arp_on)             /* ARP: only the key that plays the phrase counts */
                        run_release(d1);
                    else
                        note_off(d1);
                }
            } else if (st == 0xB0u && d1 == 1u) {
                modwheel = (uint8_t)d2;
            } else if (st == 0xB0u && (d1 == 123u || d1 == 120u)) {
                for (d1 = 0; d1 < 128u; d1++)
                    held[d1] = 0;
                for (d1 = 0; d1 < NRUN; d1++)
                    run[d1].on = run[d1].nn = 0;
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

/* -------------------------------------------------------------- LFO --- */
static int32_t lfo_s, lfo_vib_q8, lfo_trem_q15;
/* sin(2 pi phase / 2^32), Q15, a parabola (within 6 %: an LFO's shape, not a tone) */
static inline __attribute__((always_inline)) int32_t lfo_sine(uint32_t phase)
{
    int32_t q = (int32_t)phase >> 16, a = q < 0 ? -q : q;
    return clampi((4 * q * (32768 - a)) >> 15, -32767, 32767);
}

/* ------------------------------------------------------------- swell --- */
/* 0.5 - 0.5 cos(pi i / 64), Q14, i = 0..64: the sustain's raised-cosine swell (gen_bank.py raised()) */
static FM1_AUDIO_DATA const uint16_t RC[65] = {
#include "orkest_rc.inc"
};
/* the sustain's level (Q14) where it has played `pos` source samples: 0 before w_t0, 1 from w_t1; below 64 the
 * transient knob gives it the start too (wk / 256 of the way to 1) */
static inline __attribute__((always_inline)) int32_t swell(const voice_t *v, uint32_t pos)
{
    int32_t w;
    if (pos >= v->w_t1)
        w = 16384;
    else if (pos <= v->w_t0)
        w = 0;
    else {
        uint32_t span = v->w_t1 - v->w_t0, x = ((pos - v->w_t0) << 12) / span;   /* Q12 of 64 (32-bit: the swell < 2^20) */
        uint32_t ix = x >> 6, fr = x & 63u;
        w = RC[ix] + (int32_t)(((int32_t)RC[ix + 1u > 64u ? 64u : ix + 1u] - RC[ix]) * (int32_t)fr >> 6);
    }
    if (v->wk != 255u)
        w = 16384 - (((16384 - w) * (int32_t)v->wk) >> 8);
    return w;
}
static inline __attribute__((always_inline)) uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
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
        for (k = 0; k < NRUN; k++)
            run[k].on = run[k].nn = 0;
        ork_stat.voices = 0;
        ork_stat.idle = 1;
        return 0;
    }
    phrases_sync();
    events();
    {   /* the LFO for this block: vibrato depth (cents, Q8) from the panel (when on) or the mod wheel, tremolo */
        int32_t vib = ork_set.lfo_on ? ork_set.lfo_vib : 0, trem = ork_set.lfo_on ? ork_set.lfo_trem : 0;
        if (modwheel > vib)
            vib = modwheel;
        lfo_vib_q8 = vib * 60 * 256 / 127;
        lfo_trem_q15 = trem * 29491 / 127;
        clk_total += FM1_BLOCK_FRAMES;
        clk_since += FM1_BLOCK_FRAMES;
        if (clk_age < 0xFFFFu)
            clk_age++;
        ork_stat.clock = clk_age < CLK_TIMEOUT && clk_len_q8;
        ork_stat.clock_bpm10 = (uint16_t)(ork_stat.clock ? 44100u * 600u / 24u * 256u / clk_len_q8 : 0u);
        if (ork_set.lfo_sync && ork_stat.clock && ork_set.lfo_ticks) {   /* locked to the clock's ticks */
            uint32_t cyc = ork_set.lfo_ticks, per = 0xFFFFFFFFu / cyc, len = clk_len_q8 >> 8;
            uint32_t since = clk_since > 65535u ? 65535u : clk_since;
            uint32_t frac = len ? (since << 16) / len : 0u;                /* into the current tick, Q16 */
            if (frac > 65535u)
                frac = 65535u;
            lfo_phase = (clk_ticks % cyc) * per + (uint32_t)(((uint64_t)frac * per) >> 16);
        } else
            lfo_phase += ork_set.lfo_inc;           /* free (Hz) or the internal tempo */
        lfo_s = lfo_sine(lfo_phase);
    }
    morph_block(ork_stat.clock && clk_len_q8 ? 2147483648u / clk_len_q8 : ork_set.arp_inc);
    phrases();
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &voice[k];
        uint32_t stt = v->t.step, stl = v->l.step, str = v->r.step, lend;
        int32_t g0 = 32767, g1 = 32767, tremolo = 0, w0, w1, r0 = 0, r1 = 0, tg = v->tg;
        int tail;
        if (v->stage == ST_OFF)
            continue;
        busy++;
        if (v->g_n) {                               /* a slide: from the old pitch to its own, straight */
            int32_t f = (int32_t)(((uint32_t)v->g_n << 16) / v->g_total);
            stt = (uint32_t)((int32_t)stt + (int32_t)(((int64_t)((int32_t)v->g_t - (int32_t)stt) * f) >> 16));
            stl = (uint32_t)((int32_t)stl + (int32_t)(((int64_t)((int32_t)v->g_l - (int32_t)stl) * f) >> 16));
            v->g_n--;
        }
        if (lfo_vib_q8 || lfo_trem_q15) {           /* this voice's share of the LFO, faded in after its note on */
            uint32_t amt = v->lfo_amt + ork_set.lfo_dstep;
            v->lfo_amt = (uint16_t)(amt > 65535u ? 65535u : amt);
            if (lfo_vib_q8) {                       /* pitch: 1 + cents ln2 / 1200, the first order (<= 1 cent off) */
                int32_t c8 = (int32_t)(((((int64_t)lfo_vib_q8 * lfo_s >> 15) * v->lfo_amt >> 16) * v->vf) >> 7);
                int32_t d16 = (int32_t)(((int64_t)c8 * 9691) >> 16);
                stt = (uint32_t)((int64_t)stt + (((int64_t)stt * d16) >> 16));
                stl = (uint32_t)((int64_t)stl + (((int64_t)stl * d16) >> 16));
                str = (uint32_t)((int64_t)str + (((int64_t)str * d16) >> 16));
            }
            if (lfo_trem_q15) {                     /* level: down by depth x (1 - sine) / 2, glided over the block */
                g0 = v->trem;
                g1 = 32767 - (int32_t)((((((int64_t)lfo_trem_q15 * ((32767 - lfo_s) >> 1)) >> 15) * v->lfo_amt >> 16) * v->tf) >> 7);
                v->trem = (uint16_t)g1;
                tremolo = 1;
            }
        }
        /* the sustain's level over this block: its swell (where it has got to), the transient knob's share */
        lend = v->lpos + (uint32_t)(((uint64_t)stl * FM1_BLOCK_FRAMES) >> 16);
        w1 = swell(v, lend);
        w0 = v->w_last == 0xFFFFu ? swell(v, v->lpos) : v->w_last;
        v->lpos = lend;
        v->w_last = (uint16_t)w1;
        /* the release over this block: equal power with the sustain's envelope (its level sqrt(1 - env^2)) */
        tail = v->stage == ST_RELEASE && v->r.on && !v->r.ended;
        if (tail) {
            uint32_t e0 = v->env >> 15, e1 = v->env > v->estep * FM1_BLOCK_FRAMES ? (v->env - v->estep * FM1_BLOCK_FRAMES) >> 15 : 0u;
            r0 = (int32_t)((v->rg * isqrt32(32767u * 32767u - (e0 > 32767u ? 32767u : e0) * (e0 > 32767u ? 32767u : e0))) >> 15);
            r1 = (int32_t)((v->rg * isqrt32(32767u * 32767u - (e1 > 32767u ? 32767u : e1) * (e1 > 32767u ? 32767u : e1))) >> 15);
        }
        for (i = 0; i < FM1_BLOCK_FRAMES; i++) {
            int32_t s = 0, w = w0 + (((w1 - w0) * (int32_t)i) >> 7);
            if (v->stage == ST_ATTACK) {
                if (ENV_MAX - v->env <= v->estep) {
                    v->env = ENV_MAX;
                    v->stage = ST_SUSTAIN;
                } else
                    v->env += v->estep;
            } else if (v->stage == ST_RELEASE)
                v->env = v->env <= v->estep ? 0u : v->env - v->estep;
            if (v->t.on && !v->t.ended)
                s = (stream_sample(&v->t, stt) * tg) >> 14;
            if (v->l.on)
                s += (stream_sample(&v->l, stl) * w) >> 14;
            s = (s * (int32_t)(v->env >> 15)) >> 15;
            if (tail)
                s += (stream_sample(&v->r, str) * (r0 + (((r1 - r0) * (int32_t)i) >> 7))) >> 14;
            if (tremolo)
                s = (s * (g0 + (((g1 - g0) * (int32_t)i) >> 7))) >> 15;
            if (v->lp_a < 32767u) {                 /* velocity -> tone */
                v->lp1 += (int32_t)(((int64_t)(s - v->lp1) * v->lp_a) >> 15);
                v->lp2 += (int32_t)(((int64_t)(v->lp1 - v->lp2) * v->lp_a) >> 15);
                s = v->lp2;
            }
            out[2u * i] += (s * v->gl) >> 16;       /* each voice at -6 dB: big chords reach the soft clip */
            out[2u * i + 1u] += (s * v->gr) >> 16;
        }
        if ((v->stage == ST_RELEASE && !v->env && (!v->r.on || v->r.ended)) ||
            (v->one && v->t.ended && !v->t.s0 && !v->t.s1))
            v->stage = ST_OFF;                       /* released (and its release played), or a one-shot ended */
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

#ifdef FM1_HOST_TEST
/* tests/orkest_test.c: the sum of a wave's first `samples` samples as a stream decodes it: which 0 a zone's transient,
 * 1 its sustain (from its loop start, looping), 2 an instrument's release (zone = the instrument) */
long long ork_test_decode(unsigned zone, unsigned which, unsigned samples)
{
    stream_t v;
    long long sum = 0;
    const ork_zone_t *z = &ork_bank.zone[zone];
    const ork_inst_t *in = &ork_bank.inst[zone % ORK_MAX_INST];
    if (which == 0)
        stream_start(&v, z->t_off, z->t_n, 0, 0, 0, 0, 65536);
    else if (which == 1)
        stream_start(&v, z->l_off, z->l_n, z->l_ls, z->l_n, z->l_pred, z->l_idx, 65536);
    else
        stream_start(&v, in->rel_off, in->rel_n, 0, 0, 0, 0, 65536);
    sum = v.s1;
    while (--samples)
        sum += adpcm_next(&v);
    return sum;
}
uint32_t ork_test_lfo_phase(void) { return lfo_phase; }
unsigned ork_test_gliding(void)   /* voices in a slide */
{
    unsigned i, n = 0;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage != ST_OFF && voice[i].g_n)
            n++;
    return n;
}
#endif
