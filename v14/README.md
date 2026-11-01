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
| ENV | ENV-menu open/dicht (ook HOME sluit): K1 attack, K2 release, K3 TRANSIENT (de aanzet; 64 = zoals opgenomen, 127 = harder, 0 = alleen de aanhoudende toon vanaf de eerste sample), K4 TAIL (het opgenomen uitklinken na loslaten; 0 = uit) |
| ARP | ARP-menu open/dicht (ook HOME sluit): elke ingedrukte toets speelt een korte, eenstemmige frase (meestal één maat of minder) vanaf zijn noot; tot 4 toetsen tegelijk (een akkoord speelt de frase vanaf elke noot). LATCH: een nieuw akkoord vervangt het vastgezette. SELECT aan/uit, K1 frase (72: 33 patronen en versieringen — trillers (ook aanzwellend en uitstervend), mordent, dubbelslag, voorslag, Alberti, arpeggio’s, loopjes, gallop, echo — en 39 korte motiefjes (3–5 noten, hooguit twee tellen) naar Bach, Pachelbel, Vivaldi, Händel, Haydn, Mozart, Beethoven, Paganini, Rossini, Chopin, Verdi, Wagner, Strauss, Brahms, Moessorgski, Bizet, Grieg, Tsjaikovski, Dvořák, Satie), K2 snelheid (½×, 1×, 2×), K3 gate (10–100 %), K4 HOLD / ONCE / LATCH, ALGORITHM tempo. Volgt MIDI-clock als die binnenkomt. Elke noot heeft een eigen velocity (zware tel sterker, korte noten zachter, accenten en crescendo’s waar ze horen), geschaald met de velocity van je toets |
| ARP (nog eens) | tweede pagina, VELOCITY: los van de frase een velocitypatroon (22: PHRASE = die van de frase zelf, FLAT, ACCENT 2/3/4/5/7, 3 3 2, OFFBEAT, CLAVE, TRIPLET, GHOST, ECHO, RAMP UP/DOWN, SWELL, WAVE, LAST OF 12, SCATTER, LOUD SOFT). Elke gespeelde noot neemt de volgende stap; een patroon met een andere lengte dan de frase verschuift er dus tegenaan. K1 patroon, K2 diepte (0–100 %), K3 humanize, K4 STEPS: FREE (loopt door over herhalingen) of LOOP (opnieuw bij elke herhaling). ARP nog eens sluit |
| SEQ | SCALE-menu open/dicht (ook HOME sluit): K1 grondtoon (C–B), K2 toonladder (uit, majeur, mineur, harmonisch en melodisch mineur, de kerktoonladders, pentatonisch majeur/mineur, blues, heletoons, Hongaars mineur, Frygisch dominant). De ARP blijft in de toonladder: noten erbuiten schuiven naar de dichtstbijzijnde toon erin (bij gelijke afstand de lagere). Lampje aan zolang er een toonladder gekozen is. Grondtoon en toonladder worden altijd onthouden, ook zonder SAVE |
| LFO | LFO-menu open/dicht (ook HOME sluit): K1 snelheid, K2 vibrato (tot ±60 cent), K3 tremolo, K4 inloopvertraging (0–3 s). De LFO werkt zodra vibrato of tremolo boven 0 staat (lampje aan, "LFO" op het scherm) |
| in het LFO-menu: SELECT | FREE (K1 in Hz, 0,3–16) of TEMPO (K1 een notenwaarde: 4 maten … 1/32, met punt- en triolen) |
| in het LFO-menu: ALGORITHM | intern tempo, 40–300 BPM |
| MIDI-clock in (DIN/TRS, USB, Bluetooth) | neemt het tempo over zolang hij loopt ("MIDI 121 BPM"); de LFO loopt vast op de tikken, Start = op de tel |
| MIDI CC 1 (modwiel) | vibrato, ook met de LFO uit |
| OCT- / OCT+ | octaaf −3..+3 |
| SAVE | klank, attack, release, room, transient, tail, LFO, ARP en octaaf bewaren (terug na herstart) |
| MASTER | volume |
| MIDI in (DIN/TRS, USB, Bluetooth) | noten op elk kanaal, CC 120/123 = alles uit |

