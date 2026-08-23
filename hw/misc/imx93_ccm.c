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
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Root region: below GATE_BASE, blocks are ROOT_STRIDE apart. */
#define CCM_GATE_BASE       0x8000
#define CCM_ROOT_STRIDE     0x80
#define CCM_GATE_STRIDE     0x40

/* Register offsets within a root/gate block. */
#define CCM_STAT_OFFSET     0x04
#define CCM_AUTHEN_OFFSET   0x30

/* Root CONTROL fields (clk-composite-93): DIV[7:0], MUX[9:8]. */
#define CCM_CTRL_DIV_MASK   0xff
#define CCM_CTRL_MUX_SHIFT  8
#define CCM_CTRL_MUX_MASK   0x3

/* Audio clock roots we produce a real rate for (clk-imx93 root offsets). */
#define CCM_ROOT_PDM        0x2780
#define CCM_ROOT_SPDIF      0x2a80

/*
 * LPCG DIRECT registers that gate the audio functional clocks (RM): LPCG107 =
 * clk_enable_pdm_ch at 0x9AC0 feeds the MICFIL, LPCG112 = clk_enable_spdif_ch
 * at 0x9C00 feeds the XCVR. Both reset to 0x0000_0001 (gate ON). Clearing the
 * bit removes that block's clock, so the CCM folds it into the rate it hands the
 * consumer: gated off -> 0 Hz, and the consumer freezes rather than paces.
 */
#define CCM_LPCG_PDM        0x9ac0
#define CCM_LPCG_SPDIF      0x9c00

/*
 * LPCG (Low-Power Clock Gating) DIRECT registers for the TPMs. Per the i.MX93
 * RM the tpm functional clocks are gated by LPCG44..49 (LPCG44 = tpm1 ...
 * LPCG49 = tpm6, named clk_enable_tpmN_ch), whose DIRECT registers are at
 * LPCGn_DIRECT = 0x8000 + n*0x40, i.e. TPM(i) at 0x8B00 + i*0x40 for i in
 * 0..5. Each resets to 0x0000_0001 (gate ON out of reset). Bit 0 is the enable.
 * These are the same offsets the Linux clk-imx93 driver uses for the tpm gates.
 * Clearing a DIRECT bit removes that block's clock and freezes its counter.
 */
#define CCM_LPCG_TPM_BASE   0x8b00
#define CCM_NUM_TPM         6
#define CCM_LPCG_ON         0x1
/* Nominal tpm functional clock when the gate is open (matches the TPM model). */
#define CCM_TPM_HZ          24000000

/* DIRECT-register offset of TPM(i)'s LPCG (i in 0..CCM_NUM_TPM-1). */
static inline hwaddr ccm_tpm_lpcg_off(unsigned i)
{
    return CCM_LPCG_TPM_BASE + i * CCM_GATE_STRIDE;
}

/*
 * pdm_root and spdif_root select their source with AUDIO_SEL (clk-imx93
 * parent_names[AUDIO_SEL]): osc_24m, audio_pll, video_pll, clk_ext1. The audio
 * path uses osc_24m at idle and audio_pll when a stream runs; the driver never
 * routes PDM/SPDIF through video_pll or the external clock, so those two are
 * left 0 (an unmodelled source, which the downstream device treats as "no
 * clock" rather than a fabricated rate).
 *
 * osc_24m is the measured idle rate (guest clk_summary). audio_pll is 393.216
 * MHz - the standard i.MX93 audio PLL (48000 * 8192), confirmed on the wire by
 * the guest's clk_summary during capture (SOURCED, not fabricated).
 */
static const uint32_t ccm_audio_sel_hz[4] = {
    24000000,       /* osc_24m   */
    393216000,      /* audio_pll */
    0,              /* video_pll  - not on the audio path */
    0,              /* clk_ext1   - external, not present  */
};

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

/* Rate an AUDIO_SEL root produces from its CONTROL register: src / (DIV + 1). */
static uint64_t ccm_audio_root_hz(uint32_t control)
{
    uint32_t mux = (control >> CCM_CTRL_MUX_SHIFT) & CCM_CTRL_MUX_MASK;
    uint32_t div = control & CCM_CTRL_DIV_MASK;

    return ccm_audio_sel_hz[mux] / (div + 1);
}

static void imx93_ccm_update_audio_clock(IMX93CCMState *s, hwaddr control_off)
{
    Clock *clk;
    hwaddr gate_off;
    uint64_t hz;

    if (control_off == CCM_ROOT_PDM) {
        clk = s->pdm_root;
        gate_off = CCM_LPCG_PDM;
    } else if (control_off == CCM_ROOT_SPDIF) {
        clk = s->spdif_root;
        gate_off = CCM_LPCG_SPDIF;
    } else {
        return;
    }

    /* Fold the LPCG gate into the rate: gated off -> 0, so the consumer freezes
     * instead of pacing. Both the CONTROL (rate) and the DIRECT (gate) re-derive
     * this. */
    hz = (s->regs[gate_off / 4] & CCM_LPCG_ON) ?
         ccm_audio_root_hz(s->regs[control_off / 4]) : 0;
    clock_set_hz(clk, hz);
    clock_propagate(clk);
}

