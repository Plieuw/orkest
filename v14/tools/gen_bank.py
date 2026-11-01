#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Orkest's sound bank for FM1 Open (format 2): every note in three independent parts, as the firmware plays them
(app/orkest_player.c):

  TRANSIENT  what the recording has on top of its sustain while it starts: the recording minus the sustain loop
             as it swells in (bow, tonguing, breath, the first bite). Played once.
  SUSTAIN    the steady loop (tools/gen_orkest.py: resynthesised from the recording's harmonics, or for tremolo its
             own crossfaded loop), rotated so that it is in phase with the recording from the note's first sample;
             it swells in on a raised cosine over the first w_t1 samples.
  RELEASE    one per sustained instrument: the end of a whole recording where the player lets go, at the held
             level (assets/samples-cc0/<INSTRUMENT>/release/). Played from note off while the sustain hands over.

  transient + swelling sustain = the recording (checked here, every zone), so with the firmware's TRANSIENT and
  TAIL at their default the notes sound as recorded; apart they can be turned up, down, or borrowed: a patch layer
  may take its transients from another instrument (MARCATO: the spiccato's bite on the violins' sustain).

Layout (little-endian; offsets from the start of the bank):
  0    header, 64 B   magic 'ORKB', version 2, header bytes, total bytes, CRC-32 of [64, total), commit word
                      (0xFFFFFFFF in the file; the firmware writes 'CMIT' after a complete upload), counts, table
                      offsets, name
  ..   zones          40 B: transient (offset, samples), sustain (offset, samples, loop start), sustain ADPCM state
                      at the loop start (predictor, index), keys lo, hi, flags (1: has a sustain), 0, rate (/44100,
                      Q16), root (1/16 semitone), swell start, swell end (samples)
  ..   instruments    36 B: name[14], first zone, zones, one-shot, has a release, level (Q8), release level (Q8),
                      release root (1/16 semitone), release (offset, samples, rate /44100 Q16)
  ..   patches        44 B: name[14], attack, release, layers, 0, 0, 0,
                      3 x {instrument, keys lo, hi, transpose, level (Q8, 2 B), pan, transients from (255: its own)}
  ..   data           IMA ADPCM (two samples a byte, the even one low; each wave on an even byte)

  gen_bank.py OUT.bin [--header OUT.h] [--wav DIR]
"""
import math
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
import gen_orkest as g  # noqa: E402

MAGIC, VERSION, HDR = 0x424B524F, 2, 64
ZONE, INST, PATCH = 40, 36, 44
LIMIT = 520 * 1024              # the user partition (524 KiB) less the settings sector
PREROLL = 32                    # the loop's own end before its start: the ADPCM decoder settled at the loop start
TARGET = 7000.0                 # the sustain's RMS (int16)

# V14's patches: as gen_orkest.py's, but MARCATO is one voice: the violins' sustain with the spiccato's transient
PATCHES = [(n, a, r, [("VIOLINS", 0, 127, 0, 0, 0, "SPICCATO")]) if n == "MARCATO" else (n, a, r, l)
           for n, a, r, l in g.PATCHES]


def raised(n, t0, t1):
    """the sustain's swell: 0 before t0, a raised cosine to 1 at t1, 1 after (orkest_player.c swell())"""
    t = np.arange(n, dtype=np.float64)
    u = np.clip((t - t0) / max(t1 - t0, 1), 0.0, 1.0)
    return 0.5 - 0.5 * np.cos(np.pi * u)


