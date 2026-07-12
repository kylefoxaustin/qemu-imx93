/*
 * QTest for the i.MX93 ELE (EdgeLock Enclave) MU responder.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Asserts the ELE responder's honest behaviour:
 *  - coordination/probe commands whose outcome the model reproduces succeed;
 *  - commands it does NOT reproduce (FW auth, WRITE_FUSE, ...) FAIL CLOSED with
 *    a non-gating failure status, so the driver gets kStatus_Fail rather than a
 *    fabricated success;
 *  - GET_RANDOM is COMPUTED CORRECTLY - it fills the destination buffer with
 *    real entropy (asserted by value: the buffer changes, and two draws differ,
 *    which catches both fake-entropy and a static/repeatable RNG).
 * The negative test forces the "fake-uncomputed-success" escape hatch on and
 * requires GET_RANDOM to go back to acking success WITHOUT filling the buffer
 * (the old dishonest behaviour), proving the honest default is real.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define ELE_BASE        0x47520000
#define ELE_TR0         0x200
#define ELE_RR0         0x280

#define ELE_SUCCESS_IND 0xd6
#define ELE_FAILURE_IND 0x29

/* A scratch DRAM address (imx93 RAM base 0x80000000, -m 4G). */
#define RNG_DST         0x90000000

/* ELE command opcodes (byte 2 of the se_msg_hdr). */
#define ELE_CMD_PING        0x01
#define ELE_CMD_FW_AUTH     0x02
#define ELE_CMD_START_RNG   0xa3
#define ELE_CMD_GET_RANDOM  0xcd
#define ELE_CMD_WRITE_FUSE  0xd6
#define ELE_CMD_GET_INFO    0xda

/* se_msg_hdr word: byte0 ver, byte1 size(words), byte2 cmd, byte3 tag(0x17). */
static uint32_t ele_hdr(uint8_t cmd, uint8_t words)
{
    return ((uint32_t)0x17 << 24) | ((uint32_t)cmd << 16) |
           ((uint32_t)words << 8) | 0x06;
}

/* Drain any pending response words so each command starts clean. */
static void ele_drain(QTestState *q)
{
    for (int i = 0; i < 4; i++) {
        qtest_readl(q, ELE_BASE + ELE_RR0 + i * 4);
    }
}

/* Send a single-word command and return the reply's status byte (RR1 low). */
static uint8_t ele_cmd_status(QTestState *q, uint8_t cmd)
{
    qtest_writel(q, ELE_BASE + ELE_TR0, ele_hdr(cmd, 1)); /* size 1 -> process */
    qtest_readl(q, ELE_BASE + ELE_RR0);                   /* consume header */
    return qtest_readl(q, ELE_BASE + ELE_RR0 + 4) & 0xff; /* status word */
}

/* Send GET_RANDOM(dst, len) (4-word message) and return the status byte. */
static uint8_t ele_get_random(QTestState *q, uint32_t dst, uint32_t len)
{
    qtest_writel(q, ELE_BASE + ELE_TR0 + 0, ele_hdr(ELE_CMD_GET_RANDOM, 4));
    qtest_writel(q, ELE_BASE + ELE_TR0 + 4, 0);     /* flags */
    qtest_writel(q, ELE_BASE + ELE_TR0 + 8, dst);   /* destination address */
    qtest_writel(q, ELE_BASE + ELE_TR0 + 12, len);  /* length */
    qtest_readl(q, ELE_BASE + ELE_RR0);             /* consume header */
    return qtest_readl(q, ELE_BASE + ELE_RR0 + 4) & 0xff;
}

static void test_ele_faults_honestly(void)
{
    QTestState *q = qtest_init("-machine imx93-11x11-evk -m 4G "
                               "-display none -kernel /dev/null");

    /* Whitelisted coordination/probe commands: outcome reproduced -> SUCCESS. */
    g_assert_cmphex(ele_cmd_status(q, ELE_CMD_PING),      ==, ELE_SUCCESS_IND);
    g_assert_cmphex(ele_cmd_status(q, ELE_CMD_GET_INFO),  ==, ELE_SUCCESS_IND);
    g_assert_cmphex(ele_cmd_status(q, ELE_CMD_START_RNG), ==, ELE_SUCCESS_IND);

    /* Unreproduced security commands fail closed - never a fabricated success. */
    g_assert_cmphex(ele_cmd_status(q, ELE_CMD_FW_AUTH),    ==, ELE_FAILURE_IND);
    g_assert_cmphex(ele_cmd_status(q, ELE_CMD_WRITE_FUSE), ==, ELE_FAILURE_IND);

    qtest_quit(q);
}

static void test_ele_get_random_real_entropy(void)
{
    QTestState *q = qtest_init("-machine imx93-11x11-evk -m 4G "
                               "-display none -kernel /dev/null");
    uint8_t sentinel[16], first[16], second[16];

    memset(sentinel, 0xa5, sizeof(sentinel));

    /* Draw 1: buffer must change from the sentinel (real bytes were written). */
    qtest_memwrite(q, RNG_DST, sentinel, sizeof(sentinel));
    g_assert_cmphex(ele_get_random(q, RNG_DST, sizeof(first)), ==,
                    ELE_SUCCESS_IND);
    qtest_memread(q, RNG_DST, first, sizeof(first));
    g_assert(memcmp(first, sentinel, sizeof(first)) != 0);

    /* Draw 2 into a fresh sentinel: must differ from draw 1 (not static/repeat).
     * This is what catches a repeatable RNG - assert the VALUE, not the verdict.
     */
    qtest_memwrite(q, RNG_DST, sentinel, sizeof(sentinel));
    ele_drain(q);
    g_assert_cmphex(ele_get_random(q, RNG_DST, sizeof(second)), ==,
                    ELE_SUCCESS_IND);
    qtest_memread(q, RNG_DST, second, sizeof(second));
    g_assert(memcmp(second, first, sizeof(second)) != 0);

    qtest_quit(q);
}

static void test_ele_escape_hatch(void)
{
    /*
     * NB: use the explicit -global form. The shorthand
     * "-global imx93.ele.fake-uncomputed-success=on" splits on the wrong dot
     * because the QOM type name "imx93.ele" itself contains one.
     */
    QTestState *q = qtest_init("-machine imx93-11x11-evk -m 4G "
                               "-display none -kernel /dev/null "
                               "-global driver=imx93.ele,"
                               "property=fake-uncomputed-success,value=on");
    uint8_t sentinel[16], after[16];

    memset(sentinel, 0xa5, sizeof(sentinel));

    /*
     * With the hatch on, GET_RANDOM acks success but does NOT fill the buffer -
     * the old dishonest fake-entropy behaviour. If the honest default (which
     * DOES fill) ever regressed to this, the real-entropy test above would
     * fail; this proves the hatch is what flips it, so the default is real.
     */
    qtest_memwrite(q, RNG_DST, sentinel, sizeof(sentinel));
    g_assert_cmphex(ele_get_random(q, RNG_DST, sizeof(after)), ==,
                    ELE_SUCCESS_IND);
    qtest_memread(q, RNG_DST, after, sizeof(after));
    g_assert(memcmp(after, sentinel, sizeof(after)) == 0);   /* untouched */

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/ele/faults-honestly", test_ele_faults_honestly);
    qtest_add_func("/imx93/ele/get-random-real-entropy",
                   test_ele_get_random_real_entropy);
    qtest_add_func("/imx93/ele/escape-hatch", test_ele_escape_hatch);
    return g_test_run();
}
