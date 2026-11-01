/* SPDX-License-Identifier: GPL-3.0-only */
/* Basis synth: NVOICE voices of one oscillator (SINE, TRI, SAW, SQUARE) with a linear attack / release
 * envelope. Fixed point throughout (Q15 samples, 32-bit phase); runs in the audio ISR only.
 * It is a placeholder that proves the keys, the audio path and MIDI work: replace it with your own engine.
 *
 *   synth_note_on(note, vel)    start a note (MIDI note number, velocity 1..127)
 *   synth_note_off(note)        release every voice playing that note
 *   synth_all_off()             release all; synth_kill() silence at once
 *   synth_render(out, n)        n stereo frames, Q15, interleaved L R (added to nothing: written) */
enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_COUNT };
static const char *const WAVE_NAME[W_COUNT] = {"SINE", "TRI", "SAW", "SQUARE"};

static const int16_t SINE256[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
    25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
    28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
    15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
    -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
    -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
    -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
    -3212, -2410, -1608, -804,
};

/* phase increment (2^32 per cycle) of notes 120..131; a lower octave is a right shift */
static const uint32_t NOTE_INC_TOP[12] = {
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u,
};
static uint32_t note_inc(uint32_t note)
{
    if (note > 127u)
        note = 127u;
    return NOTE_INC_TOP[note % 12u] >> (10u - note / 12u);
}

#define ENV_MAX (1u << 30)
enum { ST_OFF, ST_ATTACK, ST_SUSTAIN, ST_RELEASE };
typedef struct {
    uint32_t phase, inc;
    uint32_t env, step;          /* 0..ENV_MAX; the attack's or the release's step per frame */
    uint32_t age;                /* started at (voice clock): the oldest is stolen first */
    uint16_t amp;                /* velocity, Q15 */
    uint8_t note, stage;
} voice_t;
static voice_t voice[NVOICE];
static uint32_t voice_clock;

/* 0..127 -> frames for a full rise / fall: ~1 ms .. ~3 s, squared for finer control at the short end */
static uint32_t env_step(uint32_t v)
{
    uint32_t ms = 1u + (v * v * 3000u) / (127u * 127u);
    return ENV_MAX / (ms * (FS / 1000u));
}

static void synth_note_on(uint32_t note, uint32_t vel)
{
    uint32_t i;
    voice_t *v = 0;
    for (i = 0; i < NVOICE && !v; i++)              /* the same note again: retrigger its voice */
        if (voice[i].stage != ST_OFF && voice[i].note == note)
            v = &voice[i];
    for (i = 0; i < NVOICE && !v; i++)              /* a free voice */
        if (voice[i].stage == ST_OFF)
            v = &voice[i];
    if (!v) {                                       /* steal: the oldest released voice, else the oldest */
        voice_t *rel = 0, *old = &voice[0];
        for (i = 0; i < NVOICE; i++) {
            if (voice[i].stage == ST_RELEASE && (!rel || voice[i].age < rel->age))
                rel = &voice[i];
            if (voice[i].age < old->age)
                old = &voice[i];
        }
        v = rel ? rel : old;
        v->env = 0;                                 /* (a click is better than a stuck note; add a fade if you mind) */
    }
    if (v->stage == ST_OFF) {
        v->phase = 0;
        v->env = 0;
    }
    v->note = (uint8_t)note;
    v->inc = note_inc(note);
    v->amp = (uint16_t)(vel * 258u);                /* 127 -> 32766 */
    v->step = env_step(sys.attack);
    v->stage = ST_ATTACK;
    v->age = ++voice_clock;
    sys.last_note = (uint8_t)note;
    sys.last_vel = (uint8_t)vel;
}

static void synth_note_off(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].note == note && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)) {
            voice[i].stage = ST_RELEASE;
            voice[i].step = env_step(sys.release);
        }
}

static void synth_all_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN) {
            voice[i].stage = ST_RELEASE;
            voice[i].step = env_step(sys.release);
        }
}

static void synth_kill(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        voice[i].stage = ST_OFF;
}

/* PolyBLEP: the band-limiting correction at a jump, t = phase and dt = increment (Q16 of a cycle) */
static int32_t blep(uint32_t t, uint32_t dt)
{
    int32_t x;
    if (!dt)
        return 0;
    if (t < dt) {
        x = (int32_t)((t << 15) / dt);                       /* 0..32768 */
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (t > 65536u - dt) {
        x = (int32_t)(((65536u - t) << 15) / dt);
        return -(x + x - ((x * x) >> 15) - 32768);
    }
    return 0;
}

static int32_t osc(const voice_t *v, uint32_t wave)          /* one sample, Q15 */
{
    uint32_t p = v->phase, t = p >> 16, dt = v->inc >> 16;
    switch (wave) {
    case W_SINE: {
        int32_t a = SINE256[p >> 24], b = SINE256[((p >> 24) + 1u) & 255u];
        return a + (((b - a) * (int32_t)((p >> 8) & 0xFFFFu)) >> 16);
    }
    case W_TRI: {
        int32_t s = (int32_t)(t >> 0) * 2 - 65536;           /* -65536..65534 */
        return (s < 0 ? -s : s) - 32768;
    }
    case W_SAW:
        return (int32_t)t - 32768 - blep(t, dt);
    default:
        return (t < 32768u ? 32767 : -32768) + blep(t, dt) - blep((t + 32768u) & 0xFFFFu, dt);
    }
}

static int32_t soft_clip(int32_t x)              /* Q15 in +-1.5 -> +-1: x - 4/27 x^3 */
{
    int64_t c;
    x = clamp(x, -49151, 49151);
    c = ((int64_t)x * x >> 15) * x >> 15;
    return x - (int32_t)(c * 4 / 27);
}

static void synth_render(int32_t *out, uint32_t n)
{
    uint32_t i, k, wave = sys.wave % W_COUNT, busy = 0;
    int32_t m = (int32_t)sys.master_q12;
    for (i = 0; i < n; i++)
        out[2u * i] = 0;
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &voice[k];
        if (v->stage == ST_OFF)
            continue;
        busy++;
        for (i = 0; i < n; i++) {
            switch (v->stage) {
            case ST_ATTACK:
                if (ENV_MAX - v->env <= v->step) {
                    v->env = ENV_MAX;
                    v->stage = ST_SUSTAIN;
                } else
                    v->env += v->step;
                break;
            case ST_RELEASE:
                if (v->env <= v->step) {
                    v->env = 0;
                    v->stage = ST_OFF;
                } else
                    v->env -= v->step;
                break;
            }
            {
                int32_t s = osc(v, wave), g = (int32_t)(((v->env >> 15) * v->amp) >> 15);   /* Q15 */
                out[2u * i] += (s * g >> 15) >> 2;          /* 4 voices at full level reach full scale */
            }
            v->phase += v->inc;
            if (v->stage == ST_OFF)
                break;
        }
    }
    for (i = 0; i < n; i++) {
        int32_t s = clamp(soft_clip(out[2u * i]) * m >> 12, -32767, 32767);
        out[2u * i] = out[2u * i + 1u] = s;
    }
    sys.voices = (uint8_t)busy;
}