def split(inst, kind, path, root, attack, loop_s):
    """one recording -> dict(transient, sustain (rotated, preroll), ls_rel, w_t1, sr, check) or a one-shot"""
    sr, x = g.read_wav(path)
    x = x[max(0, g.onset(x) - int(0.002 * sr)):]
    x = x - np.mean(x[:int(0.5 * sr)])
    if kind == "one":
        s, ls, le, looped, sr = g.zone(inst, kind, path, root, attack, loop_s)
        return dict(transient=s.astype(np.float64), sustain=None, sr=sr, w_t0=0, w_t1=0)
    ls = int(attack * sr)
    if kind in ("harm", "ens"):
        seg = g.steady_part(x, sr, ls)
        f0 = 440.0 * 2 ** ((g.yin_midi(seg[:int(0.4 * sr)], sr) - 69) / 12)
        if abs(1200 * math.log2(f0 / (440.0 * 2 ** ((root - 69) / 12)))) > 60:
            f0 = 440.0 * 2 ** ((root - 69) / 12)
        spread = (lambda k: min(0.6, 0.08 * (k - 1))) if kind == "ens" else None
        S, _ = g.harm_loop(x, sr, ls, f0, loop_s, g.RNG_SEED + int(root * 16) + len(inst) * 7, spread)
    else:                                       # "sus": the recording's own loop, crossfaded into what precedes it
        xf = int(0.10 * sr)
        ls, le, corr = g.find_loop(x, sr, ls, loop_s, xf)
        y = g.crossfade(g.level_loop(x, ls, le, sr), ls, le, xf, corr)
        S = y[ls:le].copy()
    L = len(S)
    xf = min(int(0.03 * sr), ls // 4)
    srms = math.sqrt(np.mean(S ** 2))
    # the swell: over the 100 ms before the transient fades, where the sustain is in phase with the recording (its
    # phases are the recording's at ls): the transient stays the attack itself, not the sustain's negative
    t1 = ls - xf
    t0 = max(0, t1 - int(0.10 * sr))
    # the sustain as it plays from the note's first sample: in phase with the recording at ls
    ext = S[(np.arange(ls) - ls) % L]
    T = x[:ls] - raised(ls, t0, t1) * ext
    T[ls - xf:] *= np.linspace(1.0, 0.0, xf)    # into the sustain alone: a linear crossfade (in phase)
    gain = min(TARGET / max(srms, 1e-9), 30000.0 / max(np.max(np.abs(x[:ls])), np.max(np.abs(S)), 1e-9))
    rot = np.roll(S, ls % L)                    # rot[0] = the sustain at the note's first sample
    return dict(transient=T * gain, sustain=rot * gain, sr=sr, w_t0=t0, w_t1=t1, ls=ls, xf=xf,
                target=x[:ls + L] * gain, L=L)


def release(inst_folder, root_of, sr_want):
    """the instrument's release: (samples at its rate, root) or None; its held part levelled to TARGET"""
    d = g.CC0 / inst_folder / "release"
    files = sorted(d.glob("*.wav")) if d.exists() else []
    if not files:
        return None
    sr, x = g.read_wav(files[0])
    w = int(0.02 * sr)
    held = math.sqrt(np.mean(x[:int(0.25 * sr)] ** 2))
    db = np.array([20 * math.log10(math.sqrt(np.mean(x[i:i + w] ** 2)) + 1e-9) for i in range(0, len(x) - w, w)])
    steady = np.median(db[:int(0.25 * sr) // w])
    last = max([i for i in range(len(db)) if db[i] > steady - 3] or [0])   # where the held level ends
    a = max(0, last * w - int(0.02 * sr))
    r = x[a:] * (TARGET / max(held, 1e-9))
    fade = min(int(0.05 * sr), len(r) // 3)
    r[len(r) - fade:] *= np.linspace(1.0, 0.0, fade)
    name = files[0].name
    root = root_of.get(name)
    return (r, root, sr) if root is not None else None


class Blob:
    def __init__(self):
        self.b = bytearray()

    def add(self, samples, mark=0):
        s = np.clip(np.round(samples), -32768, 32767).astype(np.int32)
        data, st = g.ima_encode(s, mark)
        off = len(self.b)
        self.b += data
        if len(self.b) & 1:
            self.b.append(0)
        return off, len(s), st


def build(wavdir=None):
    blob, zones, insts, worst = Blob(), [], [], []
    for inst, (folder, kind, attack, loop_s, gdb) in g.INSTR.items():
        files = sorted((g.CC0 / folder).glob("*.wav"))
        rts = g.roots(inst, files)
        order = sorted(range(len(files)), key=lambda i: rts[i])
        z0 = len(zones)
        root_of = {files[i].name[3:]: rts[i] for i in range(len(files))}
        for (lo, hi), i in zip(g.key_split([rts[i] for i in order]), order):
            z = split(inst, kind, files[i], rts[i], attack, loop_s)
            t_off, t_n, _ = blob.add(z["transient"])
            rec = dict(t_off=t_off, t_n=t_n, l_off=0, l_n=0, l_ls=0, pred=0, idx=0, lo=lo, hi=hi,
                       rate=round(z["sr"] / g.FS * 65536), root16=round(rts[i] * 16), w_t0=z["w_t0"], w_t1=z["w_t1"],
                       file=files[i].name)
            if z["sustain"] is not None:
                S = z["sustain"]
                l_off, l_n, st = blob.add(np.concatenate([S[-PREROLL:], S]), PREROLL)
                rec.update(l_off=l_off, l_n=l_n, l_ls=PREROLL, pred=st[0], idx=st[1])
                # the check: transient + swelling sustain, both as the firmware decodes them = the recording
                tdec = g.ima_decode(blob.b[t_off:], t_n)
                ldec = g.ima_decode(blob.b[l_off:], l_n, PREROLL, l_n, st, total=l_n + z["ls"] + z["L"])[PREROLL:]
                n = z["ls"] + z["L"]
                out = np.concatenate([tdec, np.zeros(n - len(tdec))]) + raised(n, z["w_t0"], z["w_t1"]) * ldec[:n]
                ref = z["target"][:z["ls"] - z["xf"]]
                got = out[:len(ref)]
                snr = 10 * math.log10(np.sum(ref ** 2) / max(np.sum((ref - got) ** 2), 1e-9))
                own = np.clip(np.round(ref), -32768, 32767).astype(np.int32)   # the recording itself as ADPCM
                d, _ = g.ima_encode(own, 0)
                own_snr = 10 * math.log10(np.sum(ref ** 2) / max(np.sum((ref - g.ima_decode(d, len(own))) ** 2), 1e-9))
                worst.append((snr - own_snr, inst, rts[i], snr))
                if wavdir:
                    g.write_wav(Path(wavdir) / f"{inst.replace(' ', '_')}_{rts[i]:.0f}_transient.wav", z["sr"], tdec)
                    g.write_wav(Path(wavdir) / f"{inst.replace(' ', '_')}_{rts[i]:.0f}_sustain.wav", z["sr"], ldec[:2 * z["L"]])
                    g.write_wav(Path(wavdir) / f"{inst.replace(' ', '_')}_{rts[i]:.0f}_together.wav", z["sr"], out)
            zones.append(rec)
        rel = release(folder, root_of, None) if kind != "one" else None
        d = dict(name=inst, z0=z0, nz=len(zones) - z0, one=kind == "one", gain=gdb, rel=None)
        if rel:
            r, rroot, rsr = rel
            off, n, _ = blob.add(r)
            d["rel"] = dict(off=off, n=n, rate=round(rsr / g.FS * 65536), root16=round(rroot * 16))
            if wavdir:
                g.write_wav(Path(wavdir) / f"{inst.replace(' ', '_')}_release.wav", rsr, g.ima_decode(blob.b[off:], n))
        insts.append(d)
    return bytes(blob.b), zones, insts, worst


def name14(s):
    b = s.encode("ascii")[:13]
    return b + bytes(14 - len(b))


def bank(wavdir=None):
    blob, zones, insts, worst = build(wavdir)
    idx = {d["name"]: i for i, d in enumerate(insts)}
    zt = b"".join(struct.pack("<5Ih4B2xIhHI", z["t_off"], z["t_n"], z["l_off"], z["l_n"], z["l_ls"], z["pred"], z["idx"],
                              z["lo"], z["hi"], 1 if z["l_n"] else 0, z["rate"], z["root16"], z["w_t0"], z["w_t1"])
                  for z in zones)
    it = b""
    for d in insts:
        r = d["rel"] or dict(off=0, n=0, rate=0, root16=0)
        it += name14(d["name"]) + struct.pack("<4BHHhIII", d["z0"], d["nz"], 1 if d["one"] else 0, 1 if d["rel"] else 0,
                                               g.q8(d["gain"]), 256, r["root16"], r["off"], r["n"], r["rate"])
    pt = b""
    for name, a, rl, layers in PATCHES:
        ls = b""
        for l in layers + [None] * (g.MAXL - len(layers)):
            if l:
                pan = l[5] if len(l) > 5 else 0
                tin = idx[l[6]] if len(l) > 6 else 255
                ls += struct.pack("<3BbHbB", idx[l[0]], l[1], l[2], l[3], g.q8(l[4]), pan, tin)
            else:
                ls += bytes(8)
        pt += name14(name) + struct.pack("<6B", a, rl, len(layers), 0, 0, 0) + ls
    assert len(zt) == ZONE * len(zones) and len(it) == INST * len(insts) and len(pt) == PATCH * len(PATCHES)
    zones_off = HDR
    inst_off = zones_off + len(zt)
    patch_off = inst_off + len(it)
    data_off = (patch_off + len(pt) + 3) & ~3
    body = zt + it + pt + bytes(data_off - patch_off - len(pt)) + blob
    total = HDR + len(body)
    if total > LIMIT:
        raise SystemExit(f"gen_bank: {total} B, more than the {LIMIT} B the flash holds")
    crc = zlib.crc32(body) & 0xFFFFFFFF
    hdr = struct.pack("<IHHIII4H5I", MAGIC, VERSION, HDR, total, crc, 0xFFFFFFFF, len(zones), len(insts),
                      len(PATCHES), g.MAXL, zones_off, inst_off, patch_off, data_off, len(blob))
    hdr = (hdr + name14("ORKEST 2")[:16].ljust(16, b"\0")).ljust(HDR, b"\0")
    return hdr + body, zones, insts, worst


def main(argv):
    out = Path(argv[0])
    wavdir = argv[argv.index("--wav") + 1] if "--wav" in argv else None
    if wavdir:
        Path(wavdir).mkdir(parents=True, exist_ok=True)
    data, zones, insts, worst = bank(wavdir)
    worst.sort()
    if worst[0][0] < -8:                        # within 8 dB of the recording's own ADPCM
        raise SystemExit(f"gen_bank: transient + sustain differ from the recording ({worst[0][1]} {worst[0][2]:.0f}: "
                         f"{worst[0][3]:.1f} dB, {-worst[0][0]:.1f} dB under its own ADPCM)")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    if "--header" in argv:
        h = Path(argv[argv.index("--header") + 1])
        lines = ["/* generated by v14/tools/gen_bank.py: the bank as the host tests load it */",
                 f"static const unsigned char ORK_BANK_FILE[{len(data)}] __attribute__((aligned(4))) = {{"]
        for i in range(0, len(data), 40):
            lines.append("    " + ",".join(map(str, data[i:i + 40])) + ",")
        lines.append("};")
        # per wave: the sum of what gen_orkest.py decodes (a sustain: from its loop start, its length and one pass more)
        body = data
        doff = struct.unpack_from("<IHHIII4H5I", data, 0)[13]
        sums = []
        for z in zones:
            sums.append(int(g.ima_decode(body[doff + z["t_off"]:], z["t_n"]).sum()))
            if z["l_n"]:
                L = z["l_n"] - z["l_ls"]
                sums.append(int(g.ima_decode(body[doff + z["l_off"]:], z["l_n"], z["l_ls"], z["l_n"], (z["pred"], z["idx"]),
                                             total=z["l_n"] + L)[z["l_ls"]:].sum()))
            else:
                sums.append(0)
        rs = [int(g.ima_decode(body[doff + d["rel"]["off"]:], d["rel"]["n"]).sum()) if d["rel"] else 0 for d in insts]
        lines.append(f"static const long long ORK_BANK_CHECK[{len(sums)}] = {{" + ",".join(f"{v}LL" for v in sums) + "};")
        lines.append(f"static const long long ORK_BANK_RCHECK[{len(rs)}] = {{" + ",".join(f"{v}LL" for v in rs) + "};")
        h.write_text("\n".join(lines) + "\n")
    print(f"bank: {len(data)} B ({len(zones)} zones, {len(insts)} instruments, {sum(1 for d in insts if d['rel'])} releases, "
          f"{len(PATCHES)} patches); transient + sustain = the recording within {-worst[0][0]:.1f} dB of its own ADPCM "
          f"(worst: {worst[0][1]} {worst[0][2]:.0f}, {worst[0][3]:.1f} dB) -> {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
