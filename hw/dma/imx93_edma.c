/*
 * NXP i.MX 93 eDMA v3 (fsl,imx93-edma3) controller
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register/TCD layout follows the Linux fsl-edma driver (drivers/dma/
 * fsl-edma-common.h, imx93_data3). Channel N's page is at base + (N+1)*0x10000
 * and contains the channel control registers followed by a 32-byte TCD at
 * offset 0x20.
 *
 * A transfer is executed when the driver sets CH_CSR.ERQ. Real hardware paces
 * each minor loop off a peripheral DMA request; here we run the whole TCD at
 * once. To preserve ordering for peripherals that need command words pushed
 * before data is read back (e.g. the LPI2C: a TX channel writes RECV_DATA
 * commands to MTDR, then an RX channel drains MRDR), a memory->device (TX)
 * channel runs immediately and then drains any device->memory (RX) channel
 * that was armed first. Direction comes from CH_SBR (RD = rx, WR = tx).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/dma/imx93_edma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Per-channel control register offsets (struct fsl_edma3_ch_reg). */
#define CH_CSR      0x00
#define CH_ES       0x04
#define CH_INT      0x08
#define CH_SBR      0x0c
#define CH_PRI      0x10
#define CH_MUX      0x14
#define CH_MATTR    0x18
/* TCD (struct fsl_edma_hw_tcd) within the channel page. */
#define TCD_SADDR   0x20
#define TCD_SOFF    0x24
#define TCD_ATTR    0x26
#define TCD_NBYTES  0x28
#define TCD_SLAST   0x2c
#define TCD_DADDR   0x30
#define TCD_DOFF    0x34
#define TCD_CITER   0x36
#define TCD_DLAST   0x38
#define TCD_CSR     0x3c
#define TCD_BITER   0x3e

#define CH_CSR_ERQ      (1u << 0)
#define CH_CSR_EARQ     (1u << 1)
#define CH_CSR_EEI      (1u << 2)
#define CH_CSR_DONE     (1u << 30)
#define CH_CSR_ACTIVE   (1u << 31)

#define CH_SBR_WR       (1u << 21)      /* tx: memory -> device */
#define CH_SBR_RD       (1u << 22)      /* rx: device -> memory */

#define TCD_CSR_START   (1u << 0)
#define TCD_CSR_INTMAJ  (1u << 1)
#define TCD_CSR_INTHALF (1u << 2)
#define TCD_CSR_DREQ    (1u << 3)
#define TCD_CSR_ESG     (1u << 4)       /* enable scatter/gather (cyclic) */

#define ATTR_DSIZE(a)   ((a) & 0x7)
#define ATTR_SSIZE(a)   (((a) >> 8) & 0x7)
#define ITER_MASK       0x7fff

#define NBYTES_SMLOE    (1u << 31)      /* source minor-loop offset enable */
#define NBYTES_DMLOE    (1u << 30)      /* dest minor-loop offset enable   */

/*
 * Decode TCD_NBYTES. With SMLOE or DMLOE set (MLOFFYES format) the byte count
 * is only the low 10 bits and bits[29:10] are a signed minor-loop offset added
 * to SADDR (if SMLOE) and/or DADDR (if DMLOE) at each minor-loop end - the
 * way a peripheral walks several data registers (e.g. MICFIL DATACH0..n) then
 * rewinds. Without SMLOE/DMLOE (MLOFFNO) the whole low 30 bits are the count.
 */
static uint32_t edma_decode_nbytes(uint32_t raw, int32_t *mloff)
{
    if (raw & (NBYTES_SMLOE | NBYTES_DMLOE)) {
        int32_t off = (raw >> 10) & 0xfffff;    /* signed 20-bit field */

        if (off & 0x80000) {
            off |= ~0xfffff;
        }
        *mloff = off;
        return raw & 0x3ff;
    }
    *mloff = 0;
    return raw & 0x3fffffff;
}

