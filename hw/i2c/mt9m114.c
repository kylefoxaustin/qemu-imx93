/*
 * onsemi MT9M114 camera sensor (I2C, register-file model)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The MT9M114 is the parallel (DVP) camera sensor on the i.MX93 EVK's
 * mt9m114 device-tree variant: sensor -> parallel-CSI -> ISI -> /dev/video.
 * This is a byte-addressable register-file model - enough for the Linux
 * mt9m114 driver to probe and register its V4L2 subdev so the media graph
 * links. No pixels are generated (no capture backend).
 *
 * Wire format: a 16-bit big-endian register address followed by 1+ data
 * bytes (the driver mixes 8- and 16-bit register widths, so the file is
 * byte-addressable). Two registers must read back specific values:
 *   - CHIP_ID (0x0000) == 0x2481, the device-ID the driver checks;
 *   - COMMAND_REGISTER (0x0080): the SET_STATE handshake polls this until the
 *     action bits clear and the OK bit is set, so it always reads 0x8000.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_MT9M114 "mt9m114"
OBJECT_DECLARE_SIMPLE_TYPE(Mt9m114State, MT9M114)

#define MT9M114_CHIP_ID_REG     0x0000
#define MT9M114_CHIP_ID         0x2481
#define MT9M114_COMMAND_REG     0x0080
#define MT9M114_COMMAND_OK      0x8000  /* OK set, all action bits clear */
#define MT9M114_NUM_REGS        0x10000

struct Mt9m114State {
    I2CSlave parent_obj;

    uint8_t  regs[MT9M114_NUM_REGS];
    uint16_t ptr;           /* byte offset of the current register */
    int      wphase;        /* bytes seen since I2C_START_SEND */
    uint8_t  addr_hi;       /* first address byte (pending) */
};

static int mt9m114_event(I2CSlave *i2c, enum i2c_event event)
{
    Mt9m114State *s = MT9M114(i2c);

    if (event == I2C_START_SEND) {
        s->wphase = 0;      /* the two address bytes come first */
    }
    return 0;
}

static int mt9m114_send(I2CSlave *i2c, uint8_t data)
{
    Mt9m114State *s = MT9M114(i2c);

    if (s->wphase == 0) {
        s->addr_hi = data;
    } else if (s->wphase == 1) {
        s->ptr = (s->addr_hi << 8) | data;
    } else {
        s->regs[s->ptr++] = data;
    }
    s->wphase++;
    return 0;
}

static uint8_t mt9m114_recv(I2CSlave *i2c)
{
    Mt9m114State *s = MT9M114(i2c);
    uint16_t reg = s->ptr & ~1;     /* 16-bit register this byte belongs to */
    uint8_t val;

    if (reg == MT9M114_CHIP_ID_REG) {
        val = (s->ptr & 1) ? (MT9M114_CHIP_ID & 0xff) : (MT9M114_CHIP_ID >> 8);
    } else if (reg == MT9M114_COMMAND_REG) {
        val = (s->ptr & 1) ? (MT9M114_COMMAND_OK & 0xff)
                           : (MT9M114_COMMAND_OK >> 8);
    } else {
        val = s->regs[s->ptr];
    }
    s->ptr++;
    return val;
}

static void mt9m114_reset(DeviceState *dev)
{
    Mt9m114State *s = MT9M114(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->wphase = 0;
}

static const VMStateDescription vmstate_mt9m114 = {
    .name = TYPE_MT9M114,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Mt9m114State),
        VMSTATE_UINT8_ARRAY(regs, Mt9m114State, MT9M114_NUM_REGS),
        VMSTATE_UINT16(ptr, Mt9m114State),
        VMSTATE_INT32(wphase, Mt9m114State),
        VMSTATE_UINT8(addr_hi, Mt9m114State),
        VMSTATE_END_OF_LIST()
    },
};

static void mt9m114_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "onsemi MT9M114 camera sensor";
    dc->vmsd = &vmstate_mt9m114;
    device_class_set_legacy_reset(dc, mt9m114_reset);
    sc->event = mt9m114_event;
    sc->recv = mt9m114_recv;
    sc->send = mt9m114_send;
}

static const TypeInfo mt9m114_types[] = {
    {
        .name          = TYPE_MT9M114,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Mt9m114State),
        .class_init    = mt9m114_class_init,
    },
};

DEFINE_TYPES(mt9m114_types)
