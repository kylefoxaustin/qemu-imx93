/*
 * NXP i.MX 93 ISI (Image Sensing Interface) - capture channel
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional capture model for the imx8-isi V4L2 driver. The i.MX 93 ISI
 * normally pulls pixels from the parallel-CSI / sensor and DMAs frames to the
 * driver's queued capture buffers. There is no real sensor data here (the
 * MT9M114 model only answers I2C register reads), so on CHNL_EN the ISI
 * synthesises a moving test pattern and DMAs it into the ping-pong output
 * buffers (CHNL_OUT_BUF1/2_ADDR_Y) at the programmed pitch, raising the
 * frame-stored interrupt (GIC SPI 172) per frame. The driver's ISR drains the
 * buffer to the V4L2 queue so a capture client (e.g. v4l2 REQBUFS/STREAMON/
 * DQBUF) gets real frames. Set ISI_DBG to trace register accesses; the exact
 * register/bit layout was captured from the live driver (see
 * tests/camera-imx93).
 *
 * Virtual camera: the "frames" property points the ISI at a host path (a
 * directory of *.raw frames or a file of back-to-back raw frames) to feed real
 * images through the capture pipeline instead of the test pattern - frames are
 * read straight from the host at each tick and scanned out, cycling/looping.
 * See tests/camera-imx93/README.md.
 */

#include "qemu/osdep.h"
#include "hw/display/imx93_isi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include <glob.h>

/*
 * imx8-isi channel-0 register map (per the mainline fsl,imx93-isi driver,
 * confirmed against a live STREAMON capture - see tests/camera-imx93).
 */
#define CHNL_CTRL               0x0000
#define   CHNL_CTRL_CHNL_EN     0x80000000  /* BIT(31) */
#define CHNL_IMG_CTRL           0x0004
#define CHNL_IMG_CFG            0x000c      /* (height << 16) | width */
#define   CHNL_IMG_CFG_W_MASK   0x00001fff
#define   CHNL_IMG_CFG_H_SHIFT  16
#define   CHNL_IMG_CFG_H_MASK   0x1fff
#define CHNL_IER                0x0010
#define CHNL_STS                0x0014
#define   CHNL_STS_FRM_STRD     0x20000000  /* BIT(29) frame stored */
#define   CHNL_STS_BUF1_ACTIVE  0x00000100  /* BIT(8) */
#define   CHNL_STS_BUF2_ACTIVE  0x00000200  /* BIT(9) */
#define CHNL_OUT_BUF1_ADDR_Y    0x0070
#define CHNL_OUT_BUF_PITCH      0x007c
#define   CHNL_OUT_BUF_PITCH_MASK 0x0000ffff
#define CHNL_OUT_BUF2_ADDR_Y    0x008c

#define R(s, off)               ((s)->regs[(off) / 4])

/* ~30 fps frame cadence. */
#define FRAME_PERIOD_NS         (NANOSECONDS_PER_SECOND / 30)

static void imx93_isi_trace(const char *op, hwaddr offset, uint32_t value)
{
    if (getenv("ISI_DBG")) {
        fprintf(stderr, "[isi] %s +0x%04x = 0x%08x\n", op,
                (unsigned)offset, value);
    }
}

static void imx93_isi_update_irq(IMX93IsiState *s)
{
    uint32_t pending = R(s, CHNL_STS) & R(s, CHNL_IER);
    qemu_set_irq(s->irq, pending ? 1 : 0);
}