## Klank

Elke noot bestaat uit drie losse delen, zoals een orkest-ROMpler:

- **TRANSIENT** – de aanzet: de opname min de aanhoudende toon (strijkstok, lip, tong), los opgeslagen. Een klank
  kan de transient van een ander instrument lenen: MARCATO = vioolsectie met de aanzet van SPICCATO.
- **SUSTAIN** – een stabiele loop die tijdens de aanzet op een cosinus inzwelt; transient + sustain = de opname.
- **RELEASE** – bij loslaten een opgenomen uitklinker van het instrument (strijkers, hout, koper), equal-power
  overgenomen van de loop.

Bij TRANSIENT en TAIL op 64 klinkt de noot zoals opgenomen. De aanhoudende toon daarna is stabiel: geen vibrato, geen zwelling.
Blazers, contrabas en soloviool krijgen een loop die opnieuw is opgebouwd uit de boventonen van de opname (hun
gemiddelde sterkte, een exact geheel aantal periodes); de strijkerssecties ook, met wat ensemble-glans op de hoge
boventonen. Blazers en contrabas komen uit VSCO's opnames zonder vibrato. Beweging voeg je zelf toe met de LFO.

## Hoe het in elkaar zit

| Pad | Wat |
| --- | --- |
| `sdk/` | de FM1 SDK zoals M-VAVE hem levert (binaire engine, headers, boot- en updatebestanden); ongewijzigd |
| `app/board/`, `app/audio_output.*` | FM1 Open's board-code; `audio.c` laat de audio-interrupt het orkest renderen |
| `app/orkest_player.c` | de speler: IMA ADPCM, transient + sustain-loop + release, lagen, envelope, 24 stemmen, galm (alles in SRAM) |
| `app/orkest_bank.c` | de klankbank in de gebruikersflash: controle (CRC, commit-woord) en de SysEx-upload |
| `app/orkest_ui.c` | scherm (M-VAVE's 5x7-font), toetsen, knoppen, LEDs, instellingen |
| `app/orkest_sdk.c` | MIDI in, SysEx, en de haken die de SDK van een app verwacht |
| `tools/gen_phrases.py` | de ARP-frases in nootnamen → `app/orkest_phrases.inc` |
| `tools/gen_bank.py` | de klankbank uit `../assets/samples-cc0` (via `../tools/gen_orkest.py`) |
| `tools/build_open.py` | bouwen op Linux: een port van M-VAVE's `Build.ps1` en `Package.cs` |
| `tools/make_site_v14.py` | de installatiepagina (`../web/index_v14.html`, `../web/orkbank.js`) |
| `tests/` | host-tests: speler, bank, upload-protocol, en de browser-upload tegen de firmwarecode |

```
python3 tools/gen_bank.py build/orkest-bank.bin
python3 tools/build_open.py . ~/.jieli/toolchain --out build/orkest-v14.fwsc   # JieLi Linux-toolchain
tests/run_tests.sh
```

De klankbank staat los van de firmware in de gebruikersflash (524 KB, nu ~480 KB gebruikt), dus nieuwe klanken
vragen geen nieuwe firmware: alleen stap 2 van de installer.

## Licentie

De app valt onder GPL-3.0 met M-VAVE's linkuitzondering voor de FM1 SDK ([LICENSE.md](LICENSE.md)); de SDK onder
M-VAVE's eigen voorwaarden ([THIRD_PARTY.md](THIRD_PARTY.md)). Code uit Felucca (GPL-3.0 zonder die uitzondering)
zit niet in deze firmware; de installer in de browser is een apart programma. Samples: VSCO 2 CE, CC0.
