/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca update loader (device side). Runs from RAM, started by the SPL from
 * an update record, with interrupts off: USB is polled from the main loop.
 * It is a USB-MIDI device "Felucca Update" (1209:0002) that answers the
 * M-UPGRADE update protocol as "ota-FM-1_900" (hosts look for "ota-"), and
 * writes the app area only (ldr_core.c). The UBOOT soft key works here too.
 * Single compilation unit. */
#include <stdint.h>
#define FELUCCA_LOADER 1
#define FELUCCA_CDC 0
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_USB_PID 0x0002
#define FELUCCA_ID "ota-FM-1_900"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fm1_time.h"
#include "fm1_sys.h"
/* the loader may write the app area and erase update records; never the head, nor Felucca's store
 * [0x97000, 0xE0000) (user data, never a record: ldr_core.c) */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0x4000u, 0x97000u) || FL_IN(off, n, 0xE0000u, 0xFC000u))
#include "fm1_flash_ldr.h"                  /* (hal/: the BSP's fm1_flash.h + the download-mode start) */
#include "../bsp/chip/libc.c"
#include "../bsp/chip/usb.c"

static uint32_t ldr_ms(void)   /* monotonic ms; survives the TIMER4 wrap (178.9 s) */
{
    static uint32_t last, acc, ms;
    uint32_t now = fm1_ticks();
    acc += now - last;
    last = now;
    ms += acc / (1000u * FM1_TICKS_PER_US);
    acc %= 1000u * FM1_TICKS_PER_US;
    return ms;
}

static void ldr_poll(void)                          /* USB at ~2 kHz, watchdog */
{
    static uint32_t last;
    uint32_t now = fm1_ticks();
    fm1_wdt_feed();
    if (now - last >= 500u * FM1_TICKS_PER_US) {
        last = now;
        usb_poll();
    }
}

/* ---- ota.c hooks (the SysEx frame hooks are in usb.c) ---- */
static uint32_t ota_now_ms(void) { return ldr_ms(); }
static void ota_idle(void) { ldr_poll(); }
/* step-1 hooks of ota.c: not used by the loader */
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { return fl_read_ram(off, p, n); }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../bsp/chip/ota.c"

/* ---- ldr_core.c hooks: interrupts are off for the whole loader ---- */
static int ldr_fread(uint32_t off, void *p, uint32_t n) { return fl_read_ram(off, p, n); }
static int ldr_erase(uint32_t off)
{
    uint32_t took;
    return fl_erase4k_ram(off, &took);
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    uint32_t took;
    return fl_prog_ram(off, p, n, &took);
}
static void ldr_record_clear(void)
{
    fm1_updata_parm_clear();                        /* CRC 0: the SPL ignores it on a warm reset */
}
static void ldr_progress(uint32_t done, uint32_t total) { (void)done; (void)total; ldr_poll(); }
#include "ldr_core.c"

/* tools/fm1_rescue.py sends this image straight to the mask ROM's UBOOT mode (no SPL, no update record, no staged copy
 * at 0xE0000) and sets the last byte to '1' in what it sends. Started that way:
 *  - a loader no installer talks to within LDR_ROM_WAIT_MS (no upgrade request) reboots into the app (untouched), instead
 *    of waiting with the watchdog fed until the battery is flat;
 *  - before the session, the flash: its pins set up from reset (the ROM leaves them; hal/fm1_flash.h fl_rom_pins), then
 *    a write test on the first sector of the update staging area (0xE0000: nothing of the app, nothing stored);
 *  - a session that fails before its first erase reboots into the app after its report; one that fails after it keeps
 *    the loader up, as always: the install is run again.
 * Started by the SPL the byte stays '0'. Any failed session reports to the host (ldr_tell). */
static volatile const char ldr_rom_start[16] = "SHD-ROMSTART2-0";
#define LDR_ROM_WAIT_MS 60000u
#define LDR_TEST_SEC 0xE0000u

/* the report: 0..8 port D and SPI0 as found (fl_regs), 9 flash id, 10 pin setting, 11 rc, 12..13 the head at 32 */
enum { LDR_D_ID = 9, LDR_D_PINS, LDR_D_RC, LDR_D_HEAD, LDR_D_N = 14 };
static uint32_t ldr_diag[LDR_D_N];

