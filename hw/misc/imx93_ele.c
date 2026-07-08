/*
 * NXP i.MX 93 ELE (EdgeLock Enclave) MU responder
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See imx93_ele.h. Register offsets and the message framing are taken from
 * the Linux imx-mailbox driver's imx_mu_cfg_imx93_s4 (V2 | S4 | IRQ):
 *   VER 0x00, PAR 0x04 (num_tr[7:0] | num_rr[15:8]),
 *   GIER 0x08, SR 0x0C, GCR 0x110, TCR 0x114, GSR 0x118,
 *   RCR 0x120, TSR 0x124, RSR 0x12C, TR0 0x200.., RR0 0x280..
 * V2 status/control bits are per-register-index BIT(x).
 *
 * The ELE message header (se_msg_hdr, little-endian word) is
 *   byte0 ver, byte1 size (words), byte2 command, byte3 tag.
 * imx93 uses cmd_tag 0x17, rsp_tag 0xe1; the driver accepts a response when
 * header.tag == rsp_tag and RES_STATUS(data[0]) == 0xD6 (ELE_SUCCESS_IND).
 * We reply to every command with a 2-word success response; GET_INFO's output
 * buffer is allocated zeroed by the guest (dma_alloc_coherent), so no DMA
 * write is needed for probe to pass.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_ele.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/*
 * Register offsets (s4 MU). NB: the imx_mu_xcr enum is
 * { CR, GIER, GCR, TCR, RCR }, so the xCR[] array {0x8,0x110,0x114,0x120,0x128}
 * maps CR=0x8, GIER=0x110, GCR=0x114, TCR=0x120, RCR=0x128. The xsr enum is
 * { SR, GSR, TSR, RSR } -> {0xC,0x118,0x124,0x12C}.
 */
#define ELE_VER     0x000
#define ELE_PAR     0x004
#define ELE_CR      0x008
#define ELE_SR      0x00C
#define ELE_GIER    0x110
#define ELE_GCR     0x114
#define ELE_GSR     0x118
#define ELE_TCR     0x120
#define ELE_TSR     0x124
#define ELE_RCR     0x128
#define ELE_RSR     0x12C
#define ELE_TR0     0x200
#define ELE_RR0     0x280

#define ELE_RSP_TAG         0xe1
#define ELE_SUCCESS_IND     0xd6

static void imx93_ele_update_irq(IMX93EleState *s)
{
    /*
     * TX interrupt: transmit registers are always reported empty (TSR all-set),
     * so the line follows TCR.TIE for slot 0 - which lets mbox txdone fire.
     * RX interrupt: receive-full on slot 0 with its enable (RCR.RIE) set.
     */
    qemu_set_irq(s->irq_tx, !!(s->tcr & BIT(0)));
    qemu_set_irq(s->irq_rx, (s->rsr & BIT(0)) && (s->rcr & BIT(0)));
}

/*
 * Response word count per command. The driver validates the response header's
 * size field against the size it expects for each command, so a one-size-fits-
 * all reply triggers "Cmd size mismatch" warnings (and fails fuse reads). Most
 * base commands expect 2 words (header + status); these are the exceptions.
 * The extra data words are returned as zero, which is fine for bring-up (e.g.
 * fuse value 0, fw version 0) - only the status byte is checked for success.
 */
static unsigned imx93_ele_rsp_words(uint8_t command)
{
    switch (command) {
    case 0x97:  /* ELE_READ_FUSE      -> 0x0C bytes */
        return 3;
    case 0x9d:  /* ELE_GET_FW_VERSION -> 0x10 bytes */
    case 0xb2:  /* ELE_GET_STATE      -> 0x10 bytes */
        return 4;
    default:    /* GET_INFO, PING, START_RNG, ... -> 0x08 bytes */
        return 2;
    }
}

/* A complete command message sits in s->txbuf[0..msg_size-1]; reply. */
static void imx93_ele_process(IMX93EleState *s)
{
    uint32_t hdr = s->txbuf[0];
    uint8_t ver = hdr & 0xff;
    uint8_t command = (hdr >> 16) & 0xff;
    unsigned words = imx93_ele_rsp_words(command);
    unsigned i;

    /*
     * Header: rsp_tag, size, command, ver. Word 1 carries the success status;
     * any further words are zero.
     */
    s->rr[0] = ((uint32_t)ELE_RSP_TAG << 24) | ((uint32_t)command << 16) |
               ((uint32_t)words << 8) | ver;
    s->rr[1] = ELE_SUCCESS_IND;
    for (i = 2; i < words; i++) {
        s->rr[i] = 0;
    }
    s->rsr = BIT(words) - 1;     /* RR0..RR[words-1] hold valid words */
    imx93_ele_update_irq(s);
}

