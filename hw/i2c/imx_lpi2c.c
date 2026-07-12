/*
 * NXP i.MX Low Power I2C (LPI2C) controller - master mode
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout and the MTDR command encoding follow the Linux
 * i2c-imx-lpi2c driver. The MTDR (0x60) is a combined command+data word
 * (cmd << 8 | data): GEN_START (4) issues a START + 8-bit address, TRAN_DATA
 * (0) sends a byte, RECV_DATA (1) requests (data+1) bytes, GEN_STOP (2) ends
 * the transfer. Received bytes are read from MRDR (0x70), MRDR_RXEMPTY (14)
 * flagging an empty FIFO. We translate these onto a QEMU I2CBus.
 */

#include "qemu/osdep.h"
#include "hw/i2c/imx_lpi2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

#define LPI2C_PARAM     0x04
#define LPI2C_MCR       0x10
#define LPI2C_MSR       0x14
#define LPI2C_MIER      0x18
#define LPI2C_MCFGR0    0x20
#define LPI2C_MCFGR1    0x24
#define LPI2C_MCFGR2    0x28
#define LPI2C_MCFGR3    0x2C
#define LPI2C_MCCR0     0x48
#define LPI2C_MCCR1     0x50
#define LPI2C_MFCR      0x58
#define LPI2C_MFSR      0x5C
#define LPI2C_MTDR      0x60
#define LPI2C_MRDR      0x70

#define MCR_MEN         BIT(0)
#define MCR_RST         BIT(1)
#define MCR_RTF         BIT(8)   /* reset tx FIFO */
#define MCR_RRF         BIT(9)   /* reset rx FIFO */

#define MSR_TDF         BIT(0)   /* tx data flag (FIFO has room) */
#define MSR_RDF         BIT(1)   /* rx data flag (FIFO has data) */
#define MSR_SDF         BIT(9)   /* STOP detect */
#define MSR_NDF         BIT(10)  /* NACK detect */
#define MSR_MBF         BIT(24)  /* master busy */
#define MSR_BBF         BIT(25)  /* bus busy */
#define MSR_W1C         (MSR_SDF | MSR_NDF | BIT(11) | BIT(13) | BIT(14))

#define MRDR_RXEMPTY    BIT(14)

/* MTDR command field (bits 10:8). */
#define CMD_TRAN_DATA   0x0
#define CMD_RECV_DATA   0x1
#define CMD_GEN_STOP    0x2
#define CMD_RECV_DISCARD 0x3
#define CMD_GEN_START   0x4

static void imx_lpi2c_update_irq(IMXLPI2CState *s)
{
    uint32_t status = s->msr;

    if (s->rx_count) {
        status |= MSR_RDF;
    }
    status |= MSR_TDF;                  /* tx FIFO never full in this model */
    qemu_set_irq(s->irq, !!(status & s->mier));
}

static uint32_t imx_lpi2c_status(IMXLPI2CState *s)
{
    uint32_t status = s->msr;

    status |= MSR_TDF;
    if (s->rx_count) {
        status |= MSR_RDF;
    }
    if (s->transfer_active) {
        status |= MSR_MBF | MSR_BBF;
    }
    return status;
}

static void imx_lpi2c_push_rx(IMXLPI2CState *s, uint8_t byte)
{
    if (s->rx_count < IMX_LPI2C_RXFIFO_SIZE) {
        unsigned tail = (s->rx_head + s->rx_count) % IMX_LPI2C_RXFIFO_SIZE;
        s->rxfifo[tail] = byte;
        s->rx_count++;
    }
}

static void imx_lpi2c_mtdr(IMXLPI2CState *s, uint32_t value)
{
    uint8_t cmd = (value >> 8) & 0x7;
    uint8_t data = value & 0xff;
    unsigned i, n;

    switch (cmd) {
    case CMD_GEN_START:
        if (s->transfer_active) {
            i2c_end_transfer(s->bus);
        }
        /* data = 8-bit address (addr << 1 | rw). */
        if (i2c_start_transfer(s->bus, data >> 1, data & 1)) {
            s->msr |= MSR_NDF;        /* no device ACKed */
            s->transfer_active = false;
        } else {
            s->transfer_active = true;
        }
        break;

    case CMD_TRAN_DATA:
        if (s->transfer_active && i2c_send(s->bus, data)) {
            s->msr |= MSR_NDF;
        }
        break;

    case CMD_RECV_DATA:
    case CMD_RECV_DISCARD:
        n = data + 1;
        for (i = 0; i < n; i++) {
            uint8_t b = s->transfer_active ? i2c_recv(s->bus) : 0xff;
            if (cmd == CMD_RECV_DATA) {
                imx_lpi2c_push_rx(s, b);
            }
        }
        break;

    case CMD_GEN_STOP:
        if (s->transfer_active) {
            i2c_end_transfer(s->bus);
            s->transfer_active = false;
        }
        s->msr |= MSR_SDF;
        break;

    default:
        break;
    }
    imx_lpi2c_update_irq(s);
}

