/*
 * Arm Ethos-U55/U65 microNPU - functional command-stream executor
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QOM device: the APB register model, job lifecycle and DMA address space.
 * The actual command-stream parsing/compute lives in ethos_u_cmdstream.c and
 * the compute units; this file fetches the stream on a worker thread and raises
 * the completion IRQ from a bottom half (so a long inference never stalls the
 * vCPU or the main loop). Modelled on hw/misc/edu.c's offload pattern.
 */

#include "qemu/osdep.h"
#include "hw/npu/ethos_u.h"
#include "ethos_u_internal.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "trace/trace-hw_npu.h"

/*
 * ID/CONFIG are reported to the driver/firmware and (for CONFIG) exact-matched
 * by Vela's verify_optimizer_config, so they must reflect the configured part.
 *   ID:     arch 1.0.6, product_major = variant, version 0.
 *   CONFIG: product[31:28] = variant, cmd_stream_version[7:4] = 0,
 *           macs_per_cc[3:0] = macs_per_cc_log2.
 */
static uint32_t ethos_u_id_value(EthosUState *s)
{
    return 0x10061000 | ((uint32_t)s->variant << 12);
}

static uint32_t ethos_u_config_value(EthosUState *s)
{
    return 0x10000000 | ((uint32_t)s->variant << 28) |
           (s->macs_per_cc_log2 & 0xf);
}

uint64_t ethos_u_region_base(EthosUState *s, int region)
{
    uint32_t off = ETHOS_U_REG_BASEP0 + region * 8;

    if (region < 0 || region >= ETHOS_U_NUM_BASEP) {
        return 0;
    }
    return s->regs[off >> 2] | ((uint64_t)s->regs[(off + 4) >> 2] << 32);
}

/*
 * Device entry point for the parser: fetch the command stream over the DMA
 * address space and decode it. Runs on the worker thread; uses dma_memory_* and
 * must not touch QOM/IRQ state.
 */
bool ethos_u_cmdstream_run(EthosUState *s, hwaddr qbase, uint32_t qsize)
{
    g_autofree uint8_t *cms = NULL;
    uint64_t basep[ETHOS_U_NUM_BASEP];
    int i;

    if (qsize == 0 || qsize > ETHOS_U_CMS_MAX || (qsize & 3)) {
        return false;
    }
    cms = g_malloc(qsize);
    if (dma_memory_read(&s->dma_as, qbase, cms, qsize,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }
    for (i = 0; i < ETHOS_U_NUM_BASEP; i++) {
        basep[i] = ethos_u_region_base(s, i);
    }

    /* Cleared here, set by the executor on an uncomputable op (honest_fault). */
    s->op_failed = false;
    if (!ethos_u_cmdstream_decode(cms, qsize, basep, ethos_u_exec_op, s)) {
        return false;
    }
    return !s->op_failed;
}

/* A single in-flight job (snapshot taken at kick time). */
typedef struct EthosUJob {
    EthosUState *s;
    hwaddr qbase;
    uint32_t qsize;
    bool ok;
} EthosUJob;

/* Bottom half (main loop, BQL held): publish completion + raise the IRQ. */
static void ethos_u_complete_bh(void *opaque)
{
    EthosUJob *job = opaque;
    EthosUState *s = job->s;
    uint32_t status = ETHOS_U_STATUS_IRQ_RAISED |
                      ETHOS_U_STATUS_CMD_END_REACHED;

    if (!job->ok) {
        status |= ETHOS_U_STATUS_CMD_PARSE_ERROR;
    }
    s->regs[ETHOS_U_REG_QREAD >> 2] = s->regs[ETHOS_U_REG_QSIZE >> 2];
    s->regs[ETHOS_U_REG_STATUS >> 2] = status;
    qemu_set_irq(s->irq, 1);
    s->busy = false;
    g_free(job);
}

/* Worker thread: run the (potentially long) command stream off the vCPU. */
static void *ethos_u_worker(void *opaque)
{
    EthosUJob *job = opaque;

    job->ok = ethos_u_cmdstream_run(job->s, job->qbase, job->qsize);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ethos_u_complete_bh, job);
    return NULL;
}

/* Bottom half (main loop): snapshot the job and spawn the worker. */
static void ethos_u_kick_bh(void *opaque)
{
    EthosUState *s = opaque;
    EthosUJob *job = g_new0(EthosUJob, 1);
    QemuThread t;

    job->s = s;
    job->qbase = s->regs[ETHOS_U_REG_QBASE >> 2] |
                 ((uint64_t)s->regs[ETHOS_U_REG_QBASE_HI >> 2] << 32);
    job->qsize = s->regs[ETHOS_U_REG_QSIZE >> 2];

    trace_ethos_u_kick(job->qbase, job->qsize);
    qemu_thread_create(&t, "ethos-u", ethos_u_worker, job,
                       QEMU_THREAD_DETACHED);
}

