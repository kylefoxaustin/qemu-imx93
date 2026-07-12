/*
 * NXP i.MX 93 FlexSPI controller (serial NOR flash)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the FlexSPI controller the spi-nxp-fspi driver uses: a LUT-driven IP
 * command path (read flash ID, read/program/erase) and an AHB-mapped flash
 * window for memory-mapped (XIP) reads. Commands are issued onto a QEMU SSI bus
 * to an attached SPI-NOR flash (hw/block/m25p80).
 */

#ifndef HW_SSI_IMX93_FLEXSPI_H
#define HW_SSI_IMX93_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_IMX93_FLEXSPI "imx93.flexspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93FlexSpiState, IMX93_FLEXSPI)

#define IMX93_FLEXSPI_REG_SIZE  0x10000
#define IMX93_FLEXSPI_AHB_SIZE  0x08000000   /* 128 MiB memory-mapped window */
#define IMX93_FLEXSPI_NUM_REGS  (0x400 / 4)
#define IMX93_FLEXSPI_NUM_CS    4

struct IMX93FlexSpiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;     /* control registers */
    MemoryRegion ahb;       /* AHB-mapped flash (XIP) window */
    qemu_irq irq;
    qemu_irq cs[IMX93_FLEXSPI_NUM_CS];
    SSIBus *bus;

    uint32_t regs[IMX93_FLEXSPI_NUM_REGS];
    bool lut_unlocked;
    Fifo8 rx;       /* received bytes (RFDR packs 4/word) */
    Fifo8 tx;       /* bytes to transmit (TFDR unpacks 4/word) */
};

#endif /* HW_SSI_IMX93_FLEXSPI_H */
