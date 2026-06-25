/*
 * NXP i.MX 93 SoC definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Modeled on hw/arm/fsl-imx8mp.h (Bernhard Beschow) and the i.MX 95 port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unlike the i.MX 95, the i.MX 93 has NO System Manager: Linux programs the
 * CCM / ANATOP / IOMUXC / SRC blocks directly (no SCMI/SM indirection), so
 * those regions must eventually be modeled functionally rather than served
 * by an SM firmware stub. v0.0.1 installs logging stubs for them.
 *
 * All base addresses and IRQ numbers below are taken from the i.MX 93 Linux
 * device tree (arch/arm64/boot/dts/freescale/imx93.dtsi) and cross-checked
 * against the i.MX 93 Reference Manual (IMX93RM).
 */

#ifndef FSL_IMX93_H
#define FSL_IMX93_H

#include "target/arm/cpu.h"
#include "hw/arm/armv7m.h"
#include "hw/core/clock.h"
#include "qemu/notify.h"
#include "hw/char/imx_lpuart.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/imx93_ccm.h"
#include "hw/misc/imx93_anatop.h"
#include "hw/misc/imx93_pxp.h"
#include "hw/misc/imx93_ele.h"
#include "hw/net/imx_fec.h"
#include "hw/net/imx93_dwmac.h"
#include "hw/net/flexcan.h"
#include "net/can_emu.h"
#include "hw/i2c/imx_lpi2c.h"
#include "hw/gpio/imx93_gpio.h"
#include "hw/display/imx93_lcdif.h"
#include "hw/display/imx93_isi.h"
#include "hw/misc/imx93_flexio.h"
#include "hw/display/imx93_dsi.h"
#include "hw/misc/imx93_media_blk.h"
#include "hw/misc/imx_mu.h"
#include "hw/npu/ethos_u.h"
#include "hw/rtc/imx93_bbnsm.h"
#include "hw/watchdog/imx93_wdog.h"
#include "hw/misc/imx93_tmu.h"
#include "hw/adc/imx93_adc.h"
#include "hw/ssi/imx93_lpspi.h"
#include "hw/nvram/imx93_ocotp.h"
#include "hw/timer/imx93_sysctr.h"
#include "hw/timer/imx93_tpm.h"
#include "hw/timer/imx93_tstmr.h"
#include "hw/misc/imx93_sema42.h"
#include "hw/ssi/imx93_flexspi.h"
#include "hw/audio/imx93_xcvr.h"
#include "hw/dma/imx93_edma.h"
#include "hw/usb/chipidea.h"
#include "hw/audio/imx93_sai.h"
#include "hw/audio/imx93_micfil.h"
#include "hw/i3c/svc_i3c.h"
#include "hw/sd/sdhci.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_FSL_IMX93 "fsl-imx93"
OBJECT_DECLARE_SIMPLE_TYPE(FslImx93State, FSL_IMX93)

/*
 * Main DDR window. The i.MX 93 maps DRAM starting at 0x8000_0000. The
 * 11x11 EVK ships with 2 GiB LPDDR4X; allow up to 4 GiB for the larger
 * board variants.
 */
#define FSL_IMX93_RAM_START         0x80000000ULL
#define FSL_IMX93_FLEXSPI_AHB_ADDR  0x28000000ULL  /* memory-mapped NOR (XIP) */
#define FSL_IMX93_RAM_SIZE_MAX      (4ULL * GiB)

/*
 * i.MX 93 application processor complex:
 *   - 2x Cortex-A55  (the main APUs we emulate here)
 *   - 1x Cortex-M33  (real-time / low-power domain - not modeled in v0.0.1;
 *                     note: the M33 is NOT a System Manager as on i.MX 95)
 * In v0.0.1 we only instantiate the A55 cluster.
 */
