/*
 * NXP i.MX 93 ANATOP (Analog Top / PLL) module
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional register model. Register layout and required behaviour come
 * from the Linux clk-fracn-gppll driver: each fractional-N PLL occupies a
 * 0x100 block (ARM_PLL @ 0x1000, AUDIO_PLL @ 0x1200, VIDEO_PLL @ 0x1400),
 * with:
 *
 *     +0x00 PLL_CTRL        : POWERUP[0], CLKMUX_EN[1], CLKMUX_BYPASS[2]
 *     +0x40 PLL_NUMERATOR   : MFN[31:2]
 *     +0x50 PLL_DENOMINATOR : MFD[29:0]
 *     +0x60 PLL_DIV         : MFI[24:16], RDIV[15:13], ODIV[7:0]
 *     +0xF0 PLL_STATUS      : LOCK[0]  -- the driver polls for LOCK to set
 *                              after powerup, so the model pins it high
 *
 * All registers are read-what-you-write backing store except PLL_STATUS,
 * whose LOCK bit is forced set so clk_fracn_gppll_wait_lock() succeeds.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_anatop.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Fractional-N PLL blocks live at and above this offset, 0x100 apart. */
#define ANATOP_PLL_BASE     0x1000
#define ANATOP_PLL_STRIDE   0x100

/* Per-PLL register offsets. */
#define PLL_NUMERATOR_OFFSET 0x40
#define PLL_STATUS_OFFSET   0xF0
#define PLL_LOCK_STATUS     (1u << 0)
#define PLL_MFN_MASK        0xfffffffcu     /* MFN field, bits 31:2 */

static bool anatop_is_pll_status(hwaddr offset)
{
    return offset >= ANATOP_PLL_BASE &&
           (offset & (ANATOP_PLL_STRIDE - 1)) == PLL_STATUS_OFFSET;
}

static uint64_t imx93_anatop_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93AnatopState *s = opaque;

    if (anatop_is_pll_status(offset)) {
        /*
         * PLL is always locked in this model. Hardware also mirrors the
         * written PLL_NUMERATOR MFN (bits 31:2) into PLL_STATUS; the
         * clk-fracn-gppll driver reads it back to verify set_rate (and WARNs
         * on a mismatch), so reflect the same PLL block's numerator here.
         */
        hwaddr num = offset - PLL_STATUS_OFFSET + PLL_NUMERATOR_OFFSET;
        return PLL_LOCK_STATUS | (s->regs[num / 4] & PLL_MFN_MASK);
    }
    return s->regs[offset / 4];
}

static void imx93_anatop_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93AnatopState *s = opaque;

    /* PLL_STATUS is read-only. */
    if (anatop_is_pll_status(offset)) {
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx93_anatop_ops = {
    .read = imx93_anatop_read,
    .write = imx93_anatop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx93_anatop_reset_hold(Object *obj, ResetType type)
{
    IMX93AnatopState *s = IMX93_ANATOP(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx93_anatop_init(Object *obj)
{
    IMX93AnatopState *s = IMX93_ANATOP(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_anatop_ops, s,
                          TYPE_IMX93_ANATOP, IMX93_ANATOP_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_imx93_anatop = {
    .name = TYPE_IMX93_ANATOP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93AnatopState, IMX93_ANATOP_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_anatop_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "i.MX 93 ANATOP (PLLs)";
    rc->phases.hold = imx93_anatop_reset_hold;
    dc->vmsd = &vmstate_imx93_anatop;
}

static const TypeInfo imx93_anatop_types[] = {
    {
        .name           = TYPE_IMX93_ANATOP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93AnatopState),
        .instance_init  = imx93_anatop_init,
        .class_init     = imx93_anatop_class_init,
    },
};

DEFINE_TYPES(imx93_anatop_types)
