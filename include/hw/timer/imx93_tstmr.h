/*
 * NXP i.MX 93 Timestamp Timer (TSTMR)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_IMX93_TSTMR_H
#define HW_TIMER_IMX93_TSTMR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_TSTMR "imx93.tstmr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93TstmrState, IMX93_TSTMR)

#define IMX93_TSTMR_SIZE 0x10000

struct IMX93TstmrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t latched_hi;    /* high word latched on the LOW read (atomic) */
};

#endif /* HW_TIMER_IMX93_TSTMR_H */
