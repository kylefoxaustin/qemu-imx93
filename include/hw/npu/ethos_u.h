/*
 * Arm Ethos-U55/U65 microNPU - functional command-stream executor
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A board-agnostic model of the Arm Ethos-U microNPU. The NPU is programmed by
 * driver/firmware that writes a Vela-compiled register command stream into
 * guest memory and kicks the engine; this device fetches and executes that
 * command stream (int8 conv/depthwise/pool/elementwise) against guest memory
 * via its DMA AddressSpace, then raises the completion IRQ. The "variant" and
 * "macs" properties select U55 vs U65 and the MAC configuration so the
 * ID/CONFIG registers report values the driver/firmware can verify.
 */

#ifndef HW_NPU_ETHOS_U_H
#define HW_NPU_ETHOS_U_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ETHOS_U "arm.ethos-u"
OBJECT_DECLARE_SIMPLE_TYPE(EthosUState, ETHOS_U)

typedef enum {
    ETHOS_U55 = 0,
    ETHOS_U65 = 1,
} EthosUVariant;

#define ETHOS_U_MMIO_SIZE  0x10000
#define ETHOS_U_NUM_REGS   (0x100 / 4)   /* APB control register window */
#define ETHOS_U_NUM_BASEP  8             /* eight 64-bit region base pointers */

struct EthosUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* Board-supplied DMA target (defaults to system memory if unset). */
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    /* Properties. */
    char *variant_str;          /* "u55" / "u65" */
    EthosUVariant variant;
    uint8_t macs_per_cc_log2;   /* CONFIG[3:0]; 8 => 2^8 = 256 MACs/cc */
    bool host_infer_fallback;   /* opt-in: defer to a host helper (debug) */

    /* APB register file (ID/STATUS/CMD/RESET/QBASE/QSIZE/QREAD/BASEPx/...). */
    uint32_t regs[ETHOS_U_NUM_REGS];

    /* Activation lookup table (exp/reciprocal for softmax etc.), 256 entries,
     * loaded by a DMA into the on-chip LUT slot before a TABLE activation. */
    uint32_t lut[256];
    bool lut_loaded;

    bool busy;                  /* a command stream is executing */
};

#endif /* HW_NPU_ETHOS_U_H */