static int ldr_rom_flash(void)
{
    static uint8_t b[256];
    uint32_t i, id;
    int pins = fl_rom_pins(&id);
    ldr_diag[LDR_D_ID] = id;
    ldr_diag[LDR_D_PINS] = (uint32_t)pins;
    if (pins < 0)
        return -51;                                 /* no flash answers on any pin setting */
    if (ldr_erase(LDR_TEST_SEC))
        return -52;
    for (i = 0; i < 256u; i++)
        b[i] = (uint8_t)(i * 7u + 1u);
    if (ldr_prog(LDR_TEST_SEC, b, 256))
        return -53;
    for (i = 0; i < 256u; i++)
        b[i] = 0;
    if (ldr_fread(LDR_TEST_SEC, b, 256))
        return -54;
    for (i = 0; i < 256u; i++)
        if (b[i] != (uint8_t)(i * 7u + 1u))
            return -55;                             /* written, read back other */
    return ldr_erase(LDR_TEST_SEC) ? -56 : 0;
}

/* a failed session, told to the host: read requests at 0xD0kk_vvvv (kk = 2 i (low half), 2 i + 1 (high), FF the end),
 * which tools/fm1_install.py answers and prints; an older host stops at the first */
static void ldr_tell(int rc)
{
    uint8_t b[8];
    uint32_t i, v;
    ldr_diag[LDR_D_RC] = (uint32_t)rc;
    if (!ldr_fread(32, b, 8))
        ldr_diag[LDR_D_HEAD] = ota_rd32(b), ldr_diag[LDR_D_HEAD + 1] = ota_rd32(b + 4);
    for (i = 0; i < LDR_D_N; i++) {
        v = ldr_diag[i];
        if (ota_read(0xD0000000u | (2u * i) << 16 | (v & 0xFFFFu), b, 4) ||
            ota_read(0xD0000000u | (2u * i + 1u) << 16 | v >> 16, b, 4))
            return;
    }
    (void)ota_read(0xD0FF0000u, b, 4);
}

extern uint32_t _bss_start[], _bss_end[];

void ldr_main(void)
{
    uint32_t talked = 0, rom = ldr_rom_start[14] == '1';
    int rc, flash = 1;                              /* (ROM start) 1 = not set up yet, else ldr_rom_flash's */
    fm1_wdt_arm(0x0D);                              /* ~8 s, fed by ldr_poll */
    fm1_time_init();
    fl_regs(ldr_diag);                              /* (as found: read only) */
    usb_start();
    for (;;) {
        ldr_poll();
        usb_retry(ldr_ms());
        if (rom && !talked && ldr_ms() > LDR_ROM_WAIT_MS) {
            usb_detach();
            fm1_delay_ms(30);
            fm1_reboot();
        }
        if (usb.uboot_req) {                        /* soft key: mask-ROM UBOOT, as in Felucca */
            usb_detach();
            fm1_delay_ms(30);
            fm1_enter_uboot();
        }
        ota_service();                              /* handshake */
        if (usb.ota_req) {
            talked = 1;
            usb.ota_req = 0;
            if (rom && flash > 0)
                flash = ldr_rom_flash();
            rc = rom && flash ? flash : ldr_session();
            if (rc == 0) {
                fm1_delay_ms(200);                  /* the "success" reply leaves */
                usb_detach();
                fm1_delay_ms(30);
                fm1_core_reset();                   /* the SPL boots the new app */
            }
            ldr_tell(rc);
            if (rom && (rc < -11 || rc > -8)) {     /* nothing of the app erased (-8..-11: the write began) */
                fm1_delay_ms(200);
                usb_detach();
                fm1_delay_ms(30);
                fm1_reboot();
            }
        }
    }
}

void ldr_cstart(void)
{
    uint32_t *p;
    ldr_record_clear();                             /* first thing: a crash below cannot loop through a
                                                     * warm reset; resume after power loss uses the flash record */
    for (p = _bss_start; p < _bss_end; p++)
        *p = 0;
    ldr_main();
}
