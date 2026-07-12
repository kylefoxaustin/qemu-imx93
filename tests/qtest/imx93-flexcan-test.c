/*
 * QTest for the i.MX93 FlexCAN receive path (acceptance filtering + overrun +
 * disabled-controller gating), driven controller-to-controller over one CAN bus.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX93 has two FlexCANs; putting both on one -object can-bus lets CAN1
 * transmit a real frame into CAN2's receive path (not a loopback shortcut).
 * These assertions target the three silent-wrongs a first-empty-mailbox model
 * hides: (1) a frame must land in the ID-MATCHING mailbox, not the first empty
 * one; (2) a second frame for a full mailbox must set CODE=OVERRUN, not be
 * silently dropped or misdelivered; (3) a disabled controller must not receive.
 * Each asserts the mailbox VALUE, so a model that ignores RXIMR/OVERRUN/MDIS
 * fails here even though a "did a frame arrive?" test would pass.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define CAN1        0x443a0000UL   /* FLEXCAN1 */
#define CAN2        0x425b0000UL   /* FLEXCAN2 */

#define MCR         0x00
#define MCR_MDIS    (1u << 31)

#define MB(i)       (0x80 + (i) * 16)      /* classic 16-byte mailboxes */
#define RXIMR(i)    (0x880 + (i) * 4)
#define TX_MB       63                     /* last MB (classic mb_count 64) */

#define CODE_MASK   (0xfu << 24)
#define CODE_EMPTY  (0x4u << 24)
#define CODE_FULL   (0x2u << 24)
#define CODE_OVER   (0x6u << 24)
#define CODE_TXDATA (0xcu << 24)

#define STD_SHIFT   18
#define STD_MASK    (0x7ffu << STD_SHIFT)  /* standard ID field in the ID reg */

static void wr(QTestState *q, uint64_t base, uint32_t off, uint32_t v)
{
    qtest_writel(q, base + off, v);
}

static uint32_t rd(QTestState *q, uint64_t base, uint32_t off)
{
    return qtest_readl(q, base + off);
}

/* MCR=0 clears MDIS and freeze -> the controller is active on the bus. */
static void activate(QTestState *q, uint64_t base)
{
    wr(q, base, MCR, 0);
}

/* Arm RX mailbox @idx to accept exactly standard ID @sid. */
static void setup_rx(QTestState *q, uint64_t base, unsigned idx, uint32_t sid)
{
    wr(q, base, MB(idx) + 4, sid << STD_SHIFT);   /* mailbox ID */
    wr(q, base, RXIMR(idx), STD_MASK);            /* exact-match individual mask */
    wr(q, base, MB(idx), CODE_EMPTY);             /* arm (CODE written last) */
}

/* Transmit a standard-ID, 2-byte frame from @base. */
static void tx_std(QTestState *q, uint64_t base, uint32_t sid, uint32_t data0)
{
    wr(q, base, MB(TX_MB) + 4, sid << STD_SHIFT);
    wr(q, base, MB(TX_MB) + 8, data0);
    wr(q, base, MB(TX_MB), CODE_TXDATA | (2u << 16));  /* DLC=2 -> triggers TX */
}

static QTestState *start(void)
{
    return qtest_init("-machine imx93-11x11-evk,canbus0=cb,canbus1=cb "
                      "-object can-bus,id=cb -m 4G -display none "
                      "-kernel /dev/null");
}

static void test_flexcan_id_match_and_overrun(void)
{
    QTestState *q = start();

    activate(q, CAN1);
    activate(q, CAN2);

    /* Two RX mailboxes, different IDs, exact match. MB1 is the FIRST empty. */
    setup_rx(q, CAN2, 1, 0x100);
    setup_rx(q, CAN2, 2, 0x200);

    /*
     * Bug 1: a frame for 0x200 must land in MB2 (the ID match), NOT MB1 (the
     * first empty mailbox). A model without ID matching drops it in MB1.
     */
    tx_std(q, CAN1, 0x200, 0xdeadbeef);
    g_assert_cmphex(rd(q, CAN2, MB(1)) & CODE_MASK, ==, CODE_EMPTY);
    g_assert_cmphex(rd(q, CAN2, MB(2)) & CODE_MASK, ==, CODE_FULL);
    g_assert_cmphex((rd(q, CAN2, MB(2) + 4) & STD_MASK) >> STD_SHIFT, ==, 0x200);

    /*
     * Bug 2: a second 0x200 frame while MB2 is FULL must OVERRUN MB2 (data
     * moves, guest is told) - not vanish, and not spill into the empty MB1.
     */
    tx_std(q, CAN1, 0x200, 0xcafef00d);
    g_assert_cmphex(rd(q, CAN2, MB(2)) & CODE_MASK, ==, CODE_OVER);
    g_assert_cmphex(rd(q, CAN2, MB(1)) & CODE_MASK, ==, CODE_EMPTY);

    qtest_quit(q);
}

static void test_flexcan_accept_all_offload(void)
{
    QTestState *q = start();

    activate(q, CAN1);
    activate(q, CAN2);

    /*
     * rx-offload regression guard: Linux arms its RX mailboxes with RXIMR=0
     * (accept ANY id) and filters in software. Two frames with DIFFERENT ids
     * must fill MB1 then MB2 (successive empty mailboxes) - exactly the pre-fix
     * behaviour - NOT overrun MB1. This is the path a real cansend/candump b2b
     * run exercises; asserting it here covers it deterministically.
     */
    wr(q, CAN2, RXIMR(1), 0);
    wr(q, CAN2, MB(1), CODE_EMPTY);
    wr(q, CAN2, RXIMR(2), 0);
    wr(q, CAN2, MB(2), CODE_EMPTY);

    tx_std(q, CAN1, 0x123, 0xa1a1a1a1);
    tx_std(q, CAN1, 0x456, 0xb2b2b2b2);

    g_assert_cmphex(rd(q, CAN2, MB(1)) & CODE_MASK, ==, CODE_FULL);
    g_assert_cmphex((rd(q, CAN2, MB(1) + 4) & STD_MASK) >> STD_SHIFT, ==, 0x123);
    g_assert_cmphex(rd(q, CAN2, MB(2)) & CODE_MASK, ==, CODE_FULL);
    g_assert_cmphex((rd(q, CAN2, MB(2) + 4) & STD_MASK) >> STD_SHIFT, ==, 0x456);

    qtest_quit(q);
}

static void test_flexcan_disabled_does_not_receive(void)
{
    QTestState *q = start();

    activate(q, CAN1);
    activate(q, CAN2);
    setup_rx(q, CAN2, 1, 0x100);

    /* Bug 3: disable CAN2, then send a matching frame - it must NOT arrive. */
    wr(q, CAN2, MCR, MCR_MDIS);
    tx_std(q, CAN1, 0x100, 0x11223344);
    g_assert_cmphex(rd(q, CAN2, MB(1)) & CODE_MASK, ==, CODE_EMPTY);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/flexcan/id-match-and-overrun",
                   test_flexcan_id_match_and_overrun);
    qtest_add_func("/imx93/flexcan/accept-all-offload",
                   test_flexcan_accept_all_offload);
    qtest_add_func("/imx93/flexcan/disabled-does-not-receive",
                   test_flexcan_disabled_does_not_receive);
    return g_test_run();
}
