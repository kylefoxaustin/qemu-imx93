/*
 * NXP i.MX 93 PXP (Pixel Pipeline) 2D engine
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional model of the i.MX 93 PXP 2D engine driven by the built-in
 * pxp_dma_v3 driver via libg2d. HW_PXP_CTRL provides the soft-reset and the
 * shared ENABLE kick; on the kick the engine runs one pass and raises the
 * completion IRQ (WAKEUPMIX PXP interrupt 0) the driver's fence waits on. Every
 * register is MXS SET/CLR/TOG aliased; set PXP_DBG to trace accesses. The op
 * register layouts were captured from the live driver (tests/pxp-imx93).
 *
 * Scope is same-format copy, constant-colour fill, opaque Fetch->Store blit,
 * src-over alpha blend and 90/180/270 rotation. CSC is not modelled; g2d scale
 * is rejected by the driver ("unsupport 2d operation"), so neither runs here.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_pxp.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/vmstate.h"

/* HW_PXP_CTRL (0x00) and STAT (0x10); both MXS SET/CLR/TOG aliased. */
#define HW_PXP_CTRL         0x00
#define HW_PXP_STAT         0x10

#define BM_PXP_CTRL_ENABLE  0x00000001
#define BM_PXP_CTRL_SFTRST  0x80000000
#define BM_PXP_CTRL_CLKGATE 0x40000000
#define BM_PXP_STAT_IRQ0    0x00000001

/* Surface registers (base offsets; the driver writes these directly). */
#define HW_PXP_OUT_CTRL     0x20    /* [7:0] FORMAT */
#define HW_PXP_OUT_BUF      0x30    /* dest physical address */
#define HW_PXP_OUT_PITCH    0x50    /* dest stride, bytes */
#define HW_PXP_OUT_LRC      0x60    /* [31:16] width-1, [15:0] height-1 */
#define HW_PXP_OUT_PS_ULC   0x70
#define HW_PXP_OUT_PS_LRC   0x80
#define HW_PXP_PS_CTRL      0xb0    /* [7:0] FORMAT */
#define HW_PXP_PS_BUF       0xc0    /* source physical address */
#define HW_PXP_PS_PITCH     0xf0    /* source stride, bytes */

/*
 * Fetch/Store engine cluster (0x400..0x6ff). g2d_clear (fill) and g2d_blit
 * (compositing) use this path instead of the legacy PS/OUT registers, kicked by
 * the same HW_PXP_CTRL ENABLE. Only the Store fill registers are modelled.
 */
#define HW_PXP_FETCH_CTRL   0x450   /* CH0 ctrl; [13:12] = rotation */
#define HW_PXP_FETCH_SIZE   0x4a0   /* INPUT_FETCH size: w-1<<16 | h-1 */
#define HW_PXP_FETCH_PITCH  0x510   /* stride: [31:16] CH1, [15:0] CH0 */
#define HW_PXP_FETCH_ADDR   0x580   /* CH0 source (blit src / blend bg) */
#define HW_PXP_FETCH_ADDR1  0x5a0   /* CH1 source (blend foreground) */
#define HW_PXP_STORE_SIZE   0x600   /* [31:16] width-1, [15:0] height-1 */
#define HW_PXP_STORE_PITCH  0x620   /* dest stride, bytes */
#define HW_PXP_STORE_CTRL   0x630   /* INPUT_STORE_CTRL_CH0 (FILL_DATA_EN) */
#define HW_PXP_STORE_ADDR   0x690   /* dest physical address */
#define HW_PXP_STORE_FILL   0x6b0   /* INPUT_STORE_FILL_DATA_CH0 */

/* Which op the driver armed for the shared ENABLE kick. */
#define PXP_OP_COPY         0       /* legacy PS->OUT copy */
#define PXP_OP_FILL         1       /* Store-engine constant-colour fill */
#define PXP_OP_BLIT         2       /* Fetch->Store surface blit */

#define R(s, off)           ((s)->regs[(off) / 4])

