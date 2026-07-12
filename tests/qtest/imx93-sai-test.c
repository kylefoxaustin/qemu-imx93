/*
 * QTest for the i.MX93 SAI (Synchronous Audio Interface) transmit FIFO.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the SAI transmit FIFO the way the fsl-sai driver / eDMA datapath does,
 * with no kernel: sets a watermark, pushes words to TDR0, enables the
 * transmitter, then advances the virtual clock so the SAI clocks words out at
 * the audio word rate. Checks the FIFO fill (via TFR0), the request/warning
 * flags, the FIFO-request interrupt, the underrun error on draining empty, and
 * FIFO reset. Targets SAI3 (the wm8962 card's cpu DAI).
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define SAI3_BASE       0x42660000ULL

#define SAI_VERID       0x00
#define SAI_PARAM       0x04
#define SAI_TCSR        0x08
#define SAI_TCR1        0x0c
#define SAI_TCR3        0x14
#define SAI_TDR0        0x20
#define SAI_TFR0        0x40

#define TCSR_TE         (1u << 31)
#define TCSR_FR         (1u << 25)
#define TCSR_FEF        (1u << 18)
#define TCSR_FWF        (1u << 17)
#define TCSR_FRF        (1u << 16)
#define TCSR_FEIE       (1u << 10)
#define TCSR_FRIE       (1u << 8)

/* Must match SAI_TX_WORD_NS in the model (48 kHz stereo: 96000 words/s). */
#define WORD_NS         (1000000000LL / 96000)

#define WATERMARK       16

static uint32_t rd(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, SAI3_BASE + off);
}

static void wr(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, SAI3_BASE + off, v);
}

static uint32_t fifo_fill(QTestState *qts)
{
    uint32_t tfr = rd(qts, SAI_TFR0);
    return ((tfr >> 16) & 0xffff) - (tfr & 0xffff);   /* wptr - rptr */
}

static void test_tx_fifo(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    uint32_t tcsr;
    int i;

    /* Identification must read back so the driver would probe. */
    g_assert_cmphex(rd(qts, SAI_VERID), ==, 0x03030000);
    g_assert_cmphex(rd(qts, SAI_PARAM), ==, 0x00050704);

    /* Program a watermark and enable the data line. */
    wr(qts, SAI_TCR1, WATERMARK);
    wr(qts, SAI_TCR3, 1u << 16);

    /* Fill the FIFO above the watermark with a known ramp. */
    for (i = 0; i < 64; i++) {
        wr(qts, SAI_TDR0, 0x1000 + i);
    }
    g_assert_cmpuint(fifo_fill(qts), ==, 64);

    /* Above watermark -> no request pending yet. */
    tcsr = rd(qts, SAI_TCSR);
    g_assert_cmphex(tcsr & TCSR_FRF, ==, 0);

    /* Enable the request interrupt and the transmitter. */
    wr(qts, SAI_TCSR, TCSR_TE | TCSR_FRIE);

    /* Clock out 48 words: fill drops 64 -> 16, crossing the watermark. */
    qtest_clock_step(qts, 48 * WORD_NS + WORD_NS / 2);
    g_assert_cmpuint(fifo_fill(qts), ==, 16);

    /* At/under watermark the request flag asserts (drives FRIE interrupt). */
    tcsr = rd(qts, SAI_TCSR);
    g_assert_cmphex(tcsr & TCSR_FRF, ==, TCSR_FRF);

    /* Drain the rest: 16 more words empties the FIFO. */
    qtest_clock_step(qts, 16 * WORD_NS + WORD_NS / 2);
    g_assert_cmpuint(fifo_fill(qts), ==, 0);
    tcsr = rd(qts, SAI_TCSR);
    g_assert_cmphex(tcsr & TCSR_FWF, ==, TCSR_FWF);

    /* One more word period with an empty FIFO -> underrun error latches. */
    wr(qts, SAI_TCSR, TCSR_TE | TCSR_FEIE);
    qtest_clock_step(qts, 2 * WORD_NS);
    tcsr = rd(qts, SAI_TCSR);
    g_assert_cmphex(tcsr & TCSR_FEF, ==, TCSR_FEF);

    /* Write-1-to-clear drops the sticky underrun flag. */
    wr(qts, SAI_TCSR, TCSR_TE | TCSR_FEF);
    tcsr = rd(qts, SAI_TCSR);
    g_assert_cmphex(tcsr & TCSR_FEF, ==, 0);

    /* Disable, refill, then FIFO-reset clears it. */
    wr(qts, SAI_TCSR, 0);
    for (i = 0; i < 10; i++) {
        wr(qts, SAI_TDR0, i);
    }
    g_assert_cmpuint(fifo_fill(qts), ==, 10);
    wr(qts, SAI_TCSR, TCSR_FR);
    g_assert_cmpuint(fifo_fill(qts), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/sai/tx-fifo", test_tx_fifo);
    return g_test_run();
}
