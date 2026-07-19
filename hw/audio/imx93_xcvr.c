/*
 * NXP i.MX 93 Audio Transceiver (XCVR / SPDIF) - registration model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The control registers (at offset 0x800) use the i.MX SET/CLR/TOG alias
 * pattern: each register R is also writable at R+4 (set bits), R+8 (clear bits)
 * and R+0xC (toggle bits). The PHY/PLL sub-registers are reached through an
 * indirect "AI" interface (PHY_AI_CTRL/WDATA/RDATA): a toggle of the PLL/PHY
 * bit triggers an access the hardware acknowledges by setting the matching DONE
 * bit, which the driver polls. Modelling these lets the fsl_xcvr driver probe
 * and register its SPDIF card; the firmware-driven audio datapath is not run.
 */

#include "qemu/osdep.h"
#include "hw/audio/imx93_xcvr.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define XCVR_VERSION        0x00
#define XCVR_PHY_AI_CTRL    0x90
#define XCVR_PHY_AI_WDATA   0xa0
#define XCVR_PHY_AI_RDATA   0xa4

#define AI_CTRL_RWB         (1u << 31)
#define AI_TOG_PLL          (1u << 24)
#define AI_DONE_PLL         (1u << 25)
#define AI_TOG_PHY          (1u << 26)
#define AI_DONE_PHY         (1u << 27)

#define XCVR_VERSION_VALUE  0x00010000

#define RFDR_FIFO 0x0c00
#define TFDR_FIFO 0x0e00

/*
 * EXT_CTRL lives at MMIO 0x810 = REG_OFF(0x800) + 0x10, so its index into the
 * control-register window is 0x10/4. Its bits gate the transmit datapath.
 */
#define XCVR_EXT_CTRL_REL       0x10
#define EXT_CTRL_TX_DPTH_RESET  (1u << 27)  /* TX datapath in reset        */
#define EXT_CTRL_DMA_WR_DIS     (1u << 24)  /* DMA disable, TX direction    */
#define EXT_CTRL_SPDIF_MODE     (1u << 23)  /* SPDIF mode selected          */
#define EXT_CTRL_TX_FWM_MASK    0x7f        /* TX FIFO watermark [6:0]     */

/* SPDIF stereo: 2 channels per frame. */
#define XCVR_TX_CHANNELS    2
/*
 * Ratio between the CCM spdif_root clock and the SPDIF sample rate Fs, i.e.
 * spdif_root = Fs * XCVR_SPDIF_RATIO, so Fs = spdif_root / XCVR_SPDIF_RATIO.
 *
 * MEASURED on the guest (clk_summary, with an IEC958 stream actually running):
 * the fsl_xcvr driver sets spdif_root to 6.144 MHz for a 48 kHz stream and
 * 4.096 MHz for 32 kHz - a ratio of 128 in both cases. 128 is the SPDIF biphase
 * clock: 64 bits per frame (two 32-bit subframes) x 2 transitions per bit for
 * biphase-mark. Note the idle root reads 12.288 MHz (256 x 48 kHz); the driver
 * halves it once a stream prepares, which is why the ratio has to be read off a
 * running stream and not the reset value.
 */
#define XCVR_SPDIF_RATIO    128

/*
 * SPDIF sample rate, derived from the CCM's spdif_root rather than a hardcoded
 * 48 kHz. Falls back to 48 kHz (logged once) if the CCM has not routed a real
 * clock, so the word timer never gets a zero period.
 */
static uint64_t xcvr_spdif_rate(IMX93XcvrState *s)
{
    uint64_t hz = s->spdif_clk ? clock_get_hz(s->spdif_clk) : 0;
    uint64_t rate = hz / XCVR_SPDIF_RATIO;

    if (rate == 0) {
        if (!s->warned_no_clock) {
            s->warned_no_clock = true;
            qemu_log_mask(LOG_GUEST_ERROR, "imx93-xcvr: no SPDIF clock from the "
                          "CCM; pacing at 48 kHz\n");
        }
        rate = 48000;
    }
    return rate;
}

