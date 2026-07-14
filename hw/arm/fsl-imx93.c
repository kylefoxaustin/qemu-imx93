/*
 * NXP i.MX 93 SoC Implementation - v0.0.1 scaffold
 *
 * Modeled on hw/arm/fsl-imx8mp.c (Bernhard Beschow) and the i.MX 95 port.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.1 scope:
 *   - 2x Cortex-A55 cluster instantiated
 *   - GICv3 wired to both cores including timer PPIs
 *   - DDR mapped at 0x8000_0000, OCRAM at 0x2048_0000
 *   - All non-CPU/GIC peripherals are create_unimplemented_device() stubs
 *     so accesses log instead of faulting
 *   - No LPUART model yet (next step in v0.0.2)
 *
 * Addresses are taken from imx93.dtsi and the i.MX 93 RM. Unlike i.MX 95,
 * the i.MX 93 has no System Manager, so CCM/ANATOP/IOMUXC/SRC are stubbed
 * here only as a starting point - they must be modeled functionally before
 * Linux clock/pinmux bring-up will succeed.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fsl-imx93.h"
#include "hw/core/boards.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/qdev-clock.h"
#include "qemu/main-loop.h"
#include "hw/display/adv7535.h"
#include "hw/display/i2c-ddc.h"
#include "hw/audio/wm8962.h"
#include "hw/i2c/i2c.h"
#include "system/kvm.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/kvm_arm.h"
#include "qapi/error.h"
#include "qobject/qlist.h"

/*
 * Single source of truth for the SoC memory map. Each entry maps a region
 * ID (from enum FslImx93MemoryRegions) to its physical base address, size,
 * and a debug name. Bases are confirmed against imx93.dtsi.
 */
static const struct {
    hwaddr      addr;
    size_t      size;
    const char *name;
} fsl_imx93_memmap[FSL_IMX93_NUM_REGIONS] = {
    [FSL_IMX93_RAM] = { FSL_IMX93_RAM_START, FSL_IMX93_RAM_SIZE_MAX, "ram" },

    /* GICv3: distributor + redistributor (imx93.dtsi gic@48000000). */
    [FSL_IMX93_GIC_DIST] = { 0x48000000, 64 * KiB, "gic_dist" },
    [FSL_IMX93_GIC_REDIST] = { 0x48040000, 768 * KiB, "gic_redist" },

    /* On-chip RAM (OCRAM). */
    [FSL_IMX93_OCRAM] = { 0x20480000, 512 * KiB, "ocram" },

    /* LPUART console block. lpuart1/2 in AONMIX, lpuart3 in WAKEUPMIX. */
    [FSL_IMX93_LPUART1] = { 0x44380000, 64 * KiB, "lpuart1" },
    [FSL_IMX93_LPUART2] = { 0x44390000, 64 * KiB, "lpuart2" },
    [FSL_IMX93_LPUART3] = { 0x42570000, 64 * KiB, "lpuart3" },
    [FSL_IMX93_LPUART4] = { 0x42580000, 64 * KiB, "lpuart4" },
    [FSL_IMX93_LPUART5] = { 0x42590000, 64 * KiB, "lpuart5" },
    [FSL_IMX93_LPUART6] = { 0x425a0000, 64 * KiB, "lpuart6" },
    [FSL_IMX93_LPUART7] = { 0x42690000, 64 * KiB, "lpuart7" },
    [FSL_IMX93_LPUART8] = { 0x426a0000, 64 * KiB, "lpuart8" },

    /* Clock / reset / pinmux (direct register programming - no SM). */
    [FSL_IMX93_CCM] = { 0x44450000, 64 * KiB, "ccm" },
    [FSL_IMX93_ANATOP] = { 0x44480000, 8 * KiB, "anatop" },
    [FSL_IMX93_IOMUXC] = { 0x443c0000, 64 * KiB, "iomuxc" },
    [FSL_IMX93_SRC] = { 0x44460000, 64 * KiB, "src" },

    /* BLK_CTRL / syscfg aggregates per power domain. */
    [FSL_IMX93_BLK_CTRL_AONMIX] = { 0x44210000, 4 * KiB, "aonmix-blk-ctrl" },
    [FSL_IMX93_BLK_CTRL_WAKEUPMIX] = { 0x42420000, 4 * KiB, "wakeupmix-blk" },
    [FSL_IMX93_BLK_CTRL_DDRMIX] = { 0x4e010000, 64 * KiB, "ddrmix-blk-ctrl" },
    [FSL_IMX93_DDRC] = { 0x4e300000, 8 * KiB, "ddrc" },

    /* Ethernet: FEC (real imx.enet) + eQOS dwmac (stub). */
    [FSL_IMX93_FEC] = { 0x42890000, 64 * KiB, "fec" },
    [FSL_IMX93_EQOS] = { 0x428a0000, 64 * KiB, "eqos" },

    /* eDMA controllers (edma1 AONMIX, edma2 WAKEUPMIX). */
    [FSL_IMX93_EDMA1] = { 0x44000000, 0x200000, "edma1" },
    [FSL_IMX93_EDMA2] = { 0x42000000, 0x210000, "edma2" },

    /* Cortex-M33 remoteproc resource table region (M33 SRAM). */
    [FSL_IMX93_RSC_TABLE] = { 0x2021e000, 4 * KiB, "m33_rsc_table" },

    /* OCOTP / efuse syscon (FEC MAC-address nvmem cells live here). */
    [FSL_IMX93_OCOTP] = { 0x47510000, 64 * KiB, "ocotp" },

    /* Messaging Units (AONMIX MU1, WAKEUPMIX MU2, ELE/Sentinel S4 MU). */
    [FSL_IMX93_MU1] = { 0x44230000, 64 * KiB, "mu1" },
    [FSL_IMX93_MU2] = { 0x42440000, 64 * KiB, "mu2" },
    [FSL_IMX93_ELE_MU] = { 0x47520000, 64 * KiB, "ele_mu_s4" },

    /* System counter. */
    [FSL_IMX93_SYSCTR] = { 0x44290000, 192 * KiB, "sysctr" },

    /* Watchdogs (wdog1/2 AONMIX, wdog3/4/5 WAKEUPMIX). */
    [FSL_IMX93_WDOG1] = { 0x442d0000, 64 * KiB, "wdog1" },
    [FSL_IMX93_WDOG2] = { 0x442e0000, 64 * KiB, "wdog2" },
    [FSL_IMX93_WDOG3] = { 0x42490000, 64 * KiB, "wdog3" },
    [FSL_IMX93_WDOG4] = { 0x424a0000, 64 * KiB, "wdog4" },
    [FSL_IMX93_WDOG5] = { 0x424b0000, 64 * KiB, "wdog5" },

    /* Trusted Resource Domain Controller. */
    [FSL_IMX93_TRDC] = { 0x44270000, 64 * KiB, "trdc" },

    /* Battery-Backed Non-Secure Module (RTC + power key). */
    [FSL_IMX93_BBNSM] = { 0x44440000, 64 * KiB, "bbnsm" },

    /* Thermal Management Unit. */
    [FSL_IMX93_TMU] = { 0x44482000, 4 * KiB, "tmu" },

    /* ADC. */
    [FSL_IMX93_ADC1] = { 0x44530000, 64 * KiB, "adc1" },

    /* uSDHC controllers (eMMC / SD / SDIO). */
    [FSL_IMX93_USDHC1] = { 0x42850000, 64 * KiB, "usdhc1" },
    [FSL_IMX93_USDHC2] = { 0x42860000, 64 * KiB, "usdhc2" },
    [FSL_IMX93_USDHC3] = { 0x428b0000, 64 * KiB, "usdhc3" },

    /* Low-speed I/O controllers (stubbed). */
    [FSL_IMX93_TPM1] = { 0x44310000, 64 * KiB, "tpm1" },
    [FSL_IMX93_TPM2] = { 0x44320000, 64 * KiB, "tpm2" },
    [FSL_IMX93_TPM3] = { 0x424e0000, 64 * KiB, "tpm3" },
    [FSL_IMX93_TPM4] = { 0x424f0000, 64 * KiB, "tpm4" },
    [FSL_IMX93_TPM5] = { 0x42500000, 64 * KiB, "tpm5" },
    [FSL_IMX93_TPM6] = { 0x42510000, 64 * KiB, "tpm6" },
    [FSL_IMX93_I3C1] = { 0x44330000, 64 * KiB, "i3c1" },
    [FSL_IMX93_I3C2] = { 0x42520000, 64 * KiB, "i3c2" },
    [FSL_IMX93_LPI2C1] = { 0x44340000, 64 * KiB, "lpi2c1" },
    [FSL_IMX93_LPI2C2] = { 0x44350000, 64 * KiB, "lpi2c2" },
    [FSL_IMX93_LPI2C3] = { 0x42530000, 64 * KiB, "lpi2c3" },
    [FSL_IMX93_LPI2C4] = { 0x42540000, 64 * KiB, "lpi2c4" },
    [FSL_IMX93_LPI2C5] = { 0x426b0000, 64 * KiB, "lpi2c5" },
    [FSL_IMX93_LPI2C6] = { 0x426c0000, 64 * KiB, "lpi2c6" },
    [FSL_IMX93_LPI2C7] = { 0x426d0000, 64 * KiB, "lpi2c7" },
    [FSL_IMX93_LPI2C8] = { 0x426e0000, 64 * KiB, "lpi2c8" },
    [FSL_IMX93_LPSPI1] = { 0x44360000, 64 * KiB, "lpspi1" },
    [FSL_IMX93_LPSPI2] = { 0x44370000, 64 * KiB, "lpspi2" },
    [FSL_IMX93_LPSPI3] = { 0x42550000, 64 * KiB, "lpspi3" },
    [FSL_IMX93_LPSPI4] = { 0x42560000, 64 * KiB, "lpspi4" },
    [FSL_IMX93_LPSPI5] = { 0x426f0000, 64 * KiB, "lpspi5" },
    [FSL_IMX93_LPSPI6] = { 0x42700000, 64 * KiB, "lpspi6" },
    [FSL_IMX93_LPSPI7] = { 0x42710000, 64 * KiB, "lpspi7" },
    [FSL_IMX93_LPSPI8] = { 0x42720000, 64 * KiB, "lpspi8" },
    [FSL_IMX93_FLEXCAN1] = { 0x443a0000, 64 * KiB, "flexcan1" },
    [FSL_IMX93_FLEXCAN2] = { 0x425b0000, 64 * KiB, "flexcan2" },
    [FSL_IMX93_SAI1] = { 0x443b0000, 64 * KiB, "sai1" },
    [FSL_IMX93_SAI2] = { 0x42650000, 64 * KiB, "sai2" },
    [FSL_IMX93_SAI3] = { 0x42660000, 64 * KiB, "sai3" },
    [FSL_IMX93_MICFIL] = { 0x44520000, 64 * KiB, "micfil" },
    [FSL_IMX93_FLEXSPI1] = { 0x425e0000, 64 * KiB, "flexspi1" },
    [FSL_IMX93_XCVR] = { 0x42680000, 64 * KiB, "xcvr" },
    [FSL_IMX93_GPIO1] = { 0x47400000, 64 * KiB, "gpio1" },
    [FSL_IMX93_GPIO2] = { 0x43810000, 64 * KiB, "gpio2" },
    [FSL_IMX93_GPIO3] = { 0x43820000, 64 * KiB, "gpio3" },
    [FSL_IMX93_GPIO4] = { 0x43830000, 64 * KiB, "gpio4" },
    [FSL_IMX93_USBOTG1] = { 0x4c100000, 64 * KiB, "usbotg1" },
    [FSL_IMX93_USBOTG2] = { 0x4c200000, 64 * KiB, "usbotg2" },

    /* MEDIAMIX: block control + imaging cluster (csi/dsi/pxp/lcdif/isi). */
    [FSL_IMX93_MEDIAMIX_PD] = { 0x44462400, 0x400, "mediamix-pd" },
    [FSL_IMX93_MEDIA_BLK_CTRL] = { 0x4ac10000, 4 * KiB, "media_blk_ctrl" },
    [FSL_IMX93_TSTMR1] = { 0x442c0000, 64 * KiB, "tstmr1" },
    [FSL_IMX93_TSTMR2] = { 0x42480000, 64 * KiB, "tstmr2" },
    [FSL_IMX93_SEMA42_1] = { 0x44260000, 64 * KiB, "sema42-1" },
    [FSL_IMX93_SEMA42_2] = { 0x42450000, 64 * KiB, "sema42-2" },
    [FSL_IMX93_FLEXIO1] = { 0x425c0000, 64 * KiB, "flexio1" },
    [FSL_IMX93_FLEXIO2] = { 0x425d0000, 64 * KiB, "flexio2" },
    [FSL_IMX93_MIPI_CSI] = { 0x4ae00000, 64 * KiB, "mipi_csi" },
    [FSL_IMX93_DSI] = { 0x4ae10000, 64 * KiB, "dsi" },
    [FSL_IMX93_PXP] = { 0x4ae20000, 64 * KiB, "pxp" },
    [FSL_IMX93_LCDIF] = { 0x4ae30000, 64 * KiB, "lcdif" },
    [FSL_IMX93_ISI] = { 0x4ae40000, 64 * KiB, "isi" },
};

