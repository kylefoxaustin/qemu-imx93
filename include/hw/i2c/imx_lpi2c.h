/*
 * NXP i.MX Low Power I2C (LPI2C) controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Master-mode model of the i.MX LPI2C controller (compatible
 * "fsl,imx7ulp-lpi2c" / "fsl,imx93-lpi2c"), bridging MMIO master transactions
 * to a QEMU I2CBus so real I2C slave models (PMIC, GPIO expanders) can be
 * attached. Target/slave mode is not modeled.
 */

#ifndef IMX_LPI2C_H
#define IMX_LPI2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX_LPI2C "imx.lpi2c"
OBJECT_DECLARE_SIMPLE_TYPE(IMXLPI2CState, IMX_LPI2C)

#define IMX_LPI2C_REG_SIZE      0x1000
#define IMX_LPI2C_RXFIFO_SIZE   256

struct IMXLPI2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus      *bus;
    qemu_irq     irq;
    char        *bus_name;  /* optional unique bus name for -device bus= */

    uint32_t mcr;       /* master control */
    uint32_t msr;       /* master status (latched flags) */
    uint32_t mier;      /* master interrupt enable */
    uint32_t mcfgr1;    /* config (AUTOSTOP/IGNACK) */

    bool     transfer_active;

    uint8_t  rxfifo[IMX_LPI2C_RXFIFO_SIZE];
    uint32_t rx_head, rx_count;
};

#endif /* IMX_LPI2C_H */