/* Nanoseconds between clocked-out words: 1 / (Fs * channels). */
static int64_t xcvr_tx_word_ns(IMX93XcvrState *s)
{
    return NANOSECONDS_PER_SECOND /
           (int64_t)(xcvr_spdif_rate(s) * XCVR_TX_CHANNELS);
}

/* TX clocks once SPDIF mode is on, the datapath released and DMA enabled. */
static bool xcvr_tx_active(IMX93XcvrState *s)
{
    uint32_t ec = s->regs[XCVR_EXT_CTRL_REL >> 2];

    return (ec & EXT_CTRL_SPDIF_MODE) && !(ec & EXT_CTRL_TX_DPTH_RESET) &&
           !(ec & EXT_CTRL_DMA_WR_DIS);
}

/* Queue clocked-out bytes for the audio backend. */
static void xcvr_cap_push(IMX93XcvrState *s, uint32_t value)
{
    unsigned i;

    for (i = 0; i < 4 && s->cap_count < IMX93_XCVR_CAP_SIZE; i++) {
        uint32_t tail = (s->cap_head + s->cap_count) % IMX93_XCVR_CAP_SIZE;
        s->cap[tail] = (value >> (8 * i)) & 0xff;
        s->cap_count++;
    }
}

static void xcvr_audio_cb(void *opaque, int free)
{
    IMX93XcvrState *s = opaque;

    while (free > 0 && s->cap_count > 0) {
        uint32_t chunk = MIN((uint32_t)free, s->cap_count);
        size_t wrote;

        chunk = MIN(chunk, IMX93_XCVR_CAP_SIZE - s->cap_head);
        wrote = audio_be_write(s->audio_be, s->voice, s->cap + s->cap_head,
                               chunk);
        if (wrote == 0) {
            break;
        }
        s->cap_head = (s->cap_head + wrote) % IMX93_XCVR_CAP_SIZE;
        s->cap_count -= wrote;
        free -= wrote;
    }
}

static void xcvr_voice_set(IMX93XcvrState *s, bool on)
{
    if (s->voice && on != s->voice_active) {
        audio_be_set_active_out(s->audio_be, s->voice, on);
        s->voice_active = on;
    }
}

static void xcvr_tx_push(IMX93XcvrState *s, uint32_t word)
{
    if (s->tx_count < IMX93_XCVR_FIFO_DEPTH) {
        s->tx_fifo[s->tx_wptr] = word;
        s->tx_wptr = (s->tx_wptr + 1) % IMX93_XCVR_FIFO_DEPTH;
        s->tx_count++;
    }
}

/* Clock one word out of the transmit FIFO at the audio word rate. */
static void xcvr_tx_tick(void *opaque)
{
    IMX93XcvrState *s = opaque;
    uint32_t watermark = s->regs[XCVR_EXT_CTRL_REL >> 2] & EXT_CTRL_TX_FWM_MASK;

    if (!xcvr_tx_active(s)) {
        return;
    }
    if (s->tx_count > 0) {
        s->tx_rptr = (s->tx_rptr + 1) % IMX93_XCVR_FIFO_DEPTH;
        s->tx_count--;
        s->tx_words++;
    }
    /* As the FIFO drains past the watermark, request the next eDMA burst. */
    if (s->tx_count <= watermark) {
        qemu_irq_pulse(s->dma_req);
    }
    timer_mod(s->tx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + xcvr_tx_word_ns(s));
}

/* Perform the indirect PHY/PLL access and acknowledge it via the DONE bits. */
static void xcvr_ai_complete(IMX93XcvrState *s)
{
    uint32_t ctrl = s->regs[XCVR_PHY_AI_CTRL >> 2];
    uint8_t addr = ctrl & 0xff;

    if (ctrl & AI_CTRL_RWB) {                       /* read */
        s->regs[XCVR_PHY_AI_RDATA >> 2] = s->ai_sub[addr];
    } else {                                        /* write */
        s->ai_sub[addr] = s->regs[XCVR_PHY_AI_WDATA >> 2];
    }
    /* DONE bit follows the TOG bit so the driver's poll completes. */
    ctrl &= ~(AI_DONE_PLL | AI_DONE_PHY);
    ctrl |= (ctrl & AI_TOG_PLL) ? AI_DONE_PLL : 0;
    ctrl |= (ctrl & AI_TOG_PHY) ? AI_DONE_PHY : 0;
    s->regs[XCVR_PHY_AI_CTRL >> 2] = ctrl;
}