/*
 * For every peripheral region we don't yet model, install a stub that
 * traces accesses but doesn't fault. This lets U-Boot / Linux probe
 * registers safely while we iterate.
 */
static void fsl_imx93_install_unimplemented(FslImx93State *s)
{
    static const int unimplemented_regions[] = {
        FSL_IMX93_IOMUXC, FSL_IMX93_SRC,
        FSL_IMX93_BLK_CTRL_AONMIX, FSL_IMX93_BLK_CTRL_WAKEUPMIX,
        FSL_IMX93_BLK_CTRL_DDRMIX,
        FSL_IMX93_DDRC,
        FSL_IMX93_TRDC,
        FSL_IMX93_MIPI_CSI,
        FSL_IMX93_I3C2,
        FSL_IMX93_FLEXIO2,
    };

    for (size_t i = 0; i < ARRAY_SIZE(unimplemented_regions); i++) {
        int r = unimplemented_regions[i];
        create_unimplemented_device(fsl_imx93_memmap[r].name,
                                    fsl_imx93_memmap[r].addr,
                                    fsl_imx93_memmap[r].size);
    }
}

/*
 * Start/stop the Cortex-M33, keeping its PSCI power_state and halt_reason
 * consistent with cs->halted. The boot BH and the SiP RPROC SMC drive the M33
 * lifecycle directly (not via the PSCI powerctl path), so we maintain these
 * fields ourselves: arm_cpu_has_work() asserts that a PSCI_OFF CPU is halted
 * for PSCI (halt_reason == HALT_PSCI). Leaving a running M33 at PSCI_OFF was
 * harmless before the WFI/WFE halt_reason rework but trips that assert after it
 * (a running core that executes WFI gets halt_reason = HALT_WFI); before the
 * rework the same inconsistency instead made WFI a busy no-op, spinning a host
 * core forever whenever the M33 idled.
 */
static void fsl_imx93_set_cpu_run(CPUState *cs, bool run)
{
    ARMCPU *cpu = ARM_CPU(cs);

    cs->halted = !run;
    cpu->power_state = run ? PSCI_ON : PSCI_OFF;
    cpu->env.halt_reason = run ? NOT_HALTED : HALT_PSCI;
}

/*
 * Release the Cortex-M33 only once firmware has actually been staged into its
 * ITCM. An M-profile reset vector starts with the initial stack pointer, so a
 * non-zero first ITCM word means a vector table is present (loaded via
 * "-device loader,...,cpu-num=2", U-Boot, or the guest's remoteproc into the
 * A55-view ITCM alias). A zeroed ITCM (a plain Linux boot loads no M33 image)
 * leaves the M33 powered off, so the A55 boot is undisturbed. Run at
 * machine-done, by when the -device loader ROM blobs have been committed.
 */
static void fsl_imx93_m33_start_bh(void *opaque)
{
    FslImx93State *s = opaque;
    const uint8_t *itcm = memory_region_get_ram_ptr(&s->m33_itcm);
    /* The firmware's vector table sits at the reset VTOR (ITCM + FW_OFFSET). */
    uint32_t initial_sp = ldl_le_p(itcm + FSL_IMX93_M33_FW_OFFSET);

    if (initial_sp != 0 && s->m33.cpu) {
        CPUState *cs = CPU(s->m33.cpu);

        fsl_imx93_set_cpu_run(cs, true);
        cpu_resume(cs);
        s->m33_started = true;
    }
}

/*
 * SiP RPROC START: Linux's remoteproc has loaded M33 firmware into the ITCM
 * (via the A55-view alias) and asked the "secure firmware" to release the core.
 * Reset it so it reloads SP/PC from the freshly-staged vector table, then run.
 */
static void fsl_imx93_m33_rproc_start_bh(void *opaque)
{
    FslImx93State *s = opaque;
    CPUState *cs = CPU(s->m33.cpu);

    cpu_reset(cs);
    fsl_imx93_set_cpu_run(cs, true);
    cpu_resume(cs);
}

static void fsl_imx93_m33_rproc_stop_bh(void *opaque)
{
    FslImx93State *s = opaque;
    CPUState *cs = CPU(s->m33.cpu);

    fsl_imx93_set_cpu_run(cs, false);
    cpu_reset(cs);
}