static uint64_t imx_lpi2c_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXLPI2CState *s = opaque;

    switch (offset) {
    case LPI2C_PARAM:
        return 0x0404;                 /* tx/rx FIFO size = 2^4 each */
    case LPI2C_MCR:
        return s->mcr;
    case LPI2C_MSR:
        return imx_lpi2c_status(s);
    case LPI2C_MIER:
        return s->mier;
    case LPI2C_MCFGR1:
        return s->mcfgr1;
    case LPI2C_MFSR:
        /* rx-count in bits [23:16], tx-count 0; coarse but enough. */
        return (s->rx_count & 0xff) << 16;
    case LPI2C_MRDR:
        if (s->rx_count == 0) {
            return MRDR_RXEMPTY;
        } else {
            uint8_t b = s->rxfifo[s->rx_head];
            s->rx_head = (s->rx_head + 1) % IMX_LPI2C_RXFIFO_SIZE;
            s->rx_count--;
            imx_lpi2c_update_irq(s);
            return b;
        }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imx_lpi2c_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMXLPI2CState *s = opaque;

    switch (offset) {
    case LPI2C_MCR:
        s->mcr = value & ~(MCR_RTF | MCR_RRF);
        if (value & MCR_RST) {
            s->msr = 0;
            s->rx_head = s->rx_count = 0;
            s->transfer_active = false;
        }
        if (value & MCR_RRF) {
            s->rx_head = s->rx_count = 0;
        }
        imx_lpi2c_update_irq(s);
        break;
    case LPI2C_MSR:
        s->msr &= ~(value & MSR_W1C);    /* W1C status flags */
        imx_lpi2c_update_irq(s);
        break;
    case LPI2C_MIER:
        s->mier = value;
        imx_lpi2c_update_irq(s);
        break;
    case LPI2C_MCFGR1:
        s->mcfgr1 = value;
        break;
    case LPI2C_MTDR:
        imx_lpi2c_mtdr(s, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps imx_lpi2c_ops = {
    .read = imx_lpi2c_read,
    .write = imx_lpi2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * The eDMA drives the data registers with sub-word accesses (16-bit
     * command words into MTDR, single bytes out of MRDR), so allow 1/2/4-byte
     * access. PIO accesses from the CPU are always 32-bit.
     */
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void imx_lpi2c_reset_hold(Object *obj, ResetType type)
{
    IMXLPI2CState *s = IMX_LPI2C(obj);

    s->mcr = s->msr = s->mier = s->mcfgr1 = 0;
    s->transfer_active = false;
    s->rx_head = s->rx_count = 0;
}

static void imx_lpi2c_realize(DeviceState *dev, Error **errp)
{
    IMXLPI2CState *s = IMX_LPI2C(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx_lpi2c_ops, s,
                          TYPE_IMX_LPI2C, IMX_LPI2C_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->bus = i2c_init_bus(dev, s->bus_name ? s->bus_name : "i2c");
}

static const Property imx_lpi2c_properties[] = {
    DEFINE_PROP_STRING("bus-name", IMXLPI2CState, bus_name),
};

static const VMStateDescription vmstate_imx_lpi2c = {
    .name = TYPE_IMX_LPI2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMXLPI2CState),
        VMSTATE_UINT32(msr, IMXLPI2CState),
        VMSTATE_UINT32(mier, IMXLPI2CState),
        VMSTATE_UINT32(mcfgr1, IMXLPI2CState),
        VMSTATE_BOOL(transfer_active, IMXLPI2CState),
        VMSTATE_UINT8_ARRAY(rxfifo, IMXLPI2CState, IMX_LPI2C_RXFIFO_SIZE),
        VMSTATE_UINT32(rx_head, IMXLPI2CState),
        VMSTATE_UINT32(rx_count, IMXLPI2CState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx_lpi2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "i.MX Low Power I2C controller";
    dc->realize = imx_lpi2c_realize;
    rc->phases.hold = imx_lpi2c_reset_hold;
    dc->vmsd = &vmstate_imx_lpi2c;
    device_class_set_props(dc, imx_lpi2c_properties);
}

static const TypeInfo imx_lpi2c_types[] = {
    {
        .name           = TYPE_IMX_LPI2C,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMXLPI2CState),
        .class_init     = imx_lpi2c_class_init,
    },
};

DEFINE_TYPES(imx_lpi2c_types)
