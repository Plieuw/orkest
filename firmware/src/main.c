/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Boot and main loop. Boot order: WDT first, boot-loop guard, fatal vectors, guards; then LCD, input (TIMER5 IRQ,
 * 10 kHz), audio (ALNK0 IRQ). Keep the safety nets in this file when you change it:
 *   - the watchdog (fm1_wdt_feed in the loop) and the boot-loop guard (two failed boots in a row -> UBOOT),
 *   - OCT- + OCT+ held 5 s -> UBOOT (update mode), and SysEx F0 22 24 35 7D F7 from the host does the same,
 *   - the M-UPGRADE update entry (ota_service / ota_session): it is how tools/fm1_install.py and the web
 *     installer put the next build on the FM-1 without UBOOT. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
#if FELUCCA_CPU2
extern uint32_t _c1_start[], _c1_end[], _c1_load[];
#endif

/* TIMER5 (10 kHz): the key / LED scan, the millisecond clock, USB and TRS MIDI polls. It outranks the audio
 * interrupt, so the scan keeps its pace while a half buffer renders; nested in it, it only scans and counts
 * (and serves the USB audio stream); the USB and UART polls wait for the first tick after the render. */
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    felucca_dbg.timer_irqs++;
    fm1_input_tick();
    {   /* milliseconds from the 24 MHz TIMER4 (robust to a late tick) */
        static uint32_t last, acc;
        acc += t0 - last;
        last = t0;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
    if (usb_due)
        owed |= 1u;                             /* 2 kHz: all USB SIE traffic lives here */
#if FELUCCA_UART
    if (sub % 5u == 2u)
        owed |= 2u;                             /* 2 kHz: <= ~7 bytes per call at 31250 baud */
#endif
    if (++sub == 10u)
        sub = 0;
    if (felucca_dbg.in_audio) {
        felucca_dbg.nested++;
#if FELUCCA_UAC
        if (usb_due)
            uac_service();
#endif
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
#if FELUCCA_UART
    if (owed & 2u)
        uart_midi_poll();
#endif
    owed = 0;
}
extern void isr_timer5(void);

static void timer5_start(void)                 /* OSC /4 = 6 MHz, PRD 600 -> 10 kHz */
{
    fm1_timer5_start(isr_timer5, 4);           /* above ALNK0 (3): the scan nests into the render */
}

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

/* a CPU fault (hal/fm1_irq.h): its vector and registers on the screen for 4 s, then a reset */
static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_wake_now();
    lcd_fill(0, 0, 240, 240, UI_CRASH_BG);
    draw_text_line(0, 8, 240, &AF_M, "CRASH", UI_CRASH_INK, UI_CRASH_BG, 1);
    hexs(b, c->vec);
    draw_text_line(10, 40, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->pc);
    draw_text_line(10, 60, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->emu);
    draw_text_line(10, 84, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->dbg);
    draw_text_line(10, 102, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->rets);
    draw_text_line(10, 120, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

/* MASTER pot: smoothed per UI frame; a square law (the knob's middle is about -12 dB) */
static int32_t master_knob = 512 * 16;
static uint32_t master_of_pot(uint32_t k10) { return (k10 * k10) >> 8; }   /* 0..1023 -> 0..4088 */
static void master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        master_knob += (a * 16 - master_knob) / 8;
        sys.master_q12 = (uint16_t)master_of_pot((uint32_t)(master_knob / 16));
    }
}
static void master_boot(void)                   /* before the audio: the first block plays at the knob's level */
{
    uint32_t i;
    (void)fm1_adc_read(FM1_ADC_MASTER);         /* (the first conversion after the pin setup: dropped) */
    for (i = 0; i < 3u; i++) {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            master_knob = a * 16;
            sys.master_q12 = (uint16_t)master_of_pot((uint32_t)a);
            return;
        }
    }
}

static void enter_uboot(const char *why)        /* intentional reset into update mode (UBOOT) */
{
    fm1_audio_stop();
    lcd_wake_now();
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 110, 240, &AF_M, why, T_THEME, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                      /* intentional reset: not a failed boot */
    fm1_enter_uboot();
}

