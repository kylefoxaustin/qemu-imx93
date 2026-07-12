/*
 * QTest for the i.MX93 PXP (Pixel Pipeline) 2D engine.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the PXP the way the pxp_dma_v3 driver does, with no kernel: each test
 * stages a source in DRAM, programs the op's registers, kicks ENABLE and checks
 * the OFM written back is byte-exact while STAT reports the completion
 * interrupt. Covers copy, fill, opaque blit, src-over blend and 90-degree
 * rotation; the register sequences match the ones captured from the live driver
 * in tests/pxp-imx93.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define PXP_BASE        0x4ae20000ULL

/* MXS register heads + the SET alias used to kick. */
#define PXP_CTRL_SET    0x04
#define PXP_STAT        0x10
#define PXP_OUT_CTRL    0x20
#define PXP_OUT_BUF     0x30
#define PXP_OUT_PITCH   0x50
#define PXP_OUT_LRC     0x60
#define PXP_OUT_PS_ULC  0x70
#define PXP_OUT_PS_LRC  0x80
#define PXP_PS_CTRL     0xb0
#define PXP_PS_BUF      0xc0
#define PXP_PS_PITCH    0xf0

/* Fetch/Store engine cluster used by the fill, blit, blend and rotate paths. */
#define PXP_FETCH_CTRL  0x450       /* [13:12] = rotation */
#define PXP_FETCH_SIZE  0x4a0
#define PXP_FETCH_PITCH 0x510
#define PXP_FETCH_ADDR  0x580       /* CH0 (blit src / blend background) */
#define PXP_FETCH_ADDR1 0x5a0       /* CH1 (blend foreground) */
#define PXP_STORE_SIZE  0x600
#define PXP_STORE_PITCH 0x620
#define PXP_STORE_ADDR  0x690
#define PXP_STORE_FILL  0x6b0

#define CTRL_ENABLE     0x1
#define STAT_IRQ0       0x1
#define FMT_RGB888      0x4     /* 32-bit stored */

/* Guest DRAM (DDR @ 0x8000_0000). */
#define SRC_ADDR        0x80100000ULL
#define DST_ADDR        0x80200000ULL

#define W   64
#define H   32
#define BPP 4
#define PITCH  (W * BPP)
#define FM_LEN (PITCH * H)

static void pxp_writel(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, PXP_BASE + off, v);
}

static void test_copy(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    g_autofree uint8_t *src = g_malloc(FM_LEN);
    g_autofree uint8_t *dst = g_malloc(FM_LEN);
    uint32_t stat;
    int i;

    for (i = 0; i < FM_LEN; i++) {
        src[i] = (uint8_t)(i * 7 + 0x11);
    }
    qtest_memwrite(qts, SRC_ADDR, src, FM_LEN);
    memset(dst, 0xa5, FM_LEN);
    qtest_memwrite(qts, DST_ADDR, dst, FM_LEN);

    /* Program the source (PS) and destination (OUT) surfaces. */
    pxp_writel(qts, PXP_PS_CTRL, FMT_RGB888);
    pxp_writel(qts, PXP_PS_BUF, (uint32_t)SRC_ADDR);
    pxp_writel(qts, PXP_PS_PITCH, PITCH);
    pxp_writel(qts, PXP_OUT_CTRL, FMT_RGB888);
    pxp_writel(qts, PXP_OUT_BUF, (uint32_t)DST_ADDR);
    pxp_writel(qts, PXP_OUT_PITCH, PITCH);
    pxp_writel(qts, PXP_OUT_LRC, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_OUT_PS_ULC, 0);
    pxp_writel(qts, PXP_OUT_PS_LRC, ((W - 1) << 16) | (H - 1));

    /* Kick: the model runs the pass synchronously on this write. */
    pxp_writel(qts, PXP_CTRL_SET, CTRL_ENABLE);

    /* Completion interrupt is latched in STAT. */
    stat = qtest_readl(qts, PXP_BASE + PXP_STAT);
    g_assert_cmphex(stat & STAT_IRQ0, ==, STAT_IRQ0);

    /* OFM must be a byte-exact copy of the source. */
    qtest_memread(qts, DST_ADDR, dst, FM_LEN);
    for (i = 0; i < FM_LEN; i++) {
        if (dst[i] != src[i]) {
            g_test_message("OFM[%d] = 0x%x, want 0x%x", i, dst[i], src[i]);
        }
        g_assert_cmpuint(dst[i], ==, src[i]);
    }

    qtest_quit(qts);
}

