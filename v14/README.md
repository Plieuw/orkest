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
| HOME: K1 of PRESETS | laag 1: klank kiezen (24); zet attack en release op die van de klank |
| HOME: K2 / K3 / K4 | klank laag 2 (OFF = geen tweede laag) / MIX van de twee lagen (100/0 .. 50/50 .. 0/100, gelijk vermogen: 50/50 elk −3 dB) / ROOM (galm, ook in GLO). Een toets speelt beide lagen; met ARP aan speelt laag 1 partij 1 en laag 2 partij 2: dezelfde klanken als op de ARP-pagina's. Attack en release staan in ENV |
| ENV | ENV-menu open/dicht (ook HOME sluit): K1 attack, K2 release, K3 TRANSIENT (de aanzet; 64 = zoals opgenomen, 127 = harder, 0 = alleen de aanhoudende toon vanaf de eerste sample), K4 TAIL (het opgenomen uitklinken na loslaten; 0 = uit) |
| ARP | ARP-menu (ook HOME sluit); openen zet de ARP meteen aan. Twee partijen, eenstemmig met voorrang voor de laatste toets: elke toets speelt partij 1 en (als die een klank heeft) partij 2 vanaf zijn noot. ARP bladert: PART 1 → VELOCITY 1 → PART 2 → VELOCITY 2 → dicht. PART-pagina: K1 klank (partij 2 ook OFF), K2 frase (72: 39 korte filmmuziek-cellen en 33 versieringen en patronen, allemaal in natuurlijk mineur op de grondtoon van de toets, zodat twee partijen altijd samen kloppen), K3 snelheid (¼×, ½×, 1×, 2×), K4 octaaf (−2..+2). VELOCITY-pagina: K1 patroon (30, los van de frase; elke noot van de frase de volgende stap, ook over herhalingen heen; een stap 0 is een rust: die noot vervalt — HALF, SKIP 3, STUTTER, SPARSE, EUCLID 3 8, EUCLID 5 8, GAP 4, BREATHE), K2 diepte (0–150 %: boven 100 nog meer contrast), K3 humanize (velocity ±45 en de lengte van elke noot ±35 % gate), K4 gate. Op elke pagina: SELECT aan/uit, ALGORITHM tempo, PRESETS HOLD / ONCE / LATCH. Volgt MIDI-clock |
| (slides) | sommige cellen glijden naar de volgende noot (portamento over de helft ervan); alleen bij instrumenten die dat kunnen: strijkers (niet pizzicato/spiccato) en trombone |
| SCL | toonladder-menu open/dicht (ook HOME sluit): K1 grondtoon (C–B), K2 toonladder (uit, majeur, mineur, harmonisch en melodisch mineur, de kerktoonladders, pentatonisch majeur/mineur, blues, heletoons, Hongaars mineur, Frygisch dominant). De ARP blijft in de toonladder: noten erbuiten schuiven naar de dichtstbijzijnde toon erin (bij gelijke afstand de lagere). Lampje aan zolang er een toonladder gekozen is. Grondtoon en toonladder worden altijd onthouden, ook zonder SAVE |
| GLO | panning-menu open/dicht (ook HOME sluit): K1 SPREAD 0–125 % (0 = alles in het midden), K3 ROOM (galm), K2 SEATS: CONCERT (de gebruikelijke opstelling, gezien vanuit de zaal: eerste violen links, altviolen en celli rechts van het midden, contrabassen rechts; harp links; hout in het midden; hoorns links, trompetten, trombones en tuba rechts; pauken midden achter, glockenspiel rechts) of SOUND (de panning van de klank zelf). Onderaan waar de lagen van de huidige klank staan |
| (LFO, standaard) | staat aan op het tempo (TEMPO, 1/8T: bij 120 BPM 6 Hz), vibrato 40, tremolo 12, inloop 0,2 s. Elk instrument neemt zijn eigen deel: strijkers vibrato (soloviool het meest, contrabas minder), fluit vibrato en ademtremolo, hobo, fagot en koper een beetje; klarinet, harp, pizzicato, spiccato, pauken en glockenspiel niets |
| SEQ | MORPH-menu open/dicht (ook HOME sluit): een AD-envelope op het tempo die bij elke toets van laag 1 naar laag 2 overvloeit (attack) en weer terug (decay), met gelijk vermogen; zo morpht de frase van partij 1 naar die van partij 2 en terug. K1 ATTACK en K2 DECAY (0, 1/16, 1/8, 1–3 tellen, 1 maat, 6 tellen, 2, 3, 4, 8 maten), K3 CURVE (LINEAR, SLOW, FAST, S), K4 MODE (OFF, ONCE, LOOP). Aan: vervangt de MIX van HOME. Het scherm toont waar hij is (1 ---#--- 2); lampje aan zolang hij aan staat |
| LFO | LFO-menu open/dicht (ook HOME sluit): K1 snelheid, K2 vibrato (tot ±60 cent), K3 tremolo, K4 inloopvertraging (0–3 s). De LFO werkt zodra vibrato of tremolo boven 0 staat (lampje aan, "LFO" op het scherm) |
| in het LFO-menu: SELECT | FREE (K1 in Hz, 0,3–16) of TEMPO (K1 een notenwaarde: 4 maten … 1/32, met punt- en triolen) |
| in het LFO-menu: ALGORITHM | intern tempo, 40–300 BPM |
| MIDI-clock in (DIN/TRS, USB, Bluetooth) | neemt het tempo over zolang hij loopt ("MIDI 121 BPM"); de LFO loopt vast op de tikken, Start = op de tel |
| MIDI CC 1 (modwiel) | vibrato, ook met de LFO uit |
| OCT- / OCT+ | octaaf −3..+3 |
| SAVE | klank, attack, release, room, transient, tail, LFO, ARP en octaaf bewaren (terug na herstart) |
| MASTER | volume |
| MIDI in (DIN/TRS, USB, Bluetooth) | maximaal 12 stemmen en 12 noten tegelijk (de nieuwste wint: de oudste noot stopt snel); noten op elk kanaal, CC 120/123 = alles uit. Velocity stuurt volume én klankkleur: zachte noten doffer, harde helderder |

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
