/*
 * QTest for the i.MX93 LPI2C controllers, focused on the expansion buses.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The EVK only wires three of the eight LPI2C controllers (1/2/8); the other
 * five (LPI2C3-7) used to be logging stubs. They are now real controllers so a
 * non-EVK board/DTB can host I2C peripherals on them. This test confirms all
 * eight respond as real hardware - PARAM reads back the FIFO-size identity
 * (0x0404, vs 0 from an unimplemented stub) and MCR latches the enable bit -
 * which a write-discarding stub region cannot do.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define LPI2C_PARAM     0x04
#define LPI2C_MCR       0x10
#define MCR_MEN         (1u << 0)

#define PARAM_VALUE     0x0404

static const struct {
    const char *name;
    uint64_t base;
} lpi2c[8] = {
    { "lpi2c1", 0x44340000 },
    { "lpi2c2", 0x44350000 },
    { "lpi2c3", 0x42530000 },   /* expansion */
    { "lpi2c4", 0x42540000 },   /* expansion */
    { "lpi2c5", 0x426b0000 },   /* expansion */
    { "lpi2c6", 0x426c0000 },   /* expansion */
    { "lpi2c7", 0x426d0000 },   /* expansion */
    { "lpi2c8", 0x426e0000 },
};

static void test_controllers(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t param, mcr;

        /* PARAM identity: a real controller, not a zero-returning stub. */
        param = qtest_readl(qts, lpi2c[i].base + LPI2C_PARAM);
        if (param != PARAM_VALUE) {
            g_test_message("%s PARAM = 0x%x, want 0x%x",
                           lpi2c[i].name, param, PARAM_VALUE);
        }
        g_assert_cmphex(param, ==, PARAM_VALUE);

        /* MCR latches: the register file is live (stubs discard writes). */
        qtest_writel(qts, lpi2c[i].base + LPI2C_MCR, MCR_MEN);
        mcr = qtest_readl(qts, lpi2c[i].base + LPI2C_MCR);
        g_assert_cmphex(mcr & MCR_MEN, ==, MCR_MEN);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/lpi2c/controllers", test_controllers);
    return g_test_run();
}
