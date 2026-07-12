/*
 * QTest for the i.MX93 FlexSPI controller + attached SPI-NOR flash.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a READ-ID (9Fh) sequence through the LUT / IP command path and checks
 * the JEDEC id of the board's is25wp064 flash comes back in RFDR, then reads
 * the AHB-mapped (XIP) window and checks an erased flash reads back 0xff.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define FSPI_BASE   0x425e0000
#define FSPI_AHB    0x28000000

#define FSPI_MCR0   0x00
#define FSPI_INTR   0x14
#define FSPI_LUTKEY 0x18
#define FSPI_LCKCR  0x1c
#define FSPI_IPCR0  0xa0
#define FSPI_IPCR1  0xa4
#define FSPI_IPCMD  0xb0
#define FSPI_IPRXFCR 0xb8
#define FSPI_RFDR   0x100
#define FSPI_LUT    0x200

#define LUTKEY_VAL  0x5af05af0
#define IS25WP064_JEDEC 0x17709d    /* 9d 70 17, packed little-endian */

static uint32_t rd(QTestState *q, uint32_t off)
{
    return qtest_readl(q, FSPI_BASE + off);
}

static void wr(QTestState *q, uint32_t off, uint32_t val)
{
    qtest_writel(q, FSPI_BASE + off, val);
}

static void test_read_id(void)
{
    QTestState *q = qtest_init("-machine imx93-11x11-evk -m 4G "
                               "-display none -kernel /dev/null");
    uint16_t cmd = (0x01 << 10) | 0x9f;     /* LUT_CMD, opcode 9Fh */
    uint16_t rd_i = (0x09 << 10) | 0x00;    /* LUT_NXP_READ */
    uint32_t intr, id, ahb;

    wr(q, FSPI_MCR0, 0x1);                   /* software reset */
    wr(q, FSPI_LUTKEY, LUTKEY_VAL);
    wr(q, FSPI_LCKCR, 0x2);                   /* unlock LUT */
    wr(q, FSPI_LUT, ((uint32_t)rd_i << 16) | cmd);   /* seqid 0 */
    wr(q, FSPI_LCKCR, 0x1);                   /* lock LUT */

    wr(q, FSPI_IPCR0, 0);                     /* address 0 */
    wr(q, FSPI_IPCR1, (0 << 16) | 3);         /* seqid 0, 3 data bytes */
    wr(q, FSPI_IPRXFCR, 0x1);                 /* clear rx fifo */
    wr(q, FSPI_IPCMD, 0x1);                   /* trigger */

    intr = rd(q, FSPI_INTR);
    g_assert_cmphex(intr & 0x1, ==, 0x1);     /* IPCMDDONE */

    id = rd(q, FSPI_RFDR) & 0xffffff;
    g_assert_cmphex(id, ==, IS25WP064_JEDEC);

    /* AHB-mapped read of an erased flash returns 0xff bytes. */
    ahb = qtest_readl(q, FSPI_AHB);
    g_assert_cmphex(ahb, ==, 0xffffffff);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx93/flexspi/read-id", test_read_id);
    return g_test_run();
}
