/*
 * NXP i.MX 93 SAR-ADC
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_IMX93_ADC_H
#define HW_ADC_IMX93_ADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_ADC "imx93.adc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93AdcState, IMX93_ADC)

#define IMX93_ADC_SIZE 0x10000
#define IMX93_ADC_NCH  8

struct IMX93AdcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t mcr;       /* main configuration */
    uint32_t isr;       /* interrupt status (W1C) */
    uint32_t imr;       /* interrupt mask */
    uint32_t ncmr0;     /* normal conversion channel mask */
    uint32_t pcdr[IMX93_ADC_NCH];   /* per-channel data */
};

#endif /* HW_ADC_IMX93_ADC_H */
