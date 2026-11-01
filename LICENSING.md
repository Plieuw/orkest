# Licensing

This firmware is a stripped-down fork of **Felucca** by Leo Kuroshita (@kurogedelic), Hügelton Instruments
(<https://github.com/hugelton/Felucca>). It is free software under the GNU General Public License, version 3 only
(`GPL-3.0-only`, full text in `LICENSE`). Felucca's own files keep their copyright line; new files carry an
`SPDX-License-Identifier: GPL-3.0-only` header.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, and the authors of the changes in this fork.

## What is GPL-3.0-only

- the firmware: `firmware/` (app, HAL, update loader)
- the build script and tools: `build.sh`, `tools/`
- the web installer: `web/`
- the host tests: `tests/`

If you distribute this firmware, or firmware derived from it (a `.fwsc` package, a device with it inside), you must
also give your recipients its complete corresponding source under the same licence.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Inter Tight font by The Inter Project Authors: the UI text, rasterised into the firmware at build time (`tools/gen_aa_font.py`) | SIL OFL 1.1 | `assets/fonts/InterTight[wght].ttf`, `LICENSES/OFL-InterTight.txt` |
| The second core's support (start, mailbox, guards): from X0X by charlesvestal (<https://github.com/charlesvestal/fm1-x0x>), with findings of Melodee (keremimo) and Jangada (zednaked) | GPL-3.0 | `firmware/hal/fm1_cpu1.{h,S}` |
| JieLi AC79 SDK by JieLi Technology: three of its files go into every `.fwsc` package (below); none are in this tree | Apache-2.0 | `LICENSES/Apache-2.0.txt` |

## JieLi SDK files in the packages (Apache-2.0)

A `.fwsc` package made by `tools/build.py` holds three unmodified files from the JieLi AC79 SDK
(<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>, `cpu/wl82/tools/`): `uboot.boot` (the boot loader), `cfg_tool.bin`
(the chip configuration) and `eq_cfg_hw.bin` (the default hardware EQ table). They are read from your SDK checkout at
build time (see BUILDING.md), are licensed under Apache-2.0 (`LICENSES/Apache-2.0.txt`), and their copyright stays with
JieLi Technology. Every distribution of a package carries the licence texts with it (`tools/build.py --release` and
`web/make_site.py` copy them).

## No vendor material

No M-VAVE material is part of this tree. The firmware links no vendor code and contains no vendor data.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments. "M-VAVE" and "FM-1" are trademarks of their
respective owners. This firmware is independent; it is not affiliated with, endorsed by or supported by them.

## Radio

This firmware never enables the Bluetooth / Wi-Fi radio of the hardware.