static uint64_t xcvr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93XcvrState *s = opaque;
    uint32_t rel, sub;

    if (getenv("XCVR_DBG") && offset != RFDR_FIFO && offset != TFDR_FIFO) {
        fprintf(stderr, "[xcvr] RD off=0x%04x sz=%u\n",
                (unsigned)offset, size);
    }

    if (offset < IMX93_XCVR_REG_OFF) {
        uint32_t v = 0, b;

        for (b = 0; b < size && offset + b < IMX93_XCVR_RAM_SIZE; b++) {
            v |= (uint32_t)s->ram[offset + b] << (b * 8);
        }
        return v;
    }
    if (offset == RFDR_FIFO || offset == TFDR_FIFO) {
        return 0;
    }
    rel = offset - IMX93_XCVR_REG_OFF;
    if ((rel >> 2) >= IMX93_XCVR_NUM_REGS) {
        return 0;
    }
    sub = rel & 0xf;
    /* SET/CLR/TOG aliases read back as the base register (except AI RDATA). */
    if (rel != XCVR_PHY_AI_RDATA && rel != XCVR_PHY_AI_WDATA &&
        (sub == 4 || sub == 8 || sub == 0xc)) {
        return s->regs[(rel & ~0xf) >> 2];
    }
    return s->regs[rel >> 2];
}

static void xcvr_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    IMX93XcvrState *s = opaque;
    uint32_t rel, sub, base;

    if (getenv("XCVR_DBG")) {
        fprintf(stderr, "[xcvr] WR off=0x%04x sz=%u val=0x%08x\n",
                (unsigned)offset, size, (uint32_t)value);
    }

    if (offset < IMX93_XCVR_REG_OFF) {
        uint32_t b;

        for (b = 0; b < size && offset + b < IMX93_XCVR_RAM_SIZE; b++) {
            s->ram[offset + b] = (value >> (b * 8)) & 0xff;
        }
        return;
    }
    if (offset == TFDR_FIFO) {
        /* The eDMA writes S32 SPDIF samples here; enqueue + hand to backend. */
        xcvr_tx_push(s, (uint32_t)value);
        xcvr_cap_push(s, (uint32_t)value);
        return;
    }
    if (offset == RFDR_FIFO) {
        return;
    }
    rel = offset - IMX93_XCVR_REG_OFF;
    if ((rel >> 2) >= IMX93_XCVR_NUM_REGS) {
        return;
    }

    /* WDATA/RDATA are distinct registers, not SET/CLR aliases. */
    if (rel == XCVR_PHY_AI_WDATA || rel == XCVR_PHY_AI_RDATA) {
        s->regs[rel >> 2] = value;
        return;
    }

    sub = rel & 0xf;
    base = (rel & ~0xf) >> 2;
    switch (sub) {
    case 4:
        s->regs[base] |= value;     /* SET */
        break;
    case 8:
        s->regs[base] &= ~value;    /* CLR */
        break;
    case 0xc:
        s->regs[base] ^= value;     /* TOG */
        break;
    default:
        s->regs[rel >> 2] = value;
        base = rel >> 2;
        break;
    }

    /* An AI toggle triggers the indirect access. */
    if (base == (XCVR_PHY_AI_CTRL >> 2)) {
        xcvr_ai_complete(s);
    }

    /* EXT_CTRL gates the transmit datapath: start/stop clocking on a change. */
    if (base == (XCVR_EXT_CTRL_REL >> 2)) {
        bool active = xcvr_tx_active(s);

        if (active && !timer_pending(s->tx_timer)) {
            uint64_t hz = s->spdif_clk ? clock_get_hz(s->spdif_clk) : 0;

            xcvr_voice_set(s, true);
            /* Report the rate we derived from spdif_root as TX starts (the same
             * value the word timer paces at), so a harness can check it tracks
             * the requested Fs instead of a constant. */
            trace_imx93_xcvr_tx_start(hz, xcvr_spdif_rate(s));
            timer_mod(s->tx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + xcvr_tx_word_ns(s));
        } else if (!active && timer_pending(s->tx_timer)) {
            timer_del(s->tx_timer);
            xcvr_voice_set(s, false);
        }
    }
}