/* Swap R and B bytes: the Store engine's internal-to-RGBA conversion. */
static uint32_t swap_rb(uint32_t v)
{
    return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
}

static void test_fill(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    const uint32_t fill = 0x11443322;          /* Store fill data */
    const uint32_t want = swap_rb(fill);        /* what lands in memory */
    g_autofree uint32_t *dst = g_malloc(FM_LEN);
    uint32_t stat;
    int i;

    for (i = 0; i < (int)(FM_LEN / 4); i++) {
        dst[i] = 0xdeadbeef;
    }
    qtest_memwrite(qts, DST_ADDR, dst, FM_LEN);

    /*
     * Program the Store engine for a fill. Writing STORE_ADDR arms the fill
     * path (vs the legacy copy path) for the shared ENABLE kick.
     */
    pxp_writel(qts, PXP_STORE_SIZE, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_STORE_PITCH, PITCH);
    pxp_writel(qts, PXP_STORE_FILL, fill);
    pxp_writel(qts, PXP_STORE_ADDR, (uint32_t)DST_ADDR);

    pxp_writel(qts, PXP_CTRL_SET, CTRL_ENABLE);

    stat = qtest_readl(qts, PXP_BASE + PXP_STAT);
    g_assert_cmphex(stat & STAT_IRQ0, ==, STAT_IRQ0);

    qtest_memread(qts, DST_ADDR, dst, FM_LEN);
    for (i = 0; i < (int)(FM_LEN / 4); i++) {
        if (dst[i] != want) {
            g_test_message("OFM[%d] = 0x%x, want 0x%x", i, dst[i], want);
        }
        g_assert_cmphex(dst[i], ==, want);
    }

    qtest_quit(qts);
}

/* Opaque Fetch->Store surface blit: src surface copied to dst, byte-exact. */
static void test_blit(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    g_autofree uint8_t *src = g_malloc(FM_LEN);
    g_autofree uint8_t *dst = g_malloc(FM_LEN);
    uint32_t stat;
    int i;

    for (i = 0; i < FM_LEN; i++) {
        src[i] = (uint8_t)(i * 13 + 0x07);
    }
    qtest_memwrite(qts, SRC_ADDR, src, FM_LEN);
    memset(dst, 0x5a, FM_LEN);
    qtest_memwrite(qts, DST_ADDR, dst, FM_LEN);

    /*
     * Program the Fetch source and Store dest. Writing FETCH_ADDR arms the
     * blit path for the shared ENABLE kick.
     */
    pxp_writel(qts, PXP_FETCH_SIZE, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_FETCH_PITCH, PITCH);
    pxp_writel(qts, PXP_STORE_SIZE, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_STORE_PITCH, PITCH);
    pxp_writel(qts, PXP_STORE_ADDR, (uint32_t)DST_ADDR);
    pxp_writel(qts, PXP_FETCH_ADDR, (uint32_t)SRC_ADDR);

    pxp_writel(qts, PXP_CTRL_SET, CTRL_ENABLE);

    stat = qtest_readl(qts, PXP_BASE + PXP_STAT);
    g_assert_cmphex(stat & STAT_IRQ0, ==, STAT_IRQ0);

    qtest_memread(qts, DST_ADDR, dst, FM_LEN);
    for (i = 0; i < FM_LEN; i++) {
        if (dst[i] != src[i]) {
            g_test_message("OFM[%d] = 0x%x, want 0x%x", i, dst[i], src[i]);
        }
        g_assert_cmpuint(dst[i], ==, src[i]);
    }

    qtest_quit(qts);
}

static uint8_t over(uint8_t s, uint8_t d, uint8_t a)
{
    return (uint8_t)((s * a + d * (255 - a) + 127) / 255);
}

