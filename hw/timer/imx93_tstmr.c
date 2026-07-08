/*
 * NXP i.MX 93 Timestamp Timer (TSTMR)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A free-running 56-bit timestamp counter clocked at 24 MHz. Software reads the
 * LOW register first (which atomically latches the HIGH bits) then the HIGH
 * register. The counter tracks the virtual clock.
 */

#include "qemu/osdep.h"
#include "hw/timer/imx93_tstmr.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"

#define TSTMR_L  0x0
#define TSTMR_H  0x4

#define TSTMR_HZ 24000000

static uint64_t tstmr_count(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), TSTMR_HZ,
                    NANOSECONDS_PER_SECOND) & ((1ULL << 56) - 1);
}

static uint64_t tstmr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93TstmrState *s = opaque;
    uint64_t cnt;

    switch (offset) {
    case TSTMR_L:
        cnt = tstmr_count();
        s->latched_hi = (cnt >> 32) & 0xffffff;     /* latch for the H read */
        return cnt & 0xffffffff;
    case TSTMR_H:
        return s->latched_hi;
    default:
        return 0;
    }
}

static void tstmr_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    /* Read-only counter. */
}

static const MemoryRegionOps tstmr_ops = {
    .read = tstmr_read,
    .write = tstmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void tstmr_realize(DeviceState *dev, Error **errp)
{
    IMX93TstmrState *s = IMX93_TSTMR(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &tstmr_ops, s,
                          TYPE_IMX93_TSTMR, IMX93_TSTMR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void tstmr_reset_hold(Object *obj, ResetType type)
{
    IMX93TstmrState *s = IMX93_TSTMR(obj);

    /*
     * The count is free-running (derived from the virtual clock); only the
     * software-visible HIGH latch is stored state.
     */
    s->latched_hi = 0;
}

static const VMStateDescription vmstate_tstmr = {
    .name = TYPE_IMX93_TSTMR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(latched_hi, IMX93TstmrState),
        VMSTATE_END_OF_LIST()
    },
};

static void tstmr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = tstmr_realize;
    rc->phases.hold = tstmr_reset_hold;
    dc->vmsd = &vmstate_tstmr;
    dc->desc = "i.MX93 timestamp timer";
}

static const TypeInfo tstmr_types[] = {
    {
        .name = TYPE_IMX93_TSTMR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93TstmrState),
        .class_init = tstmr_class_init,
    },
};

DEFINE_TYPES(tstmr_types)
