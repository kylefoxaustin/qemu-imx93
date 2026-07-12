/*
 * QTest for the i.MX93 ISI (Image Sensing Interface) capture channel.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the ISI the way the mxc-isi capture driver does, with no kernel:
 * programs the image geometry, output pitch and the BUF1/BUF2 ping-pong
 * addresses, enables the channel, then advances the virtual clock to let the
 * model deliver frames. Each frame the model DMAs a moving test pattern into
 * the buffer the hardware would be filling and latches CHNL_STS[FRM_STRD] plus
 * the BUFn_ACTIVE bit naming the next buffer. The test checks the pattern lands
 * in the right ping-pong buffer, advances between frames, and that CHNL_STS is
 * write-1-to-clear - the exact contract the driver's ISR relies on. The
 * register layout matches the live capture in tests/camera-imx93.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define ISI_BASE        0x4ae40000ULL

#define CHNL_CTRL               0x0000
#define   CHNL_CTRL_CHNL_EN     0x80000000
#define CHNL_IMG_CFG            0x000c      /* (height << 16) | width */
#define CHNL_IER                0x0010
#define   CHNL_IER_FRM_RCVD_EN  0x20000000
#define CHNL_STS                0x0014
#define   CHNL_STS_FRM_STRD     0x20000000  /* BIT(29) */
#define   CHNL_STS_BUF1_ACTIVE  0x00000100  /* BIT(8) */
#define   CHNL_STS_BUF2_ACTIVE  0x00000200  /* BIT(9) */
#define CHNL_OUT_BUF1_ADDR_Y    0x0070
#define CHNL_OUT_BUF_PITCH      0x007c
#define CHNL_OUT_BUF2_ADDR_Y    0x008c

/* ~30 fps; a 40ms clock step is guaranteed to cross one frame period. */
#define STEP_NS         (40 * 1000 * 1000)

/* Small frame to keep the DMA cheap. Guest DRAM @ 0x8000_0000. */
#define W       16
#define H       8
#define BPP     4
#define PITCH   (W * BPP)
#define FM_LEN  (PITCH * H)
#define BUF1    0x80100000ULL
#define BUF2    0x80200000ULL

static void isi_writel(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, ISI_BASE + off, v);
}

/* The pixel the model writes at (x, y) for a given frame counter. */
static uint32_t pattern_px(int x, int y, uint32_t frame)
{
    uint32_t v = (x + y + frame * 4) & 0xff;
    return 0xff000000u | (v << 16) | (v << 8) | v;
}

/* Read a DMA'd frame and assert it is the expected moving pattern. */
static void check_frame(QTestState *qts, uint64_t buf, uint32_t frame)
{
    g_autofree uint32_t *got = g_malloc(FM_LEN);
    int x, y;

    qtest_memread(qts, buf, got, FM_LEN);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t want = pattern_px(x, y, frame);
            uint32_t have = got[y * W + x];
            if (have != want) {
                g_test_message("frame %u px(%d,%d) = 0x%08x, want 0x%08x",
                               frame, x, y, have, want);
            }
            g_assert_cmphex(have, ==, want);
        }
    }
}

static void test_capture(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    g_autofree uint8_t *zero = g_malloc0(FM_LEN);
    uint32_t sts;

    /* Poison both ping-pong buffers so a missed DMA is detectable. */
    qtest_memwrite(qts, BUF1, zero, FM_LEN);
    qtest_memwrite(qts, BUF2, zero, FM_LEN);

    /* Program geometry, pitch, both buffers and enable the frame interrupt. */
    isi_writel(qts, CHNL_IMG_CFG, (H << 16) | W);
    isi_writel(qts, CHNL_OUT_BUF_PITCH, PITCH);
    isi_writel(qts, CHNL_OUT_BUF1_ADDR_Y, (uint32_t)BUF1);
    isi_writel(qts, CHNL_OUT_BUF2_ADDR_Y, (uint32_t)BUF2);
    isi_writel(qts, CHNL_IER, CHNL_IER_FRM_RCVD_EN);

    /* Stream on. */
    isi_writel(qts, CHNL_CTRL, CHNL_CTRL_CHNL_EN);

    /* Frame 0 -> BUF1, hardware switches to BUF2 (BUF2_ACTIVE set). */
    qtest_clock_step(qts, STEP_NS);
    sts = qtest_readl(qts, ISI_BASE + CHNL_STS);
    g_assert_cmphex(sts & CHNL_STS_FRM_STRD, ==, CHNL_STS_FRM_STRD);
    g_assert_cmphex(sts & CHNL_STS_BUF2_ACTIVE, ==, CHNL_STS_BUF2_ACTIVE);
    g_assert_cmphex(sts & CHNL_STS_BUF1_ACTIVE, ==, 0);
    check_frame(qts, BUF1, 0);

    /* Ack like the ISR: write-1-to-clear drops FRM_STRD. */
    isi_writel(qts, CHNL_STS, 0xffffffff);
    sts = qtest_readl(qts, ISI_BASE + CHNL_STS);
    g_assert_cmphex(sts & CHNL_STS_FRM_STRD, ==, 0);

    /* Frame 1 -> BUF2, hardware switches to BUF1 (BUF1_ACTIVE set). */
    qtest_clock_step(qts, STEP_NS);
    sts = qtest_readl(qts, ISI_BASE + CHNL_STS);
    g_assert_cmphex(sts & CHNL_STS_FRM_STRD, ==, CHNL_STS_FRM_STRD);
    g_assert_cmphex(sts & CHNL_STS_BUF1_ACTIVE, ==, CHNL_STS_BUF1_ACTIVE);
    g_assert_cmphex(sts & CHNL_STS_BUF2_ACTIVE, ==, 0);
    check_frame(qts, BUF2, 1);

    /* Stream off must stop frame delivery. */
    isi_writel(qts, CHNL_STS, 0xffffffff);
    isi_writel(qts, CHNL_CTRL, 0);
    qtest_clock_step(qts, STEP_NS);
    sts = qtest_readl(qts, ISI_BASE + CHNL_STS);
    g_assert_cmphex(sts & CHNL_STS_FRM_STRD, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/isi/capture", test_capture);
    return g_test_run();
}
