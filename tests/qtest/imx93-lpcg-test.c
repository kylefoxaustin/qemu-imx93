/*
 * QTest for i.MX93 LPCG clock gating reaching a consumer (TPM2).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The LPCG DIRECT bit used to be storage the gating never reached: a block whose
 * gate the guest cleared kept ticking, because the CCM fed the peripheral a raw
 * clock. Now the CCM feeds each TPM a clock GATED by its own LPCG (LPCG44..49,
 * DIRECT at CCM+0x8b00 + i*0x40, RM reset 0x1 each), so clearing DIRECT stops
 * exactly that block. Every TPM has a readable free-running counter: with the
 * gate on it advances; clear the gate and the counter FREEZES AND HOLDS its
 * value (no clock, no tick - not a reset to 0, which a real gated counter never
 * does); set the gate again and it resumes from where it stopped. A model that
 * ignores the gate keeps counting, and the "held == last value" assertions go
 * red. Each of TPM1..6 is exercised against its own gate, so a wrong per-TPM
 * LPCG offset (gating the wrong block, or none) also fails.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/timer.h"

#define TPM_SC          0x10
#define TPM_CNT         0x14
#define TPM_MOD         0x18
#define SC_CMOD_1       (1u << 3)    /* clock mode 01: module clock, PS = 0 */

#define CCM_BASE        0x44450000ULL
/* LPCGn_DIRECT = CCM + 0x8000 + n*0x40; tpm(i) uses LPCG(44+i) -> 0x8b00+i*0x40. */
#define CCM_TPM_DIRECT(i)   (CCM_BASE + 0x8b00ULL + (uint64_t)(i) * 0x40)
#define GATE_ON         0x1u

#define MS  (NANOSECONDS_PER_SECOND / 1000)

/* TPM1..6 MMIO bases (fsl_imx93_memmap). */
static const uint64_t tpm_base[6] = {
    0x44310000ULL, 0x44320000ULL, 0x424e0000ULL,
    0x424f0000ULL, 0x42500000ULL, 0x42510000ULL,
};

static void gate_stops_one(QTestState *qts, unsigned i)
{
    uint64_t tpm = tpm_base[i];
    uint64_t gate = CCM_TPM_DIRECT(i);
    uint32_t a, b, c, d, e;

    /* Sanity: the gate comes up RUNNING out of reset (DIRECT = 1, per RM). */
    g_assert_cmphex(qtest_readl(qts, gate) & GATE_ON, ==, GATE_ON);

    /* A wide period so 24 MHz x a few ms never wraps, and enable the counter. */
    qtest_writel(qts, tpm + TPM_MOD, 0xffff);
    qtest_writel(qts, tpm + TPM_SC, SC_CMOD_1);

    /* Gate ON: the counter advances with virtual time. */
    qtest_clock_step(qts, MS);
    a = qtest_readl(qts, tpm + TPM_CNT);
    qtest_clock_step(qts, MS / 2);
    b = qtest_readl(qts, tpm + TPM_CNT);
    g_assert_cmpuint(a, >, 0);
    g_assert_cmpuint(b, >, a);                  /* still ticking */

    /* Clear the gate: the block loses its clock and the count HOLDS at b. */
    qtest_writel(qts, gate, 0);
    qtest_clock_step(qts, MS);
    c = qtest_readl(qts, tpm + TPM_CNT);
    qtest_clock_step(qts, MS);
    d = qtest_readl(qts, tpm + TPM_CNT);
    g_assert_cmpuint(c, ==, b);                 /* frozen, not advancing */
    g_assert_cmpuint(d, ==, b);                 /* and not drifting to 0 */

    /* Re-open the gate: the clock returns and the counter resumes from b. */
    qtest_writel(qts, gate, GATE_ON);
    qtest_clock_step(qts, MS / 2);
    e = qtest_readl(qts, tpm + TPM_CNT);
    g_assert_cmpuint(e, >, b);                  /* advanced again, held its base */
}

static void test_gate_stops_tpm(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    unsigned i;

    for (i = 0; i < 6; i++) {
        gate_stops_one(qts, i);
    }
    qtest_quit(qts);
}

/*
 * The audio consumers are gated the same way: the CCM feeds MICFIL/XCVR a clock
 * that is the root rate when their LPCG is set and 0 when it is clear, and the
 * device freezes on 0. MICFIL is the observable one - a running capture pops the
 * synthesised ramp from DATACH0, but with its PDM LPCG (LPCG107 @ CCM+0x9ac0)
 * cleared there is no clock and a drained FIFO reads 0. So DATACH0 carries a
 * live signal while gated, zero once gated off, and live again on re-gate.
 */
#define MICFIL_BASE     0x44520000ULL
#define MICFIL_CTRL1    0x00
#define MICFIL_DATACH0  0x24
#define CTRL1_PDMIEN    (1u << 29)
#define CCM_PDM_DIRECT  (0x44450000ULL + 0x9ac0ULL)

static int micfil_nonzero_of(QTestState *qts, int n)
{
    int nz = 0, k;

    for (k = 0; k < n; k++) {
        if (qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0) != 0) {
            nz++;
        }
    }
    return nz;
}

static void test_gate_stops_micfil(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");

    /* PDM LPCG comes up set out of reset (RM: LPCG107_DIRECT reset 0x1). */
    g_assert_cmphex(qtest_readl(qts, CCM_PDM_DIRECT) & GATE_ON, ==, GATE_ON);

    /* Enable PDM capture: DATACH0 now yields the synthesised ramp (non-silent). */
    qtest_writel(qts, MICFIL_BASE + MICFIL_CTRL1, CTRL1_PDMIEN);
    g_assert_cmpint(micfil_nonzero_of(qts, 6), >, 0);

    /* Clear the PDM LPCG: no clock, so a drained FIFO reads 0 - capture stops. */
    qtest_writel(qts, CCM_PDM_DIRECT, 0);
    g_assert_cmpint(micfil_nonzero_of(qts, 6), ==, 0);

    /* Re-gate: the clock returns and the ramp resumes. */
    qtest_writel(qts, CCM_PDM_DIRECT, GATE_ON);
    g_assert_cmpint(micfil_nonzero_of(qts, 6), >, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/lpcg/gate-stops-tpm", test_gate_stops_tpm);
    qtest_add_func("/imx93/lpcg/gate-stops-micfil", test_gate_stops_micfil);
    return g_test_run();
}
