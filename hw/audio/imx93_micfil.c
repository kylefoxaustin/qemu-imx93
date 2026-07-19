/*
 * NXP i.MX 93 MICFIL (PDM microphone interface)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/imx93_micfil.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

/* Register map */
#define MICFIL_CTRL1    0x00    /* Control 1                */
#define MICFIL_CTRL2    0x04    /* Control 2                */
#define MICFIL_STAT     0x08    /* Status (read-only here)  */
#define MICFIL_FIFO_CTRL 0x10   /* FIFO control (watermark) */
#define MICFIL_FIFO_STAT 0x14   /* FIFO status              */
#define MICFIL_DATACH0  0x24    /* Channel 0 data (FIFO pop) */
#define MICFIL_DATACH7  0x40    /* Channel 7 data           */
#define MICFIL_VERID    0x84    /* Version ID (read-only)   */
#define MICFIL_PARAM    0x88    /* Parameter   (read-only)  */

#define MICFIL_CTRL1_MDIS       (1u << 31)  /* Module disable               */
#define MICFIL_CTRL1_PDMIEN     (1u << 29)  /* PDM interface enable         */
#define MICFIL_CTRL1_SRES       (1u << 27)  /* Software reset (self-clearing) */
#define MICFIL_CTRL1_DISEL      (3u << 24)  /* DMA/IRQ select               */
#define MICFIL_CTRL1_DISEL_DMA  (1u << 24)  /* DISEL == 01: DMA request      */
#define MICFIL_CTRL1_CHEN       0xff        /* per-channel enables (7:0)     */

#define MICFIL_FIFO_CTRL_FIFOWMK 0x1f       /* Watermark (bits 4:0)         */

/* VERID: major 1, minor 0, feature 0. */
#define MICFIL_VERID_VALUE  0x020F0000
/*
 * PARAM: 0x0000_0154 - the RM's value (0x010B_0154) with the HWVAD capability
 * masked out.
 *
 * The structure is silicon's: NPAIR = 4 (eight mic inputs) and FIFO_PTRWID = 5,
 * i.e. a 32-deep FIFO - which is also what the driver's own imx93 soc_data
 * assumes (fifo_depth = 32). We used to advertise a FIFO_PTRWID of 3, an
 * 8-deep FIFO, which was a number nobody had: the RM says 32, the driver says
 * 32, and the array in this model holds 64. Three different answers for the
 * depth of one FIFO.
 *
 * HWVAD (the voice-activity detector, its zero-crossing and energy modes, and
 * the unit count) is cleared because we do not model it. That is a decision -
 * under-reporting a capability is safe in both worlds - not a lever to keep
 * Linux out of a code path. As it happens the driver parses PARAM into a struct
 * and never reads it back, so this buys nothing either way; that is a reason to
 * tell the truth about it, not a reason to invent one.
 */
#define MICFIL_PARAM_VALUE  0x00000154

/*
 * Never advertise a FIFO deeper than the array we store into. Both sides are
 * read from the thing they describe: the depth is DECODED FROM PARAM the way
 * the guest decodes it, and compared against the real array. A hand-written
 * constant beside PARAM would be two spellings of one belief, and would not
 * fire for the drift that matters - PARAM changing without the array.
 *
 * The test is >, not !=: the model may hold MORE than it advertises (it holds
 * 64 against silicon's 32). Over-delivering is safe in both worlds.
 */
#define MICFIL_PARAM_FIFO_PTRWID  (((MICFIL_PARAM_VALUE) >> 4) & 0xf)
#define MICFIL_ADVERTISED_DEPTH   (1u << MICFIL_PARAM_FIFO_PTRWID)

QEMU_BUILD_BUG_ON(MICFIL_ADVERTISED_DEPTH >
                  ARRAY_SIZE(((IMX93MicfilState *)0)->rx_fifo));

/*
 * The fsl-micfil driver sets the PDM master clock to rate * clk_div * osr * 8
 * with clk_div = 8, osr = 16, i.e. mclk = rate * 1024 (fsl_micfil.c). So the
 * captured sample rate is pdm_root / 1024 - it must follow the CCM clock the
 * driver programmed, not a hardcoded 48 kHz (which plays every other rate at
 * the wrong speed). And one shared FIFO serves all channels: with N channels
 * enabled the eDMA pops N words per frame, so words must be clocked in at N x
 * the frame rate or an N-channel capture runs N times too slow.
 */
#define MICFIL_MCLK_RATIO   1024

#define R(s, off)   ((s)->regs[(off) >> 2])

/*
 * The sample rate the MICFIL captures at, derived from the CCM's pdm_root:
 * mclk = rate * 1024, so rate = pdm_root / 1024. This is the single source of
 * truth for pacing - the word timer AND the capture-start trace both read it,
 * so the rate the model reports is exactly the rate it clocks at.
 */
