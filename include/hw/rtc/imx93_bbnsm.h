/*
 * NXP i.MX 93 Battery-Backed Non-Secure Module (BBNSM) - RTC + power key
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the BBNSM real-time clock (a 47-bit, 32768 Hz counter that the Linux
 * rtc-nxp-bbnsm driver reads/sets and arms a seconds-granularity alarm against)
 * plus the power-key event register. The RTC tracks host wall-clock time.
 */

#ifndef HW_RTC_IMX93_BBNSM_H
#define HW_RTC_IMX93_BBNSM_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMX93_BBNSM "imx93.bbnsm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93BbnsmState, IMX93_BBNSM)

#define IMX93_BBNSM_SIZE 0x10000

struct IMX93BbnsmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer alarm;

    uint32_t ctrl;
    uint32_t int_en;
    uint32_t events;
    uint32_t pad_ctrl;
    uint32_t ta;            /* alarm time, in seconds */
    uint32_t set_ls;        /* latched RTC_LS pending an RTC enable */
    uint32_t set_ms;        /* latched RTC_MS pending an RTC enable */
    bool set_pending;       /* RTC_LS/MS written since the last RTC enable */
    int64_t offset_ticks;   /* counter = host_ticks + offset (32768 Hz) */
};

#endif /* HW_RTC_IMX93_BBNSM_H */