static uint64_t ethos_u_read(void *opaque, hwaddr offset, unsigned size)
{
    EthosUState *s = opaque;
    uint32_t val;

    switch (offset) {
    case ETHOS_U_REG_ID:
        val = ethos_u_id_value(s);
        break;
    case ETHOS_U_REG_CONFIG:
        val = ethos_u_config_value(s);
        break;
    case ETHOS_U_REG_STATUS:
        val = s->regs[ETHOS_U_REG_STATUS >> 2];
        break;
    default:
        val = (offset >> 2) < ETHOS_U_NUM_REGS ? s->regs[offset >> 2] : 0;
        break;
    }
    trace_ethos_u_read(offset, val);
    return val;
}

static void ethos_u_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    EthosUState *s = opaque;

    trace_ethos_u_write(offset, value);

    switch (offset) {
    case ETHOS_U_REG_ID:
    case ETHOS_U_REG_CONFIG:
        return;     /* read-only identification */
    case ETHOS_U_REG_CMD:
        s->regs[ETHOS_U_REG_CMD >> 2] = value;
        if ((value & ETHOS_U_CMD_TRANSITION_TO_RUNNING) && !s->busy) {
            s->busy = true;
            s->regs[ETHOS_U_REG_STATUS >> 2] = ETHOS_U_STATUS_STATE_RUNNING;
            aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                    ethos_u_kick_bh, s);
        }
        if (value & ETHOS_U_CMD_CLEAR_IRQ) {
            s->regs[ETHOS_U_REG_STATUS >> 2] &= ~ETHOS_U_STATUS_IRQ_RAISED;
            qemu_set_irq(s->irq, 0);
        }
        return;
    case ETHOS_U_REG_RESET:
        /*
         * Driver writes the requested access state ([0]=privileged,
         * [1]=non-secure) and reads PROT back to confirm; mirror it.
         */
        s->regs[ETHOS_U_REG_PROT >> 2] = value & 0x3;
        s->regs[ETHOS_U_REG_RESET >> 2] = value;
        return;
    default:
        if ((offset >> 2) < ETHOS_U_NUM_REGS) {
            s->regs[offset >> 2] = value;
        }
        return;
    }
}

static const MemoryRegionOps ethos_u_ops = {
    .read = ethos_u_read,
    .write = ethos_u_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ethos_u_reset(DeviceState *dev)
{
    EthosUState *s = ETHOS_U(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->busy = false;
}

static void ethos_u_realize(DeviceState *dev, Error **errp)
{
    EthosUState *s = ETHOS_U(dev);

    if (s->variant_str == NULL ||
        g_ascii_strcasecmp(s->variant_str, "u65") == 0) {
        s->variant = ETHOS_U65;
    } else if (g_ascii_strcasecmp(s->variant_str, "u55") == 0) {
        s->variant = ETHOS_U55;
    } else {
        error_setg(errp, "ethos-u: unknown variant '%s' (use u55 or u65)",
                   s->variant_str);
        return;
    }

    address_space_init(&s->dma_as,
                       s->dma_mr ? s->dma_mr : get_system_memory(),
                       "ethos-u-dma");

    memory_region_init_io(&s->iomem, OBJECT(dev), &ethos_u_ops, s,
                          TYPE_ETHOS_U, ETHOS_U_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_ethos_u = {
    .name = TYPE_ETHOS_U,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, EthosUState, ETHOS_U_NUM_REGS),
        VMSTATE_BOOL(busy, EthosUState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property ethos_u_properties[] = {
    DEFINE_PROP_STRING("variant", EthosUState, variant_str),
    DEFINE_PROP_UINT8("macs", EthosUState, macs_per_cc_log2, 8),
    DEFINE_PROP_BOOL("host-infer-fallback", EthosUState,
                     host_infer_fallback, false),
    DEFINE_PROP_BOOL("honest-fault", EthosUState, honest_fault, false),
    DEFINE_PROP_LINK("dma", EthosUState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void ethos_u_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ethos_u_realize;
    dc->vmsd = &vmstate_ethos_u;
    device_class_set_legacy_reset(dc, ethos_u_reset);
    device_class_set_props(dc, ethos_u_properties);
    dc->desc = "Arm Ethos-U microNPU";
}

static const TypeInfo ethos_u_types[] = {
    {
        .name = TYPE_ETHOS_U,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(EthosUState),
        .class_init = ethos_u_class_init,
    },
};

DEFINE_TYPES(ethos_u_types)