/* The single SoC instance, for the (global) SiP SMC handler. */
static FslImx93State *fsl_imx93_sip_soc;

/* i.MX SiP RPROC: Linux remoteproc boots/stops the M33 (IMX_RPROC_SMC). */
#define IMX_SIP_RPROC           0xc2000005
#define IMX_SIP_RPROC_START     0x00
#define IMX_SIP_RPROC_STARTED   0x01
#define IMX_SIP_RPROC_STOP      0x02

#define IMX_SIP_GET_SOC_INFO    0xc2000006
/* a1[31:16]=revision (major=[31:28]-9, minor=[27:24]), a1[15:0]=soc id<<8. */
#define IMX93_SOC_INFO_A1       0xa0009300   /* i.MX93, rev 1.0 */

static bool fsl_imx93_sip_handler(uint64_t fid, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t ret[4])
{
    FslImx93State *s = fsl_imx93_sip_soc;

    if (fid == IMX_SIP_GET_SOC_INFO) {
        ret[0] = 0;                          /* SMCCC_RET_SUCCESS */
        ret[1] = IMX93_SOC_INFO_A1;
        ret[2] = 0x00049f9300000000ULL;      /* uid[127:64] */
        ret[3] = 0x0000000000000001ULL;      /* uid[63:0] */
        return true;
    }

    if (fid != IMX_SIP_RPROC || !s || !s->m33.cpu) {
        return false;
    }

    switch (a1) {
    case IMX_SIP_RPROC_STARTED:
        /* 0 keeps imx_rproc in "offline" so it boots the M33 on demand. */
        ret[0] = s->m33_started ? 1 : 0;
        return true;
    case IMX_SIP_RPROC_START:
        s->m33_started = true;
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                fsl_imx93_m33_rproc_start_bh, s);
        ret[0] = 0;
        return true;
    case IMX_SIP_RPROC_STOP:
        s->m33_started = false;
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                fsl_imx93_m33_rproc_stop_bh, s);
        ret[0] = 0;
        return true;
    default:
        ret[0] = 0;
        return true;
    }
}

static void fsl_imx93_machine_done(Notifier *notifier, void *data)
{
    FslImx93State *s = container_of(notifier, FslImx93State, m33_machine_done);

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            fsl_imx93_m33_start_bh, s);
}

