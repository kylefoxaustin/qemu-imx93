/*
 * QTest for i.MX 93 device-model reset values (SAR_ADC and ULP WDOG).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A reset value is a claim the model makes on the silicon's behalf: the
 * guest can read it, read-modify-write it, and launder it back into its own
 * configuration. These cases pin the post-reset register reads to the values
 * the i.MX 93 RM specifies, so a future edit cannot silently swap in a
 * fabricated default. The WDOG case also exercises the UNLOCK/enable/re-lock
 * handshake to prove the reset value coexists with the working driver flow.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

/* i.MX 93 ADC1 and WDOG1 register bases. */
#define ADC1    0x44530000
#define WDOG1   0x442d0000

/* SAR_ADC registers. */
#define ADC_MCR    0x00
#define ADC_MSR    0x04

/* ULP WDOG registers. */
#define WDOG_CS    0x00
#define WDOG_CNT   0x04
#define WDOG_TOVAL 0x08

#define WDOG_CS_EN      (1u << 7)
#define WDOG_CS_ULK     (1u << 11)
#define WDOG_CS_RCS     (1u << 10)
#define WDOG_CS_CMD32EN (1u << 13)

/* Single 32-bit UNLOCK key (CMD32EN path). */
#define WDOG_UNLOCK 0xd928c520u

static void test_adc_reset(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -accel qtest");

    /* RM SAR_ADC reset: MCR = 0x0000_3901, MSR = 0x0000_0001. */
    g_assert_cmphex(qtest_readl(qts, ADC1 + ADC_MCR), ==, 0x00003901);
    g_assert_cmphex(qtest_readl(qts, ADC1 + ADC_MSR), ==, 0x00000001);

    qtest_quit(qts);
}

static void test_wdog_reset(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -accel qtest");

    /* RM ULP WDOG reset: CS = 0x0000_2900 (disabled, unlocked window,
     * CMD32EN, LPO clock), TOVAL = 0x0000_0400. */
    g_assert_cmphex(qtest_readl(qts, WDOG1 + WDOG_CS), ==, 0x00002900);
    g_assert_cmphex(qtest_readl(qts, WDOG1 + WDOG_TOVAL), ==, 0x00000400);

    /* Reset value must coexist with the driver flow: UNLOCK, set TOVAL,
     * UNLOCK again, then enable. The controller re-locks on the CS write
     * (ULK clears) and acknowledges the reconfigure (RCS sets). */
    qtest_writel(qts, WDOG1 + WDOG_CNT, WDOG_UNLOCK);
    qtest_writel(qts, WDOG1 + WDOG_TOVAL, 0x1000);
    g_assert_cmphex(qtest_readl(qts, WDOG1 + WDOG_TOVAL), ==, 0x1000);

    qtest_writel(qts, WDOG1 + WDOG_CNT, WDOG_UNLOCK);
    qtest_writel(qts, WDOG1 + WDOG_CS, WDOG_CS_CMD32EN | WDOG_CS_EN);

    uint32_t cs = qtest_readl(qts, WDOG1 + WDOG_CS);
    g_assert_true(cs & WDOG_CS_EN);     /* watchdog now enabled */
    g_assert_true(cs & WDOG_CS_RCS);    /* reconfigure acknowledged */
    g_assert_false(cs & WDOG_CS_ULK);   /* re-locked after the CS write */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/reset/adc", test_adc_reset);
    qtest_add_func("/imx93/reset/wdog", test_wdog_reset);
    return g_test_run();
}
