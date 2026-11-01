# Orkest

Eigen firmware voor de M-VAVE FM-1, gebouwd op de FM-1 Basis: een uitgeklede versie van
[Felucca](https://github.com/hugelton/Felucca) van Hügelton Instruments. De hardwarelaag, het update-pad en de
veiligheidsnetten komen uit Felucca; de rest is van Orkest.

> **Nieuw: Orkest V14.** Dezelfde orkestklanken op M-VAVE's eigen open firmware (FM1 Open), met Bluetooth- en
> USB-MIDI: zie [v14/](v14/README.md). De installer op https://plieuw.github.io/orkest/ is nu die van V14; deze
> Felucca-versie staat op https://plieuw.github.io/orkest/klassiek/.

## Installeren (klassiek)

**Webinstaller (Chrome of Edge):** https://plieuw.github.io/orkest/klassiek/ — sluit de FM-1 met USB aan en druk op Installeren.

Elke push naar `main` bouwt automatisch een nieuwe firmware (GitHub Actions, `.github/workflows/build.yml`), draait
de host-tests en zet de installer met die build op GitHub Pages. De versie is `VERSION` plus het buildnummer
(bijv. `v0.1-b12`) en staat op het scherm van de FM-1. Het `.fwsc`-bestand, een wav-render en
schermafbeeldingen staan als artifact bij elke run in de Actions-tab.

## Wat het doet

Orkest is een orkest-ROMpler: echte opnames van orkestinstrumenten uit
[VSCO 2 Community Edition](https://github.com/sgossner/VSCO-2-CE) (CC0), in de firmware zelf opgeslagen.
Elke noot bestaat uit de **aanslag** (de transient, één keer gespeeld) en een **sustain-loop** die doorloopt
zolang je de toets vasthoudt; bij loslaten volgt de **release**. Pizzicato, harp, pauken en glockenspiel klinken
natuurlijk uit (one-shot).

| Bediening | Functie |
| --- | --- |
| 27 toetsen | spelen de gekozen klank, sturen USB-MIDI uit (kanaal 1), lichten op |
| KNOB 1 (of PRESETS) | klank kiezen (24, zie hieronder); zet ook attack en release op de waarden van die klank |
| KNOB 2 / KNOB 3 | attack / release |
| KNOB 4 | ROOM: hoeveel galm (zaalklank) |
| OCT- / OCT+ | octaaf −3..+3 (LED aan als het octaaf niet 0 is) |
| SAVE | klank, octaaf, attack, release en room in flash (terug na herstart) |
| MASTER | volume |
| MIDI in (USB en TRS) | note on/off met velocity op elk kanaal, CC 123 = all notes off |
| USB | class-compliant MIDI + USB-audio-ingang (opnemen in je DAW) |
| GLO | tweede core aan/uit (standaard aan; geldt na SAVE en herstart). Onderaan het scherm: `2 CORES` of `1 CORE` |
| SELECT, ALGORITHM, overige knoppen | worden op het scherm getoond: vrij voor nieuwe functies |
| OCT- + OCT+ 5 s ingedrukt | UBOOT (update-modus) |

**Klanken** (KNOB 1):

| Strijkers | Hout | Koper | Slagwerk e.d. | Ensembles (lagen) |
| --- | --- | --- | --- | --- |
| VIOLINS, VIOLAS, CELLI, BASSES, SOLO VIOLIN, TREMOLO, PIZZICATO, SPICCATO | FLUTE, OBOE, CLARINET, BASSOON | HORN, TRUMPET, TROMBONE, TUBA | TIMPANI, GLOCKENSPIEL, HARP | STRINGS, WOODWINDS, BRASS, TUTTI, MARCATO |

De ensembles spelen meerdere instrumenten tegelijk, elk op hun eigen plek in het stereobeeld (violen links, celli
rechts): STRINGS = violen + celli een octaaf lager; WOODWINDS = fluit (hoog) + klarinet + fagot; BRASS = trompet +
hoorns en trombones een octaaf lager; TUTTI = violen + hoorns + celli. MARCATO legt de korte spiccato-aanslag over de
aangehouden violen: de transient van het ene instrument op de sustain van het andere.

### Klanken aanpassen

- `tools/orkest_fetch.py`: welke opnames uit VSCO 2 CE (per instrument een paar noten verspreid over het bereik);
  de fragmenten staan al in `assets/samples-cc0/` (met `ATTRIBUTION.txt`).
- `tools/gen_orkest.py`: per instrument de lengte van aanslag en loop en het volume, en de lijst met klanken
  (`PATCHES`: tot 3 lagen, elk met toetsbereik, transpositie, volume en pan). Draait bij elke build; zoekt de
  loop-punten, egaliseert het volume binnen de loop, en codeert naar IMA ADPCM.
- Geheugen: de firmware heeft ~568 KB voor code + samples; de samples mogen nu 420 KB gebruiken (nu ~390 KB:
  60 zones, 38 s audio). `gen_orkest.py --wav DIR` schrijft elke zone uit zoals de FM-1 hem afspeelt (met loops).
- `firmware/src/synth.c`: de speler (ADPCM, lagen, envelope, 24 stemmen, galm).

### Tweede core

De AC79 in de FM-1 heeft twee cores. Orkest start de tweede bij het opstarten en laat hem per audioblok de helft
van de stemmen rekenen. De eerste core rekent de andere helft plus de galm. Op sommige FM-1's leest de tweede core
af en toe verkeerde data (zie [fm1-x0x#10](https://github.com/charlesvestal/fm1-x0x/issues/10)): zodra dat één keer
gebeurt of hij niet op tijd klaar is, zet de firmware hem voorgoed stil en rekent alles op één core (hooguit één
tikje). Ging de vorige keer opstarten mis, dan start hij niet. Met GLO + SAVE + herstart blijft hij uit.

## Waar begin je

Alles wordt als één bestand gecompileerd: `firmware/src/felucca.c` bepaalt de volgorde.

| Bestand | Wat |
| --- | --- |
| `firmware/src/keys.c` | toetsen en MIDI in → noten (draait in de audio-interrupt, elke 32 samples) |
| `firmware/src/synth.c` | de orkestspeler: samples, lagen, envelope, galm |
| `firmware/src/audio.c` | de audio-interrupt: `out[]` vullen met Q15 stereo |
| `firmware/src/ui.c` | scherm, LEDs, knoppen, encoders, instellingen opslaan |
| `tools/gen_orkest.py`, `assets/samples-cc0/` | de samples en de lijst met klanken |
| `firmware/src/main.c` | opstarten en hoofdlus (met watchdog, boot-guard, UBOOT en update) |
| `firmware/src/panel.c` | welke matrix-knop welke opdruk heeft |
| `firmware/src/core.h` | gedeelde constanten en de `sys`-struct (hoofdlus ↔ audio) |
| `firmware/src/storage.c` | flash-opslag met A/B-kopieën; `OBJ_USER0..7` zijn vrij |
| `firmware/src/gfx.c`, `lcd.c` | tekenen: tekst (Inter Tight), rechthoeken, lijnen; kleuren via `T_BG`, `T_TEXT`, `T_THEME` … |
| `firmware/src/usb.c`, `midi_uart.c` | USB-MIDI/-audio en TRS MIDI (uit Felucca; USB-naam "Orkest") |
| `firmware/src/ota.c`, `ota_hw.c`, `firmware/loader/` | het update-pad: **laten staan** |
| `firmware/hal/` | registers: toetsmatrix, LEDs, encoders, LCD, I2S, ADC, flash, tweede core |

Regels van het huis (uit Felucca, en `tools/build.py` controleert ze): registertoegang alleen in `hal/`, geen
floating point in de audio-interrupt nodig (alles is fixed point), en de audio-interrupt moet binnen ~2,9 ms klaar zijn
(het scherm toont de CPU-belasting).

## Bouwen, testen, installeren

Zie [BUILDING.md](BUILDING.md). Kort:

```
tools/get_toolchain.sh                 # JieLi-toolchain (eenmalig)
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
pip3 install Pillow fonttools mido python-rtmidi numpy scipy
./build.sh                             # -> build/felucca.fwsc
tests/run_tests.sh                     # host-tests, geen FM-1 nodig (+ build/host/orkest-demo.wav en schermafbeeldingen)
python3 tools/fm1_install.py build/felucca.fwsc
```

Op macOS draait de toolchain in Docker (zie BUILDING.md); op Linux x86-64 direct.

**Veiligheid.** De installers praten met de draaiende firmware (`ota.c`). Haal dat er niet uit, anders kun je de
volgende build alleen nog via UBOOT installeren. Lukt opstarten twee keer achter elkaar niet binnen 30 s, dan gaat de
FM-1 vanzelf naar UBOOT. Herstel na een mislukte installatie: [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
Terug naar de officiële firmware: `python3 tools/fm1_install.py FM-1.fwsc` met het V15-bestand van M-VAVE.

## Licentie

GPL-3.0-only, net als Felucca ([LICENSE](LICENSE), [LICENSING.md](LICENSING.md), [CREDITS.md](CREDITS.md)). Deel je
een build, deel dan ook de broncode. Intern heten veel dingen nog `felucca` / `FELUCCA_*` (bestandsnamen, build-opties);
zichtbaar heet de firmware Orkest: het scherm, de USB-naam (`firmware/src/usb.c` STR2) en de installer. De USB-VID/PID
en de fabrikantnaam (STR1) zijn nog die van Felucca; kies een eigen VID/PID vóór je Orkest breed verspreidt.
