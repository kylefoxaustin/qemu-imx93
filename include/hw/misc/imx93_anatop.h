/*
 * NXP i.MX 93 ANATOP (Analog Top / PLL) module
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional register model of the i.MX 93 ANATOP block (compatible
 * "fsl,imx93-anatop"), which hosts the fractional-N "GPPLL" PLLs that the
 * Linux clk-imx93 / clk-fracn-gppll driver programs directly (there is no
 * System Manager on the i.MX 93). The model is read-what-you-write for the
 * PLL CTRL/NUMERATOR/DENOMINATOR/DIV registers and reports every PLL as
 * locked (PLL_STATUS.LOCK = 1) so the driver's lock-poll completes. No real
 * PLL frequencies are produced.
 */

#ifndef IMX93_ANATOP_H
#define IMX93_ANATOP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_ANATOP "imx93.anatop"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93AnatopState, IMX93_ANATOP)

/* MMIO window (imx93.dtsi: clock-controller@44480000, reg size 0x2000). */
#define IMX93_ANATOP_REG_SIZE   (8 * KiB)
#define IMX93_ANATOP_NUM_REGS   (IMX93_ANATOP_REG_SIZE / 4)

struct IMX93AnatopState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX93_ANATOP_NUM_REGS];
};

#endif /* IMX93_ANATOP_H */
