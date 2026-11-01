#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fetch Orkest's orchestral samples from VSCO 2 Community Edition (CC0) into assets/samples-cc0/.

The excerpts are already in the tree; this re-creates them from the source:
  https://github.com/sgossner/VSCO-2-CE  (Versilian Studios, CC0 1.0)

For each instrument (INSTRUMENTS below) one recording per zone is taken, only its first KEEP seconds,
mixed to mono and resampled to the instrument's rate, and stored as 16-bit WAV:
  assets/samples-cc0/<INSTRUMENT>/<NN>_<original file name>.wav   (spaces in the name: _)
plus ATTRIBUTION.txt (every file and where it came from). tools/gen_orkest.py turns them into the
firmware's sample data at build time (loops, IMA ADPCM).

  tools/orkest_fetch.py            fetch what is missing
  tools/orkest_fetch.py --force    fetch everything again
"""
import datetime
import re
import struct
import sys
import urllib.parse
import urllib.request
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "assets" / "samples-cc0"
REPO = "sgossner/VSCO-2-CE"
RAW = f"https://raw.githubusercontent.com/{REPO}/master/"
TREE = f"https://api.github.com/repos/{REPO}/git/trees/master?recursive=1"

# name: (rate, keep s, [(folder, file regex), ...])   one file per regex, first match in sorted order
S, W, B, P = "Strings/", "Woodwinds/", "Brass/", "Percussion/"
INSTRUMENTS = {
    # strings: sections and solo
    "VIOLINS":    (22050, 1.5, [(S + "Violin Section/susVib", r"_A2_v2\."), (S + "Violin Section/susVib", r"_C4_v2\."),
                                (S + "Violin Section/susVib", r"_D5_v2\.")]),
    "VIOLAS":     (22050, 1.5, [(S + "Viola Section/susvib", r"_C2_v2_1"), (S + "Viola Section/susvib", r"_D3_v2_1"),
                                (S + "Viola Section/susvib", r"_E4_v2_1")]),
    "CELLI":      (22050, 1.5, [(S + "Cello Section/susvib", r"^susvib_C1_v3_1"), (S + "Cello Section/susvib", r"^susvib_D2_v3_1"),
                                (S + "Cello Section/susvib", r"^susvib_C3_v3_1")]),
    "BASSES":     (16000, 1.5, [(S + "Solo Contrabass/SusVib", r"_A#0_v3_rr1"), (S + "Solo Contrabass/SusVib", r"_A1_v3_rr1")]),
    "SOLO VIOLIN": (22050, 1.5, [(S + "Solo Violin/Arco Vib", r"_G3_f"), (S + "Solo Violin/Arco Vib", r"_E4_f"),
                                 (S + "Solo Violin/Arco Vib", r"_C5_f"), (S + "Solo Violin/Arco Vib", r"_A5_f")]),
    "TREMOLO":    (22050, 1.5, [(S + "Violin Section/Trem", r"_A2_v2\."), (S + "Violin Section/Trem", r"_C4_v2\."),
                                (S + "Violin Section/Trem", r"_D5_v2\.")]),
    "PIZZICATO":  (22050, 0.8, [(S + "Cello Section/pizzT", r"_C1_v2_RR1"), (S + "Cello Section/pizzT", r"_D2_v2_RR1"),
                                (S + "Violin Section/Pizz", r"_A2_v2_rr1"), (S + "Violin Section/Pizz", r"_A3_v2_rr1"),
                                (S + "Violin Section/Pizz", r"_B4_v2_rr1")]),
    "SPICCATO":   (22050, 0.5, [(S + "Violin Section/Spic", r"_A2_v2_rr1"), (S + "Violin Section/Spic", r"_C4_v2_rr1"),
                                (S + "Violin Section/Spic", r"_D5_v2_rr1")]),
    "HARP":       (22050, 1.0, [(S + "Harp", r"_D2_mf"), (S + "Harp", r"_C3_mf"), (S + "Harp", r"_D4_mf"),
                                (S + "Harp", r"_C5_mf")]),
    # woodwinds
    "FLUTE":      (22050, 1.5, [(W + "Flute/susvib", r"_C3_v1_1"), (W + "Flute/susvib", r"_A3_v1_1"),
                                (W + "Flute/susvib", r"_A4_v1_1")]),
    "OBOE":       (22050, 1.5, [(W + "Oboe/Vib", r"_D3_v3_"), (W + "Oboe/Vib", r"_A#3_v3_"), (W + "Oboe/Vib", r"_F4_v3_")]),
    "CLARINET":   (22050, 1.5, [(W + "Clarinet/susLong", r"_D2_v2_rr1"), (W + "Clarinet/susLong", r"_D3_v2_rr1"),
                                (W + "Clarinet/susLong", r"_D4_v2_rr1")]),
    "BASSOON":    (16000, 1.5, [(W + "Bassoon/vib", r"_A1_v2_1"), (W + "Bassoon/vib", r"_G2_v2_1"),
                                (W + "Bassoon/vib", r"_E3_v2_1")]),
    # brass
    "HORN":       (22050, 1.5, [(B + "F Horn/sus", r"_G1_v2_1"), (B + "F Horn/sus", r"_F2_v2_1"),
                                (B + "F Horn/sus", r"_C3_v2_1")]),
    "TRUMPET":    (22050, 1.5, [(B + "Trumpet/susvib", r"_F2_v2_rr1"), (B + "Trumpet/susvib", r"_F3_v2_rr1"),
                                (B + "Trumpet/susvib", r"_F4_v2_rr1")]),
    "TROMBONE":   (22050, 1.5, [(B + "Tenor Trombone/sus", r"_F1_v2_1"), (B + "Tenor Trombone/sus", r"_D2_v2_1"),
                                (B + "Tenor Trombone/sus", r"_C3_v2_1")]),
    "TUBA":       (16000, 1.5, [(B + "Tuba/sus", r"_F0_v1_rr1"), (B + "Tuba/sus", r"_F1_v2_rr1"),
                                (B + "Tuba/sus", r"_F2_v2_rr1")]),
    # percussion
    "TIMPANI":    (16000, 1.4, [(P + "Timpani", r"^Timpani1_Hit_v3_rr1"), (P + "Timpani", r"^Timpani3_Hit_v3_rr1"),
                                (P + "Timpani", r"^Timpani5_Hit_v3_rr1")]),
    "GLOCKENSPIEL": (22050, 0.9, [(P + "Glock", r"_G4"), (P + "Glock", r"_G5"), (P + "Glock", r"_G6")]),
}


def read_wav_head(data):
    """(rate, float mono samples) from the first bytes of a WAV (the data chunk may be cut off)"""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt = 12, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            tag, ch, rate, _, align, bits = struct.unpack_from("<HHIIHH", data, body)
            if tag == 0xFFFE:                                   # WAVE_FORMAT_EXTENSIBLE: the sub-format's tag
                tag = struct.unpack_from("<H", data, body + 24)[0]
            fmt = (tag, ch, rate, align, bits)
        elif cid == b"data":
            if not fmt:
                raise ValueError("data before fmt")
            tag, ch, rate, align, bits = fmt
            raw = data[body:body + size]
            raw = raw[:len(raw) // align * align]
            if tag == 3 and bits == 32:
                x = np.frombuffer(raw, "<f4").astype(np.float64)
            elif tag == 1 and bits == 16:
                x = np.frombuffer(raw, "<i2") / 32768.0
            elif tag == 1 and bits == 24:
                b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
                v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
                x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
            elif tag == 1 and bits == 32:
                x = np.frombuffer(raw, "<i4") / 2147483648.0
            else:
                raise ValueError(f"unsupported WAV format {tag}/{bits} bit")
            return rate, x.reshape(-1, ch).mean(axis=1)
        pos = body + size + (size & 1)
    raise ValueError("no data chunk")


def write_wav16(path, rate, x):
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2").tobytes()
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
                     struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm)


def get(url, nbytes=None):
    req = urllib.request.Request(url, headers={"Range": f"bytes=0-{nbytes - 1}"} if nbytes else {})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def tree():
    """every WAV path in the repository: the GitHub API, else a blob-less shallow clone (names only)"""
    import json
    import subprocess
    import tempfile
    try:
        paths = [e["path"] for e in json.loads(get(TREE))["tree"]]
    except Exception:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout",
                            f"https://github.com/{REPO}", tmp], check=True)
            paths = subprocess.run(["git", "-C", tmp, "ls-tree", "-r", "--name-only", "HEAD"], check=True,
                                   capture_output=True, text=True).stdout.splitlines()
    return [p for p in paths if p.lower().endswith(".wav")]


def main(force):
    files = tree()
    lines = []
    for name, (rate, keep, picks) in INSTRUMENTS.items():
        d = DEST / name.replace(" ", "_")
        d.mkdir(parents=True, exist_ok=True)
        for k, (folder, rx) in enumerate(picks):
            cand = sorted(f for f in files if f.rsplit("/", 1)[0] == folder and re.search(rx, f.rsplit("/", 1)[1]))
            if not cand:
                raise SystemExit(f"{name}: nothing matches {rx} in {folder}")
            src = cand[0]
            out = d / f"{k:02d}_{src.rsplit('/', 1)[1]}"
            lines.append(f"{d.name}/{out.name}  <-  {REPO}: {src}")
            if out.exists() and not force:
                continue
            data = get(RAW + urllib.parse.quote(src), int(keep * 96000 * 2 * 4) + 4096)   # enough for 96k 32-bit stereo
            sr, x = read_wav_head(data)
            x = x[:int(keep * sr)]
            g = np.gcd(sr, rate)
            y = resample_poly(x, rate // g, sr // g)
            write_wav16(out, rate, y)
            print(f"{out.relative_to(ROOT)}: {len(y) / rate:.2f} s at {rate} Hz (from {sr} Hz)")
    (DEST / "ATTRIBUTION.txt").write_text(
        "Orkest - sample sources\n"
        f"Retrieved: {datetime.date.today().isoformat()} (the first seconds of each file, mono, resampled)\n"
        "Licence: CC0 1.0 Universal (public domain dedication)\n"
        "  https://creativecommons.org/publicdomain/zero/1.0/\n"
        "Source: Versilian Studios (Sam Gossner), VSCO-2 Community Edition\n"
        f"  https://github.com/{REPO}  (repository licence: CC0-1.0)\n\n" + "\n".join(lines) + "\n")


if __name__ == "__main__":
    main("--force" in sys.argv[1:])