enum FslImx93Configuration {
    FSL_IMX93_NUM_A55_CPUS  = 2,
    FSL_IMX93_NUM_M33       = 1,    /* Cortex-M33 real-time core */
    FSL_IMX93_NUM_LPUARTS   = 8,    /* LPUART1..LPUART8 */
    FSL_IMX93_NUM_IRQS      = 320,  /* GICv3 SPI budget for v0.0.1 */
};

/*
 * Cortex-M33 real-time core. The memory map follows the Linux imx_rproc
 * imx_rproc_att_imx93 table: the M33 sees its 256 KiB ITCM (code) at
 * 0x1FFC0000 (secure; 0x0FFC0000 non-secure) and its 256 KiB DTCM (data) at
 * 0x20000000 (non-secure; 0x30000000 secure). The same TCM RAM is visible to
 * the A55 system view at 0x201C0000 (ITCM) / 0x20200000 (DTCM) so a loader
 * (Linux remoteproc, U-Boot, -device loader) can stage firmware. The M33's
 * reset VTOR is the secure ITCM base - that is where its vector table sits.
 * Unlike the i.MX 95, the i.MX 93 has no System Manager; this M33 is purely a
 * real-time/remoteproc core.
 */
#define FSL_IMX93_M33_ITCM_MVIEW_S   0x1FFC0000ULL  /* M33 secure (VTOR) */
#define FSL_IMX93_M33_ITCM_MVIEW_NS  0x0FFC0000ULL  /* M33 non-secure    */
#define FSL_IMX93_M33_ITCM_SYSVIEW   0x201C0000ULL  /* A55 view of ITCM  */
#define FSL_IMX93_M33_DTCM_MVIEW_NS  0x20000000ULL  /* M33 non-secure    */
#define FSL_IMX93_M33_DTCM_MVIEW_S   0x30000000ULL  /* M33 secure        */
#define FSL_IMX93_M33_DTCM_SYSVIEW   0x20200000ULL  /* A55 view of DTCM  */
#define FSL_IMX93_M33_TCM_SIZE       (256 * KiB)
/*
 * NXP's i.MX93 M33 firmware (MCUXpresso SDK, rpmsg-lite, ...) links its vector
 * table 0x20000 into the ITCM (M33 view 0x0FFE0000 / secure 0x1FFE0000), with
 * .resource_table right after it - not at the ITCM base. The reset VTOR and
 * the firmware-presence check both key off this offset.
 */
#define FSL_IMX93_M33_FW_OFFSET      0x20000
#define FSL_IMX93_M33_SVTOR          (FSL_IMX93_M33_ITCM_MVIEW_S + \
                                      FSL_IMX93_M33_FW_OFFSET)
#define FSL_IMX93_M33_NUM_IRQ        256
#define FSL_IMX93_M33_CLK_HZ         200000000U     /* M33 ~200 MHz */

/*
 * All eight LPUART instances are modeled. LPUART1 is the 11x11 EVK console;
 * the board also enables LPUART5, and modeling the full set means any stock
 * board DT probes cleanly instead of taking an external abort on an unmapped
 * instance.
 */
#define FSL_IMX93_NUM_MODELED_LPUARTS   8

/* uSDHC controllers modeled with the real imx-usdhc device. */
#define FSL_IMX93_NUM_USDHCS            3

/* GPIO banks (gpio1..gpio4). */
#define FSL_IMX93_NUM_GPIOS            4

/* FlexCAN controllers (flexcan1, flexcan2). */
#define FSL_IMX93_NUM_FLEXCAN         2

/* USB OTG controllers (ChipIdea), usbotg1/usbotg2. */
#define FSL_IMX93_NUM_USBS            2

/* SAI audio interfaces (sai1, sai2, sai3). */
#define FSL_IMX93_NUM_SAIS            3

struct FslImx93State {
    SysBusDevice    parent_obj;

    ARMCPU          cpu[FSL_IMX93_NUM_A55_CPUS];
    GICv3State      gic;