static uint64_t imx93_micfil_rate(IMX93MicfilState *s)
{
    uint64_t mclk = s->pdm_clk ? clock_get_hz(s->pdm_clk) : 0;
    uint64_t rate = mclk / MICFIL_MCLK_RATIO;

    if (rate == 0) {
        /*
         * The CCM has not routed a real clock to us yet (pdm_root at its
         * osc_24m idle gives 24 MHz / 1024 = a valid rate, so 0 only happens
         * if the mux points at an unmodelled source). Say so once and pace at
         * a sane default rather than schedule a zero-period timer that
         * livelocks - a clock that is not running must not run infinitely fast.
         */
        if (!s->warned_no_clock) {
            s->warned_no_clock = true;
            qemu_log_mask(LOG_GUEST_ERROR, "imx93-micfil: no PDM clock from the "
                          "CCM; pacing at 48 kHz\n");
        }
        rate = 48000;
    }
    return rate;
}

/* Nanoseconds between clocked-in words: (1 / (rate * channels)). */
static int64_t imx93_micfil_word_ns(IMX93MicfilState *s)
{
    uint32_t chans = ctpop32(R(s, MICFIL_CTRL1) & MICFIL_CTRL1_CHEN);

    if (chans == 0) {
        chans = 1;
    }
    return NANOSECONDS_PER_SECOND / (int64_t)(imx93_micfil_rate(s) * chans);
}

/* The module clocks samples in once enabled and not disabled. */
static bool imx93_micfil_running(IMX93MicfilState *s)
{
    uint32_t ctrl1 = R(s, MICFIL_CTRL1);

    return (ctrl1 & MICFIL_CTRL1_PDMIEN) && !(ctrl1 & MICFIL_CTRL1_MDIS);
}

/*
 * Synthesise the next captured sample. With no physical PDM mic, the model
 * produces a low-frequency sawtooth (ramped in both the low and high 16 bits so
 * it is non-silent whether the card captures S16 or S32). rx_words is the lone
 * monotonic sample index, advanced whether the word is buffered by the tick or
 * generated on demand by a DATACH0 read - keeping one continuous waveform.
 */
static uint32_t imx93_micfil_next_word(IMX93MicfilState *s)
{
    uint16_t s16 = (uint16_t)((s->rx_words & 0x7f) << 9);

    s->rx_words++;
    return ((uint32_t)s16 << 16) | s16;
}

static void imx93_micfil_rx_reset(IMX93MicfilState *s)
{
    s->rx_rptr = 0;
    s->rx_wptr = 0;
    s->rx_count = 0;
}

/*
 * Receive tick: clock one sample into the FIFO per word period. As the fill
 * passes the watermark, request an eDMA drain (the eDMA reads DATACH0 -> mem,
 * the same cyclic path that fills the SAI transmit FIFO for playback).
 */
static void imx93_micfil_rx_tick(void *opaque)
{
    IMX93MicfilState *s = opaque;
    uint32_t watermark = R(s, MICFIL_FIFO_CTRL) & MICFIL_FIFO_CTRL_FIFOWMK;
    uint32_t disel = R(s, MICFIL_CTRL1) & MICFIL_CTRL1_DISEL;
    bool dma = disel == MICFIL_CTRL1_DISEL_DMA;

    if (!imx93_micfil_running(s)) {
        return;
    }

    if (s->rx_count < IMX93_MICFIL_FIFO_DEPTH) {
        s->rx_fifo[s->rx_wptr] = imx93_micfil_next_word(s);
        s->rx_wptr = (s->rx_wptr + 1) % IMX93_MICFIL_FIFO_DEPTH;
        s->rx_count++;
    }

    if (dma && s->rx_count > watermark) {
        qemu_irq_pulse(s->dma_req);
    }

    timer_mod(s->rx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + imx93_micfil_word_ns(s));
}

