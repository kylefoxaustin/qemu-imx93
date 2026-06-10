/*
 * NXP i.MX 93 FlexIO - configurable I/O (used as an I2C master)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IMX93_FLEXIO_H
#define IMX93_FLEXIO_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "qemu/units.h"
#include "qemu/timer.h"

#define TYPE_IMX93_FLEXIO "imx93.flexio"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93FlexioState, IMX93_FLEXIO)

#define IMX93_FLEXIO_SIZE       (64 * KiB)
/* Registers run from VERID (0x00) up to TIMCMP (0x500+); cover a bit past. */
#define IMX93_FLEXIO_NUM_REGS   (0x600 / 4)

struct IMX93FlexioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX93_FLEXIO_NUM_REGS];

    /* FlexIO-as-I2C-master datapath (i2c-flexio driver). */
    I2CBus *i2c_bus;
    QEMUTimer *shift_timer;
    bool     i2c_started;       /* a transfer is open on the I2C bus */
    bool     i2c_dead;          /* address NAK'd: no slave, swallow the rest */
    bool     i2c_read;          /* current transfer direction */
    uint8_t  i2c_tx_byte;       /* byte loaded into the transmit shifter */
    bool     i2c_tx_pending;    /* a shift of that byte is scheduled */
    uint8_t  i2c_rx_byte;       /* byte presented to SHIFTBUFBIS_1 */
    bool     i2c_rx_full;       /* rx byte awaiting drain: gates the next shift */
};

#endif /* IMX93_FLEXIO_H */
