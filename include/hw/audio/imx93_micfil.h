/*
 * NXP i.MX 93 MICFIL (PDM microphone interface)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The MICFIL is the i.MX93 PDM microphone front-end: it decimates PDM mic
 * streams into a FIFO that eDMA drains. This model carries the register file
 * the fsl-micfil driver needs to probe and the ASoC "micfil" card to register
 * (a correct VERID/PARAM and a self-clearing software-reset bit), and - with no
 * physical PDM mic wired - synthesises a captured sample stream into a data
 * FIFO so a real `arecord` reads non-silent audio: once the module is enabled
 * (CTRL1.PDMIEN, CTRL1.MDIS clear) it clocks samples in at the audio rate and,
 * with CTRL1.DISEL=DMA, requests an eDMA drain of DATACH0 as the FIFO fills.
 */

#ifndef IMX93_MICFIL_H
#define IMX93_MICFIL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_IMX93_MICFIL "imx93.micfil"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93MicfilState, IMX93_MICFIL)

#define IMX93_MICFIL_SIZE   0x10000
/* Registers run from CTRL1 (0x00) to VAD0_ZCD (0xa8). */
#define IMX93_MICFIL_REGS   (0xac / 4)
/* CTRL1.DISEL=IRQ / error / VAD share four interrupt lines. */
#define IMX93_MICFIL_IRQS   4
/* Deeper than the i.MX93 FIFO (32) so the fill can exceed the watermark. */
#define IMX93_MICFIL_FIFO_DEPTH 64

struct IMX93MicfilState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[IMX93_MICFIL_IRQS];
    qemu_irq dma_req;           /* FIFO-has-data request to the eDMA */
    uint32_t regs[IMX93_MICFIL_REGS];

    /* Synthesised capture FIFO drained via DATACH0. */
    QEMUTimer *rx_timer;
    uint32_t rx_fifo[IMX93_MICFIL_FIFO_DEPTH];
    uint32_t rx_rptr;
    uint32_t rx_wptr;
    uint32_t rx_count;          /* words currently in the FIFO */
    uint64_t rx_words;          /* total samples clocked in (drives waveform) */
};

#endif /* IMX93_MICFIL_H */
