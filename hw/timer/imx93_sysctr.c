/*
 * NXP i.MX 93 System Counter (SYS_CTR)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A free-running 24 MHz system counter (the timer-imx-sysctr clocksource reads
 * CNTCV_LO/HI) plus the compare-frame clockevent: writing CMPCV + enabling
 * CMPCR arms an interrupt (GIC SPI 74) that fires when the counter reaches the
 * compare value. The counter tracks the virtual clock so it agrees with the
 * Arm generic timer.
 */

#include "qemu/osdep.h"
#include "hw/timer/imx93_sysctr.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"

#define CNTCV_LO    0x00008
#define CNTCV_HI    0x0000c
#define CMP_OFFSET  0x10000
#define CMPCV_LO    (CMP_OFFSET + 0x20)
#define CMPCV_HI    (CMP_OFFSET + 0x24)
#define CMPCR       (CMP_OFFSET + 0x2c)

#define SYS_CTR_EN       0x1
#define SYS_CTR_IRQ_MASK 0x2

#define SYSCTR_HZ   24000000

static uint64_t sysctr_count(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), SYSCTR_HZ,
                    NANOSECONDS_PER_SECOND);
}

static void sysctr_update(IMX93SysctrState *s)
{
    uint64_t now = sysctr_count();

    if (!(s->cmpcr & SYS_CTR_EN)) {
        timer_del(&s->cmp);
        qemu_set_irq(s->irq, 0);
        return;
    }
    if (s->cmpcv <= now) {
        timer_del(&s->cmp);
        qemu_set_irq(s->irq, !(s->cmpcr & SYS_CTR_IRQ_MASK));
    } else {
        int64_t fire = muldiv64(s->cmpcv, NANOSECONDS_PER_SECOND, SYSCTR_HZ);

        qemu_set_irq(s->irq, 0);
        timer_mod(&s->cmp, fire);
    }
}

static void sysctr_fire(void *opaque)
{
    IMX93SysctrState *s = opaque;

    if (s->cmpcr & SYS_CTR_EN) {
        qemu_set_irq(s->irq, !(s->cmpcr & SYS_CTR_IRQ_MASK));
    }
}

static uint64_t sysctr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93SysctrState *s = opaque;
    uint64_t cnt = sysctr_count();

    switch (offset) {
    case CNTCV_LO:
        return cnt & 0xffffffff;
    case CNTCV_HI:
        return (cnt >> 32) & 0xffffffff;
    case CMPCV_LO:
        return s->cmpcv & 0xffffffff;
    case CMPCV_HI:
        return (s->cmpcv >> 32) & 0xffffffff;
    case CMPCR:
        return s->cmpcr;
    default:
        return 0;
    }
}

static void sysctr_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    IMX93SysctrState *s = opaque;

    switch (offset) {
    case CMPCV_LO:
        s->cmpcv = (s->cmpcv & ~0xffffffffULL) | (value & 0xffffffff);
        sysctr_update(s);
        break;
    case CMPCV_HI:
        s->cmpcv = (s->cmpcv & 0xffffffffULL) | ((uint64_t)value << 32);
        sysctr_update(s);
        break;
    case CMPCR:
        s->cmpcr = value;
        sysctr_update(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sysctr_ops = {
    .read = sysctr_read,
    .write = sysctr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sysctr_reset_hold(Object *obj, ResetType type)
{
    IMX93SysctrState *s = IMX93_SYSCTR(obj);

    s->cmpcr = 0;
    s->cmpcv = 0;
    timer_del(&s->cmp);
}

static void sysctr_realize(DeviceState *dev, Error **errp)
{
    IMX93SysctrState *s = IMX93_SYSCTR(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &sysctr_ops, s,
                          TYPE_IMX93_SYSCTR, IMX93_SYSCTR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->cmp, QEMU_CLOCK_VIRTUAL, sysctr_fire, s);
}

static const VMStateDescription vmstate_sysctr = {
    .name = TYPE_IMX93_SYSCTR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmpcr, IMX93SysctrState),
        VMSTATE_UINT64(cmpcv, IMX93SysctrState),
        VMSTATE_TIMER(cmp, IMX93SysctrState),
        VMSTATE_END_OF_LIST()
    },
};

static void sysctr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = sysctr_realize;
    dc->vmsd = &vmstate_sysctr;
    rc->phases.hold = sysctr_reset_hold;
    dc->desc = "i.MX93 system counter";
}

static const TypeInfo sysctr_types[] = {
    {
        .name = TYPE_IMX93_SYSCTR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93SysctrState),
        .class_init = sysctr_class_init,
    },
};

DEFINE_TYPES(sysctr_types)
