/*
 * g2d_copy oracle for the i.MX93 PXP model.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a linear PXP copy through the real userspace stack:
 *   libg2d (imx-pxp-g2d) -> ioctl(/dev/pxp_device) -> pxp_dma_v3 -> PXP model.
 * Allocates two physically-contiguous buffers, fills the source with a known
 * pattern, g2d_copy()s it to the destination, waits for completion and verifies
 * the bytes match. Prints "PXP-G2D-COPY: PASS" / "FAIL" so a boot log can be
 * scored. Used both to capture the driver's blit register sequence (run it with
 * PXP_DBG set on the QEMU side) and as the end-to-end pass/fail oracle once the
 * blit datapath is modelled. g2d_basic_test is not in the BSP, hence this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "g2d.h"

#define COPY_BYTES (64 * 1024)

#define FILL_W      64
#define FILL_H      64
#define FILL_COLOR  0x11223344    /* RGBA8888 clrcolor */

/* g2d_clear(): fill a surface rectangle with a constant colour, verify it. */
static int test_fill(void *handle)
{
    struct g2d_buf *buf = g2d_alloc(FILL_W * FILL_H * 4, 0);
    struct g2d_surface s;
    uint32_t *px;
    int i, rc, bad = 0;

    if (!buf) {
        printf("PXP-G2D-FILL: FAIL (g2d_alloc)\n");
        return 1;
    }
    px = buf->buf_vaddr;
    for (i = 0; i < FILL_W * FILL_H; i++) {
        px[i] = 0xdeadbeef;
    }
    g2d_cache_op(buf, G2D_CACHE_FLUSH);

    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888;
    s.planes[0] = buf->buf_paddr;
    s.left = 0; s.top = 0; s.right = FILL_W; s.bottom = FILL_H;
    s.stride = FILL_W; s.width = FILL_W; s.height = FILL_H;
    s.clrcolor = FILL_COLOR;

    rc = g2d_clear(handle, &s);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(buf, G2D_CACHE_INVALIDATE);

    if (rc != 0) {
        printf("PXP-G2D-FILL: FAIL (g2d_clear/finish rc=%d)\n", rc);
        bad = 1;
    } else {
        for (i = 0; i < FILL_W * FILL_H; i++) {
            if (px[i] != FILL_COLOR) {
                bad++;
            }
        }
        printf("PXP-G2D-FILL: %s (%d/%d px wrong, px0=%#x)\n",
               bad ? "FAIL" : "PASS", bad, FILL_W * FILL_H, px[0]);
    }
    g2d_free(buf);
    return bad ? 1 : 0;
}

#define BLIT_W  64
#define BLIT_H  64

/* g2d_blit(): opaque same-format surface->surface blit, verify dst == src. */
static int test_blit(void *handle)
{
    struct g2d_buf *src = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_buf *dst = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_surface s, d;
    uint32_t *sp, *dp;
    int i, rc, bad = 0;

    if (!src || !dst) {
        printf("PXP-G2D-BLIT: FAIL (g2d_alloc)\n");
        return 1;
    }
    sp = src->buf_vaddr;
    dp = dst->buf_vaddr;
    for (i = 0; i < BLIT_W * BLIT_H; i++) {
        sp[i] = 0x10000000u + (uint32_t)i;     /* unique per pixel */
        dp[i] = 0xa5a5a5a5u;
    }
    g2d_cache_op(src, G2D_CACHE_FLUSH);
    g2d_cache_op(dst, G2D_CACHE_FLUSH);

    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888; s.planes[0] = src->buf_paddr;
    s.left = 0; s.top = 0; s.right = BLIT_W; s.bottom = BLIT_H;
    s.stride = BLIT_W; s.width = BLIT_W; s.height = BLIT_H;
    s.blendfunc = G2D_ONE; s.global_alpha = 255;
    d = s;
    d.planes[0] = dst->buf_paddr;
    d.blendfunc = G2D_ZERO;

    rc = g2d_blit(handle, &s, &d);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(dst, G2D_CACHE_INVALIDATE);

    if (rc != 0) {
        printf("PXP-G2D-BLIT: FAIL (g2d_blit/finish rc=%d)\n", rc);
        bad = 1;
    } else {
        for (i = 0; i < BLIT_W * BLIT_H; i++) {
            if (dp[i] != sp[i]) {
                bad++;
            }
        }
        printf("PXP-G2D-BLIT: %s (%d/%d px wrong, px0=%#x want %#x)\n",
               bad ? "FAIL" : "PASS", bad, BLIT_W * BLIT_H, dp[0], sp[0]);
    }
    g2d_free(src);
    g2d_free(dst);
    return bad ? 1 : 0;
}