static uint64_t imx93_micfil_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93MicfilState *s = opaque;

    switch (offset) {
    case MICFIL_VERID:
        return MICFIL_VERID_VALUE;
    case MICFIL_PARAM:
        return MICFIL_PARAM_VALUE;
    case MICFIL_STAT:
        /* Not busy, no channel/error flags pending. */
        return 0;
    case MICFIL_FIFO_STAT:
        /* No overflow/underflow reported. */
        return 0;
    default:
        if (offset >= MICFIL_DATACH0 && offset <= MICFIL_DATACH7) {
            /*
             * The eDMA reads a channel's data register to drain the FIFO. Pop
             * the next sample; if the FIFO is empty but the module is running
             * (the eDMA bursts several words per request, out-running the
             * word-rate tick), synthesise on demand so a real capture never
             * reads silence.
             */
            uint32_t word;

            if (s->rx_count > 0) {
                word = s->rx_fifo[s->rx_rptr];
                s->rx_rptr = (s->rx_rptr + 1) % IMX93_MICFIL_FIFO_DEPTH;
                s->rx_count--;
            } else {
                word = imx93_micfil_running(s) ? imx93_micfil_next_word(s) : 0;
            }
            return word;
        }
        if ((offset >> 2) >= IMX93_MICFIL_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx93_micfil_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93MicfilState *s = opaque;

    switch (offset) {
    case MICFIL_VERID:
    case MICFIL_PARAM:
        /* Read-only identification registers. */
        return;
    case MICFIL_CTRL1: {
        bool was_running = imx93_micfil_running(s);
        bool now_running;

        /* Software reset is momentary: never latch it, and drain the FIFO. */
        if (value & MICFIL_CTRL1_SRES) {
            imx93_micfil_rx_reset(s);
        }
        value &= ~MICFIL_CTRL1_SRES;
        R(s, MICFIL_CTRL1) = value;

        now_running = imx93_micfil_running(s);
        if (now_running && !was_running) {
            uint64_t hz = s->pdm_clk ? clock_get_hz(s->pdm_clk) : 0;

            imx93_micfil_rx_reset(s);
            /*
             * Report the rate we derived from the CCM's pdm_root as capture
             * starts. This is the whole point of taking a real clock input: at
             * 16 kHz the guest programs pdm_root to 16.384 MHz, at 48 kHz to
             * 49.152 MHz, and the trace tracks it - a hardcoded pacer would
             * emit the same number regardless. imx93_micfil_rate() is the same
             * value the word timer paces at, so the trace cannot drift from it.
             */
            trace_imx93_micfil_capture_start(hz, imx93_micfil_rate(s));
            timer_mod(s->rx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + imx93_micfil_word_ns(s));
        } else if (!now_running && was_running) {
            timer_del(s->rx_timer);
            imx93_micfil_rx_reset(s);
        }
        return;
    }
    default:
        break;
    }

    if ((offset >> 2) < IMX93_MICFIL_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx93_micfil_ops = {
    .read = imx93_micfil_read,
    .write = imx93_micfil_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_micfil_reset(DeviceState *dev)
{
    IMX93MicfilState *s = IMX93_MICFIL(dev);

    timer_del(s->rx_timer);
    memset(s->regs, 0, sizeof(s->regs));
    imx93_micfil_rx_reset(s);
    s->rx_words = 0;
}

static void imx93_micfil_realize(DeviceState *dev, Error **errp)
{
    IMX93MicfilState *s = IMX93_MICFIL(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx93_micfil_ops, s,
                          TYPE_IMX93_MICFIL, IMX93_MICFIL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (i = 0; i < IMX93_MICFIL_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    qdev_init_gpio_out_named(dev, &s->dma_req, "dma-req", 1);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx93_micfil_rx_tick, s);
}

static void imx93_micfil_init(Object *obj)
{
    IMX93MicfilState *s = IMX93_MICFIL(obj);

    /*
     * The PDM sample-rate clock is an input driven by the CCM's pdm_root. It
     * must exist before the board wires it, so create it in instance_init -
     * qdev_connect_clock_in() asserts the device is not yet realized.
     */
    s->pdm_clk = qdev_init_clock_in(DEVICE(obj), "pdm_clk", NULL, NULL, 0);
}

static const VMStateDescription vmstate_imx93_micfil = {
    .name = TYPE_IMX93_MICFIL,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93MicfilState, IMX93_MICFIL_REGS),
        VMSTATE_TIMER_PTR(rx_timer, IMX93MicfilState),
        VMSTATE_UINT32_ARRAY(rx_fifo, IMX93MicfilState,
                             IMX93_MICFIL_FIFO_DEPTH),
        VMSTATE_UINT32(rx_rptr, IMX93MicfilState),
        VMSTATE_UINT32(rx_wptr, IMX93MicfilState),
        VMSTATE_UINT32(rx_count, IMX93MicfilState),
        VMSTATE_UINT64(rx_words, IMX93MicfilState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_micfil_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx93_micfil_realize;
    dc->vmsd = &vmstate_imx93_micfil;
    device_class_set_legacy_reset(dc, imx93_micfil_reset);
    dc->desc = "i.MX93 PDM microphone interface";
}

static const TypeInfo imx93_micfil_types[] = {
    {
        .name = TYPE_IMX93_MICFIL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93MicfilState),
        .instance_init = imx93_micfil_init,
        .class_init = imx93_micfil_class_init,
    },
};

DEFINE_TYPES(imx93_micfil_types)
