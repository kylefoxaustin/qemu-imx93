/*
 * NXP i.MX 93 MIPI DSI host (Synopsys DesignWare dw-mipi-dsi core)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register offsets/bits follow the upstream dw-mipi-dsi driver
 * (drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi.c).
 */

#include "qemu/osdep.h"
#include "hw/display/imx93_dsi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define DSI_GEN_PLD_DATA            0x70
#define DSI_CMD_PKT_STATUS          0x74
#define DSI_PHY_STATUS              0xb0
#define DSI_INT_ST0                 0xbc
#define DSI_INT_ST1                 0xc0

/* CMD_PKT_STATUS bits */
#define GEN_PLD_R_EMPTY             (1u << 4)
#define GEN_PLD_W_EMPTY             (1u << 2)
#define GEN_CMD_EMPTY               (1u << 0)

/* PHY_STATUS bits */
#define PHY_STOP_STATE_CLK_LANE     (1u << 2)
#define PHY_LOCK                    (1u << 0)

static uint64_t imx93_dsi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93DsiState *s = opaque;

    switch (offset) {
    case DSI_PHY_STATUS:
        /* PLL locked and clock lane in stop state: D-PHY is "up". */
        return PHY_LOCK | PHY_STOP_STATE_CLK_LANE;
    case DSI_CMD_PKT_STATUS:
        /* Command and payload write FIFOs always drained/ready. */
        return GEN_CMD_EMPTY | GEN_PLD_W_EMPTY;
    case DSI_INT_ST0:
    case DSI_INT_ST1:
        return 0;   /* no error/status interrupts */
    default:
        if ((offset >> 2) >= IMX93_DSI_NUM_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx93_dsi_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93DsiState *s = opaque;

    if ((offset >> 2) >= IMX93_DSI_NUM_REGS) {
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps imx93_dsi_ops = {
    .read = imx93_dsi_read,
    .write = imx93_dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_dsi_reset(DeviceState *dev)
{
    IMX93DsiState *s = IMX93_DSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx93_dsi_realize(DeviceState *dev, Error **errp)
{
    IMX93DsiState *s = IMX93_DSI(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx93_dsi_ops, s,
                          TYPE_IMX93_DSI, IMX93_DSI_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imx93_dsi = {
    .name = TYPE_IMX93_DSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93DsiState, IMX93_DSI_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_dsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx93_dsi_realize;
    dc->vmsd = &vmstate_imx93_dsi;
    device_class_set_legacy_reset(dc, imx93_dsi_reset);
    dc->desc = "i.MX93 MIPI DSI host";
}

static const TypeInfo imx93_dsi_types[] = {
    {
        .name = TYPE_IMX93_DSI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93DsiState),
        .class_init = imx93_dsi_class_init,
    },
};

DEFINE_TYPES(imx93_dsi_types)