static inline uint32_t ld32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t ld16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static inline void st32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* Execute a channel's TCD: move CITER * NBYTES bytes SADDR->DADDR. */
static void edma_run_channel(IMX93EdmaState *s, int ch)
{
    IMX93EdmaChan *c = &s->chan[ch];
    uint8_t *t = c->regs;
    uint64_t saddr = ld32(t + TCD_SADDR);
    uint64_t daddr = ld32(t + TCD_DADDR);
    int16_t soff = (int16_t)ld16(t + TCD_SOFF);
    int16_t doff = (int16_t)ld16(t + TCD_DOFF);
    uint16_t attr = ld16(t + TCD_ATTR);
    uint32_t raw_nbytes = ld32(t + TCD_NBYTES);
    int32_t mloff;
    uint32_t nbytes = edma_decode_nbytes(raw_nbytes, &mloff);
    uint16_t citer = ld16(t + TCD_CITER) & ITER_MASK;
    uint32_t ssize = 1u << ATTR_SSIZE(attr);
    uint32_t dsize = 1u << ATTR_DSIZE(attr);
    uint32_t esize = MAX(ssize, dsize);
    uint8_t buf[8];

    if (citer == 0) {
        citer = 1;
    }
    if (nbytes == 0 || esize == 0 || esize > sizeof(buf)) {
        return;
    }

    /*
     * Run the whole TCD: CITER minor loops, each moving NBYTES element by
     * element so that fixed peripheral addresses (SOFF or DOFF == 0) are hit
     * with the access width the device register expects, while the memory side
     * walks linearly. A minor-loop offset (MLOFF) rewinds the enabled side at
     * each minor-loop boundary.
     */
    for (uint16_t ml = 0; ml < citer; ml++) {
        for (uint64_t done = 0; done + esize <= nbytes; done += esize) {
            address_space_read(&address_space_memory, saddr,
                               MEMTXATTRS_UNSPECIFIED, buf, esize);
            address_space_write(&address_space_memory, daddr,
                                MEMTXATTRS_UNSPECIFIED, buf, esize);
            saddr += soff;
            daddr += doff;
        }
        if (raw_nbytes & NBYTES_SMLOE) {
            saddr += mloff;
        }
        if (raw_nbytes & NBYTES_DMLOE) {
            daddr += mloff;
        }
    }

    /* Completion: channel done, request disabled, interrupt pending. */
    st32(t + TCD_SADDR, (uint32_t)saddr);     /* harmless bookkeeping */
    st32(t + TCD_DADDR, (uint32_t)daddr);
    /* CITER reads back as 0 so the driver computes a full real count. */
    t[TCD_CITER] = 0;
    t[TCD_CITER + 1] = 0;

    uint32_t csr = ld32(t + CH_CSR);
    csr &= ~(CH_CSR_ERQ | CH_CSR_ACTIVE);
    csr |= CH_CSR_DONE;
    st32(t + CH_CSR, csr);

    st32(t + CH_INT, 1);
    if (ch < s->num_channels) {
        qemu_irq_raise(s->irq[ch]);
    }
}

/*
 * Service one minor loop of a cyclic (scatter/gather) channel - the way audio
 * playback runs: the peripheral (SAI) requests a burst as its FIFO drains, and
 * each request moves NBYTES from the ring buffer to the peripheral's data
 * register. At the end of a major loop (one period) the channel raises its
 * interrupt, reloads CITER, and follows DLAST_SGA to the next period's TCD -
 * looping forever until the driver clears ERQ. This keeps the transfer paced by
 * the peripheral instead of running the whole ring at once.
 */