/* OCT- + OCT+ held: a countdown from 2 s, UBOOT at 5 s; letting go cancels it */
static void uboot_combo(void)
{
    static uint32_t t0, shown;
    uint32_t both = btn_bit(B_OCTDN) | btn_bit(B_OCTUP), left;
    if ((fm1_in.buttons & both) != both) {
        if (shown) {
            shown = 0;
            ui_force = 1;
            ui_say("UPDATE MODE CANCELLED");
        }
        t0 = fm1_ms;
        return;
    }
    if (fm1_ms - t0 > 5000u)
        enter_uboot("UBOOT");
    if (fm1_ms - t0 <= 2000u)
        return;
    left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
    if (left != shown) {
        char d[24] = "UPDATE MODE IN  ";
        d[16] = (char)('0' + left);
        d[17] = 0;
        shown = left;
        lcd_fill(0, 0, 240, 240, T_BG);
        draw_text_box(0, 100, 240, &AF_M, d, T_THEME, 1);
        draw_text_box(0, 130, 240, &AF_S, "LET GO TO CANCEL", T_MID, 1);
    }
}

static uint32_t crash_seen __attribute__((section(".noinit")));   /* fm1_crash.count at the last boot */

static void fm1_main(void)
{
    uint32_t ms, boot_clean;
    settings_boot();                                    /* flash check, the saved settings */
#if FELUCCA_OTA
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
#endif
    palette_set(UI_GREY_INDEX);                         /* gfx.c / build/gen/ui_palettes.h: GREY GREEN AMBER ICE .. */
    lcd_init();
    draw_splash();
    if (felucca_dbg.magic != DBG_MAGIC) {
        memset(&felucca_dbg, 0, sizeof felucca_dbg);
        felucca_dbg.magic = DBG_MAGIC;
    }
    felucca_dbg.boots++;
    felucca_dbg.max_us = 0;
    felucca_dbg.in_audio = 0;                       /* .noinit: a reset inside the audio ISR left it set */
    felucca_dbg.prev_stage = felucca_dbg.stage;     /* a WDT reset leaves the last breadcrumb here */
    felucca_dbg.prev_rst = fm1_boot.p3_rst;
    fm1_input_init();
    fm1_adc_init();
    keys_init();
    master_boot();
    kb_boot_hold = 1;                               /* no notes from the keys under the splash */
    audio_init();
    usb_start();
#if FELUCCA_UART
    uart_midi_init();
#endif
    {   /* the power-on LED sweep, not after a crash, a hang (WDT) or a failed boot */
        uint32_t crashed = fm1_crash.magic == FM1_CRASH_MAGIC && fm1_crash.count != crash_seen;
        crash_seen = fm1_crash.count;
        boot_clean = !crashed && !bootguard.failed && !(fm1_boot.p3_rst & 4u);
        ui_leds_init();
        fm1_led_dim_level(0);
        if (boot_clean)
            fm1_led_anim_start(1);
    }
    timer5_start();
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    for (ms = 0; ms < 600u; ms += 10u) {            /* the splash; MIDI IN already plays */
        fm1_delay_ms(10);
        master_poll();
    }
    kb_boot_hold = 0;
    ui_force = 1;

    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {
            int32_t b = fm1_adc_read(FM1_ADC_BATT);     /* battery: slow IIR */
            if (b > 0)
                sys.batt_raw = sys.batt_raw ? sys.batt_raw + (b - sys.batt_raw) / 32 : b;
        }
        master_poll();
        uboot_combo();
#if FELUCCA_OTA
        ota_service();                                  /* M-UPGRADE handshake */
        if (usb.ota_req) {                              /* M-UPGRADE upgrade command */
            usb.ota_req = 0;
            keys_panic_req = 1;
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            ui_force = 1;
        }
#endif
        if (usb.uboot_req)                              /* SysEx F0 22 24 35 7D F7 from the host */
            enter_uboot("UBOOT (USB)");
        felucca_dbg.ui_frames++;
        felucca_dbg.stage = 1;
        ui_input();
        felucca_dbg.stage = 2;
        ui_leds();
        if (!(fm1_in.buttons & btn_bit(B_OCTDN) && fm1_in.buttons & btn_bit(B_OCTUP)))
            ui_draw();                                  /* (the countdown has the screen while both are held) */
        felucca_dbg.stage = 9;
        while (fm1_ms - m < 15u) {                      /* ~60 UI frames/s at most */
#if FELUCCA_OTA
            ota_service();
#endif
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
#if FELUCCA_CPU2
    fm1_cpu1_halt();                           /* a second core still running from before this reset (another firmware)
                                                * would read the RAM cleared below */
#endif
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* flash driver code that must run from RAM */
#if FELUCCA_CPU2
    for (s = _c1_load, d = _c1_start; d < _c1_end; s++, d++)
        *d = *s;                                /* the second core's waiting loop (hal/fm1_cpu1.h) */
#endif
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