/* src-over alpha blend: out = (src*a + dst*(255-a) + 127) / 255 per channel. */
static uint32_t srcover(uint32_t s, uint32_t d)
{
    uint32_t a = (s >> 24) & 0xff, out = 0, i;
    for (i = 0; i < 4; i++) {
        uint32_t sc = (s >> (i * 8)) & 0xff, dc = (d >> (i * 8)) & 0xff;
        uint32_t v = (i == 3) ? 0xff : (sc * a + dc * (255 - a) + 127) / 255;
        out |= v << (i * 8);
    }
    return out;
}

/* g2d_blit with G2D_BLEND: a translucent source composited over an opaque
 * destination; verify the result is src-over (within a small rounding margin). */
static int test_blend(void *handle)
{
    struct g2d_buf *src = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_buf *dst = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_surface s, d;
    uint32_t *sp, *dp, fg = 0x80c08040u /* A=80 B=c0 G=80 R=40 */, bg = 0xff102030u;
    uint32_t want;
    int i, rc, bad = 0;

    if (!src || !dst) {
        printf("PXP-G2D-BLEND: FAIL (g2d_alloc)\n");
        return 1;
    }
    sp = src->buf_vaddr; dp = dst->buf_vaddr;
    for (i = 0; i < BLIT_W * BLIT_H; i++) { sp[i] = fg; dp[i] = bg; }
    g2d_cache_op(src, G2D_CACHE_FLUSH);
    g2d_cache_op(dst, G2D_CACHE_FLUSH);

    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888; s.planes[0] = src->buf_paddr;
    s.left = 0; s.top = 0; s.right = BLIT_W; s.bottom = BLIT_H;
    s.stride = BLIT_W; s.width = BLIT_W; s.height = BLIT_H;
    s.blendfunc = G2D_SRC_ALPHA; s.global_alpha = 255;
    d = s;
    d.planes[0] = dst->buf_paddr;
    d.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;

    g2d_enable(handle, G2D_BLEND);
    rc = g2d_blit(handle, &s, &d);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_disable(handle, G2D_BLEND);
    g2d_cache_op(dst, G2D_CACHE_INVALIDATE);

    want = srcover(fg, bg);
    if (rc != 0) {
        printf("PXP-G2D-BLEND: FAIL (g2d_blit/finish rc=%d)\n", rc);
        bad = 1;
    } else {
        for (i = 0; i < BLIT_W * BLIT_H; i++) {
            uint32_t g = dp[i], j;
            for (j = 0; j < 4; j++) {
                int gv = (g >> (j * 8)) & 0xff, wv = (want >> (j * 8)) & 0xff;
                if (gv - wv > 2 || wv - gv > 2) { bad++; break; }
            }
        }
        printf("PXP-G2D-BLEND: %s (%d/%d px wrong, px0=%#x want~%#x)\n",
               bad ? "FAIL" : "PASS", bad, BLIT_W * BLIT_H, dp[0], want);
    }
    g2d_free(src); g2d_free(dst);
    return bad ? 1 : 0;
}

/* g2d_blit with rotation; src pixel encodes (y<<8|x) so the mapping is visible.
 * Prints the dst corners to deduce the rotation direction per angle. */
