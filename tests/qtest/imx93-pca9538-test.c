/*
 * QTest for the i.MX93 board's PCA9538 I/O expander power-on reset values.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The PCA9538 (camera expander @ 0x70 on LPI2C8) is modelled by the trivial
 * i2c-regdev register file, which memset-0's every register. That is not a
 * physical power-on state: the TI PCA9538 datasheet says the Configuration
 * register (0x03) powers up at 0xFF (all pins INPUTS) and the Output Port
 * register (0x01) at 0xFF - "at power-on reset all registers return to default
 * values". memset-0 got the Configuration register wrong as all-OUTPUTS.
 *
 * This drives a real LPI2C8 master transaction (the way the i2c-imx-lpi2c
 * driver does: command+data words into MTDR, bytes out of MRDR) to read those
 * registers back and pin them to the datasheet POR - so the register file is
 * no longer a value nothing reads, and a regression to memset-0 fails here.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define LPI2C8_BASE     0x426e0000ULL

#define LPI2C_MCR       0x10
#define LPI2C_MSR       0x14
#define LPI2C_MTDR      0x60
#define LPI2C_MRDR      0x70

#define MCR_MEN         (1u << 0)
#define MCR_RTF         (1u << 8)   /* reset tx FIFO */
#define MCR_RRF         (1u << 9)   /* reset rx FIFO */
#define MSR_RDF         (1u << 1)   /* rx FIFO has data */
#define MRDR_RXEMPTY    (1u << 14)

#define CMD_TRAN_DATA   0x0
#define CMD_RECV_DATA   0x1
#define CMD_GEN_STOP    0x2
#define CMD_GEN_START   0x4

#define PCA9538_ADDR    0x70

static void mtdr(QTestState *qts, uint8_t cmd, uint8_t data)
{
    qtest_writel(qts, LPI2C8_BASE + LPI2C_MTDR, (cmd << 8) | data);
}

/* One SMBus-style "set pointer, repeated-start, read one byte" transaction. */
static uint8_t pca9538_read(QTestState *qts, uint8_t reg)
{
    uint32_t v;
    int tries;

    /* START + address (write), the register pointer, repeated START + read. */
    mtdr(qts, CMD_GEN_START, (PCA9538_ADDR << 1) | 0);
    mtdr(qts, CMD_TRAN_DATA, reg);
    mtdr(qts, CMD_GEN_START, (PCA9538_ADDR << 1) | 1);
    mtdr(qts, CMD_RECV_DATA, 0);           /* request (0 + 1) = 1 byte */
    mtdr(qts, CMD_GEN_STOP, 0);

    /* Wait for the rx FIFO to carry the byte, then read it out of MRDR. */
    for (tries = 0; tries < 1000; tries++) {
        if (qtest_readl(qts, LPI2C8_BASE + LPI2C_MSR) & MSR_RDF) {
            break;
        }
    }
    v = qtest_readl(qts, LPI2C8_BASE + LPI2C_MRDR);
    g_assert_cmphex(v & MRDR_RXEMPTY, ==, 0);   /* a byte really arrived */
    return v & 0xff;
}

static void test_pca9538_por(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");

    /* Enable the master and clear its FIFOs. */
    qtest_writel(qts, LPI2C8_BASE + LPI2C_MCR, MCR_RTF | MCR_RRF);
    qtest_writel(qts, LPI2C8_BASE + LPI2C_MCR, MCR_MEN);

    /*
     * Datasheet POR (TI PCA9538): Configuration = 0xFF (all inputs),
     * Output Port = 0xFF. A regression to the memset-0 default makes the
     * Configuration read 0x00 (all outputs) and fails this.
     */
    g_assert_cmphex(pca9538_read(qts, 0x03), ==, 0xff);   /* Configuration */
    g_assert_cmphex(pca9538_read(qts, 0x01), ==, 0xff);   /* Output Port   */

    /* Polarity Inversion powers up at 0x00 (and memset agrees). */
    g_assert_cmphex(pca9538_read(qts, 0x02), ==, 0x00);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/pca9538/por", test_pca9538_por);
    return g_test_run();
}
