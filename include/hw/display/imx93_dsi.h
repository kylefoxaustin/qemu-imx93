/*
 * NXP i.MX 93 MIPI DSI host (Synopsys DesignWare dw-mipi-dsi core)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the DSI host register block at 0x4ae10000 ("fsl,imx93-mipi-dsi",
 * a dw-mipi-dsi core). Enough for the Linux dw-mipi-dsi driver to bring the
 * D-PHY "up": the PHY lock / stop-state and command-FIFO status registers
 * report ready, everything else is read-what-you-write. The actual D-PHY PLL
 * programming lands in the MEDIAMIX GPR (media_blk_ctrl), modelled separately.
 */

#ifndef IMX93_DSI_H
#define IMX93_DSI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_DSI "imx93.dsi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93DsiState, IMX93_DSI)

#define IMX93_DSI_REG_SIZE      0x4000
#define IMX93_DSI_NUM_REGS      0x100   /* highest core reg is 0xf4 */

struct IMX93DsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[IMX93_DSI_NUM_REGS];
};

#endif /* IMX93_DSI_H */
