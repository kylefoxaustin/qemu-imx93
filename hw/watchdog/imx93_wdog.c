/*
 * NXP i.MX 93 Watchdog (imx7ulp-wdt compatible)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the i.MX7ULP-style WDOG the i.MX93 uses. The driver unlocks the device
 * by writing UNLOCK to CNT (which sets CS.ULK), reconfigures via CS (which the
 * model acknowledges by setting CS.RCS), and pings by writing REFRESH to CNT.
 * When enabled (CS.EN) and not refreshed within the timeout, the QEMU watchdog
 * action fires (reset by default). Defaults disabled, so a plain boot - where
 * no bootloader started it and nothing opens /dev/watchdog - is undisturbed.
 */

#include "qemu/osdep.h"
#include "hw/watchdog/imx93_wdog.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "system/watchdog.h"

#define WDOG_CS     0x0
#define WDOG_CNT    0x4
#define WDOG_TOVAL  0x8
#define WDOG_WIN    0xc

#define CS_STOP     (1u << 0)
#define CS_WAIT     (1u << 1)
#define CS_EN       (1u << 7)
#define CS_RCS      (1u << 10)
#define CS_ULK      (1u << 11)
#define CS_PRES     (1u << 12)
#define CS_CMD32EN  (1u << 13)

#define UNLOCK      0xd928c520u
#define REFRESH     0xb480a602u

#define WDOG_HZ     1000        /* LPO clock */

static uint32_t wdog_rate(IMX93WdogState *s)
{
    return (s->cs & CS_PRES) ? (WDOG_HZ / 256) : WDOG_HZ;
}

static void wdog_arm(IMX93WdogState *s)
{
    uint32_t rate = wdog_rate(s);
    uint64_t ms;
    int64_t deadline;

    if (!(s->cs & CS_EN) || rate == 0 || s->toval == 0) {
        timer_del(&s->timer);
        return;
    }
    ms = (uint64_t)s->toval * 1000 / rate;
    deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ms * SCALE_MS;
    timer_mod(&s->timer, deadline);
}

static void wdog_expire(void *opaque)
{
    watchdog_perform_action();
}

static uint64_t wdog_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93WdogState *s = opaque;
    uint32_t cs = s->cs;

    cs |= s->unlocked ? CS_ULK : 0;
    cs |= s->rcs ? CS_RCS : 0;

    switch (offset) {
    case WDOG_CS:
        return cs;
    case WDOG_CNT:
        /* opaque to the driver; it only writes UNLOCK/REFRESH here */
        return 0;
    case WDOG_TOVAL:
        return s->toval;
    case WDOG_WIN:
        return s->win;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void wdog_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    IMX93WdogState *s = opaque;

    switch (offset) {
    case WDOG_CS:
        if (s->unlocked) {
            s->cs = value & ~(CS_ULK | CS_RCS);
            s->unlocked = false;    /* writing CS re-locks */
            s->rcs = true;          /* reconfiguration acknowledged */
            wdog_arm(s);
        }
        break;
    case WDOG_CNT:
        if (value == UNLOCK) {
            s->unlocked = true;
            s->rcs = false;
        } else if (value == REFRESH) {
            wdog_arm(s);            /* ping: restart the timeout */
        }
        break;
    case WDOG_TOVAL:
        if (s->unlocked) {
            s->toval = value;
        }
        break;
    case WDOG_WIN:
        if (s->unlocked) {
            s->win = value;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps wdog_ops = {
    .read = wdog_read,
    .write = wdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void wdog_reset_hold(Object *obj, ResetType type)
{
    IMX93WdogState *s = IMX93_WDOG(obj);

    /*
     * Default disabled (no bootloader ran to enable it under QEMU), but with
     * CMD32EN set so the driver uses the single 32-bit UNLOCK/REFRESH sequence
     * rather than two 16-bit half-word writes.
     */
    s->cs = CS_CMD32EN;
    s->toval = 0;
    s->win = 0;
    s->unlocked = false;
    s->rcs = false;
    timer_del(&s->timer);
}

static void wdog_realize(DeviceState *dev, Error **errp)
{
    IMX93WdogState *s = IMX93_WDOG(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &wdog_ops, s,
                          TYPE_IMX93_WDOG, IMX93_WDOG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, wdog_expire, s);
}

static const VMStateDescription vmstate_wdog = {
    .name = TYPE_IMX93_WDOG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cs, IMX93WdogState),
        VMSTATE_UINT32(toval, IMX93WdogState),
        VMSTATE_UINT32(win, IMX93WdogState),
        VMSTATE_BOOL(unlocked, IMX93WdogState),
        VMSTATE_BOOL(rcs, IMX93WdogState),
        VMSTATE_TIMER(timer, IMX93WdogState),
        VMSTATE_END_OF_LIST()
    },
};

static void wdog_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = wdog_realize;
    dc->vmsd = &vmstate_wdog;
    rc->phases.hold = wdog_reset_hold;
    dc->desc = "i.MX93 watchdog";
}

static const TypeInfo wdog_types[] = {
    {
        .name = TYPE_IMX93_WDOG,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93WdogState),
        .class_init = wdog_class_init,
    },
};

DEFINE_TYPES(wdog_types)
