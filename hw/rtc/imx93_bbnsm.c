/*
 * NXP i.MX 93 Battery-Backed Non-Secure Module (BBNSM) - RTC + power key
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The BBNSM RTC is a 47-bit counter clocked at 32768 Hz; Linux reads seconds as
 * counter >> 15. This models the registers the rtc-nxp-bbnsm driver uses (CTRL,
 * INT_EN, EVENTS, RTC_LS/MS, TA) with the counter tracking host wall-clock,
 * a seconds-granularity alarm, and the power-key EVENTS register. The alarm
 * fires an interrupt (GIC SPI 73) when the RTC reaches TA and TA interrupts are
 * enabled.
 */

#include "qemu/osdep.h"
#include "hw/rtc/imx93_bbnsm.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"

#define BBNSM_CTRL      0x08
#define BBNSM_INT_EN    0x10
#define BBNSM_EVENTS    0x14
#define BBNSM_PAD_CTRL  0x24
#define BBNSM_RTC_LS    0x40
#define BBNSM_RTC_MS    0x44
#define BBNSM_TA        0x50

#define RTC_EN          0x2
#define RTC_EN_MSK      0x3
#define TA_EN           (0x2 << 2)
#define TA_EN_MSK       (0x3 << 2)
#define EVENT_TA        (0x2 << 2)   /* time-alarm event in EVENTS */

#define RTC_HZ          32768
#define RTC_SECS_SHIFT  15
#define RTC_COUNTER_MASK ((1ULL << 47) - 1)

static int64_t bbnsm_host_ticks(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_HOST), RTC_HZ,
                    NANOSECONDS_PER_SECOND);
}

static uint64_t bbnsm_counter(IMX93BbnsmState *s)
{
    return (bbnsm_host_ticks() + s->offset_ticks) & RTC_COUNTER_MASK;
}

static void bbnsm_update_irq(IMX93BbnsmState *s)
{
    bool ta = (s->events & EVENT_TA) && (s->int_en & TA_EN);

    qemu_set_irq(s->irq, ta);
}

static void bbnsm_update_alarm(IMX93BbnsmState *s)
{
    int64_t target, fire_ticks, fire_ns;

    if (!(s->ctrl & TA_EN)) {
        timer_del(&s->alarm);
        return;
    }

    /* Fire when the RTC second count reaches TA, i.e. counter == TA << 15. */
    target = (int64_t)s->ta << RTC_SECS_SHIFT;
    fire_ticks = target - s->offset_ticks;
    fire_ns = muldiv64(fire_ticks, NANOSECONDS_PER_SECOND, RTC_HZ);
    timer_mod(&s->alarm, fire_ns);
}

static void bbnsm_alarm_fire(void *opaque)
{
    IMX93BbnsmState *s = opaque;

    s->events |= EVENT_TA;
    bbnsm_update_irq(s);
}

static uint64_t bbnsm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93BbnsmState *s = opaque;

    switch (offset) {
    case BBNSM_CTRL:
        return s->ctrl;
    case BBNSM_INT_EN:
        return s->int_en;
    case BBNSM_EVENTS:
        return s->events;
    case BBNSM_PAD_CTRL:
        return s->pad_ctrl;
    case BBNSM_RTC_LS:
        return bbnsm_counter(s) & 0xffffffff;
    case BBNSM_RTC_MS:
        return (bbnsm_counter(s) >> 32) & 0x7fff;
    case BBNSM_TA:
        return s->ta;
    default:
        return 0;
    }
}

static void bbnsm_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IMX93BbnsmState *s = opaque;

    switch (offset) {
    case BBNSM_CTRL:
        /*
         * Linux sets the time by clearing RTC_EN, writing RTC_LS/MS, then
         * setting RTC_EN; only then do we rebase the counter on the written
         * value. Enabling RTC_EN without a preceding write (e.g. the driver's
         * probe) must NOT reset the (battery-backed) counter, so it keeps
         * tracking host wall-clock.
         */
        if ((value & RTC_EN_MSK) == RTC_EN &&
            (s->ctrl & RTC_EN_MSK) != RTC_EN && s->set_pending) {
            uint64_t target = ((uint64_t)s->set_ms << 32) | s->set_ls;

            s->offset_ticks = (int64_t)target - bbnsm_host_ticks();
            s->set_pending = false;
        }
        s->ctrl = value;
        bbnsm_update_alarm(s);
        break;
    case BBNSM_INT_EN:
        s->int_en = value;
        bbnsm_update_irq(s);
        break;
    case BBNSM_EVENTS:
        s->events &= ~value;     /* write-1-to-clear */
        bbnsm_update_irq(s);
        break;
    case BBNSM_PAD_CTRL:
        s->pad_ctrl = value;
        break;
    case BBNSM_RTC_LS:
        s->set_ls = value;
        s->set_pending = true;
        break;
    case BBNSM_RTC_MS:
        s->set_ms = value & 0x7fff;
        s->set_pending = true;
        break;
    case BBNSM_TA:
        s->ta = value;
        bbnsm_update_alarm(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bbnsm_ops = {
    .read = bbnsm_read,
    .write = bbnsm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void bbnsm_reset_hold(Object *obj, ResetType type)
{
    IMX93BbnsmState *s = IMX93_BBNSM(obj);

    s->ctrl = 0;
    s->int_en = 0;
    s->events = 0;
    s->pad_ctrl = 0;
    s->ta = 0;
    s->set_ls = 0;
    s->set_ms = 0;
    s->set_pending = false;
    s->offset_ticks = 0;
    timer_del(&s->alarm);
    bbnsm_update_irq(s);
}

static void bbnsm_realize(DeviceState *dev, Error **errp)
{
    IMX93BbnsmState *s = IMX93_BBNSM(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &bbnsm_ops, s,
                          TYPE_IMX93_BBNSM, IMX93_BBNSM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->alarm, QEMU_CLOCK_HOST, bbnsm_alarm_fire, s);
}

static const VMStateDescription vmstate_bbnsm = {
    .name = TYPE_IMX93_BBNSM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, IMX93BbnsmState),
        VMSTATE_UINT32(int_en, IMX93BbnsmState),
        VMSTATE_UINT32(events, IMX93BbnsmState),
        VMSTATE_UINT32(pad_ctrl, IMX93BbnsmState),
        VMSTATE_UINT32(ta, IMX93BbnsmState),
        VMSTATE_UINT32(set_ls, IMX93BbnsmState),
        VMSTATE_UINT32(set_ms, IMX93BbnsmState),
        VMSTATE_BOOL(set_pending, IMX93BbnsmState),
        VMSTATE_INT64(offset_ticks, IMX93BbnsmState),
        VMSTATE_TIMER(alarm, IMX93BbnsmState),
        VMSTATE_END_OF_LIST()
    },
};

static void bbnsm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = bbnsm_realize;
    dc->vmsd = &vmstate_bbnsm;
    rc->phases.hold = bbnsm_reset_hold;
    dc->desc = "i.MX93 BBNSM RTC";
}

static const TypeInfo bbnsm_types[] = {
    {
        .name = TYPE_IMX93_BBNSM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93BbnsmState),
        .class_init = bbnsm_class_init,
    },
};

DEFINE_TYPES(bbnsm_types)
