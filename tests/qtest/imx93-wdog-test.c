/*
 * QTest for the i.MX93 ULP watchdog deadline timing.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * imx93-reset-test pins the WDOG reset VALUES and the unlock/enable handshake;
 * this pins WHEN it fires. The fsl-imx93-wdt driver always enables the /256
 * prescaler and programs TOVAL = 125 * seconds, expecting the counter to tick
 * at 32000/256 = 125 Hz. The model shipped with the imx7ulp constant (1000 Hz),
 * so the prescaled rate was 1000/256 = 3.9 Hz and a 2 s watchdog only fired
 * after ~8.5 minutes - 256x too late. A reset-value test cannot see that; only
 * advancing the clock to the deadline can. Drive the guest's exact programming
 * sequence, step past the intended deadline, and require the watchdog to have
 * fired - which it does at 2 s with the correct 32 kHz LPO and NOT within any
 * reasonable window with the wrong one.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define WDOG1       0x442d0000ULL
#define WDOG_CS     0x00
#define WDOG_CNT    0x04
#define WDOG_TOVAL  0x08

#define CS_EN       (1u << 7)
#define CS_CLK_LPO  (1u << 8)
#define CS_PRES     (1u << 12)
#define CS_CMD32EN  (1u << 13)

#define WDOG_UNLOCK 0xd928c520u

/* fsl-imx93-wdt: prescaler_enable=true, wdog_clock_rate=125 -> the prescaled
 * counter must tick at 125 Hz, so TOVAL = 125 * timeout_seconds. */
#define WDOG_CLK_HZ 125
#define NS_PER_S    1000000000LL

static void test_wdog_deadline(void)
{
    /*
     * -action watchdog=none: the watchdog emits the QMP WATCHDOG event without
     * resetting the qtest machine, so we can observe that it fired.
     */
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -accel qtest "
                                 "-action watchdog=none");
    uint32_t timeout_s = 2;
    uint32_t toval = WDOG_CLK_HZ * timeout_s;   /* exactly what the driver writes */
    QDict *ev, *data;

    /* Program as fsl-imx93-wdt does: unlock, TOVAL, unlock, enable with the
     * /256 prescaler and the LPO clock source set. */
    qtest_writel(qts, WDOG1 + WDOG_CNT, WDOG_UNLOCK);
    qtest_writel(qts, WDOG1 + WDOG_TOVAL, toval);
    qtest_writel(qts, WDOG1 + WDOG_CNT, WDOG_UNLOCK);
    qtest_writel(qts, WDOG1 + WDOG_CS,
                 CS_CMD32EN | CS_EN | CS_PRES | CS_CLK_LPO);

    /*
     * Correct: 32000/256 = 125 Hz, TOVAL 250 -> fires at 2 s. Step to 3 s, past
     * the intended deadline but far short of the ~512 s the imx7ulp constant
     * would give, and the watchdog must have fired. Reverting WDOG_HZ to 1000
     * makes this time out - the bracket that a reset-value test cannot.
     */
    qtest_clock_step(qts, 3 * NS_PER_S);

    ev = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    data = qdict_get_qdict(ev, "data");
    g_assert_cmpstr(qdict_get_str(data, "action"), ==, "none");
    qobject_unref(ev);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/wdog/deadline", test_wdog_deadline);
    return g_test_run();
}