static void fsl_imx93_realize(DeviceState *dev, Error **errp)
{
    /* The WM8962 is created with LPI2C1, but wired to SAI3 after SAI realize. */
    DeviceState *wm8962 = NULL;
    MachineState *ms = MACHINE(qdev_get_machine());
    FslImx93State *s = FSL_IMX93(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    const char *cpu_type = ms->cpu_type ?: ARM_CPU_TYPE_NAME("cortex-a55");
    /*
     * The A55 cluster is a fixed size. The Cortex-M33 is an additional,
     * always-present vCPU that is not part of the cluster (it has its own
     * NVIC, not the GIC), so the A55 wiring below is sized by n_a55, not
     * ms->smp.cpus. TCG sizes its per-CPU context table from smp.max_cpus,
     * which includes the M33, so the machine's default/max cpus is n_a55 + 1
     * and -smp must match.
     */
    const unsigned n_a55 = FSL_IMX93_NUM_A55_CPUS;
    int i;

    if (ms->smp.cpus != n_a55 + FSL_IMX93_NUM_M33) {
        error_setg(errp,
                   "%s: fixed topology is %u A55 + %d M33; run with -smp %u "
                   "(the default) - %d requested",
                   TYPE_FSL_IMX93, n_a55, FSL_IMX93_NUM_M33,
                   n_a55 + FSL_IMX93_NUM_M33, (int)ms->smp.cpus);
        return;
    }

    /* Instantiate the A55 cluster. */
    for (i = 0; i < n_a55; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(OBJECT(dev), name, &s->cpu[i], cpu_type);
    }

    for (i = 0; i < n_a55; i++) {
        if (n_a55 > 1 &&
            object_property_find(OBJECT(&s->cpu[i]), "reset-cbar")) {
            object_property_set_int(OBJECT(&s->cpu[i]), "reset-cbar",
                                    fsl_imx93_memmap[FSL_IMX93_GIC_DIST].addr,
                                    &error_abort);
        }

        /*
         * MPIDR affinity must match the stock DT: imx93.dtsi places the two
         * A55s at cpu@0 (Aff1.Aff0 = 0.0) and cpu@100 (Aff1.Aff0 = 1.0), i.e.
         * each core in its own affinity-level-1 group. QEMU otherwise numbers
         * secondaries in Aff0 (0x0, 0x1), so a PSCI CPU_ON targeting 0x100
         * would find no matching CPU and fail with -EINVAL (the
         * "psci: failed to boot CPU1 (-22)" seen on first bring-up).
         */
        object_property_set_int(OBJECT(&s->cpu[i]), "mp-affinity",
                                (uint64_t)i << 8, &error_abort);

        /* i.MX 93 system counter runs at 24 MHz. */
        object_property_set_int(OBJECT(&s->cpu[i]), "cntfrq", 24000000,
                                &error_abort);

        if (object_property_find(OBJECT(&s->cpu[i]), "has_el2")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2",
                                     !kvm_enabled(), &error_abort);
        }
        if (object_property_find(OBJECT(&s->cpu[i]), "has_el3")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3",
                                     !kvm_enabled(), &error_abort);
        }

        if (i) {
            /* Secondary CPUs come up via PSCI / SRC. */
            object_property_set_bool(OBJECT(&s->cpu[i]), "start-powered-off",
                                     true, &error_abort);
        }

        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, errp)) {
            return;
        }
    }

    /* GICv3 */
    {
        SysBusDevice *gicsbd = SYS_BUS_DEVICE(&s->gic);
        QList *redist_region_count;

        qdev_prop_set_uint32(gicdev, "num-cpu", n_a55);
        qdev_prop_set_uint32(gicdev, "num-irq",
                             FSL_IMX93_NUM_IRQS + GIC_INTERNAL);

        redist_region_count = qlist_new();
        qlist_append_int(redist_region_count, n_a55);
        qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);

        object_property_set_link(OBJECT(&s->gic), "sysmem",
                                 OBJECT(get_system_memory()), &error_fatal);
        if (!sysbus_realize(gicsbd, errp)) {
            return;
        }
        sysbus_mmio_map(gicsbd, 0, fsl_imx93_memmap[FSL_IMX93_GIC_DIST].addr);
        sysbus_mmio_map(gicsbd, 1, fsl_imx93_memmap[FSL_IMX93_GIC_REDIST].addr);

        /* Wire the per-CPU timer PPIs + IRQ/FIQ lines (same as 8MP). */
        for (i = 0; i < n_a55; i++) {
            DeviceState *cpudev = DEVICE(&s->cpu[i]);
            int intidbase = FSL_IMX93_NUM_IRQS + i * GIC_INTERNAL;
            qemu_irq irq;

            static const int timer_irqs[] = {
                [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
                [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
                [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
                [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
            };
            for (size_t j = 0; j < ARRAY_SIZE(timer_irqs); j++) {
                irq = qdev_get_gpio_in(gicdev, intidbase + timer_irqs[j]);
                qdev_connect_gpio_out(cpudev, j, irq);
            }

            sysbus_connect_irq(gicsbd, i,
                qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
            sysbus_connect_irq(gicsbd, i + n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
            sysbus_connect_irq(gicsbd, i + 2 * n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
            sysbus_connect_irq(gicsbd, i + 3 * n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        }
    }

    /*
     * Cortex-M33 real-time core. Build its private address space: a
     * low-priority alias of the A55 system memory (so the M33 sees every
     * peripheral and DRAM) with the private ITCM/DTCM layered on top at the
     * M33's view addresses. The same TCM RAM is also aliased into the A55
     * system view so firmware can be staged from the A55 side. The core is
     * held in reset until firmware is present (see fsl_imx93_m33_start_bh).
     */
    {
        DeviceState *m33 = DEVICE(&s->m33);

        memory_region_init(&s->m33_view, OBJECT(s), "imx93-m33-view", 4 * GiB);
        memory_region_init_alias(&s->m33_sysmem_alias, OBJECT(s),
                                 "imx93-m33-sysmem", get_system_memory(),
                                 0, 4 * GiB);
        memory_region_add_subregion_overlap(&s->m33_view, 0,
                                            &s->m33_sysmem_alias, -1);
        /*
         * The M33 runs secure and reaches the SoC peripherals at their secure
         * aliases (0x5xxxxxxx = 0x4xxxxxxx | 0x10000000) - e.g. the firmware's
         * IOMUXC pinmux writes go to 0x543c0000. Mirror the 0x4xxxxxxx
         * peripheral region into the 0x5xxxxxxx secure window.
         */
        memory_region_init_alias(&s->m33_secure_periph, OBJECT(s),
                                 "imx93-m33-secure-periph", get_system_memory(),
                                 0x40000000, 0x10000000);
        memory_region_add_subregion_overlap(&s->m33_view, 0x50000000,
                                            &s->m33_secure_periph, 0);

        /* ITCM (code): secure view backs the RAM; NS + sys are aliases. */
        memory_region_init_ram(&s->m33_itcm, OBJECT(s), "imx93-m33-itcm",
                               FSL_IMX93_M33_TCM_SIZE, &error_fatal);
        memory_region_add_subregion(&s->m33_view, FSL_IMX93_M33_ITCM_MVIEW_S,
                                    &s->m33_itcm);
        memory_region_init_alias(&s->m33_itcm_alias_ns, OBJECT(s),
                                 "imx93-m33-itcm-ns", &s->m33_itcm, 0,
                                 FSL_IMX93_M33_TCM_SIZE);
        memory_region_add_subregion(&s->m33_view, FSL_IMX93_M33_ITCM_MVIEW_NS,
                                    &s->m33_itcm_alias_ns);
        memory_region_init_alias(&s->m33_itcm_sysview, OBJECT(s),
                                 "imx93-m33-itcm-sys", &s->m33_itcm, 0,
                                 FSL_IMX93_M33_TCM_SIZE);
        memory_region_add_subregion(get_system_memory(),
                                    FSL_IMX93_M33_ITCM_SYSVIEW,
                                    &s->m33_itcm_sysview);

        /* DTCM (data): NS view backs the RAM; secure + sys are aliases. */
        memory_region_init_ram(&s->m33_dtcm, OBJECT(s), "imx93-m33-dtcm",
                               FSL_IMX93_M33_TCM_SIZE, &error_fatal);
        memory_region_add_subregion(&s->m33_view, FSL_IMX93_M33_DTCM_MVIEW_NS,
                                    &s->m33_dtcm);
        memory_region_init_alias(&s->m33_dtcm_alias_s, OBJECT(s),
                                 "imx93-m33-dtcm-s", &s->m33_dtcm, 0,
                                 FSL_IMX93_M33_TCM_SIZE);
        memory_region_add_subregion(&s->m33_view, FSL_IMX93_M33_DTCM_MVIEW_S,
                                    &s->m33_dtcm_alias_s);
        memory_region_init_alias(&s->m33_dtcm_sysview, OBJECT(s),
                                 "imx93-m33-dtcm-sys", &s->m33_dtcm, 0,
                                 FSL_IMX93_M33_TCM_SIZE);
        memory_region_add_subregion(get_system_memory(),
                                    FSL_IMX93_M33_DTCM_SYSVIEW,
                                    &s->m33_dtcm_sysview);

        s->m33_cpuclk = clock_new(OBJECT(s), "m33-cpuclk");
        clock_set_hz(s->m33_cpuclk, FSL_IMX93_M33_CLK_HZ);

        qdev_prop_set_string(m33, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
        qdev_prop_set_uint32(m33, "num-irq", FSL_IMX93_M33_NUM_IRQ);
        qdev_prop_set_uint32(m33, "init-svtor", FSL_IMX93_M33_SVTOR);
        qdev_prop_set_bit(m33, "start-powered-off", true);
        qdev_connect_clock_in(m33, "cpuclk", s->m33_cpuclk);
        object_property_set_link(OBJECT(&s->m33), "memory",
                                 OBJECT(&s->m33_view), &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->m33), errp)) {
            return;
        }

        s->m33_machine_done.notify = fsl_imx93_machine_done;
        qemu_add_machine_init_done_notifier(&s->m33_machine_done);

        /* Service the i.MX SiP RPROC SMCs so Linux boots the M33 on demand. */
        fsl_imx93_sip_soc = s;
        arm_register_sip_handler(fsl_imx93_sip_handler);
    }

    /*
     * MU1: the A55<->M33 messaging unit (the cm33 remoteproc's tx/rx/rxdb
     * mailbox). The A55-side endpoint is mapped here with its GIC line; the
     * M33-side peer endpoint (for the RPMsg doorbell relay) is added once the
     * M33 firmware path is wired.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->mu1);

        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[FSL_IMX93_MU1].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX93_MU1_IRQ));

        /*
         * MU1_MUA is the M33's side of the same MU. Peer-link it to MUB so a
         * TR write on one side lands in the other's RR and rings its doorbell:
         * the A55's rproc kick reaches the M33's MU IRQ (NVIC 21, per the NXP
         * firmware), and the M33's RPMsg name-service announcement reaches the
         * A55's MU1 GIC line. Realized after the M33 so its NVIC inputs exist.
         */
        SysBusDevice *sbd_a = SYS_BUS_DEVICE(&s->mu1_a);

        if (!sysbus_realize(sbd_a, errp)) {
            return;
        }
        sysbus_mmio_map(sbd_a, 0, FSL_IMX93_MU1_MUA_ADDR);
        sysbus_connect_irq(sbd_a, 0,
            qdev_get_gpio_in(DEVICE(&s->m33), FSL_IMX93_M33_MU_IRQ));
        imx_mu_set_peer(&s->mu1, &s->mu1_a);
    }

    /* On-chip RAM. */
    memory_region_init_ram(&s->ocram, NULL, "imx93-ocram",
                           fsl_imx93_memmap[FSL_IMX93_OCRAM].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx93_memmap[FSL_IMX93_OCRAM].addr,
                                &s->ocram);

    /* LPUARTs. LPUART1 is the 11x11 EVK console (stdout-path = &lpuart1). */
    {
        static const struct {
            int region;
            int irq;
        } lpuart_table[FSL_IMX93_NUM_MODELED_LPUARTS] = {
            { FSL_IMX93_LPUART1, FSL_IMX93_LPUART1_IRQ },
            { FSL_IMX93_LPUART2, FSL_IMX93_LPUART2_IRQ },
            { FSL_IMX93_LPUART3, FSL_IMX93_LPUART3_IRQ },
            { FSL_IMX93_LPUART4, FSL_IMX93_LPUART4_IRQ },
            { FSL_IMX93_LPUART5, FSL_IMX93_LPUART5_IRQ },
            { FSL_IMX93_LPUART6, FSL_IMX93_LPUART6_IRQ },
            { FSL_IMX93_LPUART7, FSL_IMX93_LPUART7_IRQ },
            { FSL_IMX93_LPUART8, FSL_IMX93_LPUART8_IRQ },
        };

        for (i = 0; i < FSL_IMX93_NUM_MODELED_LPUARTS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpuart[i]);

            qdev_prop_set_chr(DEVICE(&s->lpuart[i]), "chardev", serial_hd(i));
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx93_memmap[lpuart_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, lpuart_table[i].irq));
        }
    }

    /*
     * Clock infrastructure. The i.MX 93 has no System Manager, so these are
     * functionally modeled (not SCMI-stubbed): Linux programs them directly.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ccm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ccm), 0,
                    fsl_imx93_memmap[FSL_IMX93_CCM].addr);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->anatop), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->anatop), 0,
                    fsl_imx93_memmap[FSL_IMX93_ANATOP].addr);

    /* PXP 2D engine: soft-reset + register window + completion IRQ. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pxp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pxp), 0,
                    fsl_imx93_memmap[FSL_IMX93_PXP].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pxp), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_PXP_IRQ));

    /*
     * ELE (EdgeLock Enclave) s4muap MU + success responder. Lets the fsl-se
     * driver probe (se_soc_info no longer times out), which in turn lets the
     * OCOTP driver register the FEC/eQOS MAC nvmem cells. "tx"/"rx" IRQs are
     * SPI 31/30.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ele), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ele), 0,
                    fsl_imx93_memmap[FSL_IMX93_ELE_MU].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ele), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_ELE_TX_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ele), 1,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_ELE_RX_IRQ));

    /*
     * uSDHC controllers. Real imx-usdhc model (carries the
     * SDHCI_QUIRK_SDCLK_AUTO_GATE fix) so the sdhci-esdhc-imx driver's
     * commands complete and, critically, device_shutdown() does not wedge
     * the way it would against a logging stub - which is what lets a guest
     * poweroff reach PSCI SYSTEM_OFF.
     */
    {
        static const struct {
            int region;
            int irq;
        } usdhc_table[FSL_IMX93_NUM_USDHCS] = {
            { FSL_IMX93_USDHC1, FSL_IMX93_USDHC1_IRQ },
            { FSL_IMX93_USDHC2, FSL_IMX93_USDHC2_IRQ },
            { FSL_IMX93_USDHC3, FSL_IMX93_USDHC3_IRQ },
        };

        for (i = 0; i < FSL_IMX93_NUM_USDHCS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usdhc[i]);

            /* VEND_SPEC resets to 0x3000_7809 on i.MX 9 (soft clock enables on). */
            object_property_set_uint(OBJECT(&s->usdhc[i]), "vendor-spec-reset",
                                     0x30007809, &error_abort);
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx93_memmap[usdhc_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, usdhc_table[i].irq));
        }
    }

    /* FEC ethernet (real imx.enet); PHY at the 11x11 EVK's MDIO address 2. */
    object_property_set_uint(OBJECT(&s->fec), "phy-num",
                             FSL_IMX93_FEC_PHY_NUM, &error_abort);
    object_property_set_uint(OBJECT(&s->fec), "tx-ring-num", 3, &error_abort);
    qemu_configure_nic_device(DEVICE(&s->fec), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fec), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fec), 0,
                    fsl_imx93_memmap[FSL_IMX93_FEC].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fec), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_FEC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fec), 1,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_FEC_TIMER_IRQ));

    /* eQOS (dwmac4) ethernet - the board's second NIC. */
    qemu_configure_nic_device(DEVICE(&s->eqos), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->eqos), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->eqos), 0,
                    fsl_imx93_memmap[FSL_IMX93_EQOS].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eqos), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_EQOS_IRQ));

    /*
     * LPI2C2: real controller with the board's PMIC (pca9451a @ 0x25) and
     * PCAL6524 GPIO expander (@ 0x22) attached. The expander provides the FEC
     * PHY reset-gpio; the PMIC's regulators unblock uSDHC and friends. Other
     * LPI2C instances stay logging stubs.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c2);
        I2CSlave *pmic;

        qdev_prop_set_string(DEVICE(&s->lpi2c2), "bus-name", "lpi2c2");
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[FSL_IMX93_LPI2C2].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX93_LPI2C2_IRQ));

        pmic = i2c_slave_new(TYPE_IMX93_I2C_REGDEV, FSL_IMX93_PCA9451_ADDR);
        qdev_prop_set_uint8(DEVICE(pmic), "reg0", FSL_IMX93_PCA9451_DEVID);
        qdev_prop_set_bit(DEVICE(pmic), "pca9450", true);
        i2c_slave_realize_and_unref(pmic, s->lpi2c2.bus, &error_abort);

        I2CSlave *expander = i2c_slave_new(TYPE_IMX93_I2C_REGDEV,
                                           FSL_IMX93_PCAL6524_ADDR);
        qdev_prop_set_bit(DEVICE(expander), "pcal6524", true);
        i2c_slave_realize_and_unref(expander, s->lpi2c2.bus, &error_abort);

        /*
         * ADP5585 I/O expander (io-expander@34). Its adp5585 MFD driver only
         * checks the ID register, then registers the GPIO + PWM sub-devices
         * the board's pwm-backlight (and audio/CAN rails) depend on - which is
         * what lets the LVDS panel's backlight, and thus the whole LVDS
         * display pipeline, come out of deferred probe.
         */
        I2CSlave *adp = i2c_slave_new(TYPE_IMX93_I2C_REGDEV,
                                      FSL_IMX93_ADP5585_ADDR);
        qdev_prop_set_uint8(DEVICE(adp), "reg0", FSL_IMX93_ADP5585_ID);
        i2c_slave_realize_and_unref(adp, s->lpi2c2.bus, &error_abort);
    }

    /*
     * GPIO banks (gpio1..gpio4). Each exposes two GIC lines; the gpio-vf610
     * driver uses the first. Real models so the gpiochip + irqchip register.
     */
    {
        static const struct {
            int region, irq, irq_hi;
        } gpio_table[FSL_IMX93_NUM_GPIOS] = {
            { FSL_IMX93_GPIO1, FSL_IMX93_GPIO1_IRQ, FSL_IMX93_GPIO1_IRQ_HI },
            { FSL_IMX93_GPIO2, FSL_IMX93_GPIO2_IRQ, FSL_IMX93_GPIO2_IRQ_HI },
            { FSL_IMX93_GPIO3, FSL_IMX93_GPIO3_IRQ, FSL_IMX93_GPIO3_IRQ_HI },
            { FSL_IMX93_GPIO4, FSL_IMX93_GPIO4_IRQ, FSL_IMX93_GPIO4_IRQ_HI },
        };

        for (i = 0; i < FSL_IMX93_NUM_GPIOS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->gpio[i]);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx93_memmap[gpio_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, gpio_table[i].irq));
            sysbus_connect_irq(sbd, 1,
                qdev_get_gpio_in(gicdev, gpio_table[i].irq_hi));
        }
    }

    /*
     * Display pipeline: LCDIFv3 -> MIPI DSI host -> ADV7535 HDMI bridge.
     *
     * The MEDIAMIX block control (GPR) carries the LCDIF/DSI/LDB output mux
     * and the D-PHY PLL config; the SRC "mediamix" slice reports the media
     * power domain as on so the imx93-pd genpd power-on poll completes.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mediamix), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mediamix), 0,
                    fsl_imx93_memmap[FSL_IMX93_MEDIAMIX_PD].addr);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->media_blk_ctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->media_blk_ctrl), 0,
                    fsl_imx93_memmap[FSL_IMX93_MEDIA_BLK_CTRL].addr);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dsi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dsi), 0,
                    fsl_imx93_memmap[FSL_IMX93_DSI].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dsi), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_DSI_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lcdif), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lcdif), 0,
                    fsl_imx93_memmap[FSL_IMX93_LCDIF].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lcdif), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_LCDIF_IRQ));

    /* ISI: capture channel; synthesises frames for the imx8-isi V4L2 driver. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->isi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isi), 0,
                    fsl_imx93_memmap[FSL_IMX93_ISI].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->isi), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_ISI_IRQ));

    /*
     * eDMA1: the i.MX LPI2C driver moves any transfer >= 8 bytes (e.g. the
     * 64-byte HDMI EDID block read) through eDMA, so a working DMA engine is
     * required for the display I2C bus to read EDID. Channel N interrupts on
     * GIC SPI (95 + N).
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->edma1);

        object_property_set_uint(OBJECT(&s->edma1), "num-channels",
                                 FSL_IMX93_EDMA1_CHANNELS, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[FSL_IMX93_EDMA1].addr);
        for (i = 0; i < FSL_IMX93_EDMA1_CHANNELS; i++) {
            sysbus_connect_irq(sbd, i,
                qdev_get_gpio_in(gicdev, FSL_IMX93_EDMA1_IRQ_BASE + i));
        }
    }

    /*
     * eDMA2 (WAKEUPMIX) is the "edma4" variant: 64 channels at a 0x8000 page
     * stride, with channel interrupts paired so channel N raises GIC SPI
     * (128 + N/2). It serves the WAKEUPMIX peripherals - notably sai2/sai3, so
     * the SAI3 + wm8962 audio card can request its DMA channels.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->edma2);

        object_property_set_uint(OBJECT(&s->edma2), "num-channels",
                                 FSL_IMX93_EDMA2_CHANNELS, &error_abort);
        object_property_set_uint(OBJECT(&s->edma2), "chan-stride",
                                 FSL_IMX93_EDMA2_CHAN_STRIDE, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[FSL_IMX93_EDMA2].addr);
        for (i = 0; i < FSL_IMX93_EDMA2_CHANNELS; i++) {
            sysbus_connect_irq(sbd, i,
                qdev_get_gpio_in(gicdev, FSL_IMX93_EDMA2_IRQ_BASE + i / 2));
        }
    }

    /*
     * LPUART RX is paged through a cyclic eDMA channel by the imx-lpuart
     * driver, so each LPUART's RX DMA-request line must drive its eDMA at the
     * RX source id from the EVK DTB dmas= props - otherwise DMA-mode RX never
     * advances and received bytes never reach userspace. (TX is mem->device,
     * which the eDMA runs whole at channel start, so it needs no request line.)
     * LPUART1/2 are on eDMA1 (AONMIX), LPUART3-8 on eDMA2 (WAKEUPMIX).
     */
    {
        static const struct {
            int edma;       /* 1 = AONMIX eDMA1, 2 = WAKEUPMIX eDMA2 */
            int rx_src;
        } lpuart_dma[FSL_IMX93_NUM_MODELED_LPUARTS] = {
            { 1, 0x11 }, { 1, 0x13 },               /* LPUART1, LPUART2 */
            { 2, 0x12 }, { 2, 0x14 }, { 2, 0x16 },  /* LPUART3, 4, 5 */
            { 2, 0x18 }, { 2, 0x58 }, { 2, 0x5a },  /* LPUART6, 7, 8 */
        };
        for (i = 0; i < FSL_IMX93_NUM_MODELED_LPUARTS; i++) {
            DeviceState *edma = lpuart_dma[i].edma == 1 ?
                DEVICE(&s->edma1) : DEVICE(&s->edma2);
            qdev_connect_gpio_out_named(DEVICE(&s->lpuart[i]),
                "dma-req-rx", 0,
                qdev_get_gpio_in_named(edma, "dma-req", lpuart_dma[i].rx_src));
        }
    }

    /*
     * LPI2C1: the display side I2C bus. Carries the ADV7535 DSI-to-HDMI
     * bridge (main map @ 0x3d) plus its CEC (0x3b) and packet (0x38) maps,
     * and an EDID-serving DDC slave at the bridge's EDID address (0x3f) so
     * the adv7511 driver reads a valid monitor EDID and sets a mode.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c1);
        I2CSlave *adv, *aux;
        DeviceState *ddc;

        qdev_prop_set_string(DEVICE(&s->lpi2c1), "bus-name", "lpi2c1");
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[FSL_IMX93_LPI2C1].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX93_LPI2C1_IRQ));

        adv = i2c_slave_new(TYPE_ADV7535, FSL_IMX93_ADV7535_MAIN_ADDR);
        qdev_prop_set_bit(DEVICE(adv), "main", true);
        i2c_slave_realize_and_unref(adv, s->lpi2c1.bus, &error_abort);

        aux = i2c_slave_new(TYPE_ADV7535, FSL_IMX93_ADV7535_CEC_ADDR);
        qdev_prop_set_bit(DEVICE(aux), "main", false);
        i2c_slave_realize_and_unref(aux, s->lpi2c1.bus, &error_abort);

        aux = i2c_slave_new(TYPE_ADV7535, FSL_IMX93_ADV7535_PKT_ADDR);
        qdev_prop_set_bit(DEVICE(aux), "main", false);
        i2c_slave_realize_and_unref(aux, s->lpi2c1.bus, &error_abort);

        ddc = DEVICE(i2c_slave_new(TYPE_I2CDDC, FSL_IMX93_ADV7535_EDID_ADDR));
        qdev_prop_set_uint32(ddc, "xres", 1024);
        qdev_prop_set_uint32(ddc, "yres", 768);
        i2c_slave_realize_and_unref(I2C_SLAVE(ddc), s->lpi2c1.bus,
                                    &error_abort);

        /* WM8962 audio codec @ 0x1a (the SAI3 speaker/headphone/mic card). */
        wm8962 = DEVICE(i2c_slave_new(TYPE_WM8962, FSL_IMX93_WM8962_ADDR));
        i2c_slave_realize_and_unref(I2C_SLAVE(wm8962), s->lpi2c1.bus,
                                    &error_abort);
    }

    /*
     * LPI2C8: the camera control bus on the mt9m114 device-tree variant. It
     * carries the MT9M114 sensor (0x48) plus a PCA9538 I/O expander (0x70)
     * that gates the sensor's power rails. With these the camera pipeline
     * (mt9m114 -> parallel-CSI -> ISI) binds and the V4L2 graph links.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c8);
        I2CSlave *pca;

        qdev_prop_set_string(DEVICE(&s->lpi2c8), "bus-name", "lpi2c8");
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[FSL_IMX93_LPI2C8].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX93_LPI2C8_IRQ));

        pca = i2c_slave_new(TYPE_IMX93_I2C_REGDEV, FSL_IMX93_PCA9538_ADDR);
        i2c_slave_realize_and_unref(pca, s->lpi2c8.bus, &error_abort);

        i2c_slave_realize_and_unref(
            i2c_slave_new(TYPE_MT9M114, FSL_IMX93_MT9M114_ADDR),
            s->lpi2c8.bus, &error_abort);
    }

    /*
     * LPI2C3-7: functional but unpopulated on the EVK. Bring them up as real
     * controllers (rather than logging stubs) so the i2c-N adapters register
     * and the machine can host I2C peripherals the stock EVK never defined -
     * e.g. -device <i2c-dev>,bus=/machine/soc/lpi2c5/i2c-bus.0. This is the
     * expandability goal: a board that grows beyond its reference design.
     */
    {
        static const struct {
            int region, irq;
        } lpi2c_exp_tbl[5] = {
            { FSL_IMX93_LPI2C3, FSL_IMX93_LPI2C3_IRQ },
            { FSL_IMX93_LPI2C4, FSL_IMX93_LPI2C4_IRQ },
            { FSL_IMX93_LPI2C5, FSL_IMX93_LPI2C5_IRQ },
            { FSL_IMX93_LPI2C6, FSL_IMX93_LPI2C6_IRQ },
            { FSL_IMX93_LPI2C7, FSL_IMX93_LPI2C7_IRQ },
        };

        for (i = 0; i < ARRAY_SIZE(s->lpi2c_exp); i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c_exp[i]);
            g_autofree char *bus_name = g_strdup_printf("lpi2c%d", i + 3);

            /* Name the bus so peripherals attach via -device bus=lpi2cN. */
            qdev_prop_set_string(DEVICE(&s->lpi2c_exp[i]), "bus-name",
                                 bus_name);
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx93_memmap[lpi2c_exp_tbl[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, lpi2c_exp_tbl[i].irq));
        }

        /*
         * OV5640 MIPI camera sensor (@0x3c) on LPI2C3 (= lpi2c_exp[0]): the
         * sensor the ov5640 device-tree variant routes through the MIPI CSI-2
         * host to the ISI. With it the ov5640 driver probes and registers its
         * V4L2 subdev so the media graph links (mirrors the mt9m114 on LPI2C8).
         */
        i2c_slave_realize_and_unref(
            i2c_slave_new(TYPE_OV5640, 0x3c),
            s->lpi2c_exp[0].bus, &error_abort);
    }

    /*
     * FlexIO1: a configurable shifter/timer fabric. The EVK's flexio-i2c device
     * tree routes an extra I2C master through it (nxp,imx-flexio MFD +
     * i2c-flexio); modelling its register file lets that adapter register, so a
     * custom board can grow an I2C bus the reference design runs over LPI2C.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexio1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexio1),
                    0, fsl_imx93_memmap[FSL_IMX93_FLEXIO1].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexio1), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_FLEXIO1_IRQ));

    /*
     * virtio-mmio transports (not real i.MX93 hardware). These give the guest
     * a place to attach virtio devices - notably a virtio-keyboard so the
     * emulated HDMI/LCDIF console gets real keyboard input. Matching DTB nodes
     * are injected by the board (imx93-evk.c). SPIs 230.. are unused.
     */
    for (i = 0; i < FSL_IMX93_NUM_VIRTIO_MMIO; i++) {
        DeviceState *vmmio = qdev_new("virtio-mmio");
        SysBusDevice *sbd = SYS_BUS_DEVICE(vmmio);

        /*
         * Modern (virtio 1.0) transport; the legacy default fails feature
         * negotiation with the modern guest virtio_mmio driver.
         */
        qdev_prop_set_bit(vmmio, "force-legacy", false);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0,
            FSL_IMX93_VIRTIO_MMIO_BASE + i * FSL_IMX93_VIRTIO_MMIO_SIZE);
        sysbus_connect_irq(sbd, 0,
            qdev_get_gpio_in(gicdev, FSL_IMX93_VIRTIO_MMIO_IRQ + i));
    }

    /*
     * FlexCAN1/2. Real controllers (hw/net/can/flexcan.c) on QEMU's CAN bus
     * subsystem; each is wired to a user-supplied CAN bus via the board's
     * canbus0/canbus1 links (NULL = register-only, frames dropped). The stock
     * NXP EVK DT enables flexcan2; the Linux flexcan driver binds and brings up
     * the canN netdev.
     */
    {
        static const struct {
            int region, irq;
        } flexcan_table[FSL_IMX93_NUM_FLEXCAN] = {
            { FSL_IMX93_FLEXCAN1, FSL_IMX93_FLEXCAN1_IRQ },
            { FSL_IMX93_FLEXCAN2, FSL_IMX93_FLEXCAN2_IRQ },
        };

        for (i = 0; i < FSL_IMX93_NUM_FLEXCAN; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->flexcan[i]);

            if (s->canbus[i]) {
                object_property_set_link(OBJECT(&s->flexcan[i]), "canbus",
                                         OBJECT(s->canbus[i]), &error_abort);
            }
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                fsl_imx93_memmap[flexcan_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, flexcan_table[i].irq));
        }
    }

    /*
     * USB OTG1/2 - ChipIdea controllers (EHCI host core). The USBNC "usbmisc"
     * glue at +0x200 stays a stub, as on i.MX7. Attach USB devices with e.g.
     * "-device usb-kbd"; note the stock EVK DT sets dr_mode=otg with a Type-C
     * role switch, so host mode depends on the (unmodelled) Type-C controller.
     */
    {
        static const struct {
            int region, irq;
        } usb_table[FSL_IMX93_NUM_USBS] = {
            { FSL_IMX93_USBOTG1, FSL_IMX93_USB1_IRQ },
            { FSL_IMX93_USBOTG2, FSL_IMX93_USB2_IRQ },
        };

        for (i = 0; i < FSL_IMX93_NUM_USBS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usb[i]);
            hwaddr base = fsl_imx93_memmap[usb_table[i].region].addr;
            g_autofree char *misc = g_strdup_printf("usbmisc%d", i + 1);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, base);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, usb_table[i].irq));
            create_unimplemented_device(misc, base + 0x200, 0x200);
        }
    }

    /*
     * Audio front-ends. SAI1/3 and MICFIL are the EVK's active codecs; their
     * FIFOs are serviced by eDMA3 (sai1 + micfil on edma1, sai3 on edma2). The
     * models carry the register file the fsl-sai/fsl-micfil drivers probe so
     * the ASoC cards register; sample movement rides the eDMA datapath.
     */
    {
        static const struct {
            int region, irq;
        } sai_table[FSL_IMX93_NUM_SAIS] = {
            { FSL_IMX93_SAI1, FSL_IMX93_SAI1_IRQ },
            { FSL_IMX93_SAI2, FSL_IMX93_SAI2_IRQ },
            { FSL_IMX93_SAI3, FSL_IMX93_SAI3_IRQ },
        };
        static const int micfil_irqs[IMX93_MICFIL_IRQS] = {
            FSL_IMX93_MICFIL_IRQ0, FSL_IMX93_MICFIL_IRQ1,
            FSL_IMX93_MICFIL_IRQ2, FSL_IMX93_MICFIL_IRQ3,
        };

        for (i = 0; i < FSL_IMX93_NUM_SAIS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->sai[i]);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, fsl_imx93_memmap[sai_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, sai_table[i].irq));
        }

        /*
         * SAI3 FIFO requests are serviced by eDMA2 (the wm8962 playback +
         * capture path). Wire TX and RX to their distinct eDMA request-source
         * lines (CH_MUX source ids from the evk DTB dmas=: TX 0x3c, RX 0x3d) so
         * each advances its own cyclic channel - letting SAI3 playback, SAI3
         * capture and the XCVR (below) all run on eDMA2 at once.
         */
        /*
         * The codec is the bit-clock MASTER: it drives BCLK/LRCLK, and the rate
         * the driver programs into it over I2C is the only copy of that number
         * in the machine. SAI3 is the slave and cannot derive the rate from its
         * own registers - they are byte-identical at 48 kHz and 16 kHz - so the
         * board wires it across, and so do we. Connected here rather than at the
         * codec's creation because the SAI's "codec-rate" input does not exist
         * until it has been realized.
         */
        if (wm8962) {
            qdev_connect_gpio_out_named(wm8962, "rate", 0,
                qdev_get_gpio_in_named(DEVICE(&s->sai[2]), "codec-rate", 0));
        }

        qdev_connect_gpio_out_named(DEVICE(&s->sai[2]), "dma-req-tx", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma2), "dma-req", 0x3c));
        qdev_connect_gpio_out_named(DEVICE(&s->sai[2]), "dma-req-rx", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma2), "dma-req", 0x3d));

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->micfil), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->micfil), 0,
                        fsl_imx93_memmap[FSL_IMX93_MICFIL].addr);
        for (i = 0; i < IMX93_MICFIL_IRQS; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->micfil), i,
                               qdev_get_gpio_in(gicdev, micfil_irqs[i]));
        }

        /*
         * MICFIL channel-0 capture requests are serviced by eDMA1 (like SAI1):
         * wire its DMA-request line so the cyclic RX channel drains DATACH0 as
         * the FIFO fills, pacing PDM capture at the audio word rate.
         */
        qdev_connect_gpio_out_named(DEVICE(&s->micfil), "dma-req", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma1), "dma-req", 0x1d));
    }

    /*
     * I3C1 (AONMIX): a functional Silvaco I3C master. The imx93-...-i3c device
     * tree moves the wm8962 codec onto the I3C bus as a legacy I2C target, so
     * attach a wm8962 at 0x1a to the controller's built-in I2C bus. Other DTBs
     * omit the node, so the controller sits idle. (I3C2 stays a logging stub -
     * no DTB exercises it.)
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i3c1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i3c1), 0,
                    fsl_imx93_memmap[FSL_IMX93_I3C1].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i3c1), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_I3C1_IRQ));
    i2c_slave_create_simple(s->i3c1.bus->i2c_bus, TYPE_WM8962,
                            FSL_IMX93_WM8962_ADDR);

    /* All peripherals not yet modeled get logging stubs. */
    /*
     * Ethos-U65 microNPU @ 0x4a900000, driven by the M33 ethos firmware. The
     * surrounding NPU-mix GPR/clock blocks (0x4a880000/0x4a8c0000/0x4a8d0000)
     * the firmware also touches stay logging stubs.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->ethosu);

        /*
         * Generic Arm Ethos-U executor as the i.MX93's Ethos-U65-256: variant
         * u65 + macs_per_cc_log2 8 make ID/CONFIG report 0x10061000 and
         * 0x10000008, the values the M33 ethos firmware's
         * verify_optimizer_config matches. DMA defaults to system memory.
         */
        object_property_set_str(OBJECT(&s->ethosu), "variant", "u65",
                                &error_abort);
        object_property_set_uint(OBJECT(&s->ethosu), "macs", 8, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, FSL_IMX93_ETHOSU_ADDR);
        /*
         * The NPU interrupt is handled by the ethos firmware on the M33, so it
         * is wired to the M33 NVIC (IRQ 178, RM Table 6), not the A55 GIC. The
         * M33 is realized earlier in this function, so its NVIC inputs exist.
         */
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->m33),
                                            FSL_IMX93_ETHOSU_IRQ));
        create_unimplemented_device("npumix-gpr", 0x4a880000, 0x10000);
        create_unimplemented_device("npumix-blk1", 0x4a8c0000, 0x10000);
        create_unimplemented_device("npumix-blk2", 0x4a8d0000, 0x10000);
    }

    /* BBNSM: real-time clock + power key. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bbnsm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bbnsm), 0,
                    fsl_imx93_memmap[FSL_IMX93_BBNSM].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bbnsm), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_BBNSM_IRQ));

    /* WDOG1-5: watchdog timers. */
    for (i = 0; i < 5; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdog[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdog[i]), 0,
                        fsl_imx93_memmap[FSL_IMX93_WDOG1 + i].addr);
    }

    /* TMU: thermal monitor (temperature is polled; alarm IRQ not modelled). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tmu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tmu), 0,
                    fsl_imx93_memmap[FSL_IMX93_TMU].addr);

    /* SAR-ADC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc1), 0,
                    fsl_imx93_memmap[FSL_IMX93_ADC1].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc1), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_ADC1_IRQ));

    /* OCOTP: fuse shadow (MAC addresses, SoC unique ID). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ocotp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ocotp), 0,
                    fsl_imx93_memmap[FSL_IMX93_OCOTP].addr);

    /* System counter (clocksource + compare clockevent). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysctr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctr), 0,
                    fsl_imx93_memmap[FSL_IMX93_SYSCTR].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sysctr), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_SYSCTR_IRQ));

    /* TPM1-6: timer / PWM modules. */
    for (i = 0; i < 6; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->tpm[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tpm[i]), 0,
                        fsl_imx93_memmap[FSL_IMX93_TPM1 + i].addr);
    }

    /* MU2 messaging unit (no peer wired; disabled on the EVK). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mu2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mu2), 0,
                    fsl_imx93_memmap[FSL_IMX93_MU2].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mu2), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_MU2_IRQ));

    /* FlexSPI NOR-flash controller + an attached SPI-NOR flash. */
    {
        SysBusDevice *fsbd = SYS_BUS_DEVICE(&s->flexspi);
        DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
        DeviceState *flash;
        qemu_irq cs_line;

        if (!sysbus_realize(fsbd, errp)) {
            return;
        }
        sysbus_mmio_map(fsbd, 0, fsl_imx93_memmap[FSL_IMX93_FLEXSPI1].addr);
        sysbus_mmio_map(fsbd, 1, FSL_IMX93_FLEXSPI_AHB_ADDR);
        sysbus_connect_irq(fsbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX93_FLEXSPI1_IRQ));

        flash = qdev_new("is25wp064");
        if (dinfo) {
            qdev_prop_set_drive(flash, "drive",
                                blk_by_legacy_dinfo(dinfo));
        }
        qdev_realize_and_unref(flash, BUS(s->flexspi.bus), &error_abort);
        cs_line = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
        qdev_connect_gpio_out_named(DEVICE(&s->flexspi), "cs", 0, cs_line);
    }

    /* XCVR SPDIF audio transceiver (registration model). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xcvr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xcvr), 0,
                    fsl_imx93_memmap[FSL_IMX93_XCVR].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->xcvr), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_XCVR_IRQ));
    /*
     * The XCVR (SPDIF) shares eDMA2 with SAI3 (both in the 0x4268xxxx region):
     * wire its TX DMA-request to its own request-source line (CH_MUX source id
     * 0x42 from the evk DTB dmas=) so the cyclic playback channel advances as
     * the SPDIF TX FIFO drains - independent of the SAI3 streams on eDMA2.
     */
    qdev_connect_gpio_out_named(DEVICE(&s->xcvr), "dma-req", 0,
        qdev_get_gpio_in_named(DEVICE(&s->edma2), "dma-req", 0x42));

    /* TSTMR1/2 timestamp timers + SEMA42 hardware semaphores (Group A). */
    {
        const int tstmr_r[2] = { FSL_IMX93_TSTMR1, FSL_IMX93_TSTMR2 };
        const int sema_r[2] = { FSL_IMX93_SEMA42_1, FSL_IMX93_SEMA42_2 };

        for (i = 0; i < 2; i++) {
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->tstmr[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->tstmr[i]), 0,
                            fsl_imx93_memmap[tstmr_r[i]].addr);
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->sema42[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->sema42[i]), 0,
                            fsl_imx93_memmap[sema_r[i]].addr);
        }
    }

    /* LPSPI1-8: SPI masters (each exposes an SSI bus for slaves). */
    {
        static const int lpspi_irq[8] = { 16, 17, 65, 66, 191, 192, 193, 194 };

        for (i = 0; i < 8; i++) {
            g_autofree char *bus_name = g_strdup_printf("lpspi%d", i + 1);

            /* Name the SSI bus so flash attaches via -device bus=lpspiN. */
            qdev_prop_set_string(DEVICE(&s->lpspi[i]), "bus-name", bus_name);
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpspi[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpspi[i]), 0,
                            fsl_imx93_memmap[FSL_IMX93_LPSPI1 + i].addr);
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpspi[i]), 0,
                               qdev_get_gpio_in(gicdev, lpspi_irq[i]));
        }
    }

    fsl_imx93_install_unimplemented(s);
}

