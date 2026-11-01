#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tests/ui_shot's raw RGB565 frames -> PNG (x2, nearest), and one contact sheet.
  tests/ui_shot.py DIR"""
import struct
import sys
from pathlib import Path
from PIL import Image

d = Path(sys.argv[1])
shots = []
for raw in sorted(d.glob("*.rgb565")):
    px = struct.unpack("<57600H", raw.read_bytes())
    img = Image.new("RGB", (240, 240))
    img.putdata([((c >> 11) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31) for c in px])
    img = img.resize((480, 480), Image.NEAREST)
    img.save(raw.with_suffix(".png"))
    shots.append(img)
if shots:
    sheet = Image.new("RGB", (len(shots) * 490 - 10, 480), (128, 128, 128))
    for i, s in enumerate(shots):
        sheet.paste(s, (i * 490, 0))
    sheet.save(d / "sheet.png")
    print(f"ui shots: {len(shots)} PNGs and sheet.png in {d}")
