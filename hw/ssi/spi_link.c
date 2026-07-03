/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spi-link - an SSI peripheral that bridges an SPI bus to a QEMU chardev, so
 * two emulator instances can pass real data over an SPI board-to-board link.
 * Attach one to each instance's named LPSPI bus and connect the two chardevs
 * with a socket:
 *
 *   -chardev socket,id=spil,... -device spi-link,bus=lpspi1,chardev=spil
 *
 * On every SPI transfer the master shifts out a byte (MOSI): we forward it to
 * the chardev (-> the peer instance). The byte shifted in (MISO) is taken from
 * an rx FIFO fed by the peer over the chardev, or 0xff (idle-high) when none is
 * queued. So each direction is an independent, FIFO-buffered byte stream - one
 * side's master writes, the other's master clocks the bytes in. This models the
 * data path of a board-to-board SPI link (not cycle-accurate clock duplex).
 *
 * The MOSI write is NON-BLOCKING: outgoing bytes are queued in a tx FIFO and
 * drained with qemu_chr_fe_write(); if the socket back-pressures (a continuous
 * full-duplex clock can outrun a peer that isn't draining), a G_IO_OUT watch
 * resumes the drain when it's writable - the vCPU is never blocked inside a TDR
 * write. If the tx FIFO itself fills under sustained back-pressure, MOSI bytes
 * are dropped (a link overrun, logged once) rather than hanging the guest.
 */
#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_SPI_LINK "spi-link"
OBJECT_DECLARE_SIMPLE_TYPE(SpiLinkState, SPI_LINK)

#define SPI_LINK_RX_DEPTH 256
#define SPI_LINK_TX_DEPTH 8192      /* absorbs MOSI bursts */

struct SpiLinkState {
    SSIPeripheral parent_obj;

    CharFrontend chr;
    Fifo8 rx;               /* bytes shifted in from the peer (MISO)   */
    Fifo8 tx;               /* bytes to send to the peer (MOSI), async */
    guint watch_tag;        /* G_IO_OUT drain watch, 0 = none pending  */
    bool tx_overrun_warned;
};

static void spi_link_flush(SpiLinkState *s);

static gboolean spi_link_watch(void *do_not_use, GIOCondition cond,
                               void *opaque)
{
    SpiLinkState *s = opaque;

    s->watch_tag = 0;
    spi_link_flush(s);
    return G_SOURCE_REMOVE;
}

/* Drain the tx FIFO without blocking; re-arm the watch on back-pressure. */
static void spi_link_flush(SpiLinkState *s)
{
    while (!fifo8_is_empty(&s->tx)) {
        uint8_t b = fifo8_peek(&s->tx);
        int rc = qemu_chr_fe_write(&s->chr, &b, 1);

        if (rc < 1) {
            if (!s->watch_tag) {
                s->watch_tag =
                    qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                          spi_link_watch, s);
            }
            return;         /* resume from the watch when writable */
        }
        fifo8_pop(&s->tx);
    }
}

static uint32_t spi_link_transfer(SSIPeripheral *dev, uint32_t val)
{
    SpiLinkState *s = SPI_LINK(dev);
    uint8_t out = val & 0xff;
    uint8_t in = 0xff;      /* MISO idle-high when the peer sent nothing */

    /* Queue the MOSI byte and drain non-blocking - never block the vCPU. */
    if (!fifo8_is_full(&s->tx)) {
        fifo8_push(&s->tx, out);
    } else if (!s->tx_overrun_warned) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "spi-link: tx overrun (peer not draining), dropping MOSI\n");
        s->tx_overrun_warned = true;
    }
    spi_link_flush(s);

    if (!fifo8_is_empty(&s->rx)) {
        in = fifo8_pop(&s->rx);
    }
    return in;
}

static int spi_link_can_receive(void *opaque)
{
    SpiLinkState *s = opaque;

    return fifo8_num_free(&s->rx);
}

static void spi_link_receive(void *opaque, const uint8_t *buf, int size)
{
    SpiLinkState *s = opaque;
    int i;

    for (i = 0; i < size && !fifo8_is_full(&s->rx); i++) {
        fifo8_push(&s->rx, buf[i]);
    }
}

static void spi_link_init(Object *obj)
{
    SpiLinkState *s = SPI_LINK(obj);

    fifo8_create(&s->rx, SPI_LINK_RX_DEPTH);
    fifo8_create(&s->tx, SPI_LINK_TX_DEPTH);
}

static void spi_link_finalize(Object *obj)
{
    SpiLinkState *s = SPI_LINK(obj);

    if (s->watch_tag) {
        g_source_remove(s->watch_tag);
        s->watch_tag = 0;
    }
    fifo8_destroy(&s->tx);
    fifo8_destroy(&s->rx);
}

static void spi_link_realize(SSIPeripheral *dev, Error **errp)
{
    SpiLinkState *s = SPI_LINK(dev);

    qemu_chr_fe_set_handlers(&s->chr, spi_link_can_receive, spi_link_receive,
                             NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_spi_link = {
    .name = "spi-link",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, SpiLinkState),
        VMSTATE_FIFO8(rx, SpiLinkState),
        VMSTATE_FIFO8(tx, SpiLinkState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property spi_link_props[] = {
    DEFINE_PROP_CHR("chardev", SpiLinkState, chr),
};

static void spi_link_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = spi_link_realize;
    k->transfer = spi_link_transfer;
    dc->desc = "SPI board-to-board link (SSI peripheral <-> chardev)";
    dc->vmsd = &vmstate_spi_link;
    device_class_set_props(dc, spi_link_props);
}

static const TypeInfo spi_link_info = {
    .name             = TYPE_SPI_LINK,
    .parent           = TYPE_SSI_PERIPHERAL,
    .instance_size    = sizeof(SpiLinkState),
    .instance_init    = spi_link_init,
    .instance_finalize = spi_link_finalize,
    .class_init       = spi_link_class_init,
};

static void spi_link_register_types(void)
{
    type_register_static(&spi_link_info);
}

type_init(spi_link_register_types)