static void edma_service_minor(IMX93EdmaState *s, int ch)
{
    IMX93EdmaChan *c = &s->chan[ch];
    uint8_t *t = c->regs;
    uint64_t saddr = ld32(t + TCD_SADDR);
    uint64_t daddr = ld32(t + TCD_DADDR);
    int16_t soff = (int16_t)ld16(t + TCD_SOFF);
    int16_t doff = (int16_t)ld16(t + TCD_DOFF);
    uint16_t attr = ld16(t + TCD_ATTR);
    uint32_t raw_nbytes = ld32(t + TCD_NBYTES);
    int32_t mloff;
    uint32_t nbytes = edma_decode_nbytes(raw_nbytes, &mloff);
    uint16_t citer = ld16(t + TCD_CITER) & ITER_MASK;
    uint16_t biter = ld16(t + TCD_BITER) & ITER_MASK;
    uint16_t csr = ld16(t + TCD_CSR);
    uint32_t ssize = 1u << ATTR_SSIZE(attr);
    uint32_t dsize = 1u << ATTR_DSIZE(attr);
    uint32_t esize = MAX(ssize, dsize);
    uint8_t buf[8];

    if (!(ld32(t + CH_CSR) & CH_CSR_ERQ)) {
        return;                 /* stream stopped between request and service */
    }
    if (nbytes == 0 || esize == 0 || esize > sizeof(buf)) {
        return;
    }

    /* One minor loop: NBYTES, element by element (peripheral side is fixed). */
    for (uint64_t done = 0; done + esize <= nbytes; done += esize) {
        address_space_read(&address_space_memory, saddr,
                           MEMTXATTRS_UNSPECIFIED, buf, esize);
        address_space_write(&address_space_memory, daddr,
                            MEMTXATTRS_UNSPECIFIED, buf, esize);
        saddr += soff;
        daddr += doff;
    }
    /*
     * Apply the minor-loop offset to whichever side enabled it: a peripheral
     * that walks several data registers per minor loop (MICFIL DATACH0..n) sets
     * SMLOE with a negative MLOFF to rewind SADDR back to the first register.
     */
    if (raw_nbytes & NBYTES_SMLOE) {
        saddr += mloff;
    }
    if (raw_nbytes & NBYTES_DMLOE) {
        daddr += mloff;
    }
    /*
     * Persist both pointers. The fixed (peripheral) side has off==0 so it is
     * unchanged; the advancing (memory) side must carry over to the next minor
     * loop. Saving only SADDR works for transmit (mem->FIFO) but loses the
     * destination for receive (FIFO->mem), where DADDR is the one that walks
     * the ring - without this every minor loop overwrites the same bytes.
     */
    st32(t + TCD_SADDR, (uint32_t)saddr);
    st32(t + TCD_DADDR, (uint32_t)daddr);

    if (citer > 1) {
        citer--;
        t[TCD_CITER] = citer;
        t[TCD_CITER + 1] = citer >> 8;
        return;
    }

    /* Major loop complete: one period delivered. */
    if (csr & TCD_CSR_INTMAJ) {
        st32(t + CH_INT, 1);
        if (ch < s->num_channels) {
            qemu_irq_raise(s->irq[ch]);
        }
    }
    if (csr & TCD_CSR_ESG) {
        /* Follow DLAST_SGA to the next period's TCD (32 bytes). */
        uint64_t next = ld32(t + TCD_DLAST);
        address_space_read(&address_space_memory, next, MEMTXATTRS_UNSPECIFIED,
                           t + TCD_SADDR, 0x20);
    } else {
        t[TCD_CITER] = biter;
        t[TCD_CITER + 1] = biter >> 8;
    }
}

/* A peripheral DMA request: advance the cyclic channel that serves it. */
static void edma_dma_request(void *opaque, int n, int level)
{
    IMX93EdmaState *s = opaque;
    int i;

    if (!level) {
        return;
    }
    for (i = 0; i < s->num_channels; i++) {
        IMX93EdmaChan *c = &s->chan[i];

        if (c->cyclic && (ld32(c->regs + CH_CSR) & CH_CSR_ERQ)) {
            edma_service_minor(s, i);
            return;
        }
    }
}

static void edma_drain_armed_rx(IMX93EdmaState *s)
{
    for (int i = 0; i < s->num_channels; i++) {
        if (s->chan[i].armed) {
            s->chan[i].armed = false;
            edma_run_channel(s, i);
        }
    }
}

