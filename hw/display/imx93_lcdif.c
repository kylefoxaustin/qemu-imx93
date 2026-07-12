/*
 * NXP i.MX 93 LCDIFv3 (LCDC "V8") display controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout and programming sequence follow the upstream Linux
 * drm/mxsfb "lcdif" driver (lcdif_regs.h / lcdif_kms.c), which drives both
 * "fsl,imx8mp-lcdif" and "fsl,imx93-lcdif". Only the primary plane (layer 0)
 * is modelled; that is all the driver uses on the i.MX93.
 */

#include "qemu/osdep.h"
#include "hw/display/imx93_lcdif.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "ui/pixel_ops.h"

/* V8 register offsets (see lcdif_regs.h). */
#define LCDC_V8_CTRL                0x00
#define LCDC_V8_DISP_PARA           0x10
#define LCDC_V8_DISP_SIZE           0x14
#define LCDC_V8_HSYN_PARA           0x18
#define LCDC_V8_VSYN_PARA           0x1c
#define LCDC_V8_VSYN_HSYN_WIDTH     0x20
#define LCDC_V8_INT_STATUS_D0       0x24
#define LCDC_V8_INT_ENABLE_D0       0x28
#define LCDC_V8_INT_STATUS_D1       0x30
#define LCDC_V8_INT_ENABLE_D1       0x34
#define LCDC_V8_CTRLDESCL0_1        0x200
#define LCDC_V8_CTRLDESCL0_3        0x208
#define LCDC_V8_CTRLDESCL_LOW0_4    0x20c
#define LCDC_V8_CTRLDESCL_HIGH0_4   0x210
#define LCDC_V8_CTRLDESCL0_5        0x214

#define REG_SET                     0x4
#define REG_CLR                     0x8
#define REG_TOG                     0xc

#define CTRL_SW_RESET               (1u << 31)

#define DISP_PARA_DISP_ON           (1u << 31)

#define INT_STATUS_D0_VSYNC         (1u << 0)
#define INT_STATUS_D0_VS_BLANK      (1u << 2)
#define INT_STATUS_D0_DMA_DONE      (1u << 16)

#define CTRLDESCL0_5_EN             (1u << 31)
#define CTRLDESCL0_5_SHADOW_LOAD_EN (1u << 30)
#define CTRLDESCL0_5_BPP_SHIFT      24
#define CTRLDESCL0_5_BPP_MASK       (0xfu << 24)
#define BPP_16_RGB565               0x4
#define BPP_16_ARGB1555             0x5
#define BPP_16_ARGB4444             0x6
#define BPP_24_RGB888               0x8
#define BPP_32_ARGB8888             0x9
#define BPP_32_ABGR8888             0xa

/* 60 Hz worth of vblanks is plenty for DRM bookkeeping. */
#define VBLANK_PERIOD_NS            (NANOSECONDS_PER_SECOND / 60)

static inline uint32_t lcdif_reg(IMX93LcdifState *s, hwaddr off)
{
    return s->regs[off >> 2];
}

static bool lcdif_is_enabled(IMX93LcdifState *s)
{
    return (lcdif_reg(s, LCDC_V8_CTRLDESCL0_5) & CTRLDESCL0_5_EN) &&
           (lcdif_reg(s, LCDC_V8_DISP_PARA) & DISP_PARA_DISP_ON);
}

static void lcdif_update_irq(IMX93LcdifState *s)
{
    uint32_t d0 = s->regs[LCDC_V8_INT_STATUS_D0 >> 2] &
                  s->regs[LCDC_V8_INT_ENABLE_D0 >> 2];
    uint32_t d1 = s->regs[LCDC_V8_INT_STATUS_D1 >> 2] &
                  s->regs[LCDC_V8_INT_ENABLE_D1 >> 2];

    qemu_set_irq(s->irq, (d0 || d1) ? 1 : 0);
}

static void lcdif_vblank_tick(void *opaque)
{
    IMX93LcdifState *s = opaque;

    if (!lcdif_is_enabled(s)) {
        return;
    }

    s->regs[LCDC_V8_INT_STATUS_D0 >> 2] |=
        INT_STATUS_D0_VS_BLANK | INT_STATUS_D0_VSYNC;
    lcdif_update_irq(s);

    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBLANK_PERIOD_NS);
}

static void lcdif_refresh_enable(IMX93LcdifState *s)
{
    bool en = lcdif_is_enabled(s);

    if (en && !s->enabled) {
        s->invalidate = true;
        if (s->con) {
            qemu_console_hw_invalidate(s->con);
        }
        timer_mod(s->vblank_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBLANK_PERIOD_NS);
    } else if (!en && s->enabled) {
        timer_del(s->vblank_timer);
    }
    s->enabled = en;
}

/* ---- pixel converters: source is guest DRAM, dest is the QEMU surface ---- */

static void draw_line_rgb565(void *opaque, uint8_t *dst, const uint8_t *src,
                             int width, int dststep)
{
    uint32_t *d = (uint32_t *)dst;

    for (int i = 0; i < width; i++) {
        uint16_t p = lduw_le_p(src);
        uint8_t r = ((p >> 11) & 0x1f) << 3;
        uint8_t g = ((p >> 5) & 0x3f) << 2;
        uint8_t b = (p & 0x1f) << 3;
        *d++ = rgb_to_pixel32(r, g, b);
        src += 2;
    }
}

