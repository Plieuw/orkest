#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""The ARP button's phrases: short figures (at most two bars) after public-domain composers, as note names here,
written to app/orkest_phrases.inc as events relative to a reference note: the key that is pressed plays the
reference, the phrase moves with it (a phrase in C pressed on A plays in A).

Notation: a voice is a string of tokens NOTE:DUR, `r:DUR` a rest, `A+B+C:DUR` a chord. DUR: 1 2 4 8 16 32, a dot
for dotted, `t` for a triplet (8t). A phrase may have several voices, each from the start.

    python3 tools/gen_phrases.py [app/orkest_phrases.inc]
"""
import re
import sys
from pathlib import Path

TPQ = 24  # ticks a quarter note (MIDI clock)

# name (<= 13 characters, the big line), what it is from (<= 20), reference note, voices
PHRASES = [
    ("PRELUDE C", "BACH BWV 846", "C4", [
        "C4:16 E4:16 G4:16 C5:16 E5:16 G4:16 C5:16 E5:16 " * 2 +
        "C4:16 D4:16 A4:16 D5:16 F5:16 A4:16 D5:16 F5:16 " * 2]),
    ("CELLO SUITE", "BACH BWV 1007", "G3", [
        "G2:16 D3:16 B3:16 A3:16 B3:16 D3:16 B3:16 D3:16 " * 2 +
        "G2:16 E3:16 C4:16 B3:16 C4:16 E3:16 C4:16 E3:16 " * 2]),
    ("INVENTION 1", "BACH BWV 772", "C4", [
        "r:16 C4:16 D4:16 E4:16 F4:16 D4:16 E4:16 C4:16 G4:8 C5:8 B4:8 C5:8"]),
    ("TOCCATA", "BACH BWV 565", "D5", [
        "A5:32 G5:32 A5:2 r:8 G5:32 F5:32 E5:32 D5:32 C#5:4 D5:2"]),
    ("BADINERIE", "BACH BWV 1067", "B4", [
        "B5:8 D6:16 B5:16 F#5:8 B5:16 F#5:16 D5:8 F#5:16 D5:16 B4:4"]),
    ("JESU JOY", "BACH BWV 147", "G4", [
        "G4:8 A4:8 B4:8 D5:8 C5:8 C5:8 E5:8 D5:8 D5:8 G5:8 F#5:8 G5:8 D5:8 B4:8 G4:8 A4:8 B4:8 C5:8"]),
    ("MENUET G", "BACH ANH 114", "G4", [
        "D5:4 G4:8 A4:8 B4:8 C5:8 D5:4 G4:4 G4:4"]),
    ("CANON D", "PACHELBEL", "D3", [
        "D3:4 A2:4 B2:4 F#2:4 G2:4 D2:4 G2:4 A2:4",
        "F#4+A4+D5:4 E4+A4+C#5:4 D4+F#4+B4:4 C#4+F#4+A4:4 B3+D4+G4:4 A3+D4+F#4:4 B3+D4+G4:4 C#4+E4+A4:4"]),
    ("PRIMAVERA", "VIVALDI RV 269", "E4", [
        "E4:8 G#4:8 G#4:8 G#4:8 F#4:16 E4:16 B4:4. B4:16 A4:16 G#4:8 G#4:8 G#4:8 F#4:16 E4:16 B4:4."]),
    ("HALLELUJAH", "HANDEL HWV 56", "D4", [
        "D5:4 A4:8. B4:16 A4:4 r:4 D5:4 A4:8. B4:16 A4:4 r:4"]),
    ("NACHTMUSIK", "MOZART K 525", "G4", [
        "G3+G4:4 r:8 D3+D4:8 G3+G4:4 r:8 D3+D4:8 G3+G4:8 D3+D4:8 G3+G4:8 B3+B4:8 D4+D5:4 r:4"]),
    ("SYMPHONY 40", "MOZART K 550", "G4", [
        "Eb5:8 D5:8 D5:4 Eb5:8 D5:8 D5:4 Eb5:8 D5:8 D5:4 Bb5:4 r:4"]),
    ("ALLA TURCA", "MOZART K 331", "A4", [
        "B4:16 A4:16 G#4:16 A4:16 C5:8 r:8 D5:16 C5:16 B4:16 C5:16 E5:8 r:8 F5:16 E5:16 D#5:16 E5:16 "
        "B5:16 A5:16 G#5:16 A5:16 B5:16 A5:16 G#5:16 A5:16 C6:4"]),
    ("FUR ELISE", "BEETHOVEN WOO 59", "A4", [
        "E5:16 D#5:16 E5:16 D#5:16 E5:16 B4:16 D5:16 C5:16 A4:8 r:16 C4:16 E4:16 A4:16 "
        "B4:8 r:16 E4:16 G#4:16 B4:16 C5:8"]),
    ("FIFTH", "BEETHOVEN OP 67", "C4", [
        "r:8 G3+G4:8 G3+G4:8 G3+G4:8 Eb3+Eb4:2 r:8 F3+F4:8 F3+F4:8 F3+F4:8 D3+D4:2"]),
    ("ODE TO JOY", "BEETHOVEN OP 125", "C4", [
        "E4:4 E4:4 F4:4 G4:4 G4:4 F4:4 E4:4 D4:4"]),
    ("FUNERAL MARCH", "CHOPIN OP 35", "Bb3", [
        "Bb3:4 Bb3:8. Bb3:16 Bb3:2 Db4:8. C4:16 C4:8. Bb3:16 Bb3:8. A3:16 Bb3:4",
        "Bb1+F2:4 Gb2+Db3:4 Bb1+F2:4 Gb2+Db3:4 Bb1+F2:4 Gb2+Db3:4 Bb1+F2:4 Gb2+Db3:4"]),
    ("PRELUDE 20", "CHOPIN OP 28", "C4", [
        "C3+G3+C4+Eb4:4 F2+Ab3+C4+F4:4 G2+B3+D4+F4:8. G2+B3+D4+G4:16 C3+G3+C4+Eb4:4 "
        "Ab2+Ab3+C4+Eb4:4 Db3+Ab3+Db4+F4:4 Eb3+G3+Db4+Eb4:8. Eb3+G3+Bb3+Db4:16 Ab2+Ab3+C4+Eb4:4"]),
    ("GYMNOPEDIE", "SATIE 1888", "G3", [
        "G2:4 B3+D4+F#4:2 D2:4 A3+C#4+F#4:2"]),
]

NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(n):
    m = re.fullmatch(r"([A-G])(#|b)?(-?\d)", n)
    if not m:
        raise SystemExit(f"bad note {n!r}")
    return 12 * (int(m[3]) + 1) + NAMES[m[1]] + {"#": 1, "b": -1, None: 0}[m[2]]


def ticks(d):
    m = re.fullmatch(r"(1|2|4|8|16|32)(\.|t)?", d)
    if not m:
        raise SystemExit(f"bad duration {d!r}")
    t = TPQ * 4 // int(m[1])
    return t * 3 // 2 if m[2] == "." else t * 2 // 3 if m[2] == "t" else t


def phrase(ref, voices):
    """-> (events [(at, len, interval)], length in ticks)"""
    r, ev, length = midi(ref), [], 0
    for v in voices:
        at = 0
        for tok in v.split():
            notes, dur = tok.rsplit(":", 1)
            d = ticks(dur)
            if notes != "r":
                for n in notes.split("+"):
                    ev.append((at, d, midi(n) - r))
            at += d
        length = max(length, at)
    ev.sort()
    return ev, length


def main(argv):
    out = Path(argv[1] if len(argv) > 1 else Path(__file__).resolve().parent.parent / "app" / "orkest_phrases.inc")
    evs, heads = [], []
    for name, what, ref, voices in PHRASES:
        assert len(name) <= 13 and len(what) <= 20, name
        ev, length = phrase(ref, voices)
        assert length <= 2 * 4 * TPQ + 24 or name == "JESU JOY", (name, length)   # two bars (and a pickup)
        assert all(0 < d <= 255 and -48 <= iv <= 48 for _, d, iv in ev), name
        heads.append((len(evs), len(ev), length))
        evs += ev
    lines = ["/* generated by v14/tools/gen_phrases.py: the ARP button's phrases (ticks: 24 a quarter note) */",
             f"#define ORK_NPHRASE {len(PHRASES)}u",
             "#ifdef ORK_PHRASE_EVENTS",
             f"static FM1_AUDIO_DATA const ork_pev_t PEV[{len(evs)}] = {{"]
    for i in range(0, len(evs), 8):
        lines.append("    " + " ".join(f"{{{a}, {d}, {iv}}}," for a, d, iv in evs[i:i + 8]))
    lines += ["};", f"static FM1_AUDIO_DATA const ork_phrase_t PHRASE[ORK_NPHRASE] = {{"]
    lines += [f"    {{{f}, {n}, {l}}},   /* {nm} */" for (f, n, l), (nm, *_r) in zip(heads, PHRASES)]
    lines += ["};", "#endif", "#ifdef ORK_PHRASE_NAMES",
              "static const char *const PHRASE_NAME[ORK_NPHRASE][2] = {"]
    lines += [f'    {{"{nm}", "{what}"}},' for nm, what, *_r in PHRASES]
    lines += ["};", "#endif"]
    out.write_text("\n".join(lines) + "\n")
    print(f"phrases: {len(PHRASES)}, {len(evs)} events -> {out}")


if __name__ == "__main__":
    main(sys.argv)
