/*
 * QTest: an i.MX93 eDMA channel's IRQ line survives migration (post_load).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A channel's interrupt line level is derived from its CH_INT flag (raised when
 * a major loop completes with TCD_CSR.INTMAJ, lowered on write-1-to-clear), not
 * carried in the migration stream. Without a .post_load re-deriving it, a
 * completion interrupt pending-but-unacked at savevm is lost across loadvm and a
 * driver blocked on that channel never wakes. This is the end-to-end proof of
 * imx93_edma_post_load:
 *
 *   source     : program a tiny mem->mem TCD on channel 0 with INTMAJ and kick
 *                it via CH_CSR.ERQ; the transfer completes, CH_INT=1, line high.
 *   destination: booted -incoming defer with the eDMA's output IRQ intercepted
 *                BEFORE the stream loads, so the post_load's qemu_set_irq is
 *                captured; after migrate the channel-0 line reads HIGH.
 *
 * Mutation-proven: drop imx93_edma_post_load and the dest line reads LOW.
 *
 * The joint #5 (migration post_load) pass with 91emulator; sibling of
 * 91's tests/qtest/imx91-lpuart-migration-test.c. Follows its pattern:
 * intercept the DEVICE's "sysbus-irq" named output (not the GIC's 320-wide
 * input, which overflows libqtest MAX_IRQ), and intercept the destination
 * BEFORE migrate-incoming so the load-time qemu_set_irq is recorded.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/* eDMA1 (AONMIX); channel 0 control/TCD page is base + 0x10000. */
#define EDMA1_BASE      0x44000000ULL
#define EDMA1_CHAN0     (EDMA1_BASE + 0x10000)

/* Channel control + TCD register offsets within the channel page. */
#define CH_CSR          0x00
#define CH_INT          0x08
#define TCD_SADDR       0x20
#define TCD_SOFF        0x24
#define TCD_ATTR        0x26
#define TCD_NBYTES      0x28
#define TCD_DADDR       0x30
#define TCD_DOFF        0x34
#define TCD_CITER       0x36
#define TCD_CSR         0x3c
#define TCD_BITER       0x3e

#define CH_CSR_ERQ      0x1     /* enable request: kicks the channel */
#define TCD_CSR_INTMAJ  0x2     /* interrupt on major-loop complete */

/* Valid DRAM (i.MX93 DDR is at 0x8000_0000) for a harmless mem->mem copy. */
#define SRC_ADDR        0x80001000
#define DST_ADDR        0x80002000

/*
 * Intercept the eDMA's per-channel output IRQ list ("sysbus-irq"); channel 0 is
 * line 0. Watching the device output dodges the GIC's 320-wide input space.
 */
#define EDMA1_PATH      "/machine/soc/edma1"

static void wait_for_migration_completed(QTestState *who)
{
    while (true) {
        QDict *rsp = qtest_qmp(who, "{ 'execute': 'query-migrate' }");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_get_str(ret, "status");
        bool done = g_str_equal(status, "completed");
        bool failed = g_str_equal(status, "failed");

        qobject_unref(rsp);
        if (failed) {
            g_test_message("migration failed");
            g_assert_not_reached();
        }
        if (done) {
            return;
        }
        g_usleep(1000);
    }
}

/* Program a 4-byte mem->mem TCD with INTMAJ on channel 0 and complete it. */
static void arm_and_complete_channel0(QTestState *s)
{
    qtest_writel(s, EDMA1_CHAN0 + TCD_SADDR, SRC_ADDR);
    qtest_writew(s, EDMA1_CHAN0 + TCD_SOFF, 1);     /* soff != 0 => run now */
    qtest_writew(s, EDMA1_CHAN0 + TCD_ATTR, 0);     /* SSIZE=DSIZE=0 => 1 byte */
    qtest_writel(s, EDMA1_CHAN0 + TCD_NBYTES, 4);
    qtest_writel(s, EDMA1_CHAN0 + TCD_DADDR, DST_ADDR);
    qtest_writew(s, EDMA1_CHAN0 + TCD_DOFF, 1);
    qtest_writew(s, EDMA1_CHAN0 + TCD_CITER, 1);
    qtest_writew(s, EDMA1_CHAN0 + TCD_BITER, 1);
    qtest_writew(s, EDMA1_CHAN0 + TCD_CSR, TCD_CSR_INTMAJ);
    /* ERQ kick: the transfer runs to completion synchronously. */
    qtest_writel(s, EDMA1_CHAN0 + CH_CSR, CH_CSR_ERQ);
}

static void test_edma_irq_survives_migration(void)
{
    g_autofree char *tmp = g_dir_make_tmp("imx93-edma-mig.XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *sock = g_strdup_printf("%s/mig.sock", tmp);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    /* Source: complete a channel-0 transfer so CH_INT=1 and the line asserts. */
    QTestState *src = qtest_init("-machine imx93-11x11-evk -display none");
    qtest_irq_intercept_out_named(src, EDMA1_PATH, "sysbus-irq");
    arm_and_complete_channel0(src);
    g_assert_cmphex(qtest_readl(src, EDMA1_CHAN0 + CH_INT) & 1, ==, 1);
    g_assert_true(qtest_get_irq(src, 0));       /* asserted on source */

    /*
     * Destination: -incoming defer. Intercept the eDMA output BEFORE loading the
     * stream so the qemu_set_irq issued from imx93_edma_post_load is recorded.
     */
    QTestState *dst = qtest_init("-machine imx93-11x11-evk -display none "
                                 "-incoming defer");
    qtest_irq_intercept_out_named(dst, EDMA1_PATH, "sysbus-irq");

    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                                  "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                                  "  'arguments': { 'uri': %s } }", uri);
    wait_for_migration_completed(src);

    /* The channel-0 line must be re-asserted on the destination by post_load. */
    g_assert_true(qtest_get_irq(dst, 0));

    qtest_quit(dst);
    qtest_quit(src);
    unlink(sock);
    rmdir(tmp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/edma/irq-survives-migration",
                   test_edma_irq_survives_migration);
    return g_test_run();
}
