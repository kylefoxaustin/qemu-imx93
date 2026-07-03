/*
 * QTest for the NXP LPSPI controller model (on the i.MX 93 machine).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises the LPSPI master transfer engine (hw/ssi/imx93_lpspi.c) without a
 * kernel: VERID/PARAM identity, the CR.MEN transfer gate, a TDR write driving
 * a frame onto the SSI bus and latching TCF/FCF + an RX word, and the RX-FIFO
 * reset. (The model bridges TDR writes onto a QEMU SSI bus and returns the
 * shifted-in byte via RDR; with no slave wired on the bus the shifted-in value
 * is the idle-bus default, so this checks the engine/flags, not slave data.)
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

/* LPSPI1 register base i.MX 93. */
#define LPSPI1      0x44360000

#define VERID   0x00
#define PARAM   0x04
#define CR      0x10
#define SR      0x14
#define CFGR1   0x24
#define FSR     0x5c
#define TCR     0x60
#define TDR     0x64
#define RDR     0x74

#define CR_MEN   (1u << 0)
#define CR_RRF   (1u << 9)
#define SR_FCF   (1u << 9)
#define SR_TCF   (1u << 10)
#define TCR_CONT (1u << 21)

#define VERID_VALUE 0x02000004
/* PCSNUM (bits 19:16) = 4 chip-selects; TX/RX FIFO depth nibbles = 16 each. */
#define PARAM_VALUE 0x00040404

#define RXCOUNT(fsr) (((fsr) >> 16) & 0xff)

static void test_identity(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -accel qtest");

    g_assert_cmphex(qtest_readl(qts, LPSPI1 + VERID), ==, VERID_VALUE);
    g_assert_cmphex(qtest_readl(qts, LPSPI1 + PARAM), ==, PARAM_VALUE);

    qtest_quit(qts);
}

static void test_men_gate_and_transfer(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -accel qtest");

    /* 8-bit frame, single (non-continuous) so the frame completes (FCF). */
    qtest_writel(qts, LPSPI1 + TCR, 7);

    /* MEN off: a TDR write must be ignored - no transfer, no RX word. */
    qtest_writel(qts, LPSPI1 + CR, 0);
    qtest_writel(qts, LPSPI1 + TDR, 0xab);
    g_assert_false(qtest_readl(qts, LPSPI1 + SR) & SR_TCF);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 0);

    /* MEN on: the TDR write shifts a frame; TCF+FCF set, one RX word queued. */
    qtest_writel(qts, LPSPI1 + CR, CR_MEN);
    qtest_writel(qts, LPSPI1 + TDR, 0xab);
    g_assert_true(qtest_readl(qts, LPSPI1 + SR) & SR_TCF);
    g_assert_true(qtest_readl(qts, LPSPI1 + SR) & SR_FCF);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 1);

    /* Reading RDR pops the RX word, draining the FIFO. */
    (void)qtest_readl(qts, LPSPI1 + RDR);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 0);

    qtest_quit(qts);
}

static void test_rxfifo_reset(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -accel qtest");

    qtest_writel(qts, LPSPI1 + TCR, 7);
    qtest_writel(qts, LPSPI1 + CR, CR_MEN);
    qtest_writel(qts, LPSPI1 + TDR, 0x55);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 1);

    /* CR.RRF drains the RX FIFO. */
    qtest_writel(qts, LPSPI1 + CR, CR_MEN | CR_RRF);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 0);

    qtest_quit(qts);
}

/*
 * Full slave round-trip: attach an ISSI is25lp064 SPI NOR flash on the lpspi1
 * SSI bus (via the model's bus-name), then read its JEDEC ID (opcode 0x9F + 3
 * reads) through the controller and check it byte-exact (0x9d 0x60 0x17). This
 * exercises the TDR -> ssi_transfer -> slave -> RDR path against a real device,
 * proving -device <ssi-slave>,bus=lpspiN attach + transfer works end to end.
 */
static void test_flash_jedec(void)
{
    char tmp[] = "/tmp/imx93-lpspi-flash-XXXXXX";
    int fd = mkstemp(tmp);
    QTestState *qts;
    uint8_t id0, id1, id2;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 8 << 20), ==, 0);
    close(fd);

    qts = qtest_initf("-machine imx93-11x11-evk -accel qtest "
                      "-blockdev driver=file,filename=%s,node-name=sf "
                      "-device is25lp064,bus=lpspi1,drive=sf", tmp);

    /* 8-bit frames, continuous (CS stays asserted across the command). */
    qtest_writel(qts, LPSPI1 + CR, CR_MEN);
    qtest_writel(qts, LPSPI1 + TCR, 7 | TCR_CONT);

    /* JEDEC read: opcode 0x9F, then clock out three ID bytes. */
    qtest_writel(qts, LPSPI1 + TDR, 0x9f);
    qtest_writel(qts, LPSPI1 + TDR, 0x00);
    qtest_writel(qts, LPSPI1 + TDR, 0x00);
    qtest_writel(qts, LPSPI1 + TDR, 0x00);

    (void)qtest_readl(qts, LPSPI1 + RDR);          /* response to the opcode */
    id0 = qtest_readl(qts, LPSPI1 + RDR) & 0xff;   /* manufacturer */
    id1 = qtest_readl(qts, LPSPI1 + RDR) & 0xff;   /* memory type */
    id2 = qtest_readl(qts, LPSPI1 + RDR) & 0xff;   /* capacity */

    g_assert_cmphex(id0, ==, 0x9d);
    g_assert_cmphex(id1, ==, 0x60);
    g_assert_cmphex(id2, ==, 0x17);

    qtest_quit(qts);
    unlink(tmp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93-lpspi/identity", test_identity);
    qtest_add_func("/imx93-lpspi/men-gate-and-transfer",
                   test_men_gate_and_transfer);
    qtest_add_func("/imx93-lpspi/rxfifo-reset", test_rxfifo_reset);
    qtest_add_func("/imx93-lpspi/flash-jedec", test_flash_jedec);
    return g_test_run();
}
