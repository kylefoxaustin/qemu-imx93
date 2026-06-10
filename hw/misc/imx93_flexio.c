/*
 * NXP i.MX 93 FlexIO - configurable I/O block (modelled as an I2C master)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The FlexIO is a programmable shifter/timer fabric. On the i.MX93 EVK the
 * imx93-11x11-evk-flexio-i2c device tree routes an extra I2C master through it
 * (nxp,imx-flexio MFD + i2c-flexio). This model carries the register file the
 * MFD probes plus a functional I2C-over-shifter datapath against a real QEMU
 * I2CBus, so peripherals attach on the FlexIO bus exactly as on an LPI2C:
 *
 *   -device tmp105,bus=flexio1-i2c,address=0x49
 *
 * The driver drives the transfer one byte at a time: it loads shifter 0
 * (transmit) from SHIFTBUFBBS_0 and reads shifter 1 (receive/ACK) from
 * SHIFTBUFBIS_1, waiting on the SHIFTSTAT transmit-empty / receive-full flags
 * and the shifter interrupt. The model represents one byte clock as a single
 * atomic "shift" event on a timer: it performs the I2C-bus operation (the first
 * byte addresses the slave, later bytes send or receive) and raises both flags
 * together. Crucially the shift fires one realistic I2C byte period after the
 * byte is loaded - long enough that the driver's interrupt handler has returned
 * first - so the receive flag is set before the next handler entry and the
 * driver does exactly one transmit + one receive per byte (it samples SHIFTSTAT
 * once on entry). A sub-microsecond delay would let the timer fire mid-handler
 * and desynchronise that lock-step.
 *
 * Set FLEXIO_DBG to trace I2C operations, FLEXIO_REGDBG for register access.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_flexio.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"

/* Register offsets (per the imx-flexio MFD / i2c-flexio drivers). */
#define FLEXIO_VERID            0x00
#define FLEXIO_PARAM            0x04
#define FLEXIO_CTRL             0x08
#define   FLEXIO_CTRL_SWRST     0x2     /* software reset (self-clearing) */
#define FLEXIO_PIN              0x0c    /* live pin levels */
#define FLEXIO_SHIFTSTAT        0x10
#define   SHIFTSTAT_TX          0x1     /* shifter 0: transmit buffer empty */
#define   SHIFTSTAT_RX          0x2     /* shifter 1: receive buffer full */
#define FLEXIO_SHIFTERR         0x14
#define   SHIFTERR_RX_NAK       0x2     /* shifter 1: receive (ACK) error */
#define FLEXIO_TIMSTAT          0x18
#define FLEXIO_SHIFTSIEN        0x20    /* shifter interrupt enable */
#define FLEXIO_PINSTAT          0x50
#define FLEXIO_SHIFTBUFBIS_1    0x284   /* shifter 1 receive buffer */
#define FLEXIO_SHIFTBUFBBS_0    0x380   /* shifter 0 transmit buffer */

/*
 * One I2C byte at 100 kHz is ~90 us. Using a realistic byte period (rather than
 * a token delay) is what keeps the shift event landing between the driver's
 * interrupt handlers instead of racing inside one.
 */
#define FLEXIO_SHIFT_NS         100000

#define FLEXIO_VERID_VALUE      0x02010000
#define FLEXIO_PARAM_VALUE      0x04200808

#define R(s, off)               ((s)->regs[(off) / 4])

static bool flexio_dbg;
static bool flexio_regdbg;

static void imx93_flexio_trace(const char *op, hwaddr offset, uint32_t value)
{
    if (flexio_regdbg) {
        fprintf(stderr, "[flexio] %s +0x%03x = 0x%08x\n", op,
                (unsigned)offset, value);
    }
}

static void imx93_flexio_update_irq(IMX93FlexioState *s)
{
    uint32_t pending = R(s, FLEXIO_SHIFTSTAT) & R(s, FLEXIO_SHIFTSIEN) & 0x3;
    qemu_set_irq(s->irq, pending ? 1 : 0);
}

static void imx93_flexio_i2c_end(IMX93FlexioState *s)
{
    /* A dead (address-NAK'd) transfer was already closed at the NAK. */
    if (s->i2c_started && !s->i2c_dead) {
        i2c_end_transfer(s->i2c_bus);
        if (flexio_dbg) {
            fprintf(stderr, "[flexio]   i2c STOP\n");
        }
    }
    s->i2c_started = false;
    s->i2c_dead = false;
}

