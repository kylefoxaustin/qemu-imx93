/*
 * NXP i.MX 93 On-Chip OTP controller (OCOTP) - fuse readback
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * On the i.MX93 the OCOTP is a "syscon": the kernel reads fuse shadow values
 * directly (nvmem cells) for the Ethernet MAC addresses and the SoC unique ID.
 * A logging stub returns zero, giving an all-zero MAC. This models the fuse
 * shadow as a small read-mostly array preset with a stable MAC (NXP OUI) and a
 * unique ID, so the FEC/eQOS get a deterministic MAC and the UID nvmem cell
 * reads a non-zero value. Writes are accepted (shadow), as OTP programming is
 * not modelled.
 */

#include "qemu/osdep.h"
#include "hw/nvram/imx93_ocotp.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* nvmem cell offsets (from the EVK device tree). */
#define OCOTP_UID    0x0c0   /* soc-uid, 16 bytes */
#define OCOTP_MAC1   0x4ec   /* mac-address, 6 bytes */
#define OCOTP_MAC2   0x4f2   /* mac-address, 6 bytes */

static uint64_t ocotp_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93OcotpState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (offset + i < IMX93_OCOTP_FUSES) {
            val |= (uint64_t)s->fuses[offset + i] << (8 * i);
        }
    }
    return val;
}

static void ocotp_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IMX93OcotpState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (offset + i < IMX93_OCOTP_FUSES) {
            s->fuses[offset + i] = (value >> (8 * i)) & 0xff;
        }
    }
}

static const MemoryRegionOps ocotp_ops = {
    .read = ocotp_read,
    .write = ocotp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ocotp_reset_hold(Object *obj, ResetType type)
{
    IMX93OcotpState *s = IMX93_OCOTP(obj);
    /* NXP OUI 00:04:9f; low bytes arbitrary but stable. */
    static const uint8_t mac1[6] = { 0x00, 0x04, 0x9f, 0x93, 0x00, 0x01 };
    static const uint8_t mac2[6] = { 0x00, 0x04, 0x9f, 0x93, 0x00, 0x02 };
    int i;

    memset(s->fuses, 0, sizeof(s->fuses));
    memcpy(&s->fuses[OCOTP_MAC1], mac1, sizeof(mac1));
    memcpy(&s->fuses[OCOTP_MAC2], mac2, sizeof(mac2));
    for (i = 0; i < 16; i++) {
        s->fuses[OCOTP_UID + i] = 0x93 ^ (i * 0x11);    /* stable nonzero UID */
    }
}

static void ocotp_realize(DeviceState *dev, Error **errp)
{
    IMX93OcotpState *s = IMX93_OCOTP(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &ocotp_ops, s,
                          TYPE_IMX93_OCOTP, IMX93_OCOTP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_ocotp = {
    .name = TYPE_IMX93_OCOTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fuses, IMX93OcotpState, IMX93_OCOTP_FUSES),
        VMSTATE_END_OF_LIST()
    },
};

static void ocotp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = ocotp_realize;
    dc->vmsd = &vmstate_ocotp;
    rc->phases.hold = ocotp_reset_hold;
    dc->desc = "i.MX93 OCOTP fuse controller";
}

static const TypeInfo ocotp_types[] = {
    {
        .name = TYPE_IMX93_OCOTP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93OcotpState),
        .class_init = ocotp_class_init,
    },
};

DEFINE_TYPES(ocotp_types)
