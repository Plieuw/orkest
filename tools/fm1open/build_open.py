#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Build M-VAVE FM1 Open (github.com/m-vave/fm1_open) on Linux with the JieLi Linux toolchain.

A port of sdk/Build.ps1 -FirmwareOnly (compile the APP, link with the prebuilt SDK, check, package) and of
sdk/Package.cs (the .fwsc writer), so no Windows, PowerShell or .NET is needed.

  build_open.py ROOT TOOLCHAIN [--no-lto] [--out FILE.fwsc]
    ROOT       an fm1_open checkout
    TOOLCHAIN  the JieLi Linux toolchain (the directory with pi32v2/bin/clang)
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

USER_BYTES, UPDATE_FAMILY = 536576, 7


def run(cmd, cwd=None):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(" ".join(map(str, cmd))[:2000] + "\n" + r.stdout[-6000:] + r.stderr[-6000:])
        raise SystemExit(f"failed: {Path(str(cmd[0])).name}")
    return r.stdout


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


# ------------------------------------------------------------------ Package.cs --
AREA, ENTRY, APPDATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, USER_SIZE, BT_SIZE, MIN_VM = 0xFF000, 536576, 4096, 32768
PUBLIC_CODE = 0x980F


def _crc_table():
    t = []
    for i in range(256):
        v = i << 8
        for _ in range(8):
            v = ((v << 1) ^ (0x1021 if v & 0x8000 else 0)) & 0xFFFF
        t.append(v)
    return t


CRC_T = _crc_table()


def crc(b, begin=0, length=None):
    v = 0
    end = len(b) if length is None else begin + length
    for i in range(begin, end):
        v = ((v << 8) & 0xFFFF) ^ CRC_T[(v >> 8) ^ b[i]]
    return v


def cipher(b, begin, length, state):
    for i in range(begin, begin + length):
        b[i] ^= state & 0xFF
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xFFFF


def round_up(n, a):
    return (n + a - 1) & -a


class Image:
    def __init__(self, size, fill):
        self.b = bytearray([fill]) * size

    def num(self, off, v, w):
        for n in range(w):
            self.b[off + n] = (v >> (8 * n)) & 0xFF

    def copy(self, off, data):
        self.b[off:off + len(data)] = data

    def text(self, off, s):
        self.copy(off, s.encode("ascii"))

    def checksum(self, field, off, size):
        self.num(field, crc(self.b, off, size), 2)

    def record(self, off, name, c, addr, size, attr, last, extra):
        self.b[off:off + 32] = bytes(32)
        self.num(off + 2, c, 2)
        self.num(off + 4, addr & 0xFFFFFFFF, 4)
        self.num(off + 8, size & 0xFFFFFFFF, 4)
        self.num(off + 12, attr, 2)
        self.num(off + 14, last, 2)
        for n in range(16, 32):
            self.b[off + n] = 255
        self.text(off + 16, name)
        self.b[off + 16 + len(name)] = 0
        if extra is not None:
            self.num(off + 28, extra, 4)
        self.checksum(off, off + 2, 30)


def encode_boot(plain):
    res = bytearray(plain)
    banks = struct.unpack_from("<H", plain, 0)[0]
    if not 1 <= banks <= len(plain) // 16:
        raise SystemExit("invalid boot directory")
    prev = banks * 16
    for i in range(banks):
        h = i * 16
        length = struct.unpack_from("<H", plain, h + 2)[0]
        start = struct.unpack_from("<i", plain, h + 8)[0]
        if (length == 0 or start < prev or start > len(plain) - length or crc(plain, h, 14) != struct.unpack_from("<H", plain, h + 14)[0]
                or crc(plain, start, length) != struct.unpack_from("<H", plain, h + 12)[0]):
            raise SystemExit("invalid boot bank")
        cipher(res, h, 16, 0xFFFF)
        cipher(res, start, length, 0xFFFF)
        prev = start + length
    return res


def read_res(d, name, size):
    b = (Path(d) / name).read_bytes()
    if len(b) != size:
        raise SystemExit(f"unexpected FM1 resource {name}")
    return b


