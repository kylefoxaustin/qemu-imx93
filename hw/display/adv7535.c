/*
 * Analog Devices ADV7535 DSI-to-HDMI bridge — I2C main register map
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models just the "main" I2C map (default address 0x3d) of the ADV7535 well
 * enough for the Linux adv7511 bridge driver (info type ADV7535) to bring the
 * HDMI output up: it reports a connected monitor (STATUS.HPD), advertises the
 * DDC EDID as already fetched (DDC_STATUS == 2) and otherwise behaves as a
 * plain auto-incrementing register file. The EDID bytes themselves are served
 * by a separate "i2c-ddc" slave at the EDID address (main + 4 = 0x3f); the CEC
 * map (0x3b) is a second register-file instance. With those three the DRM
 * pipeline gets a mode and enables the LCDIF, which scans out real pixels.
 */

#include "qemu/osdep.h"
#include "hw/display/adv7535.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* adv7511 register numbers (see drivers/gpu/drm/bridge/adv7511/adv7511.h). */
#define ADV7535_REG_CHIP_REVISION   0x00
#define ADV7535_REG_STATUS          0x42
#define ADV7535_REG_INT0            0x96
#define ADV7535_REG_INT1            0x97
#define ADV7535_REG_DDC_STATUS      0xc8

#define STATUS_HPD                  (1u << 6)
#define STATUS_MONITOR_SENSE        (1u << 5)

#define DDC_STATUS_DONE             0x02

static int adv7535_event(I2CSlave *i2c, enum i2c_event event)
{
    ADV7535State *s = ADV7535(i2c);

    if (event == I2C_START_SEND) {
        s->firstbyte = true;
    }
    return 0;
}

static uint8_t adv7535_rx(I2CSlave *i2c)
{
    ADV7535State *s = ADV7535(i2c);
    uint8_t reg = s->ptr++;

    if (!s->main) {
        return s->regs[reg];        /* CEC / packet maps: plain register file */
    }

    switch (reg) {
    case ADV7535_REG_CHIP_REVISION:
        return 0x14;                            /* arbitrary, only logged */
    case ADV7535_REG_STATUS:
        return STATUS_HPD | STATUS_MONITOR_SENSE;
    case ADV7535_REG_INT0:
    case ADV7535_REG_INT1:
        return 0;                               /* no pending interrupts */
    case ADV7535_REG_DDC_STATUS:
        return DDC_STATUS_DONE;                 /* EDID already available */
    default:
        return s->regs[reg];
    }
}

static int adv7535_tx(I2CSlave *i2c, uint8_t data)
{
    ADV7535State *s = ADV7535(i2c);

    if (s->firstbyte) {
        s->ptr = data;
        s->firstbyte = false;
        return 0;
    }
    s->regs[s->ptr++] = data;
    return 0;
}

static void adv7535_reset(DeviceState *dev)
{
    ADV7535State *s = ADV7535(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->firstbyte = false;
}

static const Property adv7535_properties[] = {
    DEFINE_PROP_BOOL("main", ADV7535State, main, true),
};

static const VMStateDescription vmstate_adv7535 = {
    .name = TYPE_ADV7535,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, ADV7535State),
        VMSTATE_UINT8_ARRAY(regs, ADV7535State, ADV7535_NUM_REGS),
        VMSTATE_UINT8(ptr, ADV7535State),
        VMSTATE_BOOL(firstbyte, ADV7535State),
        VMSTATE_END_OF_LIST()
    },
};

static void adv7535_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(klass);

    isc->event = adv7535_event;
    isc->recv = adv7535_rx;
    isc->send = adv7535_tx;
    dc->vmsd = &vmstate_adv7535;
    device_class_set_props(dc, adv7535_properties);
    device_class_set_legacy_reset(dc, adv7535_reset);
    dc->desc = "ADV7535 DSI-to-HDMI bridge (main map)";
}

static const TypeInfo adv7535_types[] = {
    {
        .name = TYPE_ADV7535,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(ADV7535State),
        .class_init = adv7535_class_init,
    },
};

DEFINE_TYPES(adv7535_types)