static void imx93_pxp_trace(const char *op, hwaddr offset, uint32_t value)
{
    if (getenv("PXP_DBG")) {
        fprintf(stderr, "[pxp] %s +0x%04x = 0x%08x\n", op,
                (unsigned)offset, value);
    }
}

/* Bytes per pixel for the PS/OUT FORMAT field (RGB formats only). */
static int imx93_pxp_bpp(uint32_t ctrl)
{
    switch (ctrl & 0x1f) {
    case 0x08 ... 0x0f:     /* RGB555 / ARGB1555 / RGB565 / ARGB4444 */
        return 2;
    default:                /* ARGB8888 / RGB888-in-32 and friends */
        return 4;
    }
}

/*
 * Swap the R and B bytes of a 32-bit pixel (RGBA8888 <-> the Store internal
 * order, which the capture showed is byte-0/byte-2 swapped).
 */
static inline uint32_t imx93_pxp_swap_rb(uint32_t v)
{
    return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
}

/* Legacy PS->OUT same-format copy (g2d_copy path, registers 0x00..0x3ff). */
static void imx93_pxp_copy(IMX93PxpState *s)
{
    uint32_t lrc = R(s, HW_PXP_OUT_LRC);
    uint32_t width = ((lrc >> 16) & 0xffff) + 1;
    uint32_t height = (lrc & 0xffff) + 1;
    int bpp = imx93_pxp_bpp(R(s, HW_PXP_OUT_CTRL));
    uint64_t out_buf = R(s, HW_PXP_OUT_BUF);
    uint64_t ps_buf = R(s, HW_PXP_PS_BUF);
    uint32_t out_pitch = R(s, HW_PXP_OUT_PITCH) & 0xffff;
    uint32_t ps_pitch = R(s, HW_PXP_PS_PITCH) & 0xffff;
    size_t line_bytes = (size_t)width * bpp;
    g_autofree uint8_t *line = NULL;
    uint32_t y;

    if (!out_buf || !ps_buf || line_bytes == 0) {
        return;
    }
    out_pitch = out_pitch ? out_pitch : line_bytes;
    ps_pitch = ps_pitch ? ps_pitch : line_bytes;
    line = g_malloc(line_bytes);

    for (y = 0; y < height; y++) {
        if (dma_memory_read(&address_space_memory,
                            ps_buf + (uint64_t)y * ps_pitch, line,
                            line_bytes, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }
        dma_memory_write(&address_space_memory,
                         out_buf + (uint64_t)y * out_pitch, line, line_bytes,
                         MEMTXATTRS_UNSPECIFIED);
    }
}

/* Store-engine constant-colour fill (g2d_clear, registers 0x400..0x6ff). */
static void imx93_pxp_fill(IMX93PxpState *s)
{
    uint32_t size = R(s, HW_PXP_STORE_SIZE);
    uint32_t width = ((size >> 16) & 0xffff) + 1;
    uint32_t height = (size & 0xffff) + 1;
    uint64_t out_buf = R(s, HW_PXP_STORE_ADDR);
    uint32_t pitch = R(s, HW_PXP_STORE_PITCH) & 0xffff;
    uint32_t pixel = imx93_pxp_swap_rb(R(s, HW_PXP_STORE_FILL));
    size_t line_bytes = (size_t)width * 4;     /* 32bpp fill */
    g_autofree uint32_t *line = NULL;
    uint32_t x, y;

    if (!out_buf || line_bytes == 0) {
        return;
    }
    pitch = pitch ? pitch : line_bytes;
    line = g_malloc(line_bytes);
    for (x = 0; x < width; x++) {
        line[x] = pixel;
    }
    for (y = 0; y < height; y++) {
        dma_memory_write(&address_space_memory,
                         out_buf + (uint64_t)y * pitch, line, line_bytes,
                         MEMTXATTRS_UNSPECIFIED);
    }
}

/*
 * Fetch->Store surface blit (g2d_blit), with optional 90/180/270 rotation from
 * FETCH_CTRL[13:12]. The Fetch input conversion and the Store output conversion
 * cancel for a same-format blit, so the surface lands unchanged (just rotated).
 */
static void imx93_pxp_fetch_store(IMX93PxpState *s)
{
    uint32_t fsize = R(s, HW_PXP_FETCH_SIZE);
    uint32_t sw = ((fsize >> 16) & 0xffff) + 1;     /* source width  */
    uint32_t sh = (fsize & 0xffff) + 1;             /* source height */
    uint32_t dsize = R(s, HW_PXP_STORE_SIZE);
    uint32_t dw = ((dsize >> 16) & 0xffff) + 1;     /* dest width  */
    uint32_t dh = (dsize & 0xffff) + 1;             /* dest height */
    uint64_t src = R(s, HW_PXP_FETCH_ADDR);
    uint64_t dst = R(s, HW_PXP_STORE_ADDR);
    uint32_t src_pitch = R(s, HW_PXP_FETCH_PITCH) & 0xffff;
    uint32_t dst_pitch = R(s, HW_PXP_STORE_PITCH) & 0xffff;
    uint32_t rot = (R(s, HW_PXP_FETCH_CTRL) >> 12) & 0x3;
    g_autofree uint8_t *sbuf = NULL;
    g_autofree uint32_t *dl = NULL;
    uint32_t ox, oy;

    if (!src || !dst || sw == 0 || sh == 0) {
        return;
    }
    src_pitch = src_pitch ? src_pitch : sw * 4;
    dst_pitch = dst_pitch ? dst_pitch : dw * 4;

    if (rot == 0) {                 /* fast path: straight line copy */
        g_autofree uint8_t *line = g_malloc((size_t)sw * 4);
        for (oy = 0; oy < sh; oy++) {
            if (dma_memory_read(&address_space_memory,
                                src + (uint64_t)oy * src_pitch, line,
                                (size_t)sw * 4, MEMTXATTRS_UNSPECIFIED)
                    != MEMTX_OK) {
                break;
            }
            dma_memory_write(&address_space_memory,
                             dst + (uint64_t)oy * dst_pitch, line,
                             (size_t)sw * 4, MEMTXATTRS_UNSPECIFIED);
        }
        return;
    }

    /* Rotated: read the whole source, then remap each dest pixel from it. */
    sbuf = g_malloc((size_t)sh * src_pitch);
    if (dma_memory_read(&address_space_memory, src, sbuf,
                        (size_t)sh * src_pitch, MEMTXATTRS_UNSPECIFIED)
            != MEMTX_OK) {
        return;
    }
    dl = g_malloc((size_t)dw * 4);
    for (oy = 0; oy < dh; oy++) {
        for (ox = 0; ox < dw; ox++) {
            uint32_t sx, sy;
            switch (rot) {
            case 1:                              /* 90  */
                sx = oy;
                sy = sh - 1 - ox;
                break;
            case 2:                              /* 180 */
                sx = sw - 1 - ox;
                sy = sh - 1 - oy;
                break;
            default:                             /* 270 */
                sx = sw - 1 - oy;
                sy = ox;
                break;
            }
            if (sx < sw && sy < sh) {
                memcpy(&dl[ox],
                       sbuf + (size_t)sy * src_pitch + (size_t)sx * 4, 4);
            } else {
                dl[ox] = 0;
            }
        }
        dma_memory_write(&address_space_memory, dst + (uint64_t)oy * dst_pitch,
                         dl, (size_t)dw * 4, MEMTXATTRS_UNSPECIFIED);
    }
}

/* Per-channel non-premultiplied src-over: out = (s*a + d*(255-a) + 127)/255. */
static inline uint8_t imx93_pxp_over(uint8_t sc, uint8_t dc, uint8_t a)
{
    return (uint8_t)((sc * a + dc * (255 - a) + 127) / 255);
}

/*
 * Alpha-blended blit: Fetch CH1 (foreground) src-over Fetch CH0 (background),
 * stored back to the destination. g2d_blit with G2D_BLEND. The R/B fetch and
 * store conversions cancel per channel, so the blend runs directly in RGBA8888.
 */
static void imx93_pxp_blend(IMX93PxpState *s)
{
    uint32_t size = R(s, HW_PXP_STORE_SIZE);
    uint32_t width = ((size >> 16) & 0xffff) + 1;
    uint32_t height = (size & 0xffff) + 1;
    uint64_t fg = R(s, HW_PXP_FETCH_ADDR1);    /* CH1 foreground */
    uint64_t bg = R(s, HW_PXP_FETCH_ADDR);     /* CH0 background */
    uint64_t dst = R(s, HW_PXP_STORE_ADDR);
    uint32_t fpitch = (R(s, HW_PXP_FETCH_PITCH) >> 16) & 0xffff;
    uint32_t bpitch = R(s, HW_PXP_FETCH_PITCH) & 0xffff;
    uint32_t dpitch = R(s, HW_PXP_STORE_PITCH) & 0xffff;
    g_autofree uint32_t *fl = NULL, *bl = NULL;
    uint32_t x, y;

    if (!fg || !bg || !dst || width == 0) {
        return;
    }
    fpitch = fpitch ? fpitch : width * 4;
    bpitch = bpitch ? bpitch : width * 4;
    dpitch = dpitch ? dpitch : width * 4;
    fl = g_malloc((size_t)width * 4);
    bl = g_malloc((size_t)width * 4);

    for (y = 0; y < height; y++) {
        if (dma_memory_read(&address_space_memory, fg + (uint64_t)y * fpitch,
                            fl, (size_t)width * 4, MEMTXATTRS_UNSPECIFIED)
                != MEMTX_OK ||
            dma_memory_read(&address_space_memory, bg + (uint64_t)y * bpitch,
                            bl, (size_t)width * 4, MEMTXATTRS_UNSPECIFIED)
                != MEMTX_OK) {
            break;
        }
        for (x = 0; x < width; x++) {
            uint32_t sp = fl[x], dp = bl[x];
            uint8_t a = (sp >> 24) & 0xff, da = (dp >> 24) & 0xff;
            uint8_t r = imx93_pxp_over(sp & 0xff, dp & 0xff, a);
            uint8_t g = imx93_pxp_over((sp >> 8) & 0xff, (dp >> 8) & 0xff, a);
            uint8_t b = imx93_pxp_over((sp >> 16) & 0xff, (dp >> 16) & 0xff, a);
            uint8_t oa = a + (uint8_t)((da * (255 - a) + 127) / 255);
            bl[x] = ((uint32_t)oa << 24) | ((uint32_t)b << 16) |
                    ((uint32_t)g << 8) | r;
        }
        dma_memory_write(&address_space_memory, dst + (uint64_t)y * dpitch,
                         bl, (size_t)width * 4, MEMTXATTRS_UNSPECIFIED);
    }
}

/*
 * Execute one PXP pass on the ENABLE kick and post the completion interrupt.
 * The legacy copy, Store fill, Fetch->Store blit and blend share the kick;
 * dispatch on whichever the driver armed last (a fetch source, a fill word, a
 * legacy OUT/PS buffer, or a CH1 source for blend; see imx93_pxp_write).
 */
static void imx93_pxp_blit(IMX93PxpState *s)
{
    if (s->blend_pending) {
        imx93_pxp_blend(s);
    } else {
        switch (s->op_mode) {
        case PXP_OP_FILL:
            imx93_pxp_fill(s);
            break;
        case PXP_OP_BLIT:
            imx93_pxp_fetch_store(s);
            break;
        default:
            imx93_pxp_copy(s);
            break;
        }
    }
    s->blend_pending = false;       /* re-armed by the next CH1 write */

    /* Hardware clears ENABLE when the frame completes and posts IRQ0. */
    s->ctrl &= ~BM_PXP_CTRL_ENABLE;
    s->stat |= BM_PXP_STAT_IRQ0;
    qemu_set_irq(s->irq, 1);
}

static uint64_t imx93_pxp_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93PxpState *s = opaque;
    uint32_t base = offset & ~0xfu;
    uint32_t val;

    if (base == HW_PXP_CTRL) {
        /* Asserting SFTRST also asserts CLKGATE; the driver polls for this. */
        val = s->ctrl;
        if (val & BM_PXP_CTRL_SFTRST) {
            val |= BM_PXP_CTRL_CLKGATE;
        }
    } else if (base == HW_PXP_STAT) {
        val = s->stat;
    } else {
        val = s->regs[base / 4];
    }
    imx93_pxp_trace("rd", offset, val);
    return val;
}

