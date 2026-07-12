/*
 * QTest for the i.MX93 FlexIO block.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The FlexIO is modelled as the register file the nxp,imx-flexio MFD and the
 * i2c-flexio driver program so the extra I2C adapter registers. This test
 * confirms it is a real block, not an unimplemented stub: VERID/PARAM read back
 * the identification words, CTRL's software-reset bit self-clears (and wipes
 * the fabric), and the shifter/timer config registers latch writes.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define FLEXIO_BASE     0x425c0000ULL

#define FLEXIO_VERID    0x00
#define FLEXIO_PARAM    0x04
#define FLEXIO_CTRL     0x08
#define   CTRL_SWRST    0x2
#define SHIFTCTL_0      0x80
#define SHIFTCFG_0      0x100
#define TIMCMP_0        0x500

#define VERID_VALUE     0x02010000
#define PARAM_VALUE     0x04200808

static void test_flexio(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");

    /* Identification: a real block, not a zero-returning stub. */
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + FLEXIO_VERID), ==,
                    VERID_VALUE);
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + FLEXIO_PARAM), ==,
                    PARAM_VALUE);

    /* Shifter/timer config registers latch (the driver programs these). */
    qtest_writel(qts, FLEXIO_BASE + SHIFTCFG_0, 0x00000101);
    qtest_writel(qts, FLEXIO_BASE + SHIFTCTL_0, 0x01230003);
    qtest_writel(qts, FLEXIO_BASE + TIMCMP_0, 0x0000003b);
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + SHIFTCFG_0), ==, 0x00000101);
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + SHIFTCTL_0), ==, 0x01230003);
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + TIMCMP_0), ==, 0x0000003b);

    /* Software reset self-clears and wipes the fabric (flexio_sw_reset). */
    qtest_writel(qts, FLEXIO_BASE + FLEXIO_CTRL, CTRL_SWRST);
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + FLEXIO_CTRL) & CTRL_SWRST,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, FLEXIO_BASE + SHIFTCTL_0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/flexio/regs", test_flexio);
    return g_test_run();
}