static int imx93_isi_pathcmp(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

/*
 * Host frame source. The "frames" property points at either a single file of
 * back-to-back raw frames or a directory of *.raw frames; open it and, for a
 * directory, build a sorted list. Returns true if a source is configured.
 */
static bool imx93_isi_frames_open(IMX93IsiState *s, Error **errp)
{
    struct stat st;

    if (!s->frames_path) {
        return false;
    }
    if (stat(s->frames_path, &st) != 0) {
        error_setg_errno(errp, errno, "ISI: cannot stat frames path '%s'",
                         s->frames_path);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        g_autofree char *pat = g_strdup_printf("%s/*.raw", s->frames_path);
        glob_t gl;

        if (glob(pat, GLOB_NOSORT, NULL, &gl) != 0 || gl.gl_pathc == 0) {
            globfree(&gl);
            error_setg(errp, "ISI: no *.raw frames in directory '%s'",
                       s->frames_path);
            return false;
        }
        s->n_frame_files = gl.gl_pathc;
        s->frame_files = g_new0(char *, s->n_frame_files);
        for (int i = 0; i < s->n_frame_files; i++) {
            s->frame_files[i] = g_strdup(gl.gl_pathv[i]);
        }
        globfree(&gl);
        /* sort so frame order is deterministic (frame000.raw, frame001.raw…) */
        qsort(s->frame_files, s->n_frame_files, sizeof(char *),
              imx93_isi_pathcmp);
    } else {
        s->frame_fp = fopen(s->frames_path, "rb");
        if (!s->frame_fp) {
            error_setg_errno(errp, errno, "ISI: cannot open frames file '%s'",
                             s->frames_path);
            return false;
        }
    }
    return true;
}

/*
 * Read the next frame (fsz bytes) from the host source into frame_buf, cycling
 * through the sequence and looping at the end. Returns true on success.
 */
static bool imx93_isi_next_frame(IMX93IsiState *s, size_t fsz)
{
    if (s->frame_buf_size != fsz) {
        s->frame_buf = g_realloc(s->frame_buf, fsz);
        s->frame_buf_size = fsz;
    }

    if (s->n_frame_files > 0) {                 /* directory of frame files */
        const char *path = s->frame_files[s->frame_index];
        gsize len = 0;
        g_autofree char *data = NULL;

        s->frame_index = (s->frame_index + 1) % s->n_frame_files;
        if (!g_file_get_contents(path, &data, &len, NULL) || len < fsz) {
            return false;
        }
        memcpy(s->frame_buf, data, fsz);
        return true;
    }

    if (s->frame_fp) {                          /* single multi-frame file */
        size_t got = fread(s->frame_buf, 1, fsz, s->frame_fp);

        if (got < fsz) {                        /* short read: loop to start */
            rewind(s->frame_fp);
            got += fread(s->frame_buf + got, 1, fsz - got, s->frame_fp);
        }
        return got == fsz;
    }
    return false;
}

/*
 * Generate one moving test-pattern frame into the ping-pong output buffer the
 * hardware would be filling, then raise the frame-stored interrupt.
 *
 * The mxc-isi driver double-buffers: BUF1 (0x70) and BUF2 (0x8c). Frame N is
 * written to BUF1 on even N and BUF2 on odd N. The ISR reads CHNL_STS to learn
 * which buffer just completed: with the i.MX93 quirk buf_active_reverse=true it
 * computes "completed = (STS & BUF1_ACTIVE) ? BUF2 : BUF1", where the ACTIVE
 * bit names the *next* buffer the hardware switches to. So after filling BUF1
 * we flag BUF2 active (BUF1_ACTIVE clear); after BUF2 we flag BUF1 active. The
 * driver re-programs the just-freed slot address each IRQ, so we re-read the
 * buffer address from the register every frame.
 */
static void imx93_isi_frame_tick(void *opaque)
{
    IMX93IsiState *s = opaque;
    uint32_t cfg = R(s, CHNL_IMG_CFG);
    uint32_t width = cfg & CHNL_IMG_CFG_W_MASK;
    uint32_t height = (cfg >> CHNL_IMG_CFG_H_SHIFT) & CHNL_IMG_CFG_H_MASK;
    uint32_t pitch = R(s, CHNL_OUT_BUF_PITCH) & CHNL_OUT_BUF_PITCH_MASK;
    bool host_src = s->frames_path != NULL;
    bool have_host_frame = false;
    bool buf2 = s->frame & 1;
    uint64_t buf = buf2 ? R(s, CHNL_OUT_BUF2_ADDR_Y)
                        : R(s, CHNL_OUT_BUF1_ADDR_Y);
    uint32_t bpp, x, y;
    g_autofree uint8_t *line = NULL;

    if (!(R(s, CHNL_CTRL) & CHNL_CTRL_CHNL_EN)) {
        return;
    }

    /*
     * With a host frame source, read the next frame from the file/directory and
     * scan it out instead of the synthetic moving test pattern. The frame is
     * raw, packed width*height*bpp in the negotiated output format. With no
     * source configured, fall back to the gradient (unchanged default).
     */
    if (host_src && width && height && pitch) {
        bpp = pitch / width ? pitch / width : 4;
        have_host_frame = imx93_isi_next_frame(s, (size_t)width * height * bpp);
    }

    if (buf && width && height && pitch) {
        bpp = pitch / width ? pitch / width : 4;
        line = g_malloc((size_t)width * bpp);
        for (y = 0; y < height; y++) {
            if (have_host_frame) {
                /* packed source row from the host-supplied frame */
                memcpy(line, s->frame_buf + (size_t)y * width * bpp,
                       (size_t)width * bpp);
            } else {
                for (x = 0; x < width; x++) {
                    /* moving diagonal gradient so consecutive frames differ */
                    uint32_t v = (x + y + s->frame * 4) & 0xff;
                    uint32_t px = 0xff000000u | (v << 16) | (v << 8) | v;
                    memcpy(line + (size_t)x * bpp, &px, bpp < 4 ? bpp : 4);
                }
            }
            dma_memory_write(&address_space_memory, buf + (uint64_t)y * pitch,
                             line, (size_t)width * bpp, MEMTXATTRS_UNSPECIFIED);
        }
    }

    /* Flag the buffer the hardware switches to next, then set frame-stored. */
    R(s, CHNL_STS) &= ~(CHNL_STS_BUF1_ACTIVE | CHNL_STS_BUF2_ACTIVE);
    R(s, CHNL_STS) |= buf2 ? CHNL_STS_BUF1_ACTIVE : CHNL_STS_BUF2_ACTIVE;
    R(s, CHNL_STS) |= CHNL_STS_FRM_STRD;
    s->frame++;
    imx93_isi_update_irq(s);

    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
}

static uint64_t imx93_isi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93IsiState *s = opaque;
    uint32_t val = s->regs[offset / 4];

    imx93_isi_trace("rd", offset, val);
    return val;
}

