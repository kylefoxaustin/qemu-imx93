/*
 * NXP i.MX 93 SAR-ADC
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models enough of the i.MX93 SAR-ADC for the imx93_adc driver.
 * Self-calibration (MCR.CALSTART) completes immediately, with
 * MSR.CALBUSY/CALFAIL clear. A normal conversion (MCR.NSTART) fills the
 * per-channel data registers (PCDRn, 12-bit) for the channels in NCMR0, sets
 * the end-of-conversion status (ISR) and raises the EOC interrupt the driver
 * waits on. Conversions return a fixed mid-scale sample; no analog input
 * source is modelled.
 */

#include "qemu/osdep.h"
#include "hw/adc/imx93_adc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define ADC_MCR     0x00
#define ADC_MSR     0x04
#define ADC_ISR     0x10
#define ADC_IMR     0x20
#define ADC_NCMR0   0xa4
#define ADC_PCDR0   0x100

#define MCR_NSTART      (1u << 24)
#define MCR_CALSTART    (1u << 14)
#define MCR_PWDN        (1u << 0)

/*
 * MCR reset value per the i.MX 93 RM (SAR_ADC memory map): 0x0000_3901.
 * PWDN (bit 0) is set - the ADC comes up powered down - alongside the
 * default NRSMPL/TSAMP sampling-config and ADCLKSE bits. Only ADCLKSE
 * (bit 8) is consumed by the Linux driver, which sets it explicitly
 * during clock config regardless of reset, so the extra bits are inert
 * here; we still reset to the silicon value so a guest read-modify-write
 * of MCR does not launder a fabricated default back into its own config.
 */
#define MCR_RESET       0x00003901u

/* MSR.ADCSTATUS[2:0] codes the driver polls for. */
#define MSR_STATUS_IDLE         0
#define MSR_STATUS_POWER_DOWN   1

#define ISR_ECH         (1u << 0)
#define ISR_EOC         (1u << 1)

#define PCDR_CDATA_MASK 0xfff
#define ADC_MIDSCALE    0x800   /* fixed mid-scale sample (no analog source) */

static void adc_update_irq(IMX93AdcState *s)
{
    qemu_set_irq(s->irq, !!(s->isr & s->imr & (ISR_EOC | ISR_ECH)));
}

static void adc_convert(IMX93AdcState *s)
{
    int ch;

    for (ch = 0; ch < IMX93_ADC_NCH; ch++) {
        if (s->ncmr0 & (1u << ch)) {
            s->pcdr[ch] = ADC_MIDSCALE & PCDR_CDATA_MASK;
        }
    }
    s->isr |= ISR_EOC | ISR_ECH;
    adc_update_irq(s);
}

static uint64_t adc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93AdcState *s = opaque;

    switch (offset) {
    case ADC_MCR:
        return s->mcr;
    case ADC_MSR:
        /* Report power-down vs idle; never calibrating/busy/failed. */
        return (s->mcr & MCR_PWDN) ? MSR_STATUS_POWER_DOWN : MSR_STATUS_IDLE;
    case ADC_ISR:
        return s->isr;
    case ADC_IMR:
        return s->imr;
    case ADC_NCMR0:
        return s->ncmr0;
    default:
        if (offset >= ADC_PCDR0 && offset < ADC_PCDR0 + 4 * IMX93_ADC_NCH) {
            return s->pcdr[(offset - ADC_PCDR0) / 4];
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void adc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    IMX93AdcState *s = opaque;

    switch (offset) {
    case ADC_MCR:
        s->mcr = value & ~(MCR_NSTART | MCR_CALSTART);
        /* CALSTART self-completes; nothing to do (MSR reports idle). */
        if (value & MCR_NSTART) {
            adc_convert(s);
        }
        break;
    case ADC_ISR:
        s->isr &= ~value;       /* write-1-to-clear */
        adc_update_irq(s);
        break;
    case ADC_IMR:
        s->imr = value;
        adc_update_irq(s);
        break;
    case ADC_NCMR0:
        s->ncmr0 = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void adc_reset_hold(Object *obj, ResetType type)
{
    IMX93AdcState *s = IMX93_ADC(obj);

    s->mcr = MCR_RESET;     /* RM reset: PWDN set (powered down) + sampling defaults */
    s->isr = 0;
    s->imr = 0;
    s->ncmr0 = 0;
    memset(s->pcdr, 0, sizeof(s->pcdr));
}

static void adc_realize(DeviceState *dev, Error **errp)
{
    IMX93AdcState *s = IMX93_ADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &adc_ops, s,
                          TYPE_IMX93_ADC, IMX93_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_adc = {
    .name = TYPE_IMX93_ADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMX93AdcState),
        VMSTATE_UINT32(isr, IMX93AdcState),
        VMSTATE_UINT32(imr, IMX93AdcState),
        VMSTATE_UINT32(ncmr0, IMX93AdcState),
        VMSTATE_UINT32_ARRAY(pcdr, IMX93AdcState, IMX93_ADC_NCH),
        VMSTATE_END_OF_LIST()
    },
};

static void adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = adc_realize;
    dc->vmsd = &vmstate_adc;
    rc->phases.hold = adc_reset_hold;
    dc->desc = "i.MX93 SAR-ADC";
}

static const TypeInfo adc_types[] = {
    {
        .name = TYPE_IMX93_ADC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93AdcState),
        .class_init = adc_class_init,
    },
};

DEFINE_TYPES(adc_types)