    /* Cortex-M33 real-time core + its private TCM and address-space views. */
    ARMv7MState     m33;
    Clock           *m33_cpuclk;
    bool            m33_started;          /* SiP RPROC start/stop state */
    IMXMUState      mu1;                  /* MU1_MUB @ 0x44230000, A55 side  */
    IMXMUState      mu1_a;                /* MU1_MUA @ 0x44220000, M33 side  */
    EthosUState ethosu;                  /* Ethos-U65 NPU @ 0x4a900000      */
    IMX93BbnsmState bbnsm;               /* BBNSM RTC @ 0x44440000          */
    IMX93WdogState wdog[5];              /* WDOG1-5                         */
    IMX93TmuState tmu;                   /* thermal monitor @ 0x44482000    */
    IMX93AdcState adc1;                  /* SAR-ADC @ 0x44530000            */
    IMX93LpspiState lpspi[8];            /* LPSPI1-8                        */
    IMX93OcotpState ocotp;               /* OCOTP fuses @ 0x47510000        */
    IMX93SysctrState sysctr;             /* system counter @ 0x44290000     */
    IMX93TpmState tpm[6];                /* TPM1-6 (timer/PWM)              */
    IMXMUState mu2;                       /* MU2 @ 0x42440000                */
    IMX93TstmrState tstmr[2];            /* TSTMR1/2 timestamp timers       */
    IMX93Sema42State sema42[2];          /* SEMA42 (AON + WAKEUP)           */
    IMX93FlexSpiState flexspi;           /* FlexSPI @ 0x425e0000 (NOR flash) */
    IMX93XcvrState xcvr;                 /* SPDIF transceiver @ 0x42680000  */
    MemoryRegion    m33_view;            /* the M33's 4 GiB address space    */
    MemoryRegion    m33_sysmem_alias;    /* low-prio window onto system mem  */
    MemoryRegion    m33_secure_periph;   /* 0x5xxxxxxx secure alias of 0x4... */
    MemoryRegion    m33_itcm;            /* ITCM backing RAM (M33 secure)    */
    MemoryRegion    m33_itcm_alias_ns;   /* M33 non-secure ITCM alias        */
    MemoryRegion    m33_itcm_sysview;    /* A55 view of ITCM                 */
    MemoryRegion    m33_dtcm;            /* DTCM backing RAM (M33 ns)        */
    MemoryRegion    m33_dtcm_alias_s;    /* M33 secure DTCM alias            */
    MemoryRegion    m33_dtcm_sysview;    /* A55 view of DTCM                 */
    Notifier        m33_machine_done;
    IMXLPUARTState  lpuart[FSL_IMX93_NUM_MODELED_LPUARTS];
    IMX93CCMState   ccm;
    IMX93AnatopState anatop;
    IMX93PxpState   pxp;
    IMX93EleState   ele;
    SDHCIState      usdhc[FSL_IMX93_NUM_USDHCS];
    IMXFECState     fec;
    IMX93DwmacState eqos;
    IMX93EdmaState  edma1;
    IMX93EdmaState  edma2;
    IMXLPI2CState   lpi2c1;
    IMXLPI2CState   lpi2c2;
    IMXLPI2CState   lpi2c8;
    IMXLPI2CState   lpi2c_exp[5];        /* LPI2C3-7: open expansion buses  */
    IMX93FlexioState flexio1;            /* FlexIO1 (configurable I/O / I2C) */
    IMX93GPIOState  gpio[FSL_IMX93_NUM_GPIOS];
    IMX93MediaBlkCtrlState media_blk_ctrl;
    IMX93SrcSliceState     mediamix;
    IMX93DsiState   dsi;
    IMX93LcdifState lcdif;
    IMX93IsiState   isi;
    FlexCanState    flexcan[FSL_IMX93_NUM_FLEXCAN];
    CanBusState     *canbus[FSL_IMX93_NUM_FLEXCAN];
    ChipideaState   usb[FSL_IMX93_NUM_USBS];
    IMX93SaiState   sai[FSL_IMX93_NUM_SAIS];
    IMX93MicfilState micfil;
    SvcI3cState     i3c1;                 /* Silvaco I3C master @ 0x44330000  */
    MemoryRegion    ocram;
};

