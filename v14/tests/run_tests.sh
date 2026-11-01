#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Host tests of Orkest on FM1 Open (no FM1 needed). From v14/:  tests/run_tests.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
python3 tools/gen_bank.py build/orkest-bank.bin --header build/orkest_bank_file.h >/dev/null
INC="-DFM1_HOST_TEST -Iapp -Iapp/synth -Iapp/board -Isdk/include -Isdk/config -Ibuild"
SRC="tests/orkest_test.c app/orkest_player.c app/orkest_bank.c"
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter $INC -o build/host/orkest_test $SRC -lm
build/host/orkest_test build/host/orkest-v14-demo.wav
cc -std=gnu11 -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all $INC -o build/host/orkest_test_san $SRC -lm
echo "== under ASan / UBSan"
build/host/orkest_test_san > build/host/san.log || { cat build/host/san.log; exit 1; }
tail -1 build/host/san.log
echo "== the browser's bank upload (web/orkbank.js) against the firmware's (app/orkest_bank.c)"
cc -std=gnu11 -O1 -Wall $INC -o build/host/bank_device tests/bank_device.c app/orkest_bank.c app/orkest_player.c
node tests/upload_test.mjs
echo "== update frames: web/fm1ota.js against M-VAVE's WebUpdate.js"
node tests/frame_test.mjs
