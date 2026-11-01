#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Orkest's sound set: the CC0 excerpts in assets/samples-cc0/ (tools/orkest_fetch.py) -> build/gen/orkest_samples.h

Every recording becomes one zone: its attack (the transient, played once) and, for sustained
instruments, a crossfaded loop (the sustain, repeated while the key is held); one-shot instruments
(pizzicato, harp, timpani ...) play to the end of a faded excerpt. Stored as IMA ADPCM (4 bit) at the
instrument's rate, with the decoder state at the loop start so a loop restarts exactly.

Patches are what KNOB 1 selects: an instrument on its own, or up to MAXL instruments layered
(each with its own key range, transposition and level), e.g. STRINGS = violins + celli an octave
down, MARCATO = the spiccato transient over the violins' sustain.

  gen_orkest.py OUT.h            the header (tools/build.py)
  gen_orkest.py --wav DIR        also write every zone as decoded from the ADPCM (to listen to the loops)
"""
import hashlib
import math
import re
import struct
import sys
from pathlib import Path

import numpy as np

SRC = Path(__file__).resolve().parents[1]
CC0 = SRC / "assets" / "samples-cc0"
FS = 44100                      # the output rate (firmware/src/core.h FS)
BUDGET = 420 * 1024             # sample data limit: the app image (XIP) holds code + samples in 568 KB
MAXL = 3                        # layers per patch (firmware/src/synth.c)

NOTE = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}

# instrument: (folder, kind, attack s, loop s, gain dB)
#   kind "sus": attack + loop;  "one": one-shot, `attack` = its length (faded at the end), loop unused
INSTR = {
    "VIOLINS":      ("VIOLINS", "sus", 0.30, 0.40, 0),
    "VIOLAS":       ("VIOLAS", "sus", 0.30, 0.40, 0),
    "CELLI":        ("CELLI", "sus", 0.30, 0.40, 0),
    "BASSES":       ("BASSES", "sus", 0.30, 0.45, 0),
    "SOLO VIOLIN":  ("SOLO_VIOLIN", "sus", 0.30, 0.40, -1),
    "TREMOLO":      ("TREMOLO", "sus", 0.25, 0.35, -1),
    "PIZZICATO":    ("PIZZICATO", "one", 0.42, 0, -1),
    "SPICCATO":     ("SPICCATO", "one", 0.38, 0, -1),
    "HARP":         ("HARP", "one", 0.62, 0, -2),
    "FLUTE":        ("FLUTE", "sus", 0.30, 0.40, -1),
    "OBOE":         ("OBOE", "sus", 0.25, 0.40, -2),
    "CLARINET":     ("CLARINET", "sus", 0.30, 0.40, -1),
    "BASSOON":      ("BASSOON", "sus", 0.30, 0.40, -1),
    "HORN":         ("HORN", "sus", 0.30, 0.40, -1),
    "TRUMPET":      ("TRUMPET", "sus", 0.25, 0.40, -3),
    "TROMBONE":     ("TROMBONE", "sus", 0.25, 0.40, -2),
    "TUBA":         ("TUBA", "sus", 0.30, 0.45, -1),
    "TIMPANI":      ("TIMPANI", "one", 0.80, 0, 0),
    "GLOCKENSPIEL": ("GLOCKENSPIEL", "one", 0.45, 0, -4),
}
# measured roots (MIDI) where the file names have no note: the timpani's principal tone (spectral peak with
# its ~1.5x partial), per file
ROOTS = {"TIMPANI": [41.4, 49.5, 54.5]}

# patches, in KNOB 1 order: (name, attack 0..127, release 0..127, [(instrument, lo, hi, transpose, gain dB[, pan]), ...])
# pan -64 (left) .. +63 (right): where the section sits on stage (violins left, celli and basses right)
# an instrument's own patch is (name, ..., [(name, 0, 127, 0, 0)])
FULL = (0, 127)
PATCHES = [
    ("STRINGS",      8, 50, [("VIOLINS", *FULL, 0, 0, -24), ("CELLI", *FULL, -12, -3, 24)]),
    ("VIOLINS",      6, 45, [("VIOLINS", *FULL, 0, 0)]),
    ("VIOLAS",       6, 45, [("VIOLAS", *FULL, 0, 0)]),
    ("CELLI",        6, 45, [("CELLI", *FULL, 0, 0)]),
    ("BASSES",       6, 40, [("BASSES", *FULL, 0, 0)]),
    ("SOLO VIOLIN",  4, 40, [("SOLO VIOLIN", *FULL, 0, 0)]),
    ("MARCATO",      0, 40, [("SPICCATO", *FULL, 0, -2), ("VIOLINS", *FULL, 0, -2)]),
    ("TREMOLO",      4, 40, [("TREMOLO", *FULL, 0, 0)]),
    ("PIZZICATO",    0, 30, [("PIZZICATO", *FULL, 0, 0)]),
    ("SPICCATO",     0, 25, [("SPICCATO", *FULL, 0, 0)]),
    ("HARP",         0, 60, [("HARP", *FULL, 0, 0)]),
    ("WOODWINDS",    4, 35, [("FLUTE", 65, 127, 0, -2, -16), ("CLARINET", *FULL, 0, -2, 0), ("BASSOON", *FULL, -12, -3, 16)]),
    ("FLUTE",        2, 35, [("FLUTE", *FULL, 0, 0)]),
    ("OBOE",         2, 35, [("OBOE", *FULL, 0, 0)]),
    ("CLARINET",     2, 35, [("CLARINET", *FULL, 0, 0)]),
    ("BASSOON",      2, 35, [("BASSOON", *FULL, 0, 0)]),
    ("BRASS",        2, 40, [("TRUMPET", *FULL, 0, -3, 8), ("HORN", *FULL, -12, -2, -24), ("TROMBONE", *FULL, -12, -3, 28)]),
    ("HORN",         2, 40, [("HORN", *FULL, 0, 0)]),
    ("TRUMPET",      2, 35, [("TRUMPET", *FULL, 0, 0)]),
    ("TROMBONE",     2, 35, [("TROMBONE", *FULL, 0, 0)]),
    ("TUBA",         2, 35, [("TUBA", *FULL, 0, 0)]),
    ("TUTTI",        8, 50, [("VIOLINS", *FULL, 0, -3, -28), ("HORN", *FULL, -12, -4, -6), ("CELLI", *FULL, -12, -4, 28)]),
    ("TIMPANI",      0, 70, [("TIMPANI", *FULL, 0, 0)]),
    ("GLOCKENSPIEL", 0, 70, [("GLOCKENSPIEL", *FULL, 0, 0)]),
]

IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
            107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
            876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
            4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
            22385, 24623, 27086, 29794, 32767]
IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


# ------------------------------------------------------------------ WAV, pitch --
def read_wav(path):
    """16-bit mono WAV (as orkest_fetch.py writes) -> (rate, float samples)"""
    d = path.read_bytes()
    pos, rate = 12, None
    while pos + 8 <= len(d):
        cid, size = d[pos:pos + 4], struct.unpack_from("<I", d, pos + 4)[0]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack_from("<HHIIHH", d, pos + 8)
            if (tag, ch, bits) != (1, 1, 16):
                raise SystemExit(f"{path}: expected 16-bit mono PCM")
        elif cid == b"data":
            return rate, np.frombuffer(d[pos + 8:pos + 8 + size], "<i2") / 32768.0
        pos += 8 + size + (size & 1)
    raise SystemExit(f"{path}: no data")


def write_wav(path, rate, x):
    pcm = np.clip(np.round(np.asarray(x)), -32768, 32767).astype("<i2").tobytes()
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
                     struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm)


def yin_midi(x, sr, fmin=25.0, fmax=2500.0):
    """the pitch (MIDI, fractional) of a steady stretch: YIN's cumulative-mean-normalised difference"""
    w = min(len(x) // 2, int(sr * 0.08))
    tmin, tmax = int(sr / fmax), min(int(sr / fmin), len(x) - w - 1)
    d = np.array([np.sum((x[:w] - x[t:t + w]) ** 2) for t in range(tmax + 1)])
    c = np.concatenate([[1.0], d[1:] * np.arange(1, len(d)) / np.maximum(np.cumsum(d[1:]), 1e-12)])
    t = next((t for t in range(max(tmin, 1), len(c) - 1) if c[t] < 0.15 and c[t] <= c[t + 1]), None)
    if t is None:
        t = tmin + int(np.argmin(c[tmin:]))
    a, b, e = c[t - 1], c[t], c[t + 1]
    den = a - 2 * b + e
    hz = sr / (t + (0.5 * (a - e) / den if den else 0.0))
    return 69 + 12 * math.log2(hz / 440.0)


def onset(x):
    env = np.abs(x)
    return int(np.argmax(env > 0.04 * env.max()))


def name_note(name):
    """'_C#3' in a file name (after the NN_ prefix) -> octave * 12 + pitch class"""
    m = re.search(r"_([A-G]#?)(-?\d)", name[3:])
    return int(m.group(2)) * 12 + NOTE[m.group(1)] if m else None


def roots(inst, files):
    """MIDI roots of an instrument's files: the names' notes in the octave convention most files' pitch agrees
    with (VSCO mixes C3 = 60 and C4 = 60), or the measured ROOTS"""
    if inst in ROOTS:
        return ROOTS[inst]
    named = [name_note(p.name) for p in files]
    det = []
    for p in files:
        sr, x = read_wav(p)
        o = onset(x)
        det.append(yin_midi(x[o + int(0.3 * sr):o + int(0.6 * sr)], sr))
    votes = {off: sum(abs(d - (n + off)) < 0.6 for d, n in zip(det, named)) for off in (12, 24)}
    off = max(votes, key=votes.get)
    return [n + off for n in named]


# ------------------------------------------------------------------ loops --
def find_loop(x, sr, ls_min, length, xf):
    """loop [ls, le) of about `length` s from about ls_min: both ends searched (+-40 ms) for the best waveform match
    of what precedes them (normalised correlation over the crossfade window), so the crossfade blends similar
    waveforms. A coarse grid first, then refined around the best"""
    span, n = int(0.04 * sr), len(x)

    def score(ls, le):
        a, b = x[ls - xf:ls], x[le - xf:le]
        return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12))

    le0 = min(ls_min + int(length * sr), n - 1 - span)
    grid = [(score(ls, le), ls, le) for ls in range(max(xf, ls_min - span), ls_min + span, 8)
            for le in range(le0 - span, le0 + span, 8)]
    c, ls, le = max(grid)
    fine = [(score(a, b), a, b) for a in range(ls - 8, ls + 9) for b in range(le - 8, min(le + 9, n))]
    c, ls, le = max(fine)
    return ls, le, c