static void draw_line_rgb888(void *opaque, uint8_t *dst, const uint8_t *src,
                             int width, int dststep)
{
    uint32_t *d = (uint32_t *)dst;

    /* DRM_FORMAT_RGB888: in memory byte0=B, byte1=G, byte2=R. */
    for (int i = 0; i < width; i++) {
        uint8_t b = src[0], g = src[1], r = src[2];
        *d++ = rgb_to_pixel32(r, g, b);
        src += 3;
    }
}

static void draw_line_xrgb8888(void *opaque, uint8_t *dst, const uint8_t *src,
                               int width, int dststep)
{
    uint32_t *d = (uint32_t *)dst;

    /* DRM_FORMAT_XRGB8888: value 0x__RRGGBB. */
    for (int i = 0; i < width; i++) {
        uint32_t p = ldl_le_p(src);
        *d++ = rgb_to_pixel32((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
        src += 4;
    }
}

static void draw_line_xbgr8888(void *opaque, uint8_t *dst, const uint8_t *src,
                               int width, int dststep)
{
    uint32_t *d = (uint32_t *)dst;

    /* DRM_FORMAT_XBGR8888: value 0x__BBGGRR. */
    for (int i = 0; i < width; i++) {
        uint32_t p = ldl_le_p(src);
        *d++ = rgb_to_pixel32(p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff);
        src += 4;
    }
}

static drawfn lcdif_pick_drawfn(IMX93LcdifState *s, int *src_bpp)
{
    uint32_t bpp = (lcdif_reg(s, LCDC_V8_CTRLDESCL0_5) & CTRLDESCL0_5_BPP_MASK)
                   >> CTRLDESCL0_5_BPP_SHIFT;

    switch (bpp) {
    case BPP_16_RGB565:
        *src_bpp = 2;
        return draw_line_rgb565;
    case BPP_24_RGB888:
        *src_bpp = 3;
        return draw_line_rgb888;
    case BPP_32_ARGB8888:
        *src_bpp = 4;
        return draw_line_xrgb8888;
    case BPP_32_ABGR8888:
        *src_bpp = 4;
        return draw_line_xbgr8888;
    default:
        return NULL;
    }
}

static bool lcdif_update_display(void *opaque)
{
    IMX93LcdifState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t descl1 = lcdif_reg(s, LCDC_V8_CTRLDESCL0_1);
    uint32_t width = descl1 & 0xffff;
    uint32_t height = (descl1 >> 16) & 0xffff;
    uint32_t pitch = lcdif_reg(s, LCDC_V8_CTRLDESCL0_3) & 0xffff;
    uint64_t fb_base = lcdif_reg(s, LCDC_V8_CTRLDESCL_LOW0_4) |
        ((uint64_t)(lcdif_reg(s, LCDC_V8_CTRLDESCL_HIGH0_4) & 0xf) << 32);
    int src_bpp = 0;
    drawfn fn;
    int first = 0, last = 0;

    if (getenv("LCDIF_DBG")) {
        fprintf(stderr, "[lcdif] UPDATE en=%d w=%u h=%u descl5=0x%x\n",
                lcdif_is_enabled(s), width, height,
                lcdif_reg(s, LCDC_V8_CTRLDESCL0_5));
    }

    if (!lcdif_is_enabled(s) || width == 0 || height == 0) {
        return true;
    }

    fn = lcdif_pick_drawfn(s, &src_bpp);
    if (!fn) {
        return true;
    }
    if (pitch == 0) {
        pitch = width * src_bpp;
    }

    if (surface_width(surface) != width || surface_height(surface) != height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
        s->invalidate = true;
    }

    if (s->invalidate || s->fb_base != fb_base || s->src_width != pitch ||
        s->rows != height) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(),
                                          fb_base, height, pitch);
        s->fb_base = fb_base;
        s->src_width = pitch;
        s->rows = height;
        s->cols = width;
    }

    framebuffer_update_display(surface, &s->fbsection, width, height,
                               pitch, surface_stride(surface), 0,
                               s->invalidate, fn, s, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, width, last - first + 1);
    }
    s->invalidate = false;
    return true;
}

static void lcdif_invalidate_display(void *opaque)
{
    IMX93LcdifState *s = opaque;

    s->invalidate = true;
}

static const GraphicHwOps lcdif_gfx_ops = {
    .invalidate = lcdif_invalidate_display,
    .gfx_update = lcdif_update_display,
};

