/*
 * NXP i.MX 93 Hardware Semaphores (SEMA42)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SEMA42 provides 16 gates for inter-master mutual exclusion. Each byte gate
 * reads back the owning domain (0 = unlocked); a master locks a free gate by
 * writing its domain and unlocks by writing 0. This models the cooperative
 * lock/unlock semantics (a free gate can be taken; the owner can release it).
 * True per-bus-master domain enforcement is not modelled, as QEMU MMIO does not
 * carry the originating master's domain id; the RSTGT reset sequence frees all
 * gates.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_sema42.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define SEMA42_RSTGT_W 0x42    /* reset-gate write (sequence 0xe2,0x1d) */
#define SEMA42_RSTGT_R 0x43    /* reset-gate read-back */

static uint64_t sema42_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93Sema42State *s = opaque;

    if (offset < IMX93_SEMA42_GATES) {
        return s->gate[offset];
    }
    return 0;
}

static void sema42_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    IMX93Sema42State *s = opaque;

    if (offset < IMX93_SEMA42_GATES) {
        uint8_t want = value & 0xf;

        /* Take a free gate, or release one by writing 0. */
        if (want == 0) {
            s->gate[offset] = 0;
        } else if (s->gate[offset] == 0 || s->gate[offset] == want) {
            s->gate[offset] = want;
        }
        return;
    }
    if (offset == SEMA42_RSTGT_W) {
        /* The reset sequence (0xe2 then 0x1d) frees all gates. */
        if ((value & 0xff) == 0x1d) {
            memset(s->gate, 0, sizeof(s->gate));
        }
    }
}

static const MemoryRegionOps sema42_ops = {
    .read = sema42_read,
    .write = sema42_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void sema42_reset_hold(Object *obj, ResetType type)
{
    IMX93Sema42State *s = IMX93_SEMA42(obj);

    memset(s->gate, 0, sizeof(s->gate));
}

static void sema42_realize(DeviceState *dev, Error **errp)
{
    IMX93Sema42State *s = IMX93_SEMA42(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &sema42_ops, s,
                          TYPE_IMX93_SEMA42, IMX93_SEMA42_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_sema42 = {
    .name = TYPE_IMX93_SEMA42,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(gate, IMX93Sema42State, IMX93_SEMA42_GATES),
        VMSTATE_END_OF_LIST()
    },
};

static void sema42_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = sema42_realize;
    dc->vmsd = &vmstate_sema42;
    rc->phases.hold = sema42_reset_hold;
    dc->desc = "i.MX93 hardware semaphores";
}

static const TypeInfo sema42_types[] = {
    {
        .name = TYPE_IMX93_SEMA42,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93Sema42State),
        .class_init = sema42_class_init,
    },
};

DEFINE_TYPES(sema42_types)