/* Apply an MXS SET/CLR/TOG write (offset & 0xf selects the alias) to *reg. */
static void imx93_pxp_mxs(uint32_t *reg, hwaddr offset, uint32_t value)
{
    switch (offset & 0xf) {
    case 0x0:
        *reg = value;
        break;
    case 0x4:
        *reg |= value;
        break;
    case 0x8:
        *reg &= ~value;
        break;
    case 0xc:
        *reg ^= value;
        break;
    }
}

static void imx93_pxp_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93PxpState *s = opaque;
    uint32_t base = offset & ~0xfu;

    imx93_pxp_trace("wr", offset, (uint32_t)value);

    if (base == HW_PXP_CTRL) {
        imx93_pxp_mxs(&s->ctrl, offset, value);
        /* Writing ENABLE kicks one pass; the blit clears it on completion. */
        if (s->ctrl & BM_PXP_CTRL_ENABLE) {
            imx93_pxp_blit(s);
        }
    } else if (base == HW_PXP_STAT) {
        imx93_pxp_mxs(&s->stat, offset, value);
        /* Driver clears IRQ0 in its handler; drop the line when it does. */
        if (!(s->stat & BM_PXP_STAT_IRQ0)) {
            qemu_set_irq(s->irq, 0);
        }
    } else {
        imx93_pxp_mxs(&s->regs[base / 4], offset, value);
        /*
         * Copy, fill and blit all share the CTRL ENABLE kick, so remember which
         * the driver armed most recently by its distinguishing register write:
         * a Fetch source -> blit, a Store fill word -> fill, a legacy OUT/PS
         * buffer -> copy.
         */
        if (base == HW_PXP_FETCH_ADDR1) {
            /* CH1 source is unique to a blend; it wins at the kick. */
            s->blend_pending = true;
        } else if (base == HW_PXP_FETCH_ADDR) {
            s->op_mode = PXP_OP_BLIT;
        } else if (base == HW_PXP_STORE_FILL) {
            s->op_mode = PXP_OP_FILL;
        } else if (base == HW_PXP_OUT_BUF || base == HW_PXP_PS_BUF) {
            s->op_mode = PXP_OP_COPY;
        }
    }
}

