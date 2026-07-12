/*
 * NXP i.MX 93 MEDIAMIX support blocks (blk-ctrl GPR + SRC power slice)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_media_blk.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* ---------------------------------------------------------------------- */
/* MEDIAMIX block control / GPR syscon: plain read/write register file.   */
/* ---------------------------------------------------------------------- */

static uint64_t media_blk_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93MediaBlkCtrlState *s = opaque;

    if ((offset >> 2) >= IMX93_MEDIA_BLK_CTRL_REGS) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void media_blk_ctrl_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    IMX93MediaBlkCtrlState *s = opaque;

    if ((offset >> 2) >= IMX93_MEDIA_BLK_CTRL_REGS) {
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps media_blk_ctrl_ops = {
    .read = media_blk_ctrl_read,
    .write = media_blk_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void media_blk_ctrl_reset(DeviceState *dev)
{
    IMX93MediaBlkCtrlState *s = IMX93_MEDIA_BLK_CTRL(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void media_blk_ctrl_realize(DeviceState *dev, Error **errp)
{
    IMX93MediaBlkCtrlState *s = IMX93_MEDIA_BLK_CTRL(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &media_blk_ctrl_ops, s,
                          TYPE_IMX93_MEDIA_BLK_CTRL, IMX93_MEDIA_BLK_CTRL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_media_blk_ctrl = {
    .name = TYPE_IMX93_MEDIA_BLK_CTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93MediaBlkCtrlState,
                             IMX93_MEDIA_BLK_CTRL_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void media_blk_ctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = media_blk_ctrl_realize;
    dc->vmsd = &vmstate_media_blk_ctrl;
    device_class_set_legacy_reset(dc, media_blk_ctrl_reset);
    dc->desc = "i.MX93 MEDIAMIX block control";
}

/* ---------------------------------------------------------------------- */
/* SRC MIX power-domain slice: only FUNC_STAT is polled by the pd driver.  */
/* ---------------------------------------------------------------------- */

#define SRC_SLICE_SW_CTRL_OFF       0x20
#define SRC_SLICE_FUNC_STAT_OFF     0xb4

#define FUNC_STAT_PSW_STAT          (1u << 0)   /* power switch on   */
#define FUNC_STAT_RST_STAT          (1u << 2)   /* reset asserted    */
#define FUNC_STAT_ISO_STAT          (1u << 4)   /* isolation active  */
#define FUNC_STAT_SSAR_STAT         (1u << 8)   /* save/restore busy */

static uint64_t src_slice_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93SrcSliceState *s = opaque;

    if (offset == SRC_SLICE_FUNC_STAT_OFF) {
        /* Powered on, not isolated, not in reset, not save/restoring. */
        return FUNC_STAT_PSW_STAT;
    }
    if ((offset >> 2) >= IMX93_SRC_SLICE_REGS) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void src_slice_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93SrcSliceState *s = opaque;

    if ((offset >> 2) >= IMX93_SRC_SLICE_REGS) {
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps src_slice_ops = {
    .read = src_slice_read,
    .write = src_slice_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void src_slice_reset(DeviceState *dev)
{
    IMX93SrcSliceState *s = IMX93_SRC_SLICE(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void src_slice_realize(DeviceState *dev, Error **errp)
{
    IMX93SrcSliceState *s = IMX93_SRC_SLICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &src_slice_ops, s,
                          TYPE_IMX93_SRC_SLICE, IMX93_SRC_SLICE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_src_slice = {
    .name = TYPE_IMX93_SRC_SLICE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93SrcSliceState, IMX93_SRC_SLICE_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void src_slice_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = src_slice_realize;
    dc->vmsd = &vmstate_src_slice;
    device_class_set_legacy_reset(dc, src_slice_reset);
    dc->desc = "i.MX93 SRC power-domain slice";
}

static const TypeInfo imx93_media_blk_types[] = {
    {
        .name = TYPE_IMX93_MEDIA_BLK_CTRL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93MediaBlkCtrlState),
        .class_init = media_blk_ctrl_class_init,
    },
    {
        .name = TYPE_IMX93_SRC_SLICE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93SrcSliceState),
        .class_init = src_slice_class_init,
    },
};

DEFINE_TYPES(imx93_media_blk_types)