/*
 * One byte clock has elapsed: clock the loaded transmit byte onto the I2C bus
 * and capture the receive byte, then raise both shifter flags together. The
 * first byte of a transfer is the addressing byte (i2c_start_transfer); later
 * bytes are sent, or a dummy clock receives. Doing the bus operation and the
 * flag update here - on the timer, between the driver's interrupt handlers -
 * is what keeps the transmit/receive handshake in lock-step.
 *
 * The shift is gated on the previous receive byte having been drained. The
 * driver's handler samples SHIFTSTAT once, then loads the next transmit byte
 * and reads SHIFTBUFBIS_1 for the current one; a busy host can let this timer
 * fire between those two accesses. Clocking the next byte then would overwrite
 * the still-unread receive byte and, because the receive flag is already set,
 * swallow its interrupt edge - the race that stalls the driver. So if the
 * receive byte has not been drained yet, defer: re-arm and retry once it is.
 * The shift never runs ahead of the driver, and (unlike doing it synchronously
 * from the register handler) it still lands between handler invocations, so the
 * level-triggered interrupt keeps its clean low gap and cannot storm.
 */
static void imx93_flexio_shift(void *opaque)
{
    IMX93FlexioState *s = opaque;
    uint8_t byte = s->i2c_tx_byte;
    bool ack;

    if (!s->i2c_tx_pending) {
        return;
    }
    if (s->i2c_rx_full) {
        timer_mod(s->shift_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FLEXIO_SHIFT_NS);
        return;
    }
    s->i2c_tx_pending = false;

    if (!s->i2c_started) {
        s->i2c_read = byte & 1;
        ack = i2c_start_transfer(s->i2c_bus, byte >> 1, s->i2c_read) == 0;
        s->i2c_started = true;
        s->i2c_rx_byte = 0;
        s->i2c_dead = !ack;
        if (s->i2c_dead) {
            i2c_end_transfer(s->i2c_bus);
        }
        if (flexio_dbg) {
            fprintf(stderr, "[flexio]   i2c START addr=0x%02x %s ack=%d\n",
                    byte >> 1, s->i2c_read ? "rd" : "wr", ack);
        }
    } else if (s->i2c_dead) {
        s->i2c_rx_byte = 0xff;
        ack = false;
    } else if (s->i2c_read) {
        s->i2c_rx_byte = i2c_recv(s->i2c_bus);
        ack = true;
        if (flexio_dbg) {
            fprintf(stderr, "[flexio]   i2c RECV=0x%02x\n", s->i2c_rx_byte);
        }
    } else {
        ack = i2c_send(s->i2c_bus, byte) == 0;
        s->i2c_rx_byte = 0;
        if (flexio_dbg) {
            fprintf(stderr, "[flexio]   i2c SEND=0x%02x ack=%d\n", byte, ack);
        }
    }

    s->i2c_rx_full = true;
    R(s, FLEXIO_SHIFTSTAT) |= SHIFTSTAT_TX | SHIFTSTAT_RX;
    if (ack) {
        R(s, FLEXIO_SHIFTERR) &= ~(uint32_t)SHIFTERR_RX_NAK;
    } else {
        R(s, FLEXIO_SHIFTERR) |= SHIFTERR_RX_NAK;
    }
    imx93_flexio_update_irq(s);
}

static void imx93_flexio_tx_write(IMX93FlexioState *s, uint32_t byte)
{
    /* Only meaningful while the driver has the shifter interrupt armed. */
    if (!(R(s, FLEXIO_SHIFTSIEN) & 0x3)) {
        return;
    }
    s->i2c_tx_byte = byte;
    s->i2c_tx_pending = true;
    R(s, FLEXIO_SHIFTSTAT) &= ~(uint32_t)SHIFTSTAT_TX;   /* buffer loaded */
    imx93_flexio_update_irq(s);
    timer_mod(s->shift_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FLEXIO_SHIFT_NS);
}

static uint64_t imx93_flexio_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93FlexioState *s = opaque;
    uint32_t val;

    switch (offset) {
    case FLEXIO_VERID:
        val = FLEXIO_VERID_VALUE;
        break;
    case FLEXIO_PARAM:
        val = FLEXIO_PARAM_VALUE;
        break;
    case FLEXIO_PIN:
    case FLEXIO_PINSTAT:
        /* Idle bus: all pins high (the driver checks SDA/SCL are released). */
        val = 0xffffffff;
        break;
    case FLEXIO_SHIFTBUFBIS_1:
        val = s->i2c_rx_byte;
        s->i2c_rx_full = false;     /* drained: a deferred shift may now run */
        R(s, FLEXIO_SHIFTSTAT) &= ~(uint32_t)SHIFTSTAT_RX;   /* read clears */
        imx93_flexio_update_irq(s);
        break;
    default:
        val = (offset / 4) < IMX93_FLEXIO_NUM_REGS ? s->regs[offset / 4] : 0;
        break;
    }

    imx93_flexio_trace("rd", offset, val);
    return val;
}