/* CH1 foreground src-over CH0 background, stored to the dest, byte-exact. */
static void test_blend(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    const uint32_t fg = 0x80c08040u, bg = 0xff102030u;   /* RGBA8888 */
    g_autofree uint32_t *fbuf = g_malloc(FM_LEN);
    g_autofree uint32_t *dbuf = g_malloc(FM_LEN);
    uint32_t want, stat;
    uint8_t a = (fg >> 24) & 0xff, da = (bg >> 24) & 0xff;
    int i;

    for (i = 0; i < (int)(FM_LEN / 4); i++) {
        fbuf[i] = fg; dbuf[i] = bg;
    }
    qtest_memwrite(qts, SRC_ADDR, fbuf, FM_LEN);     /* foreground */
    qtest_memwrite(qts, DST_ADDR, dbuf, FM_LEN);     /* background + dest */

    want = ((uint32_t)(a + (uint8_t)((da * (255 - a) + 127) / 255)) << 24)
         | ((uint32_t)over((fg >> 16) & 0xff, (bg >> 16) & 0xff, a) << 16)
         | ((uint32_t)over((fg >> 8) & 0xff, (bg >> 8) & 0xff, a) << 8)
         | over(fg & 0xff, bg & 0xff, a);

    pxp_writel(qts, PXP_FETCH_SIZE, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_FETCH_PITCH, (PITCH << 16) | PITCH);  /* CH1|CH0 */
    pxp_writel(qts, PXP_STORE_SIZE, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_STORE_PITCH, PITCH);
    pxp_writel(qts, PXP_STORE_ADDR, (uint32_t)DST_ADDR);
    pxp_writel(qts, PXP_FETCH_ADDR, (uint32_t)DST_ADDR);    /* CH0 background */
    pxp_writel(qts, PXP_FETCH_ADDR1, (uint32_t)SRC_ADDR);   /* CH1 fg, blend */

    pxp_writel(qts, PXP_CTRL_SET, CTRL_ENABLE);

    stat = qtest_readl(qts, PXP_BASE + PXP_STAT);
    g_assert_cmphex(stat & STAT_IRQ0, ==, STAT_IRQ0);

    qtest_memread(qts, DST_ADDR, dbuf, FM_LEN);
    for (i = 0; i < (int)(FM_LEN / 4); i++) {
        if (dbuf[i] != want) {
            g_test_message("OFM[%d] = 0x%x, want 0x%x", i, dbuf[i], want);
        }
        g_assert_cmphex(dbuf[i], ==, want);
    }

    qtest_quit(qts);
}

/* 90-degree rotation: dst(ox,oy) maps to src(oy, RH-1-ox). */
static void test_rotate(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    enum { RW = 32, RH = 32, RP = RW * 4, RLEN = RP * RH };
    g_autofree uint32_t *sbuf = g_malloc(RLEN);
    g_autofree uint32_t *dbuf = g_malloc(RLEN);
    uint32_t stat;
    int x, y;

    for (y = 0; y < RH; y++) {
        for (x = 0; x < RW; x++) {
            sbuf[y * RW + x] = 0xff000000u | ((uint32_t)y << 8) | (uint32_t)x;
        }
    }
    qtest_memwrite(qts, SRC_ADDR, sbuf, RLEN);
    memset(dbuf, 0, RLEN);
    qtest_memwrite(qts, DST_ADDR, dbuf, RLEN);

    pxp_writel(qts, PXP_FETCH_SIZE, ((RW - 1) << 16) | (RH - 1));
    pxp_writel(qts, PXP_FETCH_PITCH, RP);
    pxp_writel(qts, PXP_FETCH_CTRL, 0x1000);             /* rotate 90 */
    pxp_writel(qts, PXP_STORE_SIZE, ((RW - 1) << 16) | (RH - 1));
    pxp_writel(qts, PXP_STORE_PITCH, RP);
    pxp_writel(qts, PXP_STORE_ADDR, (uint32_t)DST_ADDR);
    pxp_writel(qts, PXP_FETCH_ADDR, (uint32_t)SRC_ADDR);

    pxp_writel(qts, PXP_CTRL_SET, CTRL_ENABLE);

    stat = qtest_readl(qts, PXP_BASE + PXP_STAT);
    g_assert_cmphex(stat & STAT_IRQ0, ==, STAT_IRQ0);

    qtest_memread(qts, DST_ADDR, dbuf, RLEN);
    for (y = 0; y < RH; y++) {
        for (x = 0; x < RW; x++) {
            uint32_t want = 0xff000000u | ((uint32_t)(RH - 1 - x) << 8) | y;
            g_assert_cmphex(dbuf[y * RW + x], ==, want);
        }
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/aarch64/imx93-pxp/copy", test_copy);
    qtest_add_func("/aarch64/imx93-pxp/fill", test_fill);
    qtest_add_func("/aarch64/imx93-pxp/blit", test_blit);
    qtest_add_func("/aarch64/imx93-pxp/blend", test_blend);
    qtest_add_func("/aarch64/imx93-pxp/rotate", test_rotate);
    return g_test_run();
}
