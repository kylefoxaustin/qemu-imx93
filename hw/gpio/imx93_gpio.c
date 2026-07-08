/*
 * NXP i.MX 93 / i.MX 8ULP GPIO controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See imx93_gpio.h. Register offsets match the gpio-vf610 driver's
 * single-base imx8ulp layout (gpio_base = base + 0x40, port_base = base +
 * 0x80).
 */

#include "qemu/osdep.h"
#include "hw/gpio/imx93_gpio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Data registers, relative to base + GPIO_BASE_OFF (0x40). */
#define GPIO_BASE_OFF   0x40
#define GPIO_PDOR       (GPIO_BASE_OFF + 0x00)
#define GPIO_PSOR       (GPIO_BASE_OFF + 0x04)
#define GPIO_PCOR       (GPIO_BASE_OFF + 0x08)
#define GPIO_PTOR       (GPIO_BASE_OFF + 0x0c)
#define GPIO_PDIR       (GPIO_BASE_OFF + 0x10)
#define GPIO_PDDR       (GPIO_BASE_OFF + 0x14)

/* PORT registers, relative to base + PORT_BASE_OFF (0x80). */
#define PORT_BASE_OFF   0x80
#define PORT_PCR0       (PORT_BASE_OFF + 0x00)
#define PORT_PCR_LAST   (PORT_BASE_OFF + (IMX93_GPIO_PINS - 1) * 4)
#define PORT_ISFR       (PORT_BASE_OFF + 0xa0)

static void imx93_gpio_update_irq(IMX93GPIOState *s)
{
    /*
     * Raise the line when any pin with interrupts enabled (PCR.IRQC != 0)
     * has its status flag set. Nothing in the model drives external pins, so
     * ISFR stays clear and the line stays low; the driver still registers
     * its irqchip and can request the parent IRQ.
     */
    uint32_t pending = 0;
    int i;

    for (i = 0; i < IMX93_GPIO_PINS; i++) {
        if ((s->pcr[i] >> 16) & 0xf) {     /* IRQC field */
            pending |= s->isfr & BIT(i);
        }
    }
    qemu_set_irq(s->irq[0], !!pending);
}

static uint64_t imx93_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93GPIOState *s = opaque;

    switch (offset) {
    case GPIO_PDOR:
        return s->pdor;
    case GPIO_PDIR:
        /* No external inputs modeled: read back the output latch. */
        return s->pdor;
    case GPIO_PDDR:
        return s->pddr;
    case PORT_ISFR:
        return s->isfr;
    case PORT_PCR0 ... PORT_PCR_LAST:
        return s->pcr[(offset - PORT_PCR0) / 4];
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imx93_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX93GPIOState *s = opaque;

    switch (offset) {
    case GPIO_PDOR:
        s->pdor = value;
        break;
    case GPIO_PSOR:
        s->pdor |= value;
        break;
    case GPIO_PCOR:
        s->pdor &= ~(uint32_t)value;
        break;
    case GPIO_PTOR:
        s->pdor ^= value;
        break;
    case GPIO_PDDR:
        s->pddr = value;
        break;
    case PORT_ISFR:
        s->isfr &= ~(uint32_t)value;       /* W1C */
        imx93_gpio_update_irq(s);
        break;
    case PORT_PCR0 ... PORT_PCR_LAST:
        s->pcr[(offset - PORT_PCR0) / 4] = value;
        imx93_gpio_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps imx93_gpio_ops = {
    .read = imx93_gpio_read,
    .write = imx93_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_gpio_reset_hold(Object *obj, ResetType type)
{
    IMX93GPIOState *s = IMX93_GPIO(obj);

    s->pdor = s->pddr = s->isfr = 0;
    memset(s->pcr, 0, sizeof(s->pcr));
}

static void imx93_gpio_init(Object *obj)
{
    IMX93GPIOState *s = IMX93_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_gpio_ops, s,
                          TYPE_IMX93_GPIO, IMX93_GPIO_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
}

static const VMStateDescription vmstate_imx93_gpio = {
    .name = TYPE_IMX93_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pdor, IMX93GPIOState),
        VMSTATE_UINT32(pddr, IMX93GPIOState),
        VMSTATE_UINT32_ARRAY(pcr, IMX93GPIOState, IMX93_GPIO_PINS),
        VMSTATE_UINT32(isfr, IMX93GPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "i.MX 93 GPIO controller";
    rc->phases.hold = imx93_gpio_reset_hold;
    dc->vmsd = &vmstate_imx93_gpio;
}

static const TypeInfo imx93_gpio_types[] = {
    {
        .name           = TYPE_IMX93_GPIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93GPIOState),
        .instance_init  = imx93_gpio_init,
        .class_init     = imx93_gpio_class_init,
    },
};

DEFINE_TYPES(imx93_gpio_types)