/* Drive TPM(i)'s module clock from its LPCG gate: 24 MHz when open, else 0. */
static void imx93_ccm_update_tpm_gate(IMX93CCMState *s, unsigned i)
{
    bool on = s->regs[ccm_tpm_lpcg_off(i) / 4] & CCM_LPCG_ON;

    clock_set_hz(s->tpm_clk[i], on ? CCM_TPM_HZ : 0);
    clock_propagate(s->tpm_clk[i]);
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

    /* A write to an audio root's CONTROL re-derives its output frequency. */
    if (offset == CCM_ROOT_PDM || offset == CCM_ROOT_SPDIF) {
        imx93_ccm_update_audio_clock(s, offset);
    }
    /* A write to an audio LPCG DIRECT gates/ungates that same output. */
    if (offset == CCM_LPCG_PDM) {
        imx93_ccm_update_audio_clock(s, CCM_ROOT_PDM);
    }
    if (offset == CCM_LPCG_SPDIF) {
        imx93_ccm_update_audio_clock(s, CCM_ROOT_SPDIF);
    }
    /* Clearing/setting a TPM's LPCG DIRECT gates/ungates its module clock. */
    if (offset >= ccm_tpm_lpcg_off(0) &&
        offset <= ccm_tpm_lpcg_off(CCM_NUM_TPM - 1) &&
        (offset & (CCM_GATE_STRIDE - 1)) == 0) {
        imx93_ccm_update_tpm_gate(s, (offset - CCM_LPCG_TPM_BASE) /
                                     CCM_GATE_STRIDE);
    }
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

    /* Audio LPCGs reset to 0x1 (RM: LPCG107/112_DIRECT): PDM/SPDIF come up
     * clocked, so the update below reads a gated-on rate, not 0. */
    s->regs[CCM_LPCG_PDM / 4] = CCM_LPCG_ON;
    s->regs[CCM_LPCG_SPDIF / 4] = CCM_LPCG_ON;

    /* CONTROL 0 -> mux osc_24m, DIV 0 -> the roots idle at 24 MHz (matches the
     * guest clk_summary before any audio stream reprograms them). */
    imx93_ccm_update_audio_clock(s, CCM_ROOT_PDM);
    imx93_ccm_update_audio_clock(s, CCM_ROOT_SPDIF);

    /* LPCG44..49_DIRECT reset to 0x1 (RM): every TPM comes up clocked. */
    for (unsigned i = 0; i < CCM_NUM_TPM; i++) {
        s->regs[ccm_tpm_lpcg_off(i) / 4] = CCM_LPCG_ON;
        imx93_ccm_update_tpm_gate(s, i);
    }
}

static void imx93_ccm_init(Object *obj)
{
    IMX93CCMState *s = IMX93_CCM(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_ccm_ops, s,
                          TYPE_IMX93_CCM, IMX93_CCM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    s->pdm_root = qdev_init_clock_out(DEVICE(obj), "pdm_root");
    s->spdif_root = qdev_init_clock_out(DEVICE(obj), "spdif_root");
    for (unsigned i = 0; i < CCM_NUM_TPM; i++) {
        g_autofree char *name = g_strdup_printf("tpm%u_clk", i + 1);
        s->tpm_clk[i] = qdev_init_clock_out(DEVICE(obj), name);
    }
}

/*
 * The audio root clocks (pdm_root, spdif_root) are derived from the CCM's
 * register state and propagated to their consumers (MICFIL, XCVR), which read
 * the rate live and do not migrate it themselves. The registers migrate but the
 * derived clock outputs do not, so without a post_load a destination would run
 * those consumers at the reset-era rate rather than the migrated one (e.g. a
 * gated-off audio clock would come back clocked). Re-derive the audio roots from
 * the loaded registers, exactly as reset does. The TPM clocks are deliberately
 * excluded: each TPM migrates its own input clock via VMSTATE_CLOCK, so
 * re-propagating here would needlessly re-run its ClockUpdate over the restored
 * counter.
 */
static int imx93_ccm_post_load(void *opaque, int version_id)
{
    IMX93CCMState *s = opaque;

    imx93_ccm_update_audio_clock(s, CCM_ROOT_PDM);
    imx93_ccm_update_audio_clock(s, CCM_ROOT_SPDIF);
    return 0;
}

static const VMStateDescription vmstate_imx93_ccm = {
    .name = TYPE_IMX93_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = imx93_ccm_post_load,
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
