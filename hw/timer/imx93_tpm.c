/*
 * NXP i.MX 93 Timer/PWM Module (TPM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
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
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
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

/*
 * Effective module-clock rate. When a CCM LPCG clock is wired to this TPM
 * (TPM2 in this SoC), the counter runs at whatever that gated clock delivers -
 * 0 when the guest clears the gate. TPMs with no modelled gate fall back to the
 * nominal rate so their counters are unchanged.
 */
static uint32_t tpm_hz(IMX93TpmState *s)
{
    return clock_has_source(s->clk) ? clock_get_hz(s->clk) : TPM_CLK_HZ;
}

static uint32_t tpm_count(IMX93TpmState *s)
{
    uint64_t delta, period;
    uint32_t ps;

    if (!(s->sc & SC_CMOD)) {
        return 0;               /* counter disabled */
    }
    ps = s->sc & SC_PS;
    delta = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_ns,
                     tpm_hz(s), NANOSECONDS_PER_SECOND) >> ps;
    period = (s->mod & 0xffff) + 1;
    return (s->cnt_base + delta) % period;
}

/*
 * Snapshot the running count and rebase to now. Called before anything that
 * changes the effective rate (a clock-gate change, a prescaler/enable write) so
 * the counter carries its value across the change instead of restarting - which
 * is what makes gating freeze-and-hold: while the gate is clear the rate is 0,
 * delta stays 0, and the count holds at cnt_base until the gate returns.
 */
static void tpm_settle(IMX93TpmState *s)
{
    s->cnt_base = tpm_count(s);
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* The CCM LPCG gate is about to change the clock: settle at the old rate. */
static void tpm_clk_update(void *opaque, ClockEvent event)
{
    tpm_settle(opaque);
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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
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
            s->cnt_base = 0;
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case TPM_SC:
        /* Settle at the old rate, then apply the new prescaler/enable. */
        tpm_settle(s);
        if ((value & SC_CMOD) && !(s->sc & SC_CMOD)) {
            s->cnt_base = 0;    /* a fresh enable starts the count at 0 */
        }
        s->sc = value;
        break;
    case TPM_CNT:
        s->cnt_base = 0;        /* any write resets the counter to 0 */
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
    case TPM_MOD:
        tpm_settle(s);          /* fold the running count under the old period */
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
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad write offset 0x%" HWADDR_PRIx
                          " value 0x%" PRIx64 "\n",
                          __func__, offset, value);
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
    s->cnt_base = 0;
    s->base_ns = 0;
}

static void tpm_init(Object *obj)
{
    IMX93TpmState *s = IMX93_TPM(obj);

    /*
     * The module clock is an input the CCM drives, gated by this TPM's LPCG.
     * Create it in instance_init so the board can connect it before realize;
     * ClockPreUpdate lets the counter settle at the old rate before the gate
     * change takes effect.
     */
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", tpm_clk_update, s,
                                ClockPreUpdate);
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
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_CLOCK(clk, IMX93TpmState),
        VMSTATE_INT64(base_ns, IMX93TpmState),
        VMSTATE_UINT32(cnt_base, IMX93TpmState),
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
        .instance_init = tpm_init,
        .class_init = tpm_class_init,
    },
};

DEFINE_TYPES(tpm_types)