static void imx93_isi_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93IsiState *s = opaque;
    uint32_t old;

    imx93_isi_trace("wr", offset, (uint32_t)value);

    if (offset == CHNL_STS) {
        /* Write-1-to-clear status; drop the IRQ when the driver acks. */
        s->regs[offset / 4] &= ~(uint32_t)value;
        imx93_isi_update_irq(s);
        return;
    }

    old = s->regs[offset / 4];
    s->regs[offset / 4] = value;

    if (offset == CHNL_CTRL) {
        if ((value & CHNL_CTRL_CHNL_EN) && !(old & CHNL_CTRL_CHNL_EN)) {
            /* Stream on: start delivering frames. */
            s->frame = 0;
            timer_mod(s->frame_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
        } else if (!(value & CHNL_CTRL_CHNL_EN)) {
            timer_del(s->frame_timer);
            qemu_set_irq(s->irq, 0);
        }
    } else if (offset == CHNL_IER) {
        imx93_isi_update_irq(s);
    }
}

static const MemoryRegionOps imx93_isi_ops = {
    .read = imx93_isi_read,
    .write = imx93_isi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_isi_reset(DeviceState *dev)
{
    IMX93IsiState *s = IMX93_ISI(dev);

    timer_del(s->frame_timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->frame = 0;
    s->frame_index = 0;
    if (s->frame_fp) {
        rewind(s->frame_fp);
    }
    qemu_set_irq(s->irq, 0);
}

static void imx93_isi_init(Object *obj)
{
    IMX93IsiState *s = IMX93_ISI(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_isi_ops, s,
                          TYPE_IMX93_ISI, IMX93_ISI_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void imx93_isi_realize(DeviceState *dev, Error **errp)
{
    IMX93IsiState *s = IMX93_ISI(dev);

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx93_isi_frame_tick, s);

    /* Open the host frame source, if the "frames" property was given. */
    if (!imx93_isi_frames_open(s, errp) && *errp) {
        return;
    }
}

static void imx93_isi_finalize(Object *obj)
{
    IMX93IsiState *s = IMX93_ISI(obj);

    if (s->frame_fp) {
        fclose(s->frame_fp);
    }
    for (int i = 0; i < s->n_frame_files; i++) {
        g_free(s->frame_files[i]);
    }
    g_free(s->frame_files);
    g_free(s->frame_buf);
    g_free(s->frames_path);
}

static const Property imx93_isi_properties[] = {
    DEFINE_PROP_STRING("frames", IMX93IsiState, frames_path),
};

static const VMStateDescription vmstate_imx93_isi = {
    .name = TYPE_IMX93_ISI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(frame, IMX93IsiState),
        VMSTATE_UINT32_ARRAY(regs, IMX93IsiState, IMX93_ISI_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_isi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 ISI (Image Sensing Interface)";
    dc->realize = imx93_isi_realize;
    device_class_set_legacy_reset(dc, imx93_isi_reset);
    dc->vmsd = &vmstate_imx93_isi;
    device_class_set_props(dc, imx93_isi_properties);
}

static const TypeInfo imx93_isi_types[] = {
    {
        .name           = TYPE_IMX93_ISI,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93IsiState),
        .instance_init  = imx93_isi_init,
        .instance_finalize = imx93_isi_finalize,
        .class_init     = imx93_isi_class_init,
    },
};

DEFINE_TYPES(imx93_isi_types)