static void fsl_imx93_init(Object *obj)
{
    FslImx93State *s = FSL_IMX93(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);
    object_initialize_child(obj, "m33", &s->m33, TYPE_ARMV7M);
    object_initialize_child(obj, "mu1", &s->mu1, TYPE_IMX_MU);
    object_initialize_child(obj, "mu1_a", &s->mu1_a, TYPE_IMX_MU);
    object_initialize_child(obj, "ethosu", &s->ethosu, TYPE_ETHOS_U);
    object_initialize_child(obj, "bbnsm", &s->bbnsm, TYPE_IMX93_BBNSM);
    for (i = 0; i < 5; i++) {
        g_autofree char *name = g_strdup_printf("wdog%d", i + 1);
        object_initialize_child(obj, name, &s->wdog[i], TYPE_IMX93_WDOG);
    }
    object_initialize_child(obj, "tmu", &s->tmu, TYPE_IMX93_TMU);
    object_initialize_child(obj, "adc1", &s->adc1, TYPE_IMX93_ADC);
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf("lpspi%d", i + 1);
        object_initialize_child(obj, name, &s->lpspi[i], TYPE_IMX93_LPSPI);
    }
    object_initialize_child(obj, "ocotp", &s->ocotp, TYPE_IMX93_OCOTP);
    object_initialize_child(obj, "sysctr", &s->sysctr, TYPE_IMX93_SYSCTR);
    for (i = 0; i < 6; i++) {
        g_autofree char *name = g_strdup_printf("tpm%d", i + 1);
        object_initialize_child(obj, name, &s->tpm[i], TYPE_IMX93_TPM);
    }
    object_initialize_child(obj, "mu2", &s->mu2, TYPE_IMX_MU);
    object_initialize_child(obj, "flexspi", &s->flexspi, TYPE_IMX93_FLEXSPI);
    object_initialize_child(obj, "xcvr", &s->xcvr, TYPE_IMX93_XCVR);
    for (i = 0; i < 2; i++) {
        g_autofree char *tn = g_strdup_printf("tstmr%d", i + 1);
        g_autofree char *sn = g_strdup_printf("sema42-%d", i + 1);
        object_initialize_child(obj, tn, &s->tstmr[i], TYPE_IMX93_TSTMR);
        object_initialize_child(obj, sn, &s->sema42[i], TYPE_IMX93_SEMA42);
    }
    object_initialize_child(obj, "ccm", &s->ccm, TYPE_IMX93_CCM);
    object_initialize_child(obj, "anatop", &s->anatop, TYPE_IMX93_ANATOP);
    object_initialize_child(obj, "pxp", &s->pxp, TYPE_IMX93_PXP);
    object_initialize_child(obj, "ele", &s->ele, TYPE_IMX93_ELE);

    for (i = 0; i < FSL_IMX93_NUM_USDHCS; i++) {
        g_autofree char *name = g_strdup_printf("usdhc%d", i + 1);
        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX_USDHC);
    }

    object_initialize_child(obj, "fec", &s->fec, TYPE_IMX_ENET);
    object_initialize_child(obj, "eqos", &s->eqos, TYPE_IMX93_DWMAC);
    object_initialize_child(obj, "edma1", &s->edma1, TYPE_IMX93_EDMA);
    object_initialize_child(obj, "edma2", &s->edma2, TYPE_IMX93_EDMA);
    object_initialize_child(obj, "lpi2c1", &s->lpi2c1, TYPE_IMX_LPI2C);
    object_initialize_child(obj, "lpi2c2", &s->lpi2c2, TYPE_IMX_LPI2C);
    object_initialize_child(obj, "lpi2c8", &s->lpi2c8, TYPE_IMX_LPI2C);
    object_initialize_child(obj, "flexio1", &s->flexio1, TYPE_IMX93_FLEXIO);
    for (i = 0; i < ARRAY_SIZE(s->lpi2c_exp); i++) {
        g_autofree char *name = g_strdup_printf("lpi2c%d", i + 3);
        object_initialize_child(obj, name, &s->lpi2c_exp[i], TYPE_IMX_LPI2C);
    }
    object_initialize_child(obj, "mediamix", &s->mediamix,
                            TYPE_IMX93_SRC_SLICE);
    object_initialize_child(obj, "media-blk-ctrl", &s->media_blk_ctrl,
                            TYPE_IMX93_MEDIA_BLK_CTRL);
    object_initialize_child(obj, "dsi", &s->dsi, TYPE_IMX93_DSI);
    object_initialize_child(obj, "lcdif", &s->lcdif, TYPE_IMX93_LCDIF);
    object_initialize_child(obj, "isi", &s->isi, TYPE_IMX93_ISI);

    for (i = 0; i < FSL_IMX93_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("flexcan%d", i + 1);
        object_initialize_child(obj, name, &s->flexcan[i], TYPE_FLEXCAN);
    }

    for (i = 0; i < FSL_IMX93_NUM_USBS; i++) {
        g_autofree char *name = g_strdup_printf("usb%d", i + 1);
        object_initialize_child(obj, name, &s->usb[i], TYPE_CHIPIDEA);
    }

    for (i = 0; i < FSL_IMX93_NUM_SAIS; i++) {
        g_autofree char *name = g_strdup_printf("sai%d", i + 1);
        object_initialize_child(obj, name, &s->sai[i], TYPE_IMX93_SAI);
    }

    object_initialize_child(obj, "micfil", &s->micfil, TYPE_IMX93_MICFIL);
    object_initialize_child(obj, "i3c1", &s->i3c1, TYPE_SVC_I3C);

    for (i = 0; i < FSL_IMX93_NUM_GPIOS; i++) {
        g_autofree char *name = g_strdup_printf("gpio%d", i + 1);
        object_initialize_child(obj, name, &s->gpio[i], TYPE_IMX93_GPIO);
    }

    for (i = 0; i < FSL_IMX93_NUM_MODELED_LPUARTS; i++) {
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);
        object_initialize_child(obj, name, &s->lpuart[i], TYPE_IMX_LPUART);
    }
}

static const Property fsl_imx93_properties[] = {
    DEFINE_PROP_LINK("canbus0", FslImx93State, canbus[0], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus1", FslImx93State, canbus[1], TYPE_CAN_BUS,
                     CanBusState *),
};

static void fsl_imx93_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fsl_imx93_realize;
    device_class_set_props(dc, fsl_imx93_properties);
    /* This is an SoC, not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo fsl_imx93_types[] = {
    {
        .name           = TYPE_FSL_IMX93,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(FslImx93State),
        .instance_init  = fsl_imx93_init,
        .class_init     = fsl_imx93_class_init,
    },
};

DEFINE_TYPES(fsl_imx93_types)
