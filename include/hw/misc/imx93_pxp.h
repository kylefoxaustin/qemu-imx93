/*
 * NXP i.MX 93 PXP (Pixel Pipeline) 2D engine
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 93 PXP combines a Fetch-Engine / Store-Engine datapath (per-channel
 * INPUT_FETCH_* source and INPUT_STORE_* dest registers) with the legacy
 * MXS-style HW_PXP_CTRL at offset 0 for soft reset. The pxp_dma_v3 driver's
 * pxp_soft_reset() spins in an UNBOUNDED loop waiting for HW_PXP_CTRL's CLKGATE
 * to assert after software reset, so that behaviour (asserting SFTRST also sets
 * CLKGATE) is modelled. The registers are a read-what-you-write backing store;
 * set the PXP_DBG environment variable to trace every access. The G2D op
 * datapaths and the completion IRQ (WAKEUPMIX PXP interrupt 0, GIC SPI 173) are
 * implemented in imx93_pxp.c.
 */

#ifndef IMX93_PXP_H
#define IMX93_PXP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_PXP "imx93.pxp"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93PxpState, IMX93_PXP)

#define IMX93_PXP_REG_SIZE      (64 * KiB)
#define IMX93_PXP_NUM_REGS      (IMX93_PXP_REG_SIZE / 4)

struct IMX93PxpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* WAKEUPMIX PXP interrupt 0 (completion) */
    uint32_t op_mode;           /* last-armed op: 0 copy, 1 fill, 2 blit */
    bool blend_pending;         /* CH1 armed -> next kick is a src-over blend */
    uint32_t ctrl;
    uint32_t stat;
    uint32_t regs[IMX93_PXP_NUM_REGS];
};

#endif /* IMX93_PXP_H */