static const MemoryRegionOps imx93_pxp_ops = {
    .read = imx93_pxp_read,
    .write = imx93_pxp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_pxp_reset(DeviceState *dev)
{
    IMX93PxpState *s = IMX93_PXP(dev);

    s->ctrl = 0;
    s->stat = 0;
    s->op_mode = PXP_OP_COPY;
    s->blend_pending = false;
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void imx93_pxp_init(Object *obj)
{
    IMX93PxpState *s = IMX93_PXP(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_pxp_ops, s,
                          TYPE_IMX93_PXP, IMX93_PXP_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_imx93_pxp = {
    .name = TYPE_IMX93_PXP,
    .version_id = 5,
    .minimum_version_id = 5,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, IMX93PxpState),
        VMSTATE_UINT32(stat, IMX93PxpState),
        VMSTATE_UINT32(op_mode, IMX93PxpState),
        VMSTATE_BOOL(blend_pending, IMX93PxpState),
        VMSTATE_UINT32_ARRAY(regs, IMX93PxpState, IMX93_PXP_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_pxp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 PXP (Pixel Pipeline) 2D engine";
    device_class_set_legacy_reset(dc, imx93_pxp_reset);
    dc->vmsd = &vmstate_imx93_pxp;
}

static const TypeInfo imx93_pxp_types[] = {
    {
        .name           = TYPE_IMX93_PXP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93PxpState),
        .instance_init  = imx93_pxp_init,
        .class_init     = imx93_pxp_class_init,
    },
};

DEFINE_TYPES(imx93_pxp_types)