/*
 * Memory map region identifiers. The actual addresses live in the memmap
 * table in fsl-imx93.c.
 */
enum FslImx93MemoryRegions {
    FSL_IMX93_RAM,

    /* GICv3 (i.MX 93 has no ITS in the base SoC) */
    FSL_IMX93_GIC_DIST,
    FSL_IMX93_GIC_REDIST,

    /* On-chip RAM */
    FSL_IMX93_OCRAM,

    /* LPUART block (AON + Wakeup domains); all 8 instances modeled */
    FSL_IMX93_LPUART1,
    FSL_IMX93_LPUART2,
    FSL_IMX93_LPUART3,
    FSL_IMX93_LPUART4,
    FSL_IMX93_LPUART5,
    FSL_IMX93_LPUART6,
    FSL_IMX93_LPUART7,
    FSL_IMX93_LPUART8,

    /* Clock / reset / pinmux infrastructure (stubbed as unimplemented) */
    FSL_IMX93_CCM,
    FSL_IMX93_ANATOP,
    FSL_IMX93_IOMUXC,
    FSL_IMX93_SRC,

    /* BLK_CTRL / syscfg aggregates per power domain (stubbed) */
    FSL_IMX93_BLK_CTRL_AONMIX,
    FSL_IMX93_BLK_CTRL_WAKEUPMIX,
    FSL_IMX93_BLK_CTRL_DDRMIX,

    /* DDR controller register block (stubbed; the EDAC driver probes it) */
    FSL_IMX93_DDRC,

    /* Ethernet: FEC (modeled) + eQOS dwmac (stubbed - no upstream model) */
    FSL_IMX93_FEC,
    FSL_IMX93_EQOS,

    /* eDMA controllers (edma1 AONMIX, edma2 WAKEUPMIX) */
    FSL_IMX93_EDMA1,
    FSL_IMX93_EDMA2,

    /*
     * Cortex-M33 remoteproc resource table (in M33 SRAM); reads as 0 so the
     * imx_rproc driver treats it as no valid table and backs off cleanly.
     */
    FSL_IMX93_RSC_TABLE,

    /*
     * OCOTP/efuse syscon: provides the FEC MAC-address nvmem cells. Mapping
     * it (reads 0 -> zero MAC -> FEC falls back to a random MAC) lets the
     * ethernet drivers bind instead of deferring on a missing MAC supplier.
     */
    FSL_IMX93_OCOTP,

    /*
     * Messaging Units: MU1 (AONMIX), MU2 (WAKEUPMIX), and the ELE/Sentinel
     * S4 MU. All enabled on the 11x11 EVK; unmapped MMIO here faults the
     * imx-mailbox driver probe with a synchronous external abort.
     */
    FSL_IMX93_MU1,
    FSL_IMX93_MU2,
    FSL_IMX93_ELE_MU,

    /* System counter */
    FSL_IMX93_SYSCTR,

    /*
     * Watchdogs: WDOG1/2 in AONMIX, WDOG3/4/5 in WAKEUPMIX. The 11x11 EVK
     * enables wdog3 (0x42490000); the rest are mapped for completeness.
     */
    FSL_IMX93_WDOG1,
    FSL_IMX93_WDOG2,
    FSL_IMX93_WDOG3,
    FSL_IMX93_WDOG4,
    FSL_IMX93_WDOG5,

    /* Trusted Resource Domain Controller (stubbed) */
    FSL_IMX93_TRDC,

    /* Battery-Backed Non-Secure Module (RTC + power key), syscon */
    FSL_IMX93_BBNSM,

    /* Thermal Management Unit (qoriq-tmu) */
    FSL_IMX93_TMU,

    /* ADC */
    FSL_IMX93_ADC1,