static void imx93_flexio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93FlexioState *s = opaque;

    imx93_flexio_trace("wr", offset, (uint32_t)value);

    switch (offset) {
    case FLEXIO_VERID:
    case FLEXIO_PARAM:
        return;
    case FLEXIO_CTRL:
        if (value & FLEXIO_CTRL_SWRST) {
            imx93_flexio_i2c_end(s);
            timer_del(s->shift_timer);
            memset(s->regs, 0, sizeof(s->regs));
            s->i2c_tx_pending = false;
            s->i2c_rx_full = false;
            /* Reset leaves the transmit shifter empty (the driver polls it). */
            R(s, FLEXIO_SHIFTSTAT) = SHIFTSTAT_TX;
            value &= ~(uint64_t)FLEXIO_CTRL_SWRST;
        }
        break;
    case FLEXIO_SHIFTSTAT:
    case FLEXIO_SHIFTERR:
    case FLEXIO_TIMSTAT:
        /* Write-1-to-clear status registers. */
        s->regs[offset / 4] &= ~(uint32_t)value;
        imx93_flexio_update_irq(s);
        return;
    case FLEXIO_SHIFTSIEN: {
        uint32_t old = R(s, FLEXIO_SHIFTSIEN);
        s->regs[offset / 4] = value;
        if (!(value & 0x3) && (old & 0x3)) {
            /* Driver disarmed interrupts: the transfer is complete. */
            imx93_flexio_i2c_end(s);
            s->i2c_tx_pending = false;
            s->i2c_rx_full = false;
            timer_del(s->shift_timer);
            R(s, FLEXIO_SHIFTSTAT) = SHIFTSTAT_TX;   /* shifter idle/empty */
            R(s, FLEXIO_TIMSTAT) |= 0x1;     /* transfer-done poll succeeds */
        } else if (value & 0x3) {
            /*
             * Arming for a new transaction. If a previous one was left open
             * (the driver's NAK path stops without disabling interrupts),
             * close it so the next address starts cleanly rather than nesting.
             */
            imx93_flexio_i2c_end(s);
            s->i2c_tx_pending = false;
            s->i2c_rx_full = false;
            R(s, FLEXIO_SHIFTSTAT) = SHIFTSTAT_TX;
        }
        imx93_flexio_update_irq(s);
        return;
    }
    case FLEXIO_SHIFTBUFBBS_0:
        imx93_flexio_tx_write(s, value);
        return;
    default:
        break;
    }

    if ((offset / 4) < IMX93_FLEXIO_NUM_REGS) {
        s->regs[offset / 4] = value;
    }
}

static const MemoryRegionOps imx93_flexio_ops = {
    .read = imx93_flexio_read,
    .write = imx93_flexio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_flexio_reset(DeviceState *dev)
{
    IMX93FlexioState *s = IMX93_FLEXIO(dev);

    imx93_flexio_i2c_end(s);
    timer_del(s->shift_timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->i2c_tx_pending = false;
    s->i2c_rx_full = false;
    /* Transmit shifter starts empty so the driver's idle poll succeeds. */
    R(s, FLEXIO_SHIFTSTAT) = SHIFTSTAT_TX;
    qemu_set_irq(s->irq, 0);
}

static void imx93_flexio_init(Object *obj)
{
    IMX93FlexioState *s = IMX93_FLEXIO(obj);

    flexio_dbg = getenv("FLEXIO_DBG") != NULL;
    flexio_regdbg = getenv("FLEXIO_REGDBG") != NULL;
    memory_region_init_io(&s->iomem, obj, &imx93_flexio_ops, s,
                          TYPE_IMX93_FLEXIO, IMX93_FLEXIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void imx93_flexio_realize(DeviceState *dev, Error **errp)
{
    IMX93FlexioState *s = IMX93_FLEXIO(dev);

    s->i2c_bus = i2c_init_bus(dev, "flexio1-i2c");
    s->shift_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx93_flexio_shift, s);
}

static const VMStateDescription vmstate_imx93_flexio = {
    .name = TYPE_IMX93_FLEXIO,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93FlexioState, IMX93_FLEXIO_NUM_REGS),
        VMSTATE_BOOL(i2c_started, IMX93FlexioState),
        VMSTATE_BOOL(i2c_dead, IMX93FlexioState),
        VMSTATE_BOOL(i2c_read, IMX93FlexioState),
        VMSTATE_UINT8(i2c_tx_byte, IMX93FlexioState),
        VMSTATE_BOOL(i2c_tx_pending, IMX93FlexioState),
        VMSTATE_UINT8(i2c_rx_byte, IMX93FlexioState),
        VMSTATE_BOOL(i2c_rx_full, IMX93FlexioState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_flexio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 FlexIO";
    dc->realize = imx93_flexio_realize;
    device_class_set_legacy_reset(dc, imx93_flexio_reset);
    dc->vmsd = &vmstate_imx93_flexio;
}

static const TypeInfo imx93_flexio_types[] = {
    {
        .name           = TYPE_IMX93_FLEXIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93FlexioState),
        .instance_init  = imx93_flexio_init,
        .class_init     = imx93_flexio_class_init,
    },
};

DEFINE_TYPES(imx93_flexio_types)
