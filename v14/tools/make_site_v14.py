#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The Orkest V14 installer site (GitHub Pages):

  index.html                         redirect to the installer
  webapp/installer/index.html        web/index_v14.html, self-contained (fm1pkg.js, fm1ota.js, orkbank.js inlined)
  firmware/orkest-v14-VER.fwsc       the firmware (FM1 Open + Orkest)
  firmware/orkest-bank-VER.bin       the orchestra bank
  firmware/v14/                      the licences that travel with them

  make_site_v14.py FIRMWARE.fwsc BANK.bin VERSION OUT_DIR
"""
import json
import re
import shutil
import sys
from pathlib import Path

V14 = Path(__file__).resolve().parents[1]
ROOT = V14.parent
WEB = ROOT / "web"
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def main(fw, bank, version, out):
    fw, bank, out = Path(fw), Path(bank), Path(out)
    raw, bnk = fw.read_bytes(), bank.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_8\d{6}", product):
        raise SystemExit(f"{fw}: identity {product!r} is not an FM1 Open package (FM-1_8xxxxxx)")
    if bnk[:4] != b"ORKB":
        raise SystemExit(f"{bank}: not an Orkest bank")
    ver = re.sub(r"[^A-Za-z0-9.-]", "-", version)
    fw_name, bank_name = f"orkest-v14-{ver}.fwsc", f"orkest-bank-{ver}.bin"
    html = (WEB / "index_v14.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((WEB / f).read_text(encoding="utf-8")) for f in ("fm1pkg.js", "fm1ota.js", "orkbank.js"))
    meta = json.dumps({"version": version, "product": product, "pkg": "../../firmware/" + fw_name,
                       "bank": "../../firmware/" + bank_name, "bankBytes": len(bnk)})
    for mark in ("/*LIB*/", "/*META*/"):
        if html.count(mark) != 1:
            raise SystemExit(f"index_v14.html must contain {mark} once")
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta)
    inst, fwd, lic = out / "webapp" / "installer", out / "firmware", out / "firmware" / "v14"
    for d in (inst, fwd, lic):
        d.mkdir(parents=True, exist_ok=True)
    for old in list(fwd.glob("orkest-v14-*.fwsc")) + list(fwd.glob("orkest-bank-*.bin")):
        old.unlink()
    (inst / "index.html").write_text(html, encoding="utf-8")
    shutil.copy(fw, fwd / fw_name)
    shutil.copy(bank, fwd / bank_name)
    docs = {"LICENSE.md": V14 / "LICENSE.md", "THIRD_PARTY.md": V14 / "THIRD_PARTY.md", "GPL-3.0.txt": ROOT / "LICENSE",
            "SAMPLES-ATTRIBUTION.txt": ROOT / "assets" / "samples-cc0" / "ATTRIBUTION.txt",
            "Apache-2.0.txt": ROOT / "LICENSES" / "Apache-2.0.txt"}
    for n, src in docs.items():
        shutil.copy(src, lic / n)
    (lic / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>Orkest V14 licences</title><h1>Orkest V14: licences</h1><ul>'
        '<li><a href="LICENSE.md">LICENSE.md</a>: FM1 Open APP, GPL-3.0 with the FM1 SDK linking exception (M-VAVE)</li>'
        '<li><a href="THIRD_PARTY.md">THIRD_PARTY.md</a>: the FM1 SDK (binary) and its components</li>'
        '<li><a href="GPL-3.0.txt">GPL-3.0</a>: Orkest</li>'
        '<li><a href="Apache-2.0.txt">Apache-2.0</a>: the boot and update files in the package</li>'
        '<li><a href="SAMPLES-ATTRIBUTION.txt">Samples</a>: VSCO 2 Community Edition, CC0 1.0</li></ul>'
        '<p>Source: <a href="https://github.com/Plieuw/orkest">github.com/Plieuw/orkest</a> (v14/)</p>\n', encoding="utf-8")
    (out / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>Orkest</title>'
        '<meta http-equiv="refresh" content="0; url=webapp/installer/">'
        '<a href="webapp/installer/">Orkest installer</a>\n', encoding="utf-8")
    print(f"site: {out}: installer ({len(html)} B), firmware/{fw_name} ({len(raw)} B, {product}), "
          f"firmware/{bank_name} ({len(bnk)} B)")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
