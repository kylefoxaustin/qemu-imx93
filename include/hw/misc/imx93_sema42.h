/*
 * NXP i.MX 93 Hardware Semaphores (SEMA42)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX93_SEMA42_H
#define HW_MISC_IMX93_SEMA42_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_SEMA42 "imx93.sema42"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93Sema42State, IMX93_SEMA42)

#define IMX93_SEMA42_SIZE  0x10000
#define IMX93_SEMA42_GATES 16

struct IMX93Sema42State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t gate[IMX93_SEMA42_GATES];   /* 0 = unlocked, else owner domain+1 */
};

#endif /* HW_MISC_IMX93_SEMA42_H */