def build_flash(app, d, end, vm_end):
    boot = read_res(d, "uboot.boot", 14488)
    config = read_res(d, "boot-config.bin", 721)
    tool = read_res(d, "cfg_tool.bin", 383)
    image, settings = Image(end, 255), Image(34 + len(config), 0)
    for bit in range(16):
        settings.b[bit] = 16
        settings.b[16 + bit] = 16 ^ (0 if (PUBLIC_CODE >> bit) & 1 else 255)
    settings.checksum(32, 0, 32)
    settings.copy(34, config)
    image.copy(0, bytes([0, 0, 0x20, 2, 0x2f, 0x10, 0x4c, 0xc9, 0, 0xf0, 0x0f, 0, 0x10, 0x10, 0xa4, 0xff,
                         0x4e, 0x5d, 0x0b, 0x41, 0xe0, 0xcd, 0x78, 0x1d, 0xc8, 0x56, 0x41, 0x7f, 0xb3, 0x67, 0xcf, 0x9f]))
    image.checksum(0, 2, 30)
    config_at = 160 + len(boot)
    image.record(32, "uboot.boot", crc(boot), 160, len(boot), 0, 0, None)
    image.record(64, "isd_config.ini", crc(settings.b), config_at, len(settings.b), 0x8002, 0, None)
    image.record(96, "app_dir_head", 65535, AREA, 0xFFFFFFFF, 0xFF81, 0, None)
    image.record(128, "key_mac", 65535, FLASH_LIMIT, 4096, 0x0112, 65535, None)
    for p in range(0, 160, 32):
        cipher(image.b, p, 32, 0xFFFF)
    image.copy(160, encode_boot(boot))
    image.copy(config_at, settings.b)
    payload = APPDATA + len(app) + len(tool)
    a = AREA + APPDATA
    image.copy(a, app)
    image.copy(a + len(app), tool)
    image.record(AREA + 32, "app.bin", crc(app), APPDATA, len(app), 0xFF82, 0, None)
    image.record(AREA + 64, "cfg_tool.bin", crc(tool), APPDATA + len(app), len(tool), 0xFF82, 0, None)
    image.record(AREA + 96, "VM", 65535, end, vm_end - end, 0x8112, 0, MIN_VM)
    image.record(AREA + 128, "PRCT", 65535, 0, end, 0x8292, 0, 0x45444F43)
    image.record(AREA + 160, "BTIF", 65535, vm_end, BT_SIZE, 0x8192, 1, 0x4F545541)
    image.record(AREA, "app_area_head", crc(image.b, AREA + 32, payload - 32), ENTRY, payload, 0xFF83, 0, None)
    reserved = AREA + payload
    image.record(reserved + 32, "USER", 65535, FLASH_LIMIT - USER_SIZE, USER_SIZE, 0x8192, 1, 0x4F545541)
    image.record(reserved, "EXT_RESERVED", crc(image.b, reserved + 32, 32), 32, 64, 0xFF93, 1, None)
    for p in range(0, payload + 64, 32):
        cipher(image.b, AREA + p, min(32, payload + 64 - p), PUBLIC_CODE ^ (p >> 2))
    return image


def wrap(flash, loader, product):
    if not 1 <= len(loader) < 32768:
        raise SystemExit("invalid FM1 update loader")
    loader_at = 1024 + round_up(len(flash.b), 32)
    total = loader_at + round_up(len(loader), 32)
    ufw = Image(total, 255)
    ufw.b[0:224] = bytes(224)
    files, names = [bytes(flash.b), loader], ["flash.bin", "ota.bin"]
    for i, f in enumerate(files):
        rec = 64 + i * 80
        ufw.num(rec, 0 if i == 0 else 100, 2)
        ufw.num(rec + 2, i, 2)
        ufw.num(rec + 4, crc(f), 2)
        ufw.num(rec + 8, 1024 if i == 0 else loader_at, 4)
        ufw.num(rec + 12, len(f), 4)
        ufw.num(rec + 16, round_up(len(f), 32), 4)
        ufw.text(rec + 64, names[i])
        cipher(ufw.b, rec, 80, 0xFFFF)
    ufw.num(4, total, 4)
    ufw.num(8, 2, 2)
    ufw.num(10, 4, 2)
    ufw.num(12, 512, 4)
    ufw.text(16, "AC791N")
    ufw.checksum(2, 64, 160)
    ufw.checksum(0, 2, 62)
    cipher(ufw.b, 0, 64, 0xFFFF)
    ufw.copy(1024, files[0])
    ufw.copy(loader_at, loader)
    out = bytearray()
    for src in range(total):
        out.append(ufw.b[src])
        if src < 940 and (src + 1) % 47 == 0:
            idx = src // 47
            out.append((ord(product[idx]) + idx + 1) & 0xFF if idx < len(product) else 125)
    return bytes(out)


def package(app, resources, product):
    if not re.fullmatch(r"FM-1_8\d{6}", product):
        raise SystemExit("invalid FM1 update identity")
    limit = FLASH_LIMIT - USER_SIZE - BT_SIZE - MIN_VM - AREA - APPDATA - 383 - 64
    if not app or len(app) > limit:
        raise SystemExit(f"application ({len(app)} B) does not fit the FM1 layout (max {limit} B)")
    image_size = round_up(AREA + APPDATA + len(app) + 383 + 64, 4096)
    user_at = FLASH_LIMIT - USER_SIZE
    vm_end = user_at - BT_SIZE
    if image_size > vm_end - MIN_VM:
        raise SystemExit("insufficient FM1 settings space")
    img = build_flash(app, resources, image_size, vm_end)
    pkg = wrap(img, (Path(resources) / "ota.bin").read_bytes(), product)
    return pkg, dict(FLASH_BIN_SIZE=image_size, VM_REAL_SIZE=vm_end - image_size, app_limit=limit, app=len(app),
                     app_free=limit - len(app), user_bytes=USER_SIZE)