    /* uSDHC (eMMC / SD / SDIO) */
    FSL_IMX93_USDHC1,
    FSL_IMX93_USDHC2,
    FSL_IMX93_USDHC3,

    /*
     * Low-speed I/O controllers. All stubbed: they are board-enabled so an
     * unmapped instance aborts its (often deferred) probe, which blocks
     * wait_for_device_probe() and stalls the boot before init runs.
     */
    FSL_IMX93_TPM1, FSL_IMX93_TPM2, FSL_IMX93_TPM3,
    FSL_IMX93_TPM4, FSL_IMX93_TPM5, FSL_IMX93_TPM6,
    FSL_IMX93_I3C1, FSL_IMX93_I3C2,
    FSL_IMX93_LPI2C1, FSL_IMX93_LPI2C2, FSL_IMX93_LPI2C3, FSL_IMX93_LPI2C4,
    FSL_IMX93_LPI2C5, FSL_IMX93_LPI2C6, FSL_IMX93_LPI2C7, FSL_IMX93_LPI2C8,
    FSL_IMX93_LPSPI1, FSL_IMX93_LPSPI2, FSL_IMX93_LPSPI3, FSL_IMX93_LPSPI4,
    FSL_IMX93_LPSPI5, FSL_IMX93_LPSPI6, FSL_IMX93_LPSPI7, FSL_IMX93_LPSPI8,
    FSL_IMX93_FLEXCAN1, FSL_IMX93_FLEXCAN2,
    FSL_IMX93_SAI1, FSL_IMX93_SAI2, FSL_IMX93_SAI3,
    FSL_IMX93_MICFIL, FSL_IMX93_FLEXSPI1, FSL_IMX93_XCVR,
    FSL_IMX93_GPIO1, FSL_IMX93_GPIO2, FSL_IMX93_GPIO3, FSL_IMX93_GPIO4,
    FSL_IMX93_USBOTG1, FSL_IMX93_USBOTG2,

    /* MEDIAMIX: block control + imaging cluster */
    FSL_IMX93_MEDIAMIX_PD,      /* SRC power-domain slice (overlays SRC) */
    FSL_IMX93_MEDIA_BLK_CTRL,
    FSL_IMX93_MIPI_CSI,
    FSL_IMX93_DSI,
    FSL_IMX93_PXP,
    FSL_IMX93_LCDIF,
    FSL_IMX93_ISI,

    /* Group A: blocks not otherwise in the map (TSTMR/SEMA42/FLEXIO) */
    FSL_IMX93_TSTMR1,
    FSL_IMX93_TSTMR2,
    FSL_IMX93_SEMA42_1,
    FSL_IMX93_SEMA42_2,
    FSL_IMX93_FLEXIO1,
    FSL_IMX93_FLEXIO2,

    FSL_IMX93_NUM_REGIONS,
};

/*
 * IRQ assignments (GIC SPI numbers, from imx93.dtsi). These are the
 * interrupt numbers as the device tree presents them; the model adds
 * GIC_INTERNAL internally.
 */
