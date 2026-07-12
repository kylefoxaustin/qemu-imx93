/*
 * NXP i.MX 93 LCDIFv3 (LCDC "V8") display controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the LCDIFv3 controller (compatible "fsl,imx93-lcdif", same V8 register
 * set as "fsl,imx8mp-lcdif") driven by the Linux drm/mxsfb "lcdif" driver.
 * One primary plane is scanned out of guest DRAM into a QEMU graphic console;
 * a periodic VS_BLANK interrupt drives DRM vblank/page-flip bookkeeping.
 */

#ifndef IMX93_LCDIF_H
#define IMX93_LCDIF_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "qom/object.h"

#define TYPE_IMX93_LCDIF "imx93.lcdif"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93LcdifState, IMX93_LCDIF)

#define IMX93_LCDIF_REG_SIZE    0x10000
/* Backing register file: covers all V8 registers (highest is 0x238). */
#define IMX93_LCDIF_NUM_REGS    0x100

struct IMX93LcdifState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq;
    QemuConsole *con;
    QEMUTimer   *vblank_timer;

    uint32_t regs[IMX93_LCDIF_NUM_REGS];

    /* Renderer cache */
    MemoryRegionSection fbsection;
    uint32_t fb_base;
    uint32_t src_width;     /* bytes per source line */
    uint32_t rows;
    uint32_t cols;
    int      src_bpp;
    bool     invalidate;
    bool     enabled;       /* mirror of DISP_ON && DESCL EN */
};

#endif /* IMX93_LCDIF_H */
