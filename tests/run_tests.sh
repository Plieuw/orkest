#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host tests of the sources (no hardware). Run from the repo root:
#   tests/run_tests.sh
# Without build/felucca.fwsc (no ./build.sh yet) the two tests that need the package are skipped.
#   basis        the keyboard and the synth (keys.c, synth.c): notes, octave, MIDI in / out, voices, pitch, levels;
#                also with ASan / UBSan; writes build/host/basis.wav (a short render to listen to)
#   screen       ui.c and gfx.c drawn into a framebuffer: build/host/ui/*.png, nothing off the screen (needs build/gen)
#   storage      the flash store (storage.c): A/B copies, torn writes, bit rot, object map
#   input        the key / button debounce and the encoders of hal/fm1_input.h against the TIMER5 scan
#   MIDI         the TRS MIDI parser and its DMA ring (midi_uart.c); the USB descriptors and USB audio (usb.c)
#   update       the M-UPGRADE entry (ota.c) and the update loader (loader/) against this build's package;
#                the command-line installer (tools/fm1_install.py) against a simulated FM-1
set -e
cd "$(dirname "$0")/.."
if [ ! -f build/gen/ui_fonts.h ]; then         # the generated font / palette headers (as ./build.sh makes them)
    mkdir -p build/gen
    python3 tools/gen_aa_font.py build/gen/ui_fonts.h --preset inter-tight >/dev/null
    python3 tools/gen_aa_keycaps.py build/gen/ui_keycaps.h >/dev/null
    python3 tools/gen_ui_palettes.py build/gen/ui_palettes.h >/dev/null
fi
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

$CC -o "$OUT/basis_test" tests/basis_test.c -lm
run "keyboard and synth: keys, octave, MIDI in / out, voices, pitch, levels" "$OUT/basis_test" "$OUT/basis.wav"
${CC%% *} -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -o "$OUT/basis_test_san" tests/basis_test.c -lm
run "keyboard and synth under ASan / UBSan" "$OUT/basis_test_san"

mkdir -p "$OUT/ui"
$CC -Ibuild/gen -o "$OUT/ui_shot" tests/ui_shot.c
run "screen: every line on the screen, settings round trip (PNGs in $OUT/ui)" "$OUT/ui_shot" "$OUT/ui"
python3 tests/ui_shot.py "$OUT/ui" || true

$CC -o "$OUT/storage_test" tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"

$CC -o "$OUT/input_test" tests/input_test.c
run "keys and buttons: fast press, long release, bouncy contacts (one note each), glitches, encoders" "$OUT/input_test"

$CC -o "$OUT/midi_uart_test" tests/midi_uart_test.c
run "TRS MIDI parser" "$OUT/midi_uart_test"
$CC -Ifirmware/src -o "$OUT/uart_dma_test" tests/uart_dma_test.c -lm
run "TRS UART: pending events, stale counts, exact packets and continuous traffic" "$OUT/uart_dma_test"

HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/core.h)
$CC -O2 -DT_CDC=0 -DHALF_FRAMES="$HALF" -o "$OUT/uac_test" tests/uac_test.c -lm
run "USB audio input: descriptors, ring and packets, 44.1 -> 48 kHz resampler" "$OUT/uac_test"
# USB descriptor layouts: CDC UAC LAYOUT CDC-presented 48K (this build: CDC 0, UAC 1, 48K 1)
for v in 0.1.0.1.1 0.1.0.1.0 0.0.0.1.1 1.1.0.1.1 1.1.0.0.1; do
    IFS=. read -r t_cdc t_uac t_lay t_on t_48 <<EOV
$v
EOV
    $CC -DT_CDC="$t_cdc" -DT_UAC="$t_uac" -DT_LAYOUT="$t_lay" -DT_ON="$t_on" -DT_48K="$t_48" -o "$OUT/usb_desc_test" \
        tests/usb_desc_test.c
    run "USB descriptors: CDC $t_cdc (presented $t_on), UAC $t_uac (48 kHz $t_48), layout $t_lay" "$OUT/usb_desc_test"
done

run "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py

if [ -f build/felucca.fwsc ]; then
    $CC -DOWN_PKG=1 -o "$OUT/ota_test" tests/ota_test.c
    run "M-UPGRADE entry (own loader)" "$OUT/ota_test" build/felucca.fwsc
    head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
    python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
    $CC -o "$OUT/ldr_test" tests/ldr_test.c
    run "update loader: other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc
else
    echo "== skip the update entry and loader tests (no build/felucca.fwsc: run ./build.sh first)"
fi

[ "$fail" = 0 ] && echo "all tests passed" || { echo "SOME TESTS FAILED"; exit 1; }