enum FslImx93Irqs {
    FSL_IMX93_I3C1_IRQ      = 12,   /* I3C1 (AONMIX) -> GIC SPI 12 */
    FSL_IMX93_LPUART1_IRQ   = 19,
    FSL_IMX93_LPUART2_IRQ   = 20,
    FSL_IMX93_LPUART3_IRQ   = 68,
    FSL_IMX93_LPUART4_IRQ   = 69,
    FSL_IMX93_LPUART5_IRQ   = 70,
    FSL_IMX93_LPUART6_IRQ   = 71,
    FSL_IMX93_LPUART7_IRQ   = 210,
    FSL_IMX93_LPUART8_IRQ   = 211,
    FSL_IMX93_USDHC1_IRQ    = 86,
    FSL_IMX93_USDHC2_IRQ    = 87,
    FSL_IMX93_USDHC3_IRQ    = 205,
    FSL_IMX93_FEC_IRQ       = 179,  /* FEC MAC (int0) */
    FSL_IMX93_FEC_TIMER_IRQ = 182,  /* FEC 1588 timer */
    FSL_IMX93_EQOS_IRQ      = 184,
    FSL_IMX93_BBNSM_IRQ     = 73,    /* BBNSM RTC alarm -> A55 GIC SPI */
    FSL_IMX93_TMU_IRQ       = 83,    /* TMU temp alarm */
    FSL_IMX93_ADC1_IRQ      = 219,   /* SAR-ADC conversion (driver irq idx 2) */
    FSL_IMX93_PXP_IRQ       = 173,   /* WAKEUPMIX PXP interrupt 0 */
    FSL_IMX93_ISI_IRQ       = 172,   /* WAKEUPMIX ISI interrupt */
    FSL_IMX93_SYSCTR_IRQ    = 74,    /* system counter compare */
    FSL_IMX93_MU2_IRQ       = 23,    /* MU2 -> A55 GIC SPI */
    FSL_IMX93_FLEXSPI1_IRQ  = 55,    /* FlexSPI */
    FSL_IMX93_XCVR_IRQ      = 203,   /* XCVR SPDIF */
    FSL_IMX93_ELE_TX_IRQ    = 31,   /* s4muap "tx" */
    FSL_IMX93_ELE_RX_IRQ    = 30,   /* s4muap "rx" */
    FSL_IMX93_MU1_IRQ       = 22,   /* MU1_MUB -> A55 GIC SPI */
    FSL_IMX93_M33_MU_IRQ    = 21,   /* MU1_MUA -> M33 NVIC (per fw) */
    FSL_IMX93_ETHOSU_IRQ    = 178,  /* Ethos-U65 NPU -> M33 NVIC (RM Table 6) */
    FSL_IMX93_LPI2C1_IRQ    = 13,
    FSL_IMX93_LPI2C2_IRQ    = 14,
    FSL_IMX93_LPI2C3_IRQ    = 62,
    FSL_IMX93_LPI2C4_IRQ    = 63,
    FSL_IMX93_LPI2C5_IRQ    = 195,
    FSL_IMX93_LPI2C6_IRQ    = 196,
    FSL_IMX93_LPI2C7_IRQ    = 197,
    FSL_IMX93_LPI2C8_IRQ    = 198,
    FSL_IMX93_FLEXIO1_IRQ   = 53,
    FSL_IMX93_FLEXCAN1_IRQ  = 8,
    FSL_IMX93_FLEXCAN2_IRQ  = 51,
    FSL_IMX93_USB1_IRQ      = 187,
    FSL_IMX93_USB2_IRQ      = 188,
    FSL_IMX93_SAI1_IRQ      = 45,
    FSL_IMX93_SAI2_IRQ      = 170,
    FSL_IMX93_SAI3_IRQ      = 171,
    /* MICFIL has four lines (error, stream, VAD events). */
    FSL_IMX93_MICFIL_IRQ0   = 202,
    FSL_IMX93_MICFIL_IRQ1   = 201,
    FSL_IMX93_MICFIL_IRQ2   = 200,
    FSL_IMX93_MICFIL_IRQ3   = 199,
    /* eDMA1: channel N raises GIC SPI (EDMA1_IRQ_BASE + N). */
    FSL_IMX93_EDMA1_IRQ_BASE = 95,
    FSL_IMX93_EDMA1_CHANNELS = 31,
    /* eDMA2 (edma4): 64 channels, paired - channel N -> GIC SPI (128 + N/2). */
    FSL_IMX93_EDMA2_IRQ_BASE = 128,
    FSL_IMX93_EDMA2_CHANNELS = 64,
    FSL_IMX93_EDMA2_CHAN_STRIDE = 0x8000,
    FSL_IMX93_DSI_IRQ       = 177,
    FSL_IMX93_LCDIF_IRQ     = 176,
    /* GPIO banks: each has two GIC lines (the driver uses the first). */
    FSL_IMX93_GPIO1_IRQ     = 10,
    FSL_IMX93_GPIO1_IRQ_HI  = 11,
    FSL_IMX93_GPIO2_IRQ     = 57,
    FSL_IMX93_GPIO2_IRQ_HI  = 58,
    FSL_IMX93_GPIO3_IRQ     = 59,
    FSL_IMX93_GPIO3_IRQ_HI  = 60,
    FSL_IMX93_GPIO4_IRQ     = 189,
    FSL_IMX93_GPIO4_IRQ_HI  = 190,
};

