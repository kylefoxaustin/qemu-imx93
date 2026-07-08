/*
 * NXP i.MX 93 Watchdog (imx7ulp-wdt compatible)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_WATCHDOG_IMX93_WDOG_H
#define HW_WATCHDOG_IMX93_WDOG_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMX93_WDOG "imx93.wdog"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93WdogState, IMX93_WDOG)

#define IMX93_WDOG_SIZE 0x10000

struct IMX93WdogState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer timer;

    uint32_t cs;            /* control/status (EN/INT_EN/PRES/...) */
    uint32_t toval;         /* timeout value, in clock ticks */
    uint32_t win;           /* window value */
    bool unlocked;          /* UNLOCK sequence accepted, config writable */
    bool rcs;               /* reconfiguration succeeded */
};

#endif /* HW_WATCHDOG_IMX93_WDOG_H */
