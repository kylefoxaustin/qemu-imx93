/*
 * NXP i.MX 93 Thermal Monitoring Unit (TMU) - qoriq-tmu compatible
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX93_TMU_H
#define HW_MISC_IMX93_TMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_TMU "imx93.tmu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93TmuState, IMX93_TMU)

#define IMX93_TMU_SIZE 0x10000

struct IMX93TmuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t tmr;               /* mode register (TMR_ME enables monitoring) */
    uint32_t tmsr;              /* monitor site register */
    uint32_t tier;             /* interrupt enable */
    int32_t temperature;        /* reported temperature, millicelsius */
};

#endif /* HW_MISC_IMX93_TMU_H */
