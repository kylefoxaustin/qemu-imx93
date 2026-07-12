/*
 * NXP i.MX 93 System Counter (SYS_CTR)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_IMX93_SYSCTR_H
#define HW_TIMER_IMX93_SYSCTR_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMX93_SYSCTR "imx93.sysctr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93SysctrState, IMX93_SYSCTR)

#define IMX93_SYSCTR_SIZE 0x30000

struct IMX93SysctrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer cmp;

    uint32_t cmpcr;         /* compare control (enable / irq mask) */
    uint64_t cmpcv;         /* compare value (counter ticks) */
};

#endif /* HW_TIMER_IMX93_SYSCTR_H */
