/*
 * NXP i.MX 93 ELE (EdgeLock Enclave / sentinel) Messaging Unit responder
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the s4muap MU (compatible "fsl,imx93-mu-s4") AND a minimal ELE
 * firmware responder behind it. The real ELE is a separate security
 * subsystem we do not emulate; instead, when Linux's fsl-se driver sends an
 * ELE command over the MU, this model answers the coordination/probe commands
 * whose real-silicon outcome we genuinely reproduce (PING, GET_INFO, GET_STATE,
 * GET_FW_VERSION, READ_FUSE, START_RNG, SERVICE_SWAP, voltage-change) so that
 * se_if_probe() completes and the OCOTP driver gets its se-fw handle (which
 * registers the MAC nvmem cells, unblocking the FEC/eQOS probes).
 *
 * Only the transport + those coordination/probe outcomes are modeled - no real
 * enclave services (RNG bytes, crypto, fuse programming) are provided. Any
 * command NOT on the whitelist FAILS CLOSED: the reply is well-formed and
 * non-gating (handshake completes, the driver never hangs) but carries a
 * failure status, so the driver returns kStatus_Fail rather than being handed a
 * fabricated success. This matters most for ELE_GET_RANDOM (0xCD): a blanket
 * success there would return the guest's un-written buffer AS CRYPTOGRAPHIC
 * RANDOMNESS - a crypto stack would seed with zeros and believe it succeeded.
 * Being honest to the host (a log line) while the guest silently gets a wrong
 * answer is not being honest; the fault reaches the guest. The escape hatch
 * "fake-uncomputed-success" (default off) knowingly restores the old
 * blanket-success behaviour for debugging.
 */

#ifndef IMX93_ELE_H
#define IMX93_ELE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_ELE "imx93.ele"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93EleState, IMX93_ELE)

#define IMX93_ELE_REG_SIZE      (64 * KiB)
#define IMX93_ELE_NUM_TR        4
#define IMX93_ELE_NUM_RR        4
#define IMX93_ELE_MSG_MAX       64

struct IMX93EleState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq_tx;    /* "tx" interrupt (SPI 31) */
    qemu_irq     irq_rx;    /* "rx" interrupt (SPI 30) */

    /* MU control/status shadow. */
    uint32_t gier, gcr, tcr, rcr;

    /* TX message accumulation (words arrive sequentially via TR regs). */
    uint32_t txbuf[IMX93_ELE_MSG_MAX];
    uint32_t txn;
    uint32_t msg_size;

    /* RX response registers + receive-full status bits. */
    uint32_t rr[IMX93_ELE_NUM_RR];
    uint32_t rsr;

    /*
     * Escape hatch (default false): when true, every command is answered with
     * a fabricated SUCCESS - the old behaviour, kept for debugging. When false
     * (the default), commands outside the reproduced-outcome whitelist fail
     * closed so the guest is never handed a fake success (e.g. fake entropy).
     */
    bool fake_uncomputed_success;
};

#endif /* IMX93_ELE_H */