static uint64_t lcdif_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93LcdifState *s = opaque;

    /* SET/CLR/TOG aliases of CTRL all read back the CTRL value. */
    if (offset == REG_SET || offset == REG_CLR || offset == REG_TOG) {
        return s->regs[LCDC_V8_CTRL >> 2];
    }
    if ((offset >> 2) >= IMX93_LCDIF_NUM_REGS) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void lcdif_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IMX93LcdifState *s = opaque;
    uint32_t val = value;
    hwaddr idx;

    if (getenv("LCDIF_DBG") &&
        (offset == LCDC_V8_CTRLDESCL0_5 || offset == LCDC_V8_CTRLDESCL0_1 ||
         offset == LCDC_V8_CTRL || offset == REG_SET || offset == REG_CLR)) {
        fprintf(stderr, "[lcdif] WR +0x%03x = 0x%08x\n",
                (unsigned)offset, (uint32_t)value);
    }

    /* CTRL has SET/CLR/TOG aliases at +0x4/+0x8/+0xc. */
    if (offset == REG_SET || offset == REG_CLR || offset == REG_TOG) {
        uint32_t cur = s->regs[LCDC_V8_CTRL >> 2];
        if (offset == REG_SET) {
            cur |= val;
        } else if (offset == REG_CLR) {
            cur &= ~val;
        } else {
            cur ^= val;
        }
        s->regs[LCDC_V8_CTRL >> 2] = cur & ~CTRL_SW_RESET;
        return;
    }

    if ((offset >> 2) >= IMX93_LCDIF_NUM_REGS) {
        return;
    }
    idx = offset >> 2;

    switch (offset) {
    case LCDC_V8_CTRL:
        /* SW reset self-clears immediately in this model. */
        s->regs[idx] = val & ~CTRL_SW_RESET;
        return;
    case LCDC_V8_INT_STATUS_D0:
    case LCDC_V8_INT_STATUS_D1:
        /* Write-1-to-clear. */
        s->regs[idx] &= ~val;
        lcdif_update_irq(s);
        return;
    case LCDC_V8_INT_ENABLE_D0:
    case LCDC_V8_INT_ENABLE_D1:
        s->regs[idx] = val;
        lcdif_update_irq(s);
        return;
    case LCDC_V8_DISP_PARA:
        s->regs[idx] = val;
        lcdif_refresh_enable(s);
        return;
    case LCDC_V8_CTRLDESCL0_5:
        /* SHADOW_LOAD_EN is a self-clearing trigger. */
        s->regs[idx] = val & ~CTRLDESCL0_5_SHADOW_LOAD_EN;
        lcdif_refresh_enable(s);
        if (s->con) {
            qemu_console_hw_invalidate(s->con);
        }
        return;
    case LCDC_V8_CTRLDESCL0_1:
    case LCDC_V8_CTRLDESCL0_3:
    case LCDC_V8_CTRLDESCL_LOW0_4:
    case LCDC_V8_CTRLDESCL_HIGH0_4:
        s->regs[idx] = val;
        s->invalidate = true;
        if (s->con) {
            qemu_console_hw_invalidate(s->con);
        }
        return;
    default:
        s->regs[idx] = val;
        return;
    }
}

static const MemoryRegionOps lcdif_ops = {
    .read = lcdif_read,
    .write = lcdif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void lcdif_reset(DeviceState *dev)
{
    IMX93LcdifState *s = IMX93_LCDIF(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->fb_base = 0;
    s->src_width = 0;
    s->rows = 0;
    s->cols = 0;
    s->src_bpp = 0;
    s->enabled = false;
    s->invalidate = true;
    timer_del(s->vblank_timer);
    lcdif_update_irq(s);
}

static int lcdif_post_load(void *opaque, int version_id)
{
    IMX93LcdifState *s = opaque;

    s->fb_base = 0;
    s->src_width = 0;
    s->rows = 0;
    s->invalidate = true;
    s->enabled = false;
    lcdif_update_irq(s);
    lcdif_refresh_enable(s);
    return 0;
}

static const VMStateDescription vmstate_lcdif = {
    .name = TYPE_IMX93_LCDIF,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lcdif_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93LcdifState, IMX93_LCDIF_NUM_REGS),
        VMSTATE_TIMER_PTR(vblank_timer, IMX93LcdifState),
        VMSTATE_END_OF_LIST()
    },
};

static void lcdif_realize(DeviceState *dev, Error **errp)
{
    IMX93LcdifState *s = IMX93_LCDIF(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &lcdif_ops, s,
                          TYPE_IMX93_LCDIF, IMX93_LCDIF_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcdif_vblank_tick, s);
    s->invalidate = true;
    s->con = qemu_graphic_console_create(dev, 0, &lcdif_gfx_ops, s);
}

static void lcdif_unrealize(DeviceState *dev)
{
    IMX93LcdifState *s = IMX93_LCDIF(dev);

    timer_del(s->vblank_timer);
    timer_free(s->vblank_timer);
    s->vblank_timer = NULL;

    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
    }
}

static void lcdif_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lcdif_realize;
    dc->unrealize = lcdif_unrealize;
    dc->vmsd = &vmstate_lcdif;
    device_class_set_legacy_reset(dc, lcdif_reset);
    dc->desc = "i.MX93 LCDIFv3 display controller";
}

static const TypeInfo lcdif_types[] = {
    {
        .name = TYPE_IMX93_LCDIF,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93LcdifState),
        .class_init = lcdif_class_init,
    },
};

DEFINE_TYPES(lcdif_types)