static int test_rotate(void *handle, int rot, const char *name)
{
    struct g2d_buf *src = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_buf *dst = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_surface s, d;
    uint32_t *sp, *dp;
    int x, y, rc;

    if (!src || !dst) {
        printf("PXP-G2D-ROT%s: FAIL (g2d_alloc)\n", name);
        return 1;
    }
    sp = src->buf_vaddr; dp = dst->buf_vaddr;
    for (y = 0; y < BLIT_H; y++) {
        for (x = 0; x < BLIT_W; x++) {
            sp[y * BLIT_W + x] = 0xff000000u | ((uint32_t)y << 8) | (uint32_t)x;
        }
    }
    memset(dp, 0, BLIT_W * BLIT_H * 4);
    g2d_cache_op(src, G2D_CACHE_FLUSH);
    g2d_cache_op(dst, G2D_CACHE_FLUSH);

    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888; s.planes[0] = src->buf_paddr;
    s.left = 0; s.top = 0; s.right = BLIT_W; s.bottom = BLIT_H;
    s.stride = BLIT_W; s.width = BLIT_W; s.height = BLIT_H;
    s.blendfunc = G2D_ONE; s.rot = G2D_ROTATION_0;
    d = s;
    d.planes[0] = dst->buf_paddr;
    d.blendfunc = G2D_ZERO;
    d.rot = rot;

    rc = g2d_blit(handle, &s, &d);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(dst, G2D_CACHE_INVALIDATE);

    int bad = 0;
    if (rc != 0) {
        printf("PXP-G2D-ROT%s: FAIL (rc=%d)\n", name, rc);
        bad = 1;
    } else {
        /* dst(ox,oy) maps to src(sx,sy) per the rotation; src encodes y<<8|x. */
        for (y = 0; y < BLIT_H && bad < 1; y++) {
            for (x = 0; x < BLIT_W; x++) {
                int sx, sy;
                if (rot == G2D_ROTATION_90)      { sx = y; sy = BLIT_H - 1 - x; }
                else if (rot == G2D_ROTATION_180){ sx = BLIT_W - 1 - x; sy = BLIT_H - 1 - y; }
                else                             { sx = BLIT_W - 1 - y; sy = x; }
                uint32_t want = 0xff000000u | ((uint32_t)sy << 8) | (uint32_t)sx;
                if (dp[y * BLIT_W + x] != want) { bad++; break; }
            }
        }
        printf("PXP-G2D-ROT%s: %s (d[0,0]=%#x)\n", name,
               bad ? "FAIL" : "PASS", dp[0]);
    }
    g2d_free(src); g2d_free(dst);
    return bad ? 1 : 0;
}

int main(void)
{
    void *handle = NULL;
    struct g2d_buf *src, *dst;
    unsigned char *sp, *dp;
    int i, rc, bad = 0;

    if (g2d_open(&handle) || !handle) {
        printf("PXP-G2D-COPY: FAIL (g2d_open)\n");
        return 1;
    }
    src = g2d_alloc(COPY_BYTES, 0);
    dst = g2d_alloc(COPY_BYTES, 0);
    if (!src || !dst) {
        printf("PXP-G2D-COPY: FAIL (g2d_alloc)\n");
        return 1;
    }

    sp = src->buf_vaddr;
    dp = dst->buf_vaddr;
    for (i = 0; i < COPY_BYTES; i++) {
        sp[i] = (unsigned char)(i * 7 + 0x11);   /* deterministic pattern */
    }
    memset(dp, 0xa5, COPY_BYTES);
    g2d_cache_op(src, G2D_CACHE_FLUSH);
    g2d_cache_op(dst, G2D_CACHE_FLUSH);

    rc = g2d_copy(handle, dst, src, COPY_BYTES);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(dst, G2D_CACHE_INVALIDATE);

    if (rc != 0) {
        printf("PXP-G2D-COPY: FAIL (g2d_copy/finish rc=%d)\n", rc);
    } else {
        for (i = 0; i < COPY_BYTES; i++) {
            if (dp[i] != sp[i]) {
                if (bad < 4) {
                    printf("  byte %d: got %#x want %#x\n", i, dp[i], sp[i]);
                }
                bad++;
            }
        }
        printf("PXP-G2D-COPY: %s (%d/%d bytes mismatched)\n",
               bad ? "FAIL" : "PASS", bad, COPY_BYTES);
    }

    g2d_free(src);
    g2d_free(dst);

    /* Second op: constant-colour fill (g2d_clear) via the Store engine. */
    bad += test_fill(handle);

    /* Third op: opaque surface->surface blit (g2d_blit, fetch->store). */
    bad += test_blit(handle);

    /* Fourth op: alpha-blended blit (g2d_blit + G2D_BLEND, src-over). */
    bad += test_blend(handle);

    /* Fifth: rotations 90/180/270 (capture/observe the angle encoding). */
    bad += test_rotate(handle, G2D_ROTATION_90, "90");
    bad += test_rotate(handle, G2D_ROTATION_180, "180");
    bad += test_rotate(handle, G2D_ROTATION_270, "270");

    g2d_close(handle);
    return bad ? 1 : 0;
}
