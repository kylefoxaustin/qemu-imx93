/*
 * NXP i.MX 93 Clock Control Module (CCM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional register model of the i.MX 9 CCM. The register layout and the
 * behaviours the model must satisfy are taken directly from the Linux
 * clk-imx93 / clk-composite-93 / clk-gate-93 drivers:
 *
 *   Clock-root region (0x0000..0x7FFF, one 0x80-spaced block per root):
 *     +0x00 CONTROL : DIV[7:0], MUX[9:8], OFF[24]   (read/write)
 *     +0x04 STATUS  : BUSY[28]   -- driver polls for !BUSY (500us); the
 *                      model reports idle (0) so the poll succeeds at once
 *     +0x30 AUTHEN  : TZ_NS[9], WHITELIST[16+domain] -- if these are not
 *                      set the driver skips the clock, so the model returns
 *                      a permissive constant
 *
 *   LPCG gate region (0x8000..0xFFFF, one 0x40-spaced block per gate):
 *     +0x00 DIRECT  : gate enable bits             (read/write)
 *     +0x30 AUTHEN  : same permissive semantics as above
 *
 * Everything else is plain read-what-you-write backing store. No actual
 * clock frequencies are produced; the Linux clk framework only needs the
 * registers to read back consistently and the status/authen gates to pass.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_ccm.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Root region: below GATE_BASE, blocks are ROOT_STRIDE apart. */
#define CCM_GATE_BASE       0x8000
#define CCM_ROOT_STRIDE     0x80
#define CCM_GATE_STRIDE     0x40

/* Register offsets within a root/gate block. */
#define CCM_STAT_OFFSET     0x04
#define CCM_AUTHEN_OFFSET   0x30

/* STATUS.BUSY (root region, +0x04). */
#define CCM_BUSY_SHIFT      28

/*
 * AUTHEN constant returned for every root/gate block: TrustZone non-secure
 * access allowed (TZ_NS, bit 9) and every domain whitelisted (bits 31:16).
 */
#define CCM_TZ_NS           (1u << 9)
#define CCM_WHITELIST_ALL   (0xffffu << 16)
#define CCM_AUTHEN_DEFAULT  (CCM_TZ_NS | CCM_WHITELIST_ALL)

static bool ccm_is_status(hwaddr offset)
{
    return offset < CCM_GATE_BASE &&
           (offset & (CCM_ROOT_STRIDE - 1)) == CCM_STAT_OFFSET;
}

static bool ccm_is_authen(hwaddr offset)
{
    uint32_t stride = offset < CCM_GATE_BASE ?
                      CCM_ROOT_STRIDE : CCM_GATE_STRIDE;
    return (offset & (stride - 1)) == CCM_AUTHEN_OFFSET;
}

static uint64_t imx93_ccm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93CCMState *s = opaque;

    if (ccm_is_status(offset)) {
        /* Clock change always complete: BUSY clear. */
        return 0;
    }
    if (ccm_is_authen(offset)) {
        return CCM_AUTHEN_DEFAULT;
    }
    return s->regs[offset / 4];
}

static void imx93_ccm_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93CCMState *s = opaque;

    /* STATUS and AUTHEN are read-only in this model. */
    if (ccm_is_status(offset) || ccm_is_authen(offset)) {
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx93_ccm_ops = {
    .read = imx93_ccm_read,
    .write = imx93_ccm_write,
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

static void imx93_ccm_reset_hold(Object *obj, ResetType type)
{
    IMX93CCMState *s = IMX93_CCM(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx93_ccm_init(Object *obj)
{
    IMX93CCMState *s = IMX93_CCM(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_ccm_ops, s,
                          TYPE_IMX93_CCM, IMX93_CCM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_imx93_ccm = {
    .name = TYPE_IMX93_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93CCMState, IMX93_CCM_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_ccm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "i.MX 93 Clock Control Module";
    rc->phases.hold = imx93_ccm_reset_hold;
    dc->vmsd = &vmstate_imx93_ccm;
}

static const TypeInfo imx93_ccm_types[] = {
    {
        .name           = TYPE_IMX93_CCM,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93CCMState),
        .instance_init  = imx93_ccm_init,
        .class_init     = imx93_ccm_class_init,
    },
};

DEFINE_TYPES(imx93_ccm_types)
