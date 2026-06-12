/*
 * NXP i.MX 93 ISI (Image Sensing Interface) - capture channel
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IMX93_ISI_H
#define IMX93_ISI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_ISI "imx93.isi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93IsiState, IMX93_ISI)

#define IMX93_ISI_REG_SIZE      (64 * KiB)
#define IMX93_ISI_NUM_REGS      (IMX93_ISI_REG_SIZE / 4)

struct IMX93IsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* WAKEUPMIX ISI interrupt (GIC SPI 172) */
    QEMUTimer *frame_timer;
    uint32_t frame;             /* frame counter; parity selects BUF1/BUF2 */
    uint32_t regs[IMX93_ISI_NUM_REGS];

    /*
     * Optional host frame source - a "virtual camera". When the "frames"
     * property points at a host path, captured frames are read from it (raw,
     * packed width*height*bpp in the negotiated output format) instead of the
     * synthetic test pattern, so a user can feed a series of real images into
     * the CSI/ISI pipeline with no sensor. The path may be a single file of
     * back-to-back frames or a directory of *.raw frames; either way the model
     * cycles through them, looping at the end. Read directly at each frame
     * tick (whole-frame reads - no streaming/flow-control). Not migrated.
     */
    char    *frames_path;       /* "frames" property: file or directory */
    char   **frame_files;       /* sorted *.raw paths when path is a dir */
    int      n_frame_files;     /* entries in frame_files (0 => single file) */
    int      frame_index;       /* next frame_files[] entry to read */
    FILE    *frame_fp;          /* open single-file source (n_frame_files==0) */
    uint8_t *frame_buf;         /* scratch buffer for the frame being read */
    size_t   frame_buf_size;    /* allocation size of frame_buf */
};

#endif /* IMX93_ISI_H */