/* Trivial register-file I2C slave used for the board's PMIC + GPIO expander. */
#define TYPE_IMX93_I2C_REGDEV   "imx93.i2c-regdev"

/* MT9M114 camera sensor I2C slave (mt9m114 device-tree variant). */
#define TYPE_MT9M114            "mt9m114"

/* OV5640 MIPI camera sensor I2C slave (ov5640 device-tree variant). */
#define TYPE_OV5640             "ov5640"

/* I2C addresses on lpi2c2: PMIC + GPIO expander. */
#define FSL_IMX93_PCA9451_ADDR  0x25
#define FSL_IMX93_PCA9451_DEVID 0x90    /* DEV_ID high nibble 0x9 = pca9451a */
#define FSL_IMX93_PCAL6524_ADDR 0x22
/*
 * ADP5585 I/O expander (io-expander@34): MFD whose GPIO+PWM drive the LVDS
 * panel's pwm-backlight. ID reg 0x00 high nibble must read 0x2 to probe.
 */
#define FSL_IMX93_ADP5585_ADDR  0x34
#define FSL_IMX93_ADP5585_ID    0x20    /* ADP5585_MAN_ID_VALUE, bits [7:4] */

/* FEC RGMII PHY MDIO address on the 11x11 EVK (ethphy2, reg = <2>). */
#define FSL_IMX93_FEC_PHY_NUM   2

/*
 * virtio-mmio transports. Not present on real i.MX93 silicon; we add a few
 * slots in an unused hole of the memory map and inject matching device-tree
 * nodes (see imx93-evk.c) so a guest can bind e.g. a virtio-keyboard, giving
 * the emulated HDMI/LCDIF console real keyboard input. SPIs 230.. are unused
 * by the SoC; the GIC is configured with 320 SPIs.
 */
#define FSL_IMX93_VIRTIO_MMIO_BASE  0x70000000
#define FSL_IMX93_VIRTIO_MMIO_SIZE  0x200
#define FSL_IMX93_NUM_VIRTIO_MMIO   4
#define FSL_IMX93_VIRTIO_MMIO_IRQ   230     /* first SPI; one per transport */

/*
 * Display: ADV7535 DSI-to-HDMI bridge I2C addresses on lpi2c1. The adv7511
 * driver derives the auxiliary maps from the main address: edid = main + 4,
 * cec = main - 1 (overridden to 0x3b by adi,addr-cec), packet = main - 5.
 */
#define FSL_IMX93_ADV7535_MAIN_ADDR     0x3d
#define FSL_IMX93_ADV7535_EDID_ADDR     0x3f
#define FSL_IMX93_ADV7535_CEC_ADDR      0x3b
#define FSL_IMX93_ADV7535_PKT_ADDR      0x38

/* WM8962 audio codec on LPI2C1. */
#define FSL_IMX93_WM8962_ADDR           0x1a

/* MU1_MUA: the M33 side of MU1 (the A55 side, MU1_MUB, is FSL_IMX93_MU1). */
#define FSL_IMX93_MU1_MUA_ADDR          0x44220000

/* Ethos-U65 NPU APB block (driven by the M33 ethos firmware). */
#define FSL_IMX93_ETHOSU_ADDR           0x4a900000

/* Camera (mt9m114 device-tree variant) on LPI2C8. */
#define FSL_IMX93_MT9M114_ADDR          0x48
#define FSL_IMX93_PCA9538_ADDR          0x70

#endif /* FSL_IMX93_H */