static void edma_trace_tcd(IMX93EdmaState *s, int ch)
{
    IMX93EdmaChan *c = &s->chan[ch];
    uint8_t *t = c->regs;

    if (!getenv("EDMA_DBG")) {
        return;
    }
    fprintf(stderr, "[edma] ch%d ERQ sbr=0x%08x mux=0x%08x csr(tcd)=0x%04x "
            "saddr=0x%08x daddr=0x%08x soff=%d doff=%d nbytes=0x%x "
            "citer=%u biter=%u dlast=0x%08x\n", ch,
            ld32(t + CH_SBR), ld32(t + CH_MUX), ld16(t + TCD_CSR),
            ld32(t + TCD_SADDR), ld32(t + TCD_DADDR),
            (int16_t)ld16(t + TCD_SOFF), (int16_t)ld16(t + TCD_DOFF),
            ld32(t + TCD_NBYTES) & 0x3fffffff,
            ld16(t + TCD_CITER) & ITER_MASK, ld16(t + TCD_BITER) & ITER_MASK,
            ld32(t + TCD_DLAST));
}

/* CH_CSR was written with ERQ set: arm or run the channel. */
static void edma_trigger(IMX93EdmaState *s, int ch)
{
    IMX93EdmaChan *c = &s->chan[ch];
    uint32_t sbr = ld32(c->regs + CH_SBR);
    int16_t soff = (int16_t)ld16(c->regs + TCD_SOFF);
    bool is_rx;

    edma_trace_tcd(s, ch);

    /*
     * Scatter/gather (ESG) means a cyclic, peripheral-paced transfer (audio):
     * don't run it now, wait for the peripheral's DMA requests to drive it one
     * minor loop at a time. Non-SG transfers keep the run-it-now behaviour.
     */
    if (ld16(c->regs + TCD_CSR) & TCD_CSR_ESG) {
        c->cyclic = true;
        return;
    }
    c->cyclic = false;

    /* Prefer CH_SBR direction; fall back to "source offset fixed" = rx. */
    if (sbr & (CH_SBR_RD | CH_SBR_WR)) {
        is_rx = sbr & CH_SBR_RD;
    } else {
        is_rx = (soff == 0);
    }

    if (is_rx) {
        /* device -> memory: defer until the peripheral has data (a tx run). */
        c->armed = true;
    } else {
        /* memory -> device: push now, then let any armed rx channel drain. */
        edma_run_channel(s, ch);
        edma_drain_armed_rx(s);
    }
}

static int edma_channel_of(IMX93EdmaState *s, hwaddr offset)
{
    if (offset < IMX93_EDMA_CHAN_OFFSET) {
        return -1;
    }
    return (offset - IMX93_EDMA_CHAN_OFFSET) / s->chan_stride;
}

static uint64_t imx93_edma_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93EdmaState *s = opaque;
    int ch = edma_channel_of(s, offset);
    uint64_t val = 0;

    if (ch < 0) {
        uint32_t idx = offset >> 2;
        return idx < IMX93_EDMA_MGMT_REGS ? s->mgmt[idx] : 0;
    }
    if (ch >= s->num_channels) {
        return 0;
    }

    hwaddr coff = (offset - IMX93_EDMA_CHAN_OFFSET) % s->chan_stride;
    if (coff + size <= IMX93_EDMA_CHAN_REGS_SZ) {
        for (unsigned i = 0; i < size; i++) {
            val |= (uint64_t)s->chan[ch].regs[coff + i] << (8 * i);
        }
    }
    return val;
}