def level_loop(x, ls, le, sr):
    """a slow gain ramp over the loop so its end is as loud as its start (a swelling or fading recording would
    otherwise jump back in level at every repeat): RMS of 60 ms before each end, ratio limited to +-6 dB"""
    w = int(0.06 * sr)
    a = math.sqrt(np.mean(x[ls - w:ls] ** 2))
    b = math.sqrt(np.mean(x[le - w:le] ** 2))
    r = min(2.0, max(0.5, a / max(b, 1e-9)))
    y = x.copy()
    y[ls:le] *= np.linspace(1.0, r, le - ls)
    y[le:] *= r
    return y


def crossfade(x, ls, le, xf, corr):
    """blend the end of the loop into what precedes its start: playing le-xf..le fades from the loop's end to
    ls-xf..ls, so the jump le -> ls continues seamlessly. Equal-power for weakly correlated material"""
    y = x.copy()
    a = (np.arange(xf) + 0.5) / xf
    if corr > 0.7:
        g_out, g_in = 1 - a, a
    else:
        g_out, g_in = np.cos(a * math.pi / 2), np.sin(a * math.pi / 2)
    y[le - xf:le] = x[le - xf:le] * g_out + x[ls - xf:ls] * g_in
    return y


# ------------------------------------------------------------------ IMA ADPCM --
def ima_encode(s, mark):
    """int samples -> (bytes, (pred, idx) before sample `mark`): two samples a byte, the even one low; the encoder
    follows the decoder's arithmetic exactly (firmware/src/synth.c adpcm_next)"""
    out = bytearray((len(s) + 1) // 2)
    pred, idx, st = 0, 0, (0, 0)
    for n, v in enumerate(s):
        if n == mark:
            st = (pred, idx)
        step = IMA_STEP[idx]
        diff = int(v) - pred
        code = 8 if diff < 0 else 0
        diff = abs(diff)
        vd = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            vd += step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
            vd += step >> 1
        if diff >= step >> 2:
            code |= 1
            vd += step >> 2
        pred = max(-32768, min(32767, pred - vd if code & 8 else pred + vd))
        idx = max(0, min(88, idx + IMA_IDX[code & 7]))
        out[n >> 1] |= code << (4 * (n & 1))
    if mark >= len(s):
        st = (pred, idx)
    return bytes(out), st


def ima_decode(data, n, ls=None, le=None, st=None, total=None):
    """decode n samples (or `total`, looping ls..le with the stored state) as the firmware does"""
    out, pred, idx, pos = [], 0, 0, 0
    for _ in range(total or n):
        code = (data[pos >> 1] >> (4 * (pos & 1))) & 15
        step = IMA_STEP[idx]
        vd = step >> 3
        if code & 4:
            vd += step
        if code & 2:
            vd += step >> 1
        if code & 1:
            vd += step >> 2
        pred = max(-32768, min(32767, pred - vd if code & 8 else pred + vd))
        idx = max(0, min(88, idx + IMA_IDX[code & 7]))
        out.append(pred)
        pos += 1
        if le is not None and pos == le:
            pos, (pred, idx) = ls, st
        elif pos >= n:
            break
    return np.array(out, dtype=np.float64)


# ------------------------------------------------------------------ build --
def zone(inst, kind, p, root, attack, loop):
    """one recording -> (int16 samples, ls, le, looped) at its file's rate, levelled"""
    sr, x = read_wav(p)
    x = x[max(0, onset(x) - int(0.002 * sr)):]
    x = x - np.mean(x[:int(0.5 * sr)])                          # no DC (it would click at the loop)
    if kind == "sus":
        xf = int(0.10 * sr)
        ls, le, corr = find_loop(x, sr, int(attack * sr), loop, xf)
        x = crossfade(level_loop(x, ls, le, sr), ls, le, xf, corr)[:le]
        rms = math.sqrt(np.mean(x[ls:le] ** 2))                 # level: the sustain's loudness
        looped = True
    else:
        n = min(len(x), int(attack * sr))
        x = x[:n].copy()
        fade = int(min(0.25, attack / 3) * sr)
        x[n - fade:] *= np.cos(np.linspace(0, math.pi / 2, fade)) ** 2
        rms = math.sqrt(np.mean(x[:int(0.15 * sr)] ** 2))     # level: the attack
        ls = le = n
        looped = False
    g = min(7000.0 / max(rms, 1e-9), 30000.0 / max(np.max(np.abs(x)), 1e-9))
    return np.round(x * g).astype(np.int32), ls, le, looped, sr


def key_split(rts):
    """key ranges for zones of the given roots (sorted): split half-way between neighbours"""
    out = []
    for i, r in enumerate(rts):
        lo = 0 if i == 0 else int(math.floor((rts[i - 1] + r) / 2)) + 1
        hi = 127 if i == len(rts) - 1 else int(math.floor((r + rts[i + 1]) / 2))
        out.append((lo, hi))
    return out


def build(wavdir=None):
    blob, zones, insts = bytearray(), [], []
    names = list(INSTR)
    for inst in names:
        folder, kind, attack, loop, gdb = INSTR[inst]
        files = sorted((CC0 / folder).glob("*.wav"))
        if not files:
            raise SystemExit(f"{inst}: no samples in {CC0 / folder} (tools/orkest_fetch.py)")
        rts = roots(inst, files)
        order = sorted(range(len(files)), key=lambda i: rts[i])
        z0 = len(zones)
        for (lo, hi), i in zip(key_split([rts[i] for i in order]), order):
            s, ls, le, looped, sr = zone(inst, kind, files[i], rts[i], attack, loop)
            data, st = ima_encode(s, ls)
            off = len(blob)
            blob += data
            if len(blob) & 1:
                blob.append(0)
            zones.append(dict(off=off, n=len(s), ls=ls, le=le, rate=round(sr / FS * 65536), root16=round(rts[i] * 16),
                              pred=st[0], idx=st[1], lo=lo, hi=hi, looped=looped, file=files[i].name))
            if wavdir:
                y = ima_decode(data, len(s), ls, le, st, total=len(s) + (3 * (le - ls) if looped else 0))
                write_wav(Path(wavdir) / f"{inst.replace(' ', '_')}_{rts[i]:.0f}.wav", sr, y)
        insts.append(dict(name=inst, z0=z0, nz=len(zones) - z0, one=kind == "one", gain=gdb))
    if len(blob) > BUDGET:
        raise SystemExit(f"gen_orkest: {len(blob)} B of sample data, more than the {BUDGET} B budget")
    return blob, zones, insts


def q8(db):
    return int(round(256 * 10 ** (db / 20)))


def header(blob, zones, insts):
    idx = {d["name"]: i for i, d in enumerate(insts)}
    L = ["/* generated by tools/gen_orkest.py from assets/samples-cc0 (VSCO 2 CE, CC0): do not edit */",
         "#pragma once", "#include <stdint.h>", "",
         f"#define ORK_DATA_BYTES {len(blob)}",
         f"static const uint8_t ORK_DATA[{len(blob)}] = {{"]
    for i in range(0, len(blob), 40):
        L.append("    " + ",".join(map(str, blob[i:i + 40])) + ",")
    L.append("};")
    L.append("/* off, n, ls, le (sample index), rate (/ 44100, Q16), root (1/16 semitone), ADPCM state at ls, keys lo..hi,"
             " looped */")
    L.append(f"#define ORK_NZONES {len(zones)}")
    L.append("static const ork_zone_t ORK_ZONES[ORK_NZONES] = {")
    for z in zones:
        L.append(f"    {{{z['off']}, {z['n']}, {z['ls']}, {z['le']}, {z['rate']}, {z['root16']}, {z['pred']}, {z['idx']}, "
                 f"{z['lo']}, {z['hi']}, {1 if z['looped'] else 0}}},   /* {z['file']} */")
    L.append("};")
    L.append("#ifdef ORK_HOST_TEST   /* tests/basis_test.c: the sum of each zone as tools/gen_orkest.py decodes it, its")
    L.append("                        * n samples and, for a loop, one more pass of it (the loop's restart state) */")
    L.append("static const int64_t ORK_CHECK[ORK_NZONES] = {")
    for z in zones:
        tot = z["n"] + (z["le"] - z["ls"] if z["looped"] else 0)
        d = ima_decode(blob[z["off"]:], z["n"], z["ls"], z["le"], (z["pred"], z["idx"]), total=tot) if z["looped"] \
            else ima_decode(blob[z["off"]:], z["n"])
        L.append(f"    {int(d.sum())}LL,")
    L.append("};")
    L.append("#endif")
    L.append(f"#define ORK_NINST {len(insts)}")
    L.append("/* name, first zone, zones, one-shot (ignores note off), level (Q8) */")
    L.append("static const ork_inst_t ORK_INST[ORK_NINST] = {")
    for d in insts:
        L.append(f'    {{"{d["name"]}", {d["z0"]}, {d["nz"]}, {1 if d["one"] else 0}, {q8(d["gain"])}}},')
    L.append("};")
    L.append("/* 2^(i/192), Q30: pitch ratios in 1/16 semitones within an octave */")
    L.append("static const uint32_t ORK_POW2[192] = {")
    p2 = [round(2 ** (i / 192) * (1 << 30)) for i in range(192)]
    for i in range(0, 192, 8):
        L.append("    " + ", ".join(f"{v}u" for v in p2[i:i + 8]) + ",")
    L.append("};")
    L.append(f"#define ORK_NPATCH {len(PATCHES)}")
    L.append(f"#define ORK_MAXL {MAXL}")
    L.append("/* name, attack, release, layers, {instrument, keys lo..hi, transpose, level (Q8), pan} */")
    L.append("static const ork_patch_t ORK_PATCH[ORK_NPATCH] = {")
    for name, a, r, layers in PATCHES:
        if not 1 <= len(layers) <= MAXL:
            raise SystemExit(f"patch {name}: 1..{MAXL} layers")
        ls = ", ".join(f"{{{idx[l[0]]}, {l[1]}, {l[2]}, {l[3]}, {q8(l[4])}, {l[5] if len(l) > 5 else 0}}}" for l in layers)
        L.append(f'    {{"{name}", {a}, {r}, {len(layers)}, {{{ls}}}}},')
    L.append("};")
    return "\n".join(L) + "\n"


def main(argv):
    wavdir = None
    if "--wav" in argv:
        i = argv.index("--wav")
        wavdir = argv[i + 1]
        del argv[i:i + 2]
        Path(wavdir).mkdir(parents=True, exist_ok=True)
    out = Path(argv[0])
    out.parent.mkdir(parents=True, exist_ok=True)
    cache = out.with_suffix(".cache")             # the header under a hash of this script and every sample
    h = hashlib.sha256(Path(__file__).read_bytes() + repr(sys.version_info[:2]).encode() + np.__version__.encode())
    for p in sorted(CC0.glob("*/*.wav")):
        h.update(str(p.relative_to(CC0)).encode() + b"\0" + hashlib.sha256(p.read_bytes()).digest())
    if not wavdir:
        try:
            key, summary, text = cache.read_text().split("\n", 2)
            if key == h.hexdigest():
                out.write_text(text)
                print(summary + " (cached)")
                return
        except (OSError, ValueError):
            pass
    blob, zones, insts = build(wavdir)
    text = header(blob, zones, insts)
    out.write_text(text)
    secs = sum(z["n"] / (z["rate"] / 65536 * FS) for z in zones)
    summary = (f"orkest: {len(insts)} instruments, {len(PATCHES)} patches, {len(zones)} zones, "
               f"{secs:.1f} s of audio, {len(blob)} B ADPCM (budget {BUDGET} B)")
    cache.write_text(f"{h.hexdigest()}\n{summary}\n{text}")
    print(summary)


if __name__ == "__main__":
    main(sys.argv[1:])
