# Building

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `felucca.fwsc` | the installable package (app + loader) |

## Prerequisites (macOS)

- Python 3 with Pillow and fontTools: `pip3 install Pillow fonttools` (the UI font is
  rasterised at build time)
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 1.0` makes a release build: the package identity becomes `FM-1_910`
and the version string `v1.0`; the package is `build/felucca-1.0.fwsc`, and
`build/release-1.0/` holds what a release ships: the package, the app
(`felucca-1.0-app.bin`), `SHA256SUMS`, `LICENSE`, `LICENSING.md` and
`LICENSES/` (the package contains Apache-2.0 SDK files, so the licence texts travel with it).

Build options (environment, `0` or `1`; defaults in `firmware/src/felucca.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_FLASH` | 1 | the flash driver and store (`storage.c`): the settings SAVE keeps |
| `FELUCCA_OTA` | 1 | update entry (needs `FELUCCA_FLASH`) |
| `FELUCCA_CDC` | 0 | USB serial port (`firmware/src/usb.c`; `1` needs a `cdc_task` of your own in the main loop) |
| `FELUCCA_UAC` | 1 | USB audio input (the master output, stereo, 44.1 or 48 kHz) |
| `FELUCCA_UAC_48K` | 1 | `0`: the USB audio input at 44.1 kHz only, as up to 1.0.5.2 (`firmware/src/usb.c`) |
| `FELUCCA_UAC_AS_FIRST` | 1 | the audio control header lists the audio streaming interface before the MIDI one, so the USB audio driver of older macOS (up to 15) finds it; `0`: 1.0 to 1.5's order, byte for byte (`firmware/src/usb.c`) |
| `FELUCCA_CPU2` | 1 | the second core's support is built in (`firmware/hal/fm1_cpu1.h`; never started unless you call `fm1_cpu1_start()`), and a core left running by another firmware is held at boot; `0`: none of it |
| `FELUCCA_UART` | 1 | TRS MIDI IN |

`FELUCCA_USB_LAYOUT` (`0` to `3`, default `0`) picks other USB descriptor layouts for testing; see
`firmware/src/usb.c`.

## Tests

```
tests/run_tests.sh
```

Host tests, no hardware needed (a C compiler and Python 3): the keyboard and the synth (`tests/basis_test.c`, also
under ASan / UBSan; it writes `build/host/basis.wav`, a short render to listen to), the flash store, the key / button
debounce and the encoders, the TRS MIDI parser, the USB descriptors and USB audio, the command-line installer against a
simulated FM-1; after `./build.sh` also the update entry and the update loader against the package just built.

## Install

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/felucca.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Or with the web installer in Chrome or Edge: make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
python3 web/make_site.py build/felucca.fwsc dev /tmp/felucca-site
cd /tmp/felucca-site && python3 -m http.server 8000
# open http://localhost:8000/webapp/installer/
```

Both talk to the running firmware (the M-UPGRADE entry, `firmware/src/ota.c`): keep it in your own builds, or
the next install needs UBOOT. Back to the official firmware: `python3 tools/fm1_install.py FM-1.fwsc` with the
official V15 file from M-VAVE (the web installer's "Return to official V15" button needs Felucca's editor protocol,
which this firmware does not have).

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
