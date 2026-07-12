/*
 * NXP i.MX 93 Clock Control Module (CCM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 93 has no System Manager, so Linux programs the CCM directly
 * (compatible "fsl,imx93-ccm"). This is a functional register model of the
 * i.MX 9 "clock root + LPCG" CCM, sufficient for the Linux clk-imx93 driver
 * to probe and bring up clocks: it implements read-what-you-write for the
 * root CONTROL and gate DIRECT registers, reports the per-root BUSY status
 * as always-idle so the driver's change-poll completes, and returns a
 * permissive AUTHEN (TrustZone non-secure + all domains whitelisted) so the
 * driver does not skip clocks. No real clock frequencies are produced.
 */

#ifndef IMX93_CCM_H
#define IMX93_CCM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_CCM "imx93.ccm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93CCMState, IMX93_CCM)

/* MMIO window (imx93.dtsi: clock-controller@44450000, reg size 0x10000). */
#define IMX93_CCM_REG_SIZE      (64 * KiB)
#define IMX93_CCM_NUM_REGS      (IMX93_CCM_REG_SIZE / 4)

struct IMX93CCMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX93_CCM_NUM_REGS];
};

#endif /* IMX93_CCM_H */
