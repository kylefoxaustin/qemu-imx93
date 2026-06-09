/*
 * NXP i.MX 93 Audio Transceiver (XCVR / SPDIF) - registration model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the XCVR control registers so the fsl_xcvr driver probes and
 * registers its SPDIF card, plus a functional transmit FIFO. The i.MX93 XCVR is
 * SPDIF-only and firmware-free (soc_data has no fw_name/PHY), so once the
 * driver releases the TX datapath (EXT_CTRL.TX_DPTH_RESET clear) with DMA-read
 * enabled, words the eDMA writes to TX_FIFO (0xe00) are clocked out at the
 * audio word rate and a dma-req is pulsed as the FIFO drains past the TX
 * watermark - the same cyclic playback contract the SAI uses. Played samples
 * go to the audio backend.
 */

#ifndef HW_AUDIO_IMX93_XCVR_H
#define HW_AUDIO_IMX93_XCVR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "qemu/audio.h"

#define TYPE_IMX93_XCVR "imx93.xcvr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93XcvrState, IMX93_XCVR)

#define IMX93_XCVR_SIZE     0x10000
#define IMX93_XCVR_REG_OFF  0x800            /* control registers start here */
#define IMX93_XCVR_NUM_REGS (0x400 / 4)      /* control register window */
#define IMX93_XCVR_RAM_SIZE 0x800            /* firmware RAM below the regs */
#define IMX93_XCVR_FIFO_DEPTH 128
#define IMX93_XCVR_CAP_SIZE   16384

struct IMX93XcvrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_req;           /* TX FIFO-needs-data request to the eDMA */
    uint32_t regs[IMX93_XCVR_NUM_REGS];
    uint8_t ram[IMX93_XCVR_RAM_SIZE];
    uint32_t ai_sub[256];   /* PHY/PLL sub-registers via the AI interface */

    /* Transmit FIFO (SPDIF playback). */
    QEMUTimer *tx_timer;
    uint32_t tx_fifo[IMX93_XCVR_FIFO_DEPTH];
    uint32_t tx_rptr;
    uint32_t tx_wptr;
    uint32_t tx_count;
    uint64_t tx_words;

    /* Audio backend: clocked-out samples go to an -audiodev (e.g. wav). */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool      voice_active;
    uint8_t   cap[IMX93_XCVR_CAP_SIZE];
    uint32_t  cap_head;
    uint32_t  cap_count;
};

#endif /* HW_AUDIO_IMX93_XCVR_H */
