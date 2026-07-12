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
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "system/dma.h"

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
#define ELE_FAILURE_IND     0x29    /* any non-0xd6 low byte reads as failure */

/*
 * ELE command opcodes (se_msg_hdr byte 2), from the Linux fsl-se driver
 * (drivers/firmware/imx/ele_base_msg.h + ele_fw_api.h). The WHITELIST below is
 * exactly the set whose real-silicon OUTCOME this model genuinely reproduces:
 *  - pure coordination, so SUCCESS is honestly true: PING, voltage-change
 *    start/finish, START_RNG (kicks off the TRNG; produces no bytes here), and
 *    SERVICE_SWAP (IMEM export/import handshake);
 *  - info reads where a zero value / guest-zeroed buffer is the CORRECT answer
 *    for a model with no resident ELE firmware or blown fuses: GET_INFO,
 *    GET_STATE, GET_FW_VERSION, READ_FUSE.
 * ELE_GET_RANDOM is special: rather than fake success (fake entropy) OR fail
 * closed (a dead hwrng), it is COMPUTED CORRECTLY - real host-CSPRNG bytes are
 * DMA'd into the guest's destination buffer. For an RNG the consumer reads
 * "success" as "these bytes are entropy", so real randomness is the only honest
 * answer; deterministic repro is available under QEMU's -seed. Everything else
 * we cannot reproduce - WRITE_FUSE, FW auth, INIT_FW, unknown - fails closed.
 */
#define IMX93_ELE_RNG_MAX_LEN   0x10000  /* clamp guest-supplied length */
#define ELE_CMD_PING            0x01
#define ELE_CMD_FW_AUTH         0x02
#define ELE_CMD_VOLT_START      0x12
#define ELE_CMD_VOLT_FINISH     0x13
#define ELE_CMD_INIT_FW         0x17
#define ELE_CMD_DEBUG_DUMP      0x21
#define ELE_CMD_READ_FUSE       0x97
#define ELE_CMD_GET_FW_VERSION  0x9d
#define ELE_CMD_START_RNG       0xa3
#define ELE_CMD_GET_STATE       0xb2
#define ELE_CMD_GET_RANDOM      0xcd    /* returns bytes into a guest buffer */
#define ELE_CMD_WRITE_FUSE      0xd6
#define ELE_CMD_GET_INFO        0xda
#define ELE_CMD_SERVICE_SWAP    0xdf

/* True iff we genuinely reproduce this command's real-silicon outcome. */
static bool imx93_ele_cmd_reproduced(uint8_t command)
{
    switch (command) {
    case ELE_CMD_PING:              /* coordination: nothing computed */
    case ELE_CMD_VOLT_START:
    case ELE_CMD_VOLT_FINISH:
    case ELE_CMD_START_RNG:         /* starts the TRNG; hands back no bytes */
    case ELE_CMD_SERVICE_SWAP:      /* IMEM export/import handshake */
    case ELE_CMD_GET_INFO:          /* info reads: 0 / zeroed buffer is correct */
    case ELE_CMD_GET_STATE:
    case ELE_CMD_GET_FW_VERSION:
    case ELE_CMD_READ_FUSE:
        return true;
    default:                        /* GET_RANDOM, WRITE_FUSE, auth, unknown */
        return false;
    }
}

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

/*
 * ELE_GET_RANDOM: fill the guest's destination buffer with real host-CSPRNG
 * entropy. Message layout (from the Linux ele_get_random / ele_rng_msg_data):
 * word0 header, word1 flags, word2 destination address, word3 length. The
 * ele-reserved DMA buffer sits in the low 4 GiB, so a 32-bit address suffices.
 * We stream via a stack buffer so any length needs no large allocation, and
 * clamp a guest-supplied length to keep a malformed message bounded.
 */
static void imx93_ele_get_random(IMX93EleState *s)
{
    uint32_t addr, len, off;
    uint8_t chunk[256];

    if (s->msg_size < 4) {
        return;
    }
    addr = s->txbuf[2];
    len = s->txbuf[3];
    if (len > IMX93_ELE_RNG_MAX_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: GET_RANDOM length 0x%x clamped to 0x%x\n",
                      __func__, len, IMX93_ELE_RNG_MAX_LEN);
        len = IMX93_ELE_RNG_MAX_LEN;
    }
    for (off = 0; off < len; off += sizeof(chunk)) {
        uint32_t n = MIN(len - off, sizeof(chunk));
        qemu_guest_getrandom_nofail(chunk, n);
        dma_memory_write(&address_space_memory, addr + off, chunk, n,
                         MEMTXATTRS_UNSPECIFIED);
    }
}

/* A complete command message sits in s->txbuf[0..msg_size-1]; reply. */
static void imx93_ele_process(IMX93EleState *s)
{
    uint32_t hdr = s->txbuf[0];
    uint8_t ver = hdr & 0xff;
    uint8_t command = (hdr >> 16) & 0xff;
    bool reproduced = s->fake_uncomputed_success ||
                      imx93_ele_cmd_reproduced(command);
    unsigned words;
    unsigned i;

    /*
     * GET_RANDOM is computed correctly: hand the guest REAL entropy rather than
     * a fabricated success (fake entropy) or a failure (dead hwrng). Under the
     * escape hatch we skip the fill to reproduce the old dishonest behaviour.
     */
    if (command == ELE_CMD_GET_RANDOM && !s->fake_uncomputed_success) {
        imx93_ele_get_random(s);
        reproduced = true;
    }

    /*
     * Fail closed for any command whose outcome we do not reproduce: a
     * well-formed, NON-GATING reply (the RX interrupt still fires, the driver's
     * handshake completes and it never hangs) that carries a failure status, so
     * the caller gets kStatus_Fail instead of a fabricated success. The failure
     * reply is always header + status (2 words). The most important case is
     * ELE_GET_RANDOM: a fake success there hands the guest its own un-written
     * buffer as cryptographic randomness.
     */
    if (!reproduced) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ELE command 0x%02x not reproduced - failing closed "
                      "(guest gets ELE_FAILURE, not a fabricated success)\n",
                      __func__, command);
        s->rr[0] = ((uint32_t)ELE_RSP_TAG << 24) | ((uint32_t)command << 16) |
                   (2u << 8) | ver;
        s->rr[1] = ELE_FAILURE_IND;
        s->rsr = BIT(2) - 1;
        imx93_ele_update_irq(s);
        return;
    }

    words = imx93_ele_rsp_words(command);

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

static const Property imx93_ele_properties[] = {
    DEFINE_PROP_BOOL("fake-uncomputed-success", IMX93EleState,
                     fake_uncomputed_success, false),
};

static void imx93_ele_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "i.MX 93 ELE (EdgeLock Enclave) MU responder";
    rc->phases.hold = imx93_ele_reset_hold;
    dc->vmsd = &vmstate_imx93_ele;
    device_class_set_props(dc, imx93_ele_properties);
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
