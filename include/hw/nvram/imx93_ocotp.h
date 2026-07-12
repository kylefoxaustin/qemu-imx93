/*
 * NXP i.MX 93 On-Chip OTP controller (OCOTP) - fuse readback
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NVRAM_IMX93_OCOTP_H
#define HW_NVRAM_IMX93_OCOTP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_OCOTP "imx93.ocotp"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93OcotpState, IMX93_OCOTP)

#define IMX93_OCOTP_SIZE   0x10000
#define IMX93_OCOTP_FUSES  0x800     /* modelled fuse-shadow window */

struct IMX93OcotpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t fuses[IMX93_OCOTP_FUSES];
};

#endif /* HW_NVRAM_IMX93_OCOTP_H */