static const MemoryRegionOps xcvr_ops = {
    .read = xcvr_read,
    .write = xcvr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void xcvr_reset(DeviceState *dev)
{
    IMX93XcvrState *s = IMX93_XCVR(dev);

    timer_del(s->tx_timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ram, 0, sizeof(s->ram));
    memset(s->ai_sub, 0, sizeof(s->ai_sub));
    s->regs[XCVR_VERSION >> 2] = XCVR_VERSION_VALUE;
    s->tx_rptr = s->tx_wptr = s->tx_count = 0;
    s->tx_words = 0;
    s->cap_head = s->cap_count = 0;
    xcvr_voice_set(s, false);
}

static void xcvr_init(Object *obj)
{
    IMX93XcvrState *s = IMX93_XCVR(obj);

    /*
     * The SPDIF sample-rate clock is an input driven by the CCM's spdif_root.
     * It must exist before the board wires it, so create it in instance_init -
     * qdev_connect_clock_in() asserts the device is not yet realized.
     */
    s->spdif_clk = qdev_init_clock_in(DEVICE(obj), "spdif_clk", NULL, NULL, 0);
}

static void xcvr_realize(DeviceState *dev, Error **errp)
{
    IMX93XcvrState *s = IMX93_XCVR(dev);
    struct audsettings as = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S32,
        .big_endian = false,
    };

    memory_region_init_io(&s->iomem, OBJECT(dev), &xcvr_ops, s,
                          TYPE_IMX93_XCVR, IMX93_XCVR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_req, "dma-req", 1);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, xcvr_tx_tick, s);

    /* Best-effort audio backend: -audio captures the SPDIF playback. */
    if (audio_be_check(&s->audio_be, NULL)) {
        s->voice = audio_be_open_out(s->audio_be, NULL, "imx93-xcvr-tx", s,
                                     xcvr_audio_cb, &as);
    }
}

static const VMStateDescription vmstate_xcvr = {
    .name = TYPE_IMX93_XCVR,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93XcvrState, IMX93_XCVR_NUM_REGS),
        VMSTATE_UINT8_ARRAY(ram, IMX93XcvrState, IMX93_XCVR_RAM_SIZE),
        VMSTATE_UINT32_ARRAY(ai_sub, IMX93XcvrState, 256),
        VMSTATE_UINT32_ARRAY(tx_fifo, IMX93XcvrState, IMX93_XCVR_FIFO_DEPTH),
        VMSTATE_UINT32(tx_rptr, IMX93XcvrState),
        VMSTATE_UINT32(tx_wptr, IMX93XcvrState),
        VMSTATE_UINT32(tx_count, IMX93XcvrState),
        VMSTATE_UINT64(tx_words, IMX93XcvrState),
        VMSTATE_END_OF_LIST()
    },
};

static void xcvr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xcvr_realize;
    dc->vmsd = &vmstate_xcvr;
    device_class_set_legacy_reset(dc, xcvr_reset);
    dc->desc = "i.MX93 audio transceiver (SPDIF)";
}

static const TypeInfo xcvr_types[] = {
    {
        .name = TYPE_IMX93_XCVR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93XcvrState),
        .instance_init = xcvr_init,
        .class_init = xcvr_class_init,
    },
};

DEFINE_TYPES(xcvr_types)
