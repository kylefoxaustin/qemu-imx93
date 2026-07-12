/*
 * Minimal Cortex-M33 bring-up firmware for the i.MX93 QEMU machine.
 *
 * Proves the modelled M33 fetches its vector table from ITCM and executes:
 * it writes a magic word to DTCM[0] and then bumps a heartbeat counter in
 * DTCM[1] forever. The A55 side sees the same RAM at 0x20200000, so a
 * QEMU-monitor "xp /3xw 0x20200000" shows the magic + a rising counter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 */
#define DTCM_BASE   0x20000000u   /* M33 view of its DTCM */
#define DTCM_TOP    0x20040000u   /* top of the 256 KiB DTCM (initial SP) */
#define MAGIC       0xC0FFEE33u

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vectors[2])(void) = {
    (void (*)(void))DTCM_TOP,   /* [0] initial stack pointer */
    reset_handler,              /* [1] reset vector (Thumb) */
};

void reset_handler(void)
{
    volatile unsigned *dtcm = (volatile unsigned *)DTCM_BASE;
    unsigned c = 0;

    dtcm[0] = MAGIC;
    for (;;) {
        dtcm[1] = ++c;
    }
}