# ------------------------------------------------------------------ build --
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("toolchain", type=Path)
    ap.add_argument("--no-lto", action="store_true")
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    root, tc = a.root.resolve(), a.toolchain.resolve()
    sdk, build = root / "sdk", root / "build" / "dx7"
    binp = tc / "pi32v2" / "bin"
    clang, ld = binp / "clang", binp / ("pi32v2-ld" if (binp / "pi32v2-ld").exists() else "ld")
    nm = next((p for p in (binp / "llvm-nm", binp / "nm", tc / "common" / "bin" / "llvm-nm") if p.exists()), binp / "nm")
    objcopy = next((p for p in (binp / "llvm-objcopy", tc / "common" / "bin" / "llvm-objcopy", binp / "objcopy") if p.exists()),
                   binp / "llvm-objcopy")
    gold = next((p for p in (tc / "pi32v2" / "lib" / "LLVMgold.so", binp / "LLVMgold.so", tc / "pi32v2" / "lib" / "llvm" / "LLVMgold.so")
                 if p.exists()), None)
    lto = gold is not None and not a.no_lto
    print(f"toolchain: {clang} ({run([clang, '--version']).splitlines()[0]}), ld {ld.name}, LTO {'on: ' + str(gold) if lto else 'off'}")

    manifest = json.loads((sdk / "manifest.json").read_text())
    bad = [f for f, h in manifest["files"].items() if sha(root / f) != h]
    if bad:
        raise SystemExit(f"SDK payload changed: {bad}")
    synth = json.loads((sdk / "synth.json").read_text())
    if synth["native_sha256"] != sha(sdk / "lib" / "libfm1_engine.a"):
        raise SystemExit("native DSP kernels and SDK contracts do not match")
    for name, h in synth.get("portable_contracts", {}).items():
        if sha(root / "app" / "synth" / name) != h:
            raise SystemExit(f"fixed DSP coefficients differ: {name}")
    ver = (root / "app" / "version.h").read_text()
    release = re.search(r'(?m)^#define FM1_RELEASE_VERSION "(V\d+)"', ver).group(1)
    product = re.search(r'(?m)^#define FM1_UPDATE_PRODUCT "(FM-1_8\d{6})"', ver).group(1)
    print(f"build FM1 Open {release} ({product})")

    (build / "obj").mkdir(parents=True, exist_ok=True)
    (build / "generated").mkdir(parents=True, exist_ok=True)
    for f in (build / "obj").glob("*"):
        f.unlink()
    include = ["sdk/include", "sdk/include/libc", "app/synth", "sdk/config", "app", "app/board"]
    flags = ["-target", "pi32v2", "-integrated-as", "-mcpu=r3", "-mfprev1", "-Wuninitialized", "-Wno-invalid-noreturn",
             "-fno-common", "-fallow-pointer-null", "-fprefer-gnu-section", "-femulated-tls", "-Wno-shift-negative-value",
             "-Wframe-larger-than=2560", "-fms-extensions", "-fno-unwind-tables", "-ffunction-sections", "-fdata-sections",
             "-fmessage-length=0", "-cl-single-precision-constant", "-mllvm", "-pi32v2-large-program=true",
             "-D__GCC_PI32V2__", "-O2"] + (["-flto"] if lto else [])
    flags += [f"-I{root / d}" for d in include]
    sources = sorted(p for p in (root / "app").rglob("*") if p.suffix in (".c", ".S") and "synth" not in p.relative_to(root / "app").parts[:1])
    objs = []
    for i, src in enumerate(sources):
        rel = src.relative_to(root).as_posix()
        out = build / "obj" / f"{i}_{re.sub(r'[^a-zA-Z0-9._-]', '_', rel)}.obj"
        opt = list(flags)
        if rel in ("app/screen.c", "app/transfer.c", "app/midi_router.c"):
            opt.append("-fno-lto")
        if rel == "app/board/panel.c":
            opt += ["-fno-lto", "-fno-pic"]
        opt += ["-fwrapv", "-fno-fast-math", "-ffp-contract=off"]
        if rel in ("app/project.c", "app/transfer.c", "app/samples.c", "app/screen.c", "app/presets.c", "app/midi_router.c",
                   "app/fm_factory.c", "app/fm_library.c"):
            opt.append("-Oz")
        if rel == "app/screen.c":
            opt.append("-fno-inline-functions")
        print(f"compile {rel}")
        run([clang, *opt, "-c", src, "-o", out])
        objs.append(out)
    ident = [  # the build identity, as Build.ps1 (profile, manifest hash, every app file)
        "dx7", sha(sdk / "manifest.json")] + [f"{p.relative_to(root).as_posix()}:{sha(p)}" for p in sorted((root / "app").rglob("*")) if p.is_file()]
    bid = hashlib.sha256("\n".join(ident).encode()).hexdigest()
    (build / "generated" / "build_identity.c").write_text(
        '#include "layout.h"\n'
        f"const struct fm1_firmware_layout fm1_layout = {{{USER_BYTES}U,{UPDATE_FAMILY}U}};\n"
        "const unsigned char fm1_build_id[32] = {" + ",".join("0x" + bid[i:i + 2] for i in range(0, 64, 2)) + "};\n"
        f'const unsigned char fm1_update_identity[27] = "{product}";\n')
    run([clang, *flags, "-c", build / "generated" / "build_identity.c", "-o", build / "obj" / "build_identity.obj"])
    objs.append(build / "obj" / "build_identity.obj")
    linker = (sdk / "config" / "linker.ld.in").read_text().replace("@SBC_DECODER@", "sbc_decoder\n") \
        .replace("@ACL_TX_EXTRA@", " + 1024").replace("@ACL_RX_EXTRA@", " + 4096")
    (build / "generated" / "linker").write_text(linker)
    link = []
    if lto:
        link += ["--plugin", gold]
    link += ["-o", "sdk.elf", *objs]
    if lto:
        link += ["--plugin-opt=-pi32v2-always-use-itblock=false", "--plugin-opt=-enable-ipra=true",
                 "--plugin-opt=-pi32v2-merge-max-offset=4096", "--plugin-opt=-pi32v2-enable-simd=true", "--plugin-opt=mcpu=r3",
                 "--plugin-opt=-global-merge-on-const", "--plugin-opt=-inline-threshold=5",
                 "--plugin-opt=-inline-max-allocated-size=32", "--plugin-opt=-inline-normal-into-special-section=true",
                 "--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar",
                 "--plugin-opt=-pi32v2-enable-rep-memop", "--plugin-opt=-emulated-tls",
                 "--plugin-opt=-pi32v2-large-program=true", "--plugin-opt=-pi32v2-csync-before-rti=true"]
    link += ["--sort-common", "--start-group", "--whole-archive", sdk / "lib" / "libfm1_engine.a", "--no-whole-archive",
             sdk / "lib" / "libc.a", sdk / "lib" / "libcompiler_rt.a", "--end-group", "--gc-sections",
             "-Tgenerated/linker", "-M=sdk.map", "--undefined=connectivity_wl82_bluetooth_start",
             "--undefined=audio_app_bluetooth_open", "--undefined=bt_get_local_name", "--undefined=bt_get_mac_addr"]
    print("link")
    run([ld, *link], cwd=build)
    elf = build / "sdk.elf"

    # checks (as Build.ps1 Test-Firmware): the audio callbacks in internal SRAM, the entry
    syms = {}
    for line in run([nm, "--print-size", elf]).splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?([A-Za-z])\s+(\S+)", line)
        if m:
            syms[m.group(4)] = int(m.group(1), 16)
    need = ["fm1_engine_render", "fm1_engine_output_level", "fm1_audio_copy", "fm1_output_mix", "fm1_usb_audio_read",
            "fm1_bluetooth_audio_read", "fm1_usb_audio_write"] + list(synth["sram_required"])
    bad = [n for n in need if not 0x01C00000 <= syms.get(n, 0) < 0x01C80000]
    if bad:
        raise SystemExit(f"audio callbacks absent or outside internal SRAM: {bad}")
    entry = struct.unpack_from("<I", elf.read_bytes(), 24)[0]
    if entry != ENTRY or syms.get("_start") != ENTRY:
        raise SystemExit(f"entry {entry:#x} does not match the ROM entry")

    image = bytearray()
    for sec in ("text", "data", "ram0_data", "cache_ram_data"):
        out = build / "generated" / f"{sec}.bin"
        run([objcopy, "-O", "binary", "-j", f".{sec}", elf, out])
        image += out.read_bytes()
        out.unlink()
    pkg, layout = package(bytes(image), sdk / "tools" / "pack", product)
    dest = a.out or build / "generated" / "FM1_OFFICIAL_OPEN.fwsc"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(pkg)
    (build / "package-report.json").write_text(json.dumps(layout, indent=1))
    print(f"package {dest}: {len(pkg)} B; app {layout['app']} B of {layout['app_limit']} B ({layout['app_free']} B free), "
          f"VM {layout['VM_REAL_SIZE']} B, user {layout['user_bytes']} B")


if __name__ == "__main__":
    main()
