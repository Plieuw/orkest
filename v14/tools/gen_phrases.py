#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""The ARP button's phrases: short figures (at most two bars) after public-domain composers, as note names here,
written to app/orkest_phrases.inc as events relative to a reference note: the key that is pressed plays the
reference, the phrase moves with it (a phrase in C pressed on A plays in A).

Notation: a line of tokens NOTE:DUR, `r:DUR` a rest. DUR: 1 2 4 8 16 32, a dot for dotted, `t` for a triplet (8t).
One note at a time: the phrases are monophonic.

Velocity: every note gets one from where it falls (the bar's first beat strongest, then beats, eighths, sixteenths,
the shortest notes softest) and from its line (a note above its neighbours a little stronger); after the duration, `>`
accents a note, `_` softens it, `@NN` sets it (100 = the key's own velocity). The player scales the key's velocity by
it.

    python3 tools/gen_phrases.py [app/orkest_phrases.inc]
"""
import re
import sys
from pathlib import Path

TPQ = 24  # ticks a quarter note (MIDI clock)

# name (<= 13 characters, the big line), what it is from (<= 20), reference note, the line (one voice: monophonic,
# at most six beats; most are one bar or less)
PHRASES = [
    ("PRELUDE C", "BACH BWV 846", "C4", "C4:16 E4:16 G4:16 C5:16 E5:16 G4:16 C5:16 E5:16"),
    ("CELLO SUITE", "BACH BWV 1007", "G3", "G2:16 D3:16 B3:16 A3:16 B3:16 D3:16 B3:16 D3:16"),
    ("INVENTION 1", "BACH BWV 772", "C4", "C4:16 D4:16 E4:16 F4:16 D4:16 E4:16 C4:16 G4:8"),
    ("TOCCATA", "BACH BWV 565", "D5", "A5:32 G5:32 A5:4. G5:32 F5:32 E5:32 D5:32 C#5:4 D5:4"),
    ("BADINERIE", "BACH BWV 1067", "B4", "B5:8 D6:16 B5:16 F#5:8 B5:16 F#5:16 D5:8 F#5:16 D5:16 B4:4"),
    ("JESU JOY", "BACH BWV 147", "G4", "G4:8 A4:8 B4:8 D5:8 C5:8 C5:8 E5:8 D5:8 D5:8"),
    ("MENUET G", "BACH ANH 114", "G4", "D5:4 G4:8 A4:8 B4:8 C5:8 D5:4"),
    ("FUGUE G MIN", "BACH BWV 578", "G4", "G4:8 D5:8 Bb4:8. A4:16 G4:16 Bb4:16 A4:16 G4:16 F#4:16 A4:16 D4:8"),
    ("CANON BASS", "PACHELBEL", "D3", "D3:8 A2:8 B2:8 F#2:8 G2:8 D2:8 G2:8 A2:8"),
    ("CANON", "PACHELBEL", "D5", "F#5:8 E5:8 D5:8 C#5:8 B4:8 A4:8 B4:8 C#5:8"),
    ("PRIMAVERA", "VIVALDI RV 269", "E4", "E4:8 G#4:8 G#4:8 G#4:8 F#4:16 E4:16 B4:4."),
    ("HALLELUJAH", "HANDEL HWV 56", "D5", "D5:4> A4:8. B4:16_ A4:4>"),
    ("SURPRISE", "HAYDN HOB I 94", "C4", "C4:8 C4:8 E4:8 E4:8 G4:8 G4:8 E4:4"),
    ("EMPEROR", "HAYDN HOB III 77", "G4", "G4:8. A4:16 B4:8 A4:8 C5:8 B4:8 A4:16 F#4:16 G4:8"),
    ("NACHTMUSIK", "MOZART K 525", "G4", "G4:4 r:8 D4:8 G4:4 r:8 D4:8"),
    ("NACHTMUSIK 2", "MOZART K 525", "G4", "G4:8 D4:8 G4:8 B4:8 D5:4"),
    ("SYMPHONY 40", "MOZART K 550", "G4", "Eb5:8 D5:8 D5:4 Eb5:8 D5:8 D5:4"),
    ("ALLA TURCA", "MOZART K 331", "A4", "B4:16 A4:16 G#4:16 A4:16 C5:8"),
    ("TWINKLE", "MOZART K 265", "C4", "C4:8 C4:8 G4:8 G4:8 A4:8 A4:8 G4:4"),
    ("SONATA C", "MOZART K 545", "C5", "C5:4 E5:8 G5:8 B4:8. C5:32 D5:32 C5:4"),
    ("FUR ELISE", "BEETHOVEN WOO 59", "A4", "E5:16 D#5:16 E5:16 D#5:16 E5:16 B4:16 D5:16 C5:16 A4:8"),
    ("FIFTH", "BEETHOVEN OP 67", "C4", "r:8 G4:8> G4:8> G4:8> Eb4:2>"),
    ("ODE TO JOY", "BEETHOVEN OP 125", "C4", "E4:8 E4:8 F4:8 G4:8 G4:8 F4:8 E4:8 D4:8"),
    ("MOONLIGHT", "BEETHOVEN OP 27 2", "C#4", "G#3:8t C#4:8t E4:8t G#3:8t C#4:8t E4:8t"),
    ("SEVENTH", "BEETHOVEN OP 92", "A3", "E4:4> E4:8_ E4:8_ E4:4 E4:4"),
    ("CAPRICE 24", "PAGANINI OP 1", "A4", "A4:8 A4:16 C5:16 B4:16 A4:16 E5:8"),
    ("WILLIAM TELL", "ROSSINI 1829", "C4", "G4:16_ G4:16_ G4:8> G4:16_ G4:16_ G4:8> G4:16_ G4:16_ C5:8> D5:8> E5:8>"),
    ("FUNERAL MARCH", "CHOPIN OP 35", "Bb3", "Db4:8. C4:16 C4:8. Bb3:16 Bb3:8. A3:16 Bb3:4"),
    ("DONNA MOBILE", "VERDI RIGOLETTO", "B3", "F#4:8 F#4:8 F#4:8 A4:8. G#4:16 E4:4"),
    ("VALKYRIES", "WAGNER WWV 86B", "B4", "F#4:16_ B4:8.> F#4:16_ B4:8.> D5:4> B4:4"),
    ("BLUE DANUBE", "STRAUSS OP 314", "D4", "D4:8_ D4:8_ F#4:8 A4:8> A4:4"),
    ("LULLABY", "BRAHMS OP 49 4", "C4", "E4:8 E4:8 G4:4. E4:8 E4:8 G4:4."),
    ("PROMENADE", "MUSSORGSKY 1874", "Bb4", "G4:4 F4:4 Bb4:4 C5:8 F5:8 D5:4"),
    ("HABANERA", "BIZET CARMEN", "D5", "D5:4 C#5:8 C5:8 C5:8 B4:8 Bb4:8 A4:8"),
    ("MORNING", "GRIEG OP 46", "E4", "B4:8 G#4:8 F#4:8 E4:8 F#4:8 G#4:8"),
    ("MOUNTAIN KING", "GRIEG OP 46", "B3", "B3:8 C#4:8 D4:8 E4:8 F#4:8 D4:8 F#4:4"),
    ("SWAN LAKE", "TCHAIKOVSKY OP 20", "B4", "F#5:4 B4:16 C#5:16 D5:16 E5:16 F#5:8. D5:16 F#5:8. D5:16"),
    ("NEW WORLD", "DVORAK OP 95", "C4", "E4:8. G4:16 G4:4 E4:8. D4:16 C4:4"),
    ("GYMNOPEDIE", "SATIE 1888", "D5", "F#5:8 A5:8 G5:8 F#5:8 C#5:8 B4:8 C#5:8 D5:8 A4:4"),
    # ornaments and patterns (from the key: C4 here)
    ("TRILL", "WHOLE STEP", "C4", "C4:32 D4:32 C4:32 D4:32 C4:32 D4:32 C4:32 D4:32"),
    ("TRILL HALF", "HALF STEP", "C4", "C4:32 Db4:32 C4:32 Db4:32 C4:32 Db4:32 C4:32 Db4:32"),
    ("TRILL TURN", "WITH AN ENDING", "C4", "D4:32@70 C4:32@66 D4:32@76 C4:32@70 D4:32@82 C4:32@76 D4:32@88 C4:32@82 B3:32@86 C4:32@88 D4:32@94 C4:32@92 E4:4@118"),
    ("TREMOLO 3RD", "MINOR THIRD", "C4", "C4:32 Eb4:32 C4:32 Eb4:32 C4:32 Eb4:32 C4:32 Eb4:32"),
    ("MORDENT", "ORNAMENT", "C4", "C4:32_ D4:32_ C4:8.> r:8"),
    ("LOW MORDENT", "ORNAMENT", "C4", "C4:32_ B3:32_ C4:8.> r:8"),
    ("TURN", "GRUPPETTO", "C4", "D4:16> C4:16_ B3:16_ C4:16 E4:4>"),
    ("GRACE NOTE", "ACCIACCATURA", "C4", "B3:32_ C4:8> r:16. D4:32_ E4:8> r:16."),
    ("SLIDE UP", "SCHLEIFER", "C4", "G3:32@60 A3:32@75 B3:32@90 C4:4> r:8."),
    ("ALBERTI", "BROKEN CHORD", "C4", "C4:16> G4:16_ E4:16 G4:16_"),
    ("ARP UP", "CHORD TONES", "C4", "C4:16 E4:16 G4:16 C5:16"),
    ("ARP DOWN", "CHORD TONES", "C4", "C5:16 G4:16 E4:16 C4:16"),
    ("ARP UP DOWN", "CHORD TONES", "C4", "C4:16 E4:16 G4:16 C5:16 G4:16 E4:16"),
    ("ARP 7TH", "MAJOR SEVENTH", "C4", "C4:16 E4:16 G4:16 B4:16 C5:16 B4:16 G4:16 E4:16"),
    ("TRIPLETS", "UP AND DOWN", "C4", "C4:8t E4:8t G4:8t C5:8t G4:8t E4:8t"),
    ("CASCADE", "FALLING", "C5", "C6:32@115 G5:32@105 E5:32@95 C5:32@88 G4:32@80 E4:32@74 C4:8@70 r:16"),
    ("OCTAVES", "BROKEN OCTAVE", "C4", "C4:16> C5:16_"),
    ("FIFTHS", "OPEN FIFTHS", "C4", "C4:16 G4:16 C5:16 G4:16"),
    ("PEDAL", "PEDAL POINT", "C4", "C5:16> C4:16_ D5:16 C4:16_ E5:16> C4:16_ D5:16 C4:16_"),
    ("RUN UP", "SCALE", "C4", "C4:32@60 D4:32@66 E4:32@72 F4:32@78 G4:32@84 A4:32@90 B4:32@96 C5:8> r:16."),
    ("RUN DOWN", "SCALE", "C5", "C5:32@110 B4:32@102 A4:32@96 G4:32@90 F4:32@84 E4:32@78 D4:32@72 C4:8@80 r:16."),
    ("CHROMATIC", "RUN UP", "C4", "C4:32@60 C#4:32@66 D4:32@72 D#4:32@78 E4:32@84 F4:32@90 F#4:32@96 G4:8> r:16."),
    ("THIRDS", "SEQUENCE", "C4", "C4:16 E4:16 D4:16 F4:16 E4:16 G4:16 F4:16 A4:16"),
    ("ZIGZAG", "SEQUENCE", "C4", "C4:16 D4:16 E4:16 C4:16 D4:16 E4:16 F4:16 D4:16"),
    ("GALLOP", "REPEATED", "C4", "C4:16_ C4:16_ C4:8> C4:16_ C4:16_ C4:8>"),
    ("STACCATO", "REPEATED", "C4", "C4:16> r:16 C4:16_ r:16 C4:16 C4:16> r:8"),
    ("CALL", "FANFARE", "C4", "G3:8t@70 G3:8t@76 G3:8t@82 C4:4@100 G3:8@90 C4:8@104 E4:4@120"),
    ("TRILL CRESC", "GROWING", "C4", "C4:32@50 D4:32@55 C4:32@60 D4:32@66 C4:32@72 D4:32@78 C4:32@84 D4:32@90 "
     "C4:32@96 D4:32@102 C4:32@108 D4:32@114 C4:8@124"),
    ("TRILL DIM", "FADING", "C4", "C4:32@120 D4:32@110 C4:32@100 D4:32@92 C4:32@84 D4:32@76 C4:32@68 D4:32@60 "
     "C4:32@54 D4:32@48 C4:32@42 D4:32@36"),
    ("SWELL", "REPEATED", "C4", "C4:16@50 C4:16@62 C4:16@74 C4:16@86 C4:16@98 C4:16@110 C4:16@122 C4:16@134"),
    ("OFFBEAT", "ACCENTS", "C4", "C4:16_ G4:16> C4:16_ G4:16> C4:16_ G4:16> C4:16_ G4:16>"),
    ("3 3 2", "ACCENTS", "C4", "C4:16> E4:16_ G4:16_ C4:16> E4:16_ G4:16_ C4:16> G4:16_"),
    ("ECHO", "OCTAVE DOWN", "C5", "C5:8 E5:8 G5:8> r:8 C4:8@55 E4:8@52 G4:8@58 r:8"),
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


def metric(at):
    """the velocity (100 = the key's) of a note starting `at` ticks into the phrase"""
    if at % (4 * TPQ) == 0:
        return 108
    if at % TPQ == 0:
        return 100
    if at % (TPQ // 2) == 0:
        return 92
    if at % (TPQ // 3) == 0:      # triplet eighths
        return 90
    if at % (TPQ // 4) == 0:
        return 86
    return 82


def phrase(ref, line):
    """-> (events [(at, len, interval, velocity)], length in ticks); one note at a time"""
    r, ev, at, marks = midi(ref), [], 0, []
    for tok in line.split():
        note, dur = tok.rsplit(":", 1)
        m = re.fullmatch(r"([0-9t.]+)(>|_)?(?:@(\d+))?", dur)
        if not m:
            raise SystemExit(f"bad token {tok!r}")
        d = ticks(m[1])
        if note != "r":
            if "+" in note:
                raise SystemExit(f"{tok}: the phrases are monophonic")
            ev.append([at, d, midi(note) - r, metric(at)])
            marks.append((m[2], m[3]))
        at += d
    for i, e in enumerate(ev):         # a peak in the line: a little stronger
        prev = ev[i - 1][2] if i else None
        nxt = ev[i + 1][2] if i + 1 < len(ev) else None
        if prev is not None and nxt is not None and e[2] > prev and e[2] > nxt:
            e[3] += 6
    for e, (acc, fixed) in zip(ev, marks):
        if fixed:
            e[3] = int(fixed)
        elif acc == ">":
            e[3] += 22
        elif acc == "_":
            e[3] -= 28
        e[3] = max(20, min(160, e[3]))
    return [tuple(e) for e in ev], at


def main(argv):
    out = Path(argv[1] if len(argv) > 1 else Path(__file__).resolve().parent.parent / "app" / "orkest_phrases.inc")
    evs, heads = [], []
    for name, what, ref, line in PHRASES:
        assert len(name) <= 13 and len(what) <= 20, name
        ev, length = phrase(ref, line)
        assert length <= 6 * TPQ, (name, length)   # six beats at most
        assert all(0 < d <= 255 and -48 <= iv <= 48 for _, d, iv, _v in ev), name
        heads.append((len(evs), len(ev), length))
        evs += ev
    lines = ["/* generated by v14/tools/gen_phrases.py: the ARP button's phrases (ticks: 24 a quarter note) */",
             f"#define ORK_NPHRASE {len(PHRASES)}u",
             "#ifdef ORK_PHRASE_EVENTS",
             f"static FM1_AUDIO_DATA const ork_pev_t PEV[{len(evs)}] = {{"]
    for i in range(0, len(evs), 8):
        lines.append("    " + " ".join(f"{{{a}, {d}, {iv}, {v}}}," for a, d, iv, v in evs[i:i + 8]))
    lines += ["};", f"static FM1_AUDIO_DATA const ork_phrase_t PHRASE[ORK_NPHRASE] = {{"]
    lines += [f"    {{{f}, {n}, {l}}},   /* {nm} */" for (f, n, l), (nm, *_r) in zip(heads, PHRASES)]
    lines += ["};", "#endif", "#ifdef ORK_PHRASE_NAMES",
              "static const char *const PHRASE_NAME[ORK_NPHRASE][2] = {"]
    lines += [f'    {{"{nm}", "{what}"}},' for nm, what, *_r in PHRASES]
    lines += ["};", "static const unsigned char PHRASE_REF[ORK_NPHRASE] = {   /* the written key (MIDI note) */"]
    lines += ["    " + ", ".join(str(midi(ref)) for _n, _w, ref, _l in PHRASES)]
    lines += ["};", "#endif"]
    out.write_text("\n".join(lines) + "\n")
    print(f"phrases: {len(PHRASES)}, {len(evs)} events -> {out}")


if __name__ == "__main__":
    main(sys.argv)
