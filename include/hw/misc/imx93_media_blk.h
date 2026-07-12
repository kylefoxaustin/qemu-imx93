/*
 * NXP i.MX 93 MEDIAMIX support blocks
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two small helpers needed to bring the display pipeline out of reset:
 *
 *  - IMX93_MEDIA_BLK_CTRL: the MEDIAMIX block control / GPR syscon
 *    ("fsl,imx93-media-blk-ctrl") at 0x4ac10000. Modelled as a plain
 *    read/write register file: the blk-ctrl genpd driver and the LCDIF/LDB/DSI
 *    "fsl,gpr" output mux just need their bits to stick.
 *
 *  - IMX93_SRC_SLICE: an SRC MIX power-domain slice ("fsl,imx93-src-slice"),
 *    e.g. the mediamix domain at 0x44462400. Its FUNC_STAT register is the only
 *    thing the imx93-pd driver polls, so we report "powered, not isolated".
 */

#ifndef IMX93_MEDIA_BLK_H
#define IMX93_MEDIA_BLK_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_MEDIA_BLK_CTRL "imx93.media-blk-ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93MediaBlkCtrlState, IMX93_MEDIA_BLK_CTRL)

#define IMX93_MEDIA_BLK_CTRL_SIZE   0x1000
#define IMX93_MEDIA_BLK_CTRL_REGS   (IMX93_MEDIA_BLK_CTRL_SIZE / 4)

struct IMX93MediaBlkCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX93_MEDIA_BLK_CTRL_REGS];
};

#define TYPE_IMX93_SRC_SLICE "imx93.src-slice"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93SrcSliceState, IMX93_SRC_SLICE)

#define IMX93_SRC_SLICE_SIZE        0x400
#define IMX93_SRC_SLICE_REGS        (IMX93_SRC_SLICE_SIZE / 4)

struct IMX93SrcSliceState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX93_SRC_SLICE_REGS];
};

#endif /* IMX93_MEDIA_BLK_H */
