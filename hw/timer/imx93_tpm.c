/*
 * NXP i.MX 93 Timer/PWM Module (TPM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the TPM registers the pwm-imx-tpm (and TPM timer) drivers use: PARAM
 * (channel count), GLOBAL (software reset), SC (clock mode + prescaler), CNT (a
 * free-running modulo counter that advances while a clock is selected), MOD
 * (period) and the per-channel CnSC/CnV (PWM mode + compare value). The PWM
 * output is not observable (no physical pin), so this is a functional register
 * model that lets the driver bind, register a pwmchip, and configure PWMs.
 */

#include "qemu/osdep.h"
#include "hw/timer/imx93_tpm.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"

#define TPM_PARAM   0x04
#define TPM_GLOBAL  0x08
#define TPM_SC      0x10
#define TPM_CNT     0x14
#define TPM_MOD     0x18
#define TPM_C0SC    0x20    /* CnSC(n) = 0x20 + n*8, CnV(n) = 0x24 + n*8 */

#define SC_CMOD     (3u << 3)   /* clock mode: 0 = disabled */
#define SC_PS       (7u << 0)   /* prescaler = 1 << PS */
#define GLOBAL_RST  (1u << 1)

#define TPM_CLK_HZ  24000000    /* nominal module clock */

static uint32_t tpm_count(IMX93TpmState *s)
{
    uint64_t ticks, period;
    uint32_t ps;

    if (!(s->sc & SC_CMOD)) {
        return 0;               /* counter disabled */
    }
    ps = s->sc & SC_PS;
    ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_ns,
                     TPM_CLK_HZ, NANOSECONDS_PER_SECOND) >> ps;
    period = (s->mod & 0xffff) + 1;
    return ticks % period;
}

static uint64_t tpm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93TpmState *s = opaque;

    switch (offset) {
    case TPM_PARAM:
        return IMX93_TPM_CHANNELS;          /* CHAN in [7:0] */
    case TPM_SC:
        return s->sc;
    case TPM_CNT:
        return tpm_count(s);
    case TPM_MOD:
        return s->mod;
    default:
        if (offset >= TPM_C0SC &&
            offset < TPM_C0SC + IMX93_TPM_CHANNELS * 8) {
            uint32_t n = (offset - TPM_C0SC) / 8;

            return ((offset - TPM_C0SC) & 4) ? s->cnv[n] : s->cnsc[n];
        }
        return 0;
    }
}

static void tpm_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    IMX93TpmState *s = opaque;

    switch (offset) {
    case TPM_GLOBAL:
        if (value & GLOBAL_RST) {
            s->sc = 0;
            s->mod = 0;
            memset(s->cnsc, 0, sizeof(s->cnsc));
            memset(s->cnv, 0, sizeof(s->cnv));
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case TPM_SC:
        /* Restart the counter timebase when the clock is (re)enabled. */
        if ((value & SC_CMOD) && !(s->sc & SC_CMOD)) {
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        s->sc = value;
        break;
    case TPM_CNT:
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);  /* write resets */
        break;
    case TPM_MOD:
        s->mod = value;
        break;
    default:
        if (offset >= TPM_C0SC &&
            offset < TPM_C0SC + IMX93_TPM_CHANNELS * 8) {
            uint32_t n = (offset - TPM_C0SC) / 8;

            if ((offset - TPM_C0SC) & 4) {
                s->cnv[n] = value;
            } else {
                s->cnsc[n] = value;
            }
        }
        break;
    }
}

static const MemoryRegionOps tpm_ops = {
    .read = tpm_read,
    .write = tpm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void tpm_reset_hold(Object *obj, ResetType type)
{
    IMX93TpmState *s = IMX93_TPM(obj);

    s->sc = 0;
    s->mod = 0;
    memset(s->cnsc, 0, sizeof(s->cnsc));
    memset(s->cnv, 0, sizeof(s->cnv));
    s->base_ns = 0;
}

static void tpm_realize(DeviceState *dev, Error **errp)
{
    IMX93TpmState *s = IMX93_TPM(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &tpm_ops, s,
                          TYPE_IMX93_TPM, IMX93_TPM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_tpm = {
    .name = TYPE_IMX93_TPM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(base_ns, IMX93TpmState),
        VMSTATE_UINT32(sc, IMX93TpmState),
        VMSTATE_UINT32(mod, IMX93TpmState),
        VMSTATE_UINT32_ARRAY(cnsc, IMX93TpmState, IMX93_TPM_CHANNELS),
        VMSTATE_UINT32_ARRAY(cnv, IMX93TpmState, IMX93_TPM_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void tpm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = tpm_realize;
    dc->vmsd = &vmstate_tpm;
    rc->phases.hold = tpm_reset_hold;
    dc->desc = "i.MX93 timer/PWM module";
}

static const TypeInfo tpm_types[] = {
    {
        .name = TYPE_IMX93_TPM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93TpmState),
        .class_init = tpm_class_init,
    },
};

DEFINE_TYPES(tpm_types)
