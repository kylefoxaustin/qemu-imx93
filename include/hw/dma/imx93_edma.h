/*
 * NXP i.MX 93 eDMA v3 (fsl,imx93-edma3) controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the management block plus per-channel pages of the EDMA3 engine
 * well enough to execute the TCD-described transfers the Linux fsl-edma
 * driver programs. Channel N's register page is at base + (N+1) * 0x10000;
 * each page holds the channel control registers (CH_CSR/ES/INT/SBR/...) and
 * the 32-byte TCD at offset 0x20. A transfer runs when CH_CSR.ERQ is set.
 */

#ifndef IMX93_EDMA_H
#define IMX93_EDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_EDMA "imx93.edma3"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93EdmaState, IMX93_EDMA)

#define IMX93_EDMA_MAX_CHANNELS     64
#define IMX93_EDMA_CHAN_OFFSET      0x10000     /* first channel page */
#define IMX93_EDMA_CHAN_STRIDE      0x10000     /* default page size (edma3) */
#define IMX93_EDMA_CHAN_REGS_SZ     0x40        /* control regs + TCD */
#define IMX93_EDMA_MGMT_REGS        0x40        /* mgmt words 0x0..0xff */

typedef struct IMX93EdmaChan {
    uint8_t  regs[IMX93_EDMA_CHAN_REGS_SZ];     /* byte-addressable page head */
    bool     armed;             /* dev->mem deferred until data */
    bool     cyclic;            /* scatter/gather (ESG): paced by peripheral */
} IMX93EdmaChan;

struct IMX93EdmaState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t     num_channels;
    uint32_t     chan_stride;   /* page size: edma3 0x10000, edma4 0x8000 */

    uint32_t     mgmt[IMX93_EDMA_MGMT_REGS];
    IMX93EdmaChan chan[IMX93_EDMA_MAX_CHANNELS];
    qemu_irq     irq[IMX93_EDMA_MAX_CHANNELS];
};

#endif /* IMX93_EDMA_H */
