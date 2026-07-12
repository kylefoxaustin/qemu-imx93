/*
 * NXP i.MX 93 / i.MX 8ULP GPIO controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the VF610-family GPIO used on the i.MX 93 (compatible
 * "fsl,imx93-gpio" / "fsl,imx8ulp-gpio"), single-base "imx8ulp" layout:
 * the data registers (PDOR/PSOR/PCOR/PTOR/PDIR/PDDR) sit at +0x40 and the
 * PORT pin-control + interrupt-status registers (PCR[n], ISFR) at +0x80.
 * Enough is modeled for the gpio-vf610 driver to register a gpiochip and
 * its irqchip; no external lines are driven, so no interrupt is raised.
 */

#ifndef IMX93_GPIO_H
#define IMX93_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_GPIO "imx93.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93GPIOState, IMX93_GPIO)

#define IMX93_GPIO_REG_SIZE     0x1000
#define IMX93_GPIO_PINS         32

struct IMX93GPIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq[2];        /* DT lists two GIC lines; driver uses [0] */

    uint32_t pdor;              /* output data latch */
    uint32_t pddr;              /* data direction */
    uint32_t pcr[IMX93_GPIO_PINS];  /* per-pin control (IRQC etc.) */
    uint32_t isfr;             /* interrupt status flags (W1C) */
};

#endif /* IMX93_GPIO_H */