static void imx93_ele_tx_word(IMX93EleState *s, uint32_t val)
{
    if (s->txn < IMX93_ELE_MSG_MAX) {
        s->txbuf[s->txn] = val;
    }
    if (s->txn == 0) {
        s->msg_size = (val >> 8) & 0xff;        /* header.size = word count */
        if (s->msg_size == 0) {
            s->msg_size = 1;
        }
        if (s->msg_size > IMX93_ELE_MSG_MAX) {
            s->msg_size = IMX93_ELE_MSG_MAX;
        }
    }
    s->txn++;
    if (s->txn >= s->msg_size) {
        imx93_ele_process(s);
        s->txn = 0;
    }
}

static uint64_t imx93_ele_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93EleState *s = opaque;

    switch (offset) {
    case ELE_PAR:
        return IMX93_ELE_NUM_TR | (IMX93_ELE_NUM_RR << 8);
    case ELE_TSR:
        /* All transmit registers always empty/ready (SE drains instantly). */
        return BIT(IMX93_ELE_NUM_TR) - 1;
    case ELE_RSR:
        return s->rsr;
    case ELE_GIER:
        return s->gier;
    case ELE_GCR:
        return s->gcr;
    case ELE_TCR:
        return s->tcr;
    case ELE_RCR:
        return s->rcr;
    case ELE_RR0 ... ELE_RR0 + (IMX93_ELE_NUM_RR - 1) * 4: {
        unsigned idx = (offset - ELE_RR0) / 4;
        uint32_t val = s->rr[idx];
        s->rsr &= ~BIT(idx);            /* reading consumes the word */
        imx93_ele_update_irq(s);
        return val;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imx93_ele_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93EleState *s = opaque;

    switch (offset) {
    case ELE_TR0 ... ELE_TR0 + (IMX93_ELE_NUM_TR - 1) * 4:
        imx93_ele_tx_word(s, value);
        break;
    case ELE_GIER:
        s->gier = value;
        break;
    case ELE_GCR:
        s->gcr = value;
        break;
    case ELE_TCR:
        s->tcr = value;
        imx93_ele_update_irq(s);
        break;
    case ELE_RCR:
        s->rcr = value;
        imx93_ele_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps imx93_ele_ops = {
    .read = imx93_ele_read,
    .write = imx93_ele_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_ele_reset_hold(Object *obj, ResetType type)
{
    IMX93EleState *s = IMX93_ELE(obj);

    s->gier = s->gcr = s->tcr = s->rcr = 0;
    s->txn = s->msg_size = 0;
    s->rsr = 0;
    memset(s->txbuf, 0, sizeof(s->txbuf));
    memset(s->rr, 0, sizeof(s->rr));
    imx93_ele_update_irq(s);
}

static void imx93_ele_init(Object *obj)
{
    IMX93EleState *s = IMX93_ELE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_ele_ops, s,
                          TYPE_IMX93_ELE, IMX93_ELE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_tx);
    sysbus_init_irq(sbd, &s->irq_rx);
}

static const VMStateDescription vmstate_imx93_ele = {
    .name = TYPE_IMX93_ELE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gier, IMX93EleState),
        VMSTATE_UINT32(gcr, IMX93EleState),
        VMSTATE_UINT32(tcr, IMX93EleState),
        VMSTATE_UINT32(rcr, IMX93EleState),
        VMSTATE_UINT32_ARRAY(txbuf, IMX93EleState, IMX93_ELE_MSG_MAX),
        VMSTATE_UINT32(txn, IMX93EleState),
        VMSTATE_UINT32(msg_size, IMX93EleState),
        VMSTATE_UINT32_ARRAY(rr, IMX93EleState, IMX93_ELE_NUM_RR),
        VMSTATE_UINT32(rsr, IMX93EleState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_ele_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "i.MX 93 ELE (EdgeLock Enclave) MU responder";
    rc->phases.hold = imx93_ele_reset_hold;
    dc->vmsd = &vmstate_imx93_ele;
}

static const TypeInfo imx93_ele_types[] = {
    {
        .name           = TYPE_IMX93_ELE,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93EleState),
        .instance_init  = imx93_ele_init,
        .class_init     = imx93_ele_class_init,
    },
};

DEFINE_TYPES(imx93_ele_types)
