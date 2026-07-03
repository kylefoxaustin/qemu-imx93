/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * i2c-link - an I2C target that bridges its bus to a QEMU chardev, so two
 * emulator instances can pass real data over an I2C board-to-board link.
 * Attach one to each instance's named LPI2C bus at the same address and connect
 * the two chardevs with a socket:
 *
 *   -chardev socket,id=i2cl,... -device i2c-link,bus=lpi2c3,address=0x42,chardev=i2cl
 *
 * Each board's LPI2C runs as the master. When a master WRITES bytes to the
 * link (I2C send) they are forwarded to the chardev (-> the peer instance).
 * When a master READS bytes from the link (I2C recv) they are taken from an rx
 * FIFO fed by the peer over the chardev, or 0xff when none is queued. So one
 * side's master writes a payload and the other side's master reads it - the
 * data path of a board-to-board I2C link (a bridged mailbox, not a single
 * shared bus; the analogue of spi-link for I2C).
 *
 * The forward is NON-BLOCKING: outgoing bytes queue in a tx FIFO drained with
 * qemu_chr_fe_write(); if the socket back-pressures, a G_IO_OUT watch resumes
 * the drain when writable - the vCPU is never blocked inside an I2C send. A
 * sustained-back-pressure tx overflow drops bytes (logged once) rather than
 * hanging the guest.
 */
#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_I2C_LINK "i2c-link"
OBJECT_DECLARE_SIMPLE_TYPE(I2cLinkState, I2C_LINK)

#define I2C_LINK_RX_DEPTH 256
#define I2C_LINK_TX_DEPTH 8192

struct I2cLinkState {
    I2CSlave parent_obj;

    CharFrontend chr;
    Fifo8 rx;               /* bytes from the peer (for master reads)  */
    Fifo8 tx;               /* bytes to the peer (from master writes)  */
    guint watch_tag;        /* G_IO_OUT drain watch, 0 = none pending  */
    bool tx_overrun_warned;
};

static void i2c_link_flush(I2cLinkState *s);

static gboolean i2c_link_watch(void *do_not_use, GIOCondition cond,
                               void *opaque)
{
    I2cLinkState *s = opaque;

    s->watch_tag = 0;
    i2c_link_flush(s);
    return G_SOURCE_REMOVE;
}

/* Drain the tx FIFO without blocking; re-arm the watch on back-pressure. */
static void i2c_link_flush(I2cLinkState *s)
{
    while (!fifo8_is_empty(&s->tx)) {
        uint8_t b = fifo8_peek(&s->tx);
        int rc = qemu_chr_fe_write(&s->chr, &b, 1);

        if (rc < 1) {
            if (!s->watch_tag) {
                s->watch_tag =
                    qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                          i2c_link_watch, s);
            }
            return;         /* resume from the watch when writable */
        }
        fifo8_pop(&s->tx);
    }
}

/* Master -> link (I2C write): queue the byte to the peer, drain non-blocking. */
static int i2c_link_send(I2CSlave *i2c, uint8_t data)
{
    I2cLinkState *s = I2C_LINK(i2c);

    if (!fifo8_is_full(&s->tx)) {
        fifo8_push(&s->tx, data);
    } else if (!s->tx_overrun_warned) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "i2c-link: tx overrun (peer not draining), dropping\n");
        s->tx_overrun_warned = true;
    }
    i2c_link_flush(s);
    return 0;               /* ACK */
}

/* Link -> master (I2C read): the peer's byte, or 0xff (idle) when none. */
static uint8_t i2c_link_recv(I2CSlave *i2c)
{
    I2cLinkState *s = I2C_LINK(i2c);

    return fifo8_is_empty(&s->rx) ? 0xff : fifo8_pop(&s->rx);
}

static int i2c_link_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;              /* stateless bridge; nothing to do on START/STOP */
}

static int i2c_link_can_receive(void *opaque)
{
    I2cLinkState *s = opaque;

    return fifo8_num_free(&s->rx);
}

static void i2c_link_receive(void *opaque, const uint8_t *buf, int size)
{
    I2cLinkState *s = opaque;
    int i;

    for (i = 0; i < size && !fifo8_is_full(&s->rx); i++) {
        fifo8_push(&s->rx, buf[i]);
    }
}

static void i2c_link_realize(DeviceState *dev, Error **errp)
{
    I2cLinkState *s = I2C_LINK(dev);

    qemu_chr_fe_set_handlers(&s->chr, i2c_link_can_receive, i2c_link_receive,
                             NULL, NULL, s, NULL, true);
}

static void i2c_link_instance_init(Object *obj)
{
    I2cLinkState *s = I2C_LINK(obj);

    fifo8_create(&s->rx, I2C_LINK_RX_DEPTH);
    fifo8_create(&s->tx, I2C_LINK_TX_DEPTH);
}

static void i2c_link_instance_finalize(Object *obj)
{
    I2cLinkState *s = I2C_LINK(obj);

    if (s->watch_tag) {
        g_source_remove(s->watch_tag);
        s->watch_tag = 0;
    }
    fifo8_destroy(&s->tx);
    fifo8_destroy(&s->rx);
}

static const VMStateDescription vmstate_i2c_link = {
    .name = "i2c-link",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, I2cLinkState),
        VMSTATE_FIFO8(rx, I2cLinkState),
        VMSTATE_FIFO8(tx, I2cLinkState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property i2c_link_props[] = {
    DEFINE_PROP_CHR("chardev", I2cLinkState, chr),
};

static void i2c_link_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = i2c_link_realize;
    dc->desc = "I2C board-to-board link (I2C target <-> chardev)";
    dc->vmsd = &vmstate_i2c_link;
    device_class_set_props(dc, i2c_link_props);
    k->send = i2c_link_send;
    k->recv = i2c_link_recv;
    k->event = i2c_link_event;
}

static const TypeInfo i2c_link_info = {
    .name             = TYPE_I2C_LINK,
    .parent           = TYPE_I2C_SLAVE,
    .instance_size    = sizeof(I2cLinkState),
    .instance_init    = i2c_link_instance_init,
    .instance_finalize = i2c_link_instance_finalize,
    .class_init       = i2c_link_class_init,
};

static void i2c_link_register_types(void)
{
    type_register_static(&i2c_link_info);
}

type_init(i2c_link_register_types)
