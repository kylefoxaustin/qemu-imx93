/*
 * OmniVision OV5640 camera sensor (I2C, register-file model)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The OV5640 is the MIPI CSI-2 camera sensor on the i.MX93 ov5640 device-tree
 * variant: sensor -> MIPI CSI-2 host (dw-mipi-csi2) -> ISI -> /dev/video. This
 * is a byte-addressable register-file model - enough for the Linux ov5640
 * driver to probe and register its V4L2 subdev so the media graph links and
 * the ISI (which synthesises the frames) can stream. No pixels are generated
 * here.
 *
 * Wire format: a 16-bit big-endian register address followed by 8-bit data
 * bytes (OV5640 registers are all 8-bit, the address is 16-bit). The chip-ID
 * the driver checks lives in two registers:
 *   - CHIP_ID_HIGH (0x300A) == 0x56
 *   - CHIP_ID_LOW  (0x300B) == 0x40   -> device id 0x5640
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_OV5640 "ov5640"
OBJECT_DECLARE_SIMPLE_TYPE(Ov5640State, OV5640)

#define OV5640_CHIP_ID_HIGH     0x300A
#define OV5640_CHIP_ID_LOW      0x300B
#define OV5640_CHIP_ID_HI_VAL   0x56
#define OV5640_CHIP_ID_LO_VAL   0x40
#define OV5640_NUM_REGS         0x10000

struct Ov5640State {
    I2CSlave parent_obj;

    uint8_t  regs[OV5640_NUM_REGS];
    uint16_t ptr;           /* current register address */
    int      wphase;        /* bytes seen since I2C_START_SEND */
    uint8_t  addr_hi;       /* first address byte (pending) */
};

static int ov5640_event(I2CSlave *i2c, enum i2c_event event)
{
    Ov5640State *s = OV5640(i2c);

    if (event == I2C_START_SEND) {
        s->wphase = 0;      /* the two address bytes come first */
    }
    return 0;
}

static int ov5640_send(I2CSlave *i2c, uint8_t data)
{
    Ov5640State *s = OV5640(i2c);

    if (s->wphase == 0) {
        s->addr_hi = data;
    } else if (s->wphase == 1) {
        s->ptr = (s->addr_hi << 8) | data;
    } else {
        s->regs[s->ptr++] = data;       /* 8-bit data, auto-increment */
    }
    s->wphase++;
    return 0;
}

static uint8_t ov5640_recv(I2CSlave *i2c)
{
    Ov5640State *s = OV5640(i2c);
    uint8_t val;

    switch (s->ptr) {
    case OV5640_CHIP_ID_HIGH:
        val = OV5640_CHIP_ID_HI_VAL;
        break;
    case OV5640_CHIP_ID_LOW:
        val = OV5640_CHIP_ID_LO_VAL;
        break;
    default:
        val = s->regs[s->ptr];
        break;
    }
    s->ptr++;
    return val;
}

static void ov5640_reset(DeviceState *dev)
{
    Ov5640State *s = OV5640(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->wphase = 0;
}

static const VMStateDescription vmstate_ov5640 = {
    .name = TYPE_OV5640,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Ov5640State),
        VMSTATE_UINT8_ARRAY(regs, Ov5640State, OV5640_NUM_REGS),
        VMSTATE_UINT16(ptr, Ov5640State),
        VMSTATE_INT32(wphase, Ov5640State),
        VMSTATE_UINT8(addr_hi, Ov5640State),
        VMSTATE_END_OF_LIST()
    },
};

static void ov5640_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "OmniVision OV5640 camera sensor";
    dc->vmsd = &vmstate_ov5640;
    device_class_set_legacy_reset(dc, ov5640_reset);
    sc->event = ov5640_event;
    sc->recv = ov5640_recv;
    sc->send = ov5640_send;
}

static const TypeInfo ov5640_types[] = {
    {
        .name          = TYPE_OV5640,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Ov5640State),
        .class_init    = ov5640_class_init,
    },
};

DEFINE_TYPES(ov5640_types)
