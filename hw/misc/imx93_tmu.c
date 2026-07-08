/*
 * NXP i.MX 93 Thermal Monitoring Unit (TMU) - qoriq-tmu compatible
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX93 TMU is handled by the qoriq_thermal driver (TMU "Rev2"). Once the
 * driver enables monitoring (TMR.ME), it reads a per-site immediate-temperature
 * register TRITSR(n) whose bit 31 is "valid" and whose low 9 bits hold the
 * temperature in Kelvin. This model reports a fixed temperature (settable via
 * the "temperature" property, in millicelsius) so the thermal zone reads a
 * plausible value instead of failing. The optional threshold interrupt is not
 * modelled (the driver reads temperature by polling).
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_tmu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TMU_REG_TMR     0x000   /* mode register */
#define TMU_REG_TMSR    0x008   /* monitor site register (V2) */
#define TMU_REG_TIER    0x020   /* interrupt enable */
#define TMU_REG_TIDR    0x024   /* interrupt detect */
#define TMU_REG_TRITSR0 0x100   /* immediate temperature, site 0 (+16*n) */

#define TMR_ME          0x80000000  /* monitor enable */
#define TRITSR_V        0x80000000  /* measurement valid */

static uint64_t tmu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93TmuState *s = opaque;

    switch (offset) {
    case TMU_REG_TMR:
        return s->tmr;
    case TMU_REG_TMSR:
        return s->tmsr;
    case TMU_REG_TIER:
        return s->tier;
    case TMU_REG_TIDR:
        return 0;       /* no rising/falling-edge events (ERR052243 check) */
    default:
        /* TRITSR(n): valid + temperature in Kelvin, once monitoring is on. */
        if (offset >= TMU_REG_TRITSR0 && offset < TMU_REG_TRITSR0 + 16 * 16) {
            uint32_t kelvin;

            if (!(s->tmr & TMR_ME)) {
                return 0;
            }
            kelvin = 273 + s->temperature / 1000;
            return TRITSR_V | (kelvin & 0x1ff);
        }
        return 0;
    }
}

static void tmu_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    IMX93TmuState *s = opaque;

    switch (offset) {
    case TMU_REG_TMR:
        s->tmr = value;
        break;
    case TMU_REG_TMSR:
        s->tmsr = value;
        break;
    case TMU_REG_TIER:
        s->tier = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps tmu_ops = {
    .read = tmu_read,
    .write = tmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void tmu_reset_hold(Object *obj, ResetType type)
{
    IMX93TmuState *s = IMX93_TMU(obj);

    s->tmr = 0;
    s->tmsr = 0;
    s->tier = 0;
}

static void tmu_realize(DeviceState *dev, Error **errp)
{
    IMX93TmuState *s = IMX93_TMU(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &tmu_ops, s,
                          TYPE_IMX93_TMU, IMX93_TMU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_tmu = {
    .name = TYPE_IMX93_TMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(tmr, IMX93TmuState),
        VMSTATE_UINT32(tmsr, IMX93TmuState),
        VMSTATE_UINT32(tier, IMX93TmuState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property tmu_properties[] = {
    DEFINE_PROP_INT32("temperature", IMX93TmuState, temperature, 40000),
};

static void tmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = tmu_realize;
    dc->vmsd = &vmstate_tmu;
    rc->phases.hold = tmu_reset_hold;
    device_class_set_props(dc, tmu_properties);
    dc->desc = "i.MX93 thermal monitoring unit";
}

static const TypeInfo tmu_types[] = {
    {
        .name = TYPE_IMX93_TMU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93TmuState),
        .class_init = tmu_class_init,
    },
};

DEFINE_TYPES(tmu_types)
