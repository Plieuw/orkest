# Orkest V14

Orkest op **[FM1 Open](https://github.com/m-vave/fm1_open)**, M-VAVE's eigen open firmware voor de FM-1 (V14,
oktober 2026): een orkest-ROMpler met strijkers, hout, koper, slagwerk en ensembles. Van FM1 Open blijft het
platform: USB-MIDI en USB-audio, Bluetooth-MIDI en Bluetooth-audio, DIN/TRS-MIDI, scherm, knoppen, flash en het
officiële update-protocol. De klankengines van de originele firmware (DX7, 303, 808, PSG, pluck, sampler) en de
sequencer worden niet gebruikt.

**Installeren:** https://plieuw.github.io/orkest/ (Chrome of Edge): eerst de firmware, dan de orkestklanken.

## Bediening

| Bediening | Functie |
| --- | --- |
| 27 toetsen | spelen de klank, sturen MIDI uit (kanaal 1, USB/Bluetooth) |
| KNOB 1 of PRESETS | klank kiezen (24); zet attack en release op die van de klank |
| KNOB 2 / 3 / 4 | attack / release / ROOM (galm) |
| LFO (kort) | modulatie aan/uit (lampje brandt als hij aan staat) |
| LFO vasthouden + knoppen | K1 snelheid (0,3–16 Hz), K2 vibrato (tot ±60 cent), K3 tremolo, K4 inloopvertraging (0–3 s) |
| MIDI CC 1 (modwiel) | vibrato, ook met de LFO uit |
| OCT- / OCT+ | octaaf −3..+3 |
| SAVE | klank, attack, release, room en octaaf bewaren (terug na herstart) |
| MASTER | volume |
| MIDI in (DIN/TRS, USB, Bluetooth) | noten op elk kanaal, CC 120/123 = alles uit |

## Klank

De aanslag van elke noot is de echte opname. De aanhoudende toon daarna is stabiel: geen vibrato, geen zwelling.
Blazers, contrabas en soloviool krijgen een loop die opnieuw is opgebouwd uit de boventonen van de opname (hun
gemiddelde sterkte, een exact geheel aantal periodes); de strijkerssecties ook, met wat ensemble-glans op de hoge
boventonen. Blazers en contrabas komen uit VSCO's opnames zonder vibrato. Beweging voeg je zelf toe met de LFO.

## Hoe het in elkaar zit

| Pad | Wat |
| --- | --- |
| `sdk/` | de FM1 SDK zoals M-VAVE hem levert (binaire engine, headers, boot- en updatebestanden); ongewijzigd |
| `app/board/`, `app/audio_output.*` | FM1 Open's board-code; `audio.c` laat de audio-interrupt het orkest renderen |
| `app/orkest_player.c` | de speler: IMA ADPCM, aanslag + loop, lagen, envelope, 24 stemmen, galm (alles in SRAM) |
| `app/orkest_bank.c` | de klankbank in de gebruikersflash: controle (CRC, commit-woord) en de SysEx-upload |
| `app/orkest_ui.c` | scherm (M-VAVE's 5x7-font), toetsen, knoppen, LEDs, instellingen |
| `app/orkest_sdk.c` | MIDI in, SysEx, en de haken die de SDK van een app verwacht |
| `tools/gen_bank.py` | de klankbank uit `../assets/samples-cc0` (via `../tools/gen_orkest.py`) |
| `tools/build_open.py` | bouwen op Linux: een port van M-VAVE's `Build.ps1` en `Package.cs` |
| `tools/make_site_v14.py` | de installatiepagina (`../web/index_v14.html`, `../web/orkbank.js`) |
| `tests/` | host-tests: speler, bank, upload-protocol, en de browser-upload tegen de firmwarecode |

```
python3 tools/gen_bank.py build/orkest-bank.bin
python3 tools/build_open.py . ~/.jieli/toolchain --out build/orkest-v14.fwsc   # JieLi Linux-toolchain
tests/run_tests.sh
```

De klankbank staat los van de firmware in de gebruikersflash (524 KB, nu ~400 KB gebruikt), dus nieuwe klanken
vragen geen nieuwe firmware: alleen stap 2 van de installer.

## Licentie

De app valt onder GPL-3.0 met M-VAVE's linkuitzondering voor de FM1 SDK ([LICENSE.md](LICENSE.md)); de SDK onder
M-VAVE's eigen voorwaarden ([THIRD_PARTY.md](THIRD_PARTY.md)). Code uit Felucca (GPL-3.0 zonder die uitzondering)
zit niet in deze firmware; de installer in de browser is een apart programma. Samples: VSCO 2 CE, CC0.