static void imx93_edma_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX93EdmaState *s = opaque;
    int ch = edma_channel_of(s, offset);

    if (ch < 0) {
        uint32_t idx = offset >> 2;
        if (idx < IMX93_EDMA_MGMT_REGS) {
            s->mgmt[idx] = value;
        }
        return;
    }
    if (ch >= s->num_channels) {
        return;
    }

    hwaddr coff = (offset - IMX93_EDMA_CHAN_OFFSET) % s->chan_stride;
    if (coff + size > IMX93_EDMA_CHAN_REGS_SZ) {
        return;
    }

    /* CH_INT is write-1-to-clear; also drops the channel interrupt line. */
    if (coff == CH_INT && size >= 4) {
        if (value & 1) {
            st32(s->chan[ch].regs + CH_INT, 0);
            qemu_irq_lower(s->irq[ch]);
        }
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        s->chan[ch].regs[coff + i] = (value >> (8 * i)) & 0xff;
    }

    /* Writing CH_CSR with ERQ set kicks the channel; clearing it stops it. */
    if (coff == CH_CSR) {
        uint32_t csr = ld32(s->chan[ch].regs + CH_CSR);
        if (csr & CH_CSR_ERQ) {
            edma_trigger(s, ch);
        } else {
            s->chan[ch].cyclic = false;
        }
    }
}

static const MemoryRegionOps imx93_edma_ops = {
    .read = imx93_edma_read,
    .write = imx93_edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void imx93_edma_reset(DeviceState *dev)
{
    IMX93EdmaState *s = IMX93_EDMA(dev);

    memset(s->mgmt, 0, sizeof(s->mgmt));
    for (int i = 0; i < IMX93_EDMA_MAX_CHANNELS; i++) {
        memset(s->chan[i].regs, 0, sizeof(s->chan[i].regs));
        s->chan[i].armed = false;
        s->chan[i].cyclic = false;
        if (i < s->num_channels) {
            qemu_irq_lower(s->irq[i]);
        }
    }
}

static void imx93_edma_realize(DeviceState *dev, Error **errp)
{
    IMX93EdmaState *s = IMX93_EDMA(dev);
    uint64_t region_sz;

    if (s->num_channels == 0 || s->num_channels > IMX93_EDMA_MAX_CHANNELS) {
        error_setg(errp, "imx93.edma3: invalid num-channels %u",
                   s->num_channels);
        return;
    }

    if (s->chan_stride < IMX93_EDMA_CHAN_REGS_SZ) {
        error_setg(errp, "imx93.edma3: invalid chan-stride 0x%x",
                   s->chan_stride);
        return;
    }

    region_sz = IMX93_EDMA_CHAN_OFFSET +
                (uint64_t)s->num_channels * s->chan_stride;
    memory_region_init_io(&s->iomem, OBJECT(dev), &imx93_edma_ops, s,
                          TYPE_IMX93_EDMA, region_sz);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    for (int i = 0; i < s->num_channels; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }

    /* Peripheral DMA request line (e.g. the SAI's FIFO-needs-data request). */
    qdev_init_gpio_in_named(dev, edma_dma_request, "dma-req", 1);
}

static const Property imx93_edma_properties[] = {
    DEFINE_PROP_UINT32("num-channels", IMX93EdmaState, num_channels, 31),
    DEFINE_PROP_UINT32("chan-stride", IMX93EdmaState, chan_stride,
                       IMX93_EDMA_CHAN_STRIDE),
};

static const VMStateDescription vmstate_imx93_edma_chan = {
    .name = "imx93.edma3.chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, IMX93EdmaChan, IMX93_EDMA_CHAN_REGS_SZ),
        VMSTATE_BOOL(armed, IMX93EdmaChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_imx93_edma = {
    .name = TYPE_IMX93_EDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(mgmt, IMX93EdmaState, IMX93_EDMA_MGMT_REGS),
        VMSTATE_STRUCT_ARRAY(chan, IMX93EdmaState, IMX93_EDMA_MAX_CHANNELS, 1,
                             vmstate_imx93_edma_chan, IMX93EdmaChan),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx93_edma_realize;
    dc->vmsd = &vmstate_imx93_edma;
    device_class_set_props(dc, imx93_edma_properties);
    device_class_set_legacy_reset(dc, imx93_edma_reset);
    dc->desc = "i.MX93 eDMA v3 controller";
}

static const TypeInfo imx93_edma_types[] = {
    {
        .name = TYPE_IMX93_EDMA,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93EdmaState),
        .class_init = imx93_edma_class_init,
    },
};

DEFINE_TYPES(imx93_edma_types)
