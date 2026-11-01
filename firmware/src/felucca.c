/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM-1 Basis, built on Felucca: one compilation unit (the HAL is header-only). Each file uses what the files
 * above it define, so the order matters. Start reading at main.c (boot, main loop), keys.c (the keyboard),
 * synth.c (the sound) and ui.c (screen, LEDs, knobs). */

/* ---------------------------------------------------- build options --- */
/* build.py: FELUCCA_<NAME>=0 / 1 in the environment overrides these. Elsewhere: FELUCCA_OTA_RAMONLY (ota.c). */
#ifndef FELUCCA_FLASH
#define FELUCCA_FLASH 1          /* flash driver + storage.c: the settings (ui.c) */
#endif
#ifndef FELUCCA_OTA
#define FELUCCA_OTA 1            /* M-UPGRADE update entry (how the installers reach the FM-1); needs FELUCCA_FLASH */
#endif
#ifndef FELUCCA_OTA_DRYRUN
#define FELUCCA_OTA_DRYRUN 0     /* 1 = stage, ask "success", then undo: no record, no reset */
#endif
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0            /* USB CDC-ACM serial port (usb.c); 1 needs a cdc_task of your own in the main loop */
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1            /* USB audio input: the master output, 16-bit stereo 44.1 kHz (usb.c) */
#endif
#ifndef FELUCCA_UAC_TONE
#define FELUCCA_UAC_TONE 0       /* bench: the USB input sends test triangles instead of the music */
#endif
#ifndef FELUCCA_UART
#define FELUCCA_UART 1           /* TRS MIDI IN on UART1 / PH8 */
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_900"    /* package identity (build.py: the .fwsc marker string) */
#endif
#ifndef FELUCCA_CPU2
#define FELUCCA_CPU2 1           /* 1: the second core's support is built in (hal/fm1_cpu1.h; never started here: call
                                  * fm1_cpu1_start() at boot to use it) and a core left running by an earlier firmware
                                  * is held at reset. 0 (FELUCCA_CPU2=0 build.py): none of it */
#endif
#define FM1_CPU1_USED FELUCCA_CPU2   /* hal/fm1_sys.h: the second core is built in; every reset holds it first (its
                                      * waiting loop is in RAM the ROM and the SPL use at boot) */
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "v0.1"   /* shown on the screen; build.py --release X.Y */
#endif
#if FELUCCA_OTA && !FELUCCA_FLASH
#error "FELUCCA_OTA needs FELUCCA_FLASH"
#endif

/* ------------------------------------------------------------- HAL --- */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#if FELUCCA_CPU2
#include "fm1_cpu1.h"
#endif
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"

/* ----------------------------------------------------- base, display --- */
#include "libc.c"
#include "lcd.c"
#include "gfx.c"                 /* fonts, palettes, text (build/gen: tools/gen_aa_font.py ...) */

/* --------------------------------------------------------- core, USB --- */
#include "core.h"
#include "usb.c"                 /* USB-MIDI (+ USB audio input), the update SysEx; shared with the loader */
#if FELUCCA_UART
#include "midi_uart.c"
#endif
#include "panel.c"

/* ----------------------------------------------------------- sound --- */
#include "synth.c"
#include "keys.c"
#include "audio.c"

/* -------------------------------------------------- storage, update --- */
#include "fm1_flash.h"
#include "storage_hw.c"
#include "storage.c"
#if FELUCCA_OTA
#include "ota.c"
#include "ota_hw.c"
#endif

/* -------------------------------------------------------- UI, main --- */
#include "ui.c"
#include "main.c"
