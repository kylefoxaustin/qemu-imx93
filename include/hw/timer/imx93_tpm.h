/*
 * NXP i.MX 93 Timer/PWM Module (TPM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_IMX93_TPM_H
#define HW_TIMER_IMX93_TPM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_TPM "imx93.tpm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93TpmState, IMX93_TPM)

#define IMX93_TPM_SIZE      0x10000
#define IMX93_TPM_CHANNELS  6

struct IMX93TpmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    int64_t base_ns;        /* virtual time at which the counter last started */
    uint32_t sc;            /* status/control (clock mode + prescaler) */
    uint32_t mod;           /* modulo (period) */
    uint32_t cnsc[IMX93_TPM_CHANNELS];
    uint32_t cnv[IMX93_TPM_CHANNELS];
};

#endif /* HW_TIMER_IMX93_TPM_H */
