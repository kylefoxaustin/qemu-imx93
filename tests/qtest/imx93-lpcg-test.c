/*
 * QTest for i.MX93 LPCG clock gating reaching a consumer (TPM2).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The LPCG DIRECT bit used to be storage the gating never reached: a block whose
 * gate the guest cleared kept ticking, because the CCM fed the peripheral a raw
 * clock. Now the CCM feeds TPM2 a clock GATED by its own LPCG (LPCG45_DIRECT at
 * CCM+0x8b40, RM reset 0x1), so clearing DIRECT stops exactly that block. TPM2
 * has a readable free-running counter: with the gate on it advances; clear the
 * gate and the counter FREEZES AND HOLDS its value (no clock, no tick - not a
 * reset to 0, which a real gated counter never does); set the gate again and it
 * resumes from where it stopped. A model that ignores the gate keeps counting,
 * and the "held == last value" assertions below go red.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/timer.h"

#define TPM2_BASE       0x44320000ULL
#define TPM_SC          0x10
#define TPM_CNT         0x14
#define TPM_MOD         0x18
#define SC_CMOD_1       (1u << 3)    /* clock mode 01: module clock, PS = 0 */

/* CCM LPCG45_DIRECT gates TPM2 (ccm@44450000 + 0x8b40, bit0 = clock on). */
#define CCM_TPM2_DIRECT 0x44458b40ULL
#define GATE_ON         0x1u

#define MS  (NANOSECONDS_PER_SECOND / 1000)

static void test_gate_stops_tpm(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    uint32_t a, b, c, d, e;

    /* Sanity: the gate comes up RUNNING out of reset (DIRECT = 1, per RM). */
    g_assert_cmphex(qtest_readl(qts, CCM_TPM2_DIRECT) & GATE_ON, ==, GATE_ON);

    /* A wide period so 24 MHz x a few ms never wraps, and enable the counter. */
    qtest_writel(qts, TPM2_BASE + TPM_MOD, 0xffff);
    qtest_writel(qts, TPM2_BASE + TPM_SC, SC_CMOD_1);

    /* Gate ON: the counter advances with virtual time. */
    qtest_clock_step(qts, MS);
    a = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    qtest_clock_step(qts, MS / 2);
    b = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    g_assert_cmpuint(a, >, 0);
    g_assert_cmpuint(b, >, a);                  /* still ticking */

    /* Clear the gate: the block loses its clock and the count HOLDS at b. */
    qtest_writel(qts, CCM_TPM2_DIRECT, 0);
    qtest_clock_step(qts, MS);
    c = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    qtest_clock_step(qts, MS);
    d = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    g_assert_cmpuint(c, ==, b);                 /* frozen, not advancing */
    g_assert_cmpuint(d, ==, b);                 /* and not drifting to 0 */

    /* Re-open the gate: the clock returns and the counter resumes from b. */
    qtest_writel(qts, CCM_TPM2_DIRECT, GATE_ON);
    qtest_clock_step(qts, MS / 2);
    e = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    g_assert_cmpuint(e, >, b);                  /* advanced again, held its base */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/lpcg/gate-stops-tpm", test_gate_stops_tpm);
    return g_test_run();
}
