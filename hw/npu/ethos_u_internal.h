/*
 * Arm Ethos-U55/U65 microNPU - device-internal definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register offsets, command-stream opcodes and the resolved-operation
 * descriptor shared between the QOM device (ethos_u.c) and the command-stream
 * parser / compute units. Not exported outside hw/npu/. Opcode and register
 * values follow the Arm Ethos-U core driver / Vela register command stream.
 */

#ifndef HW_NPU_ETHOS_U_INTERNAL_H
#define HW_NPU_ETHOS_U_INTERNAL_H

#include "hw/npu/ethos_u.h"

/* --- APB control registers (byte offsets) --- */
#define ETHOS_U_REG_ID         0x00
#define ETHOS_U_REG_STATUS     0x04
#define ETHOS_U_REG_CMD        0x08
#define ETHOS_U_REG_RESET      0x0c
#define ETHOS_U_REG_QBASE      0x10
#define ETHOS_U_REG_QBASE_HI   0x14
#define ETHOS_U_REG_QREAD      0x18
#define ETHOS_U_REG_QCONFIG    0x1c
#define ETHOS_U_REG_QSIZE      0x20
#define ETHOS_U_REG_PROT       0x24
#define ETHOS_U_REG_CONFIG     0x28
#define ETHOS_U_REG_REGIONCFG  0x3c
#define ETHOS_U_REG_BASEP0     0x80   /* BASEP0..7: eight 64-bit region bases */

/* Bound on a buffered command stream (sanity, not architectural). */
#define ETHOS_U_CMS_MAX (4 * 1024 * 1024)

/* CMD register bits. */
#define ETHOS_U_CMD_TRANSITION_TO_RUNNING (1u << 0)
#define ETHOS_U_CMD_CLEAR_IRQ             (1u << 1)

/* STATUS register bits. */
#define ETHOS_U_STATUS_STATE_RUNNING   (1u << 0)
#define ETHOS_U_STATUS_IRQ_RAISED      (1u << 1)
#define ETHOS_U_STATUS_BUS_ERROR       (1u << 2)
#define ETHOS_U_STATUS_CMD_PARSE_ERROR (1u << 4)
#define ETHOS_U_STATUS_CMD_END_REACHED (1u << 5)

/* Command-stream framing. */
/* set => cmd1 (a 32-bit payload word follows the code word) */
#define ETHOS_U_CMD_PAYLOAD_BIT 0x4000
#define ETHOS_U_CMD_OPCODE_MASK 0x03ff

/* cmd0 opcodes (no payload; 16-bit immediate param). */
typedef enum {
    NPU_OP_STOP            = 0x000,
    NPU_OP_IRQ             = 0x001,
    NPU_OP_CONV            = 0x002,
    NPU_OP_DEPTHWISE       = 0x003,
    NPU_OP_POOL            = 0x005,
    NPU_OP_ELEMENTWISE     = 0x006,
    NPU_OP_DMA_START       = 0x010,
    NPU_OP_DMA_WAIT        = 0x011,
    NPU_OP_KERNEL_WAIT     = 0x012,

    NPU_SET_IFM_PAD_TOP    = 0x100,
    NPU_SET_IFM_PAD_LEFT   = 0x101,
    NPU_SET_IFM_PAD_RIGHT  = 0x102,
    NPU_SET_IFM_PAD_BOTTOM = 0x103,
    NPU_SET_IFM_DEPTH_M1   = 0x104,
    NPU_SET_IFM_PRECISION  = 0x105,
    NPU_SET_IFM_ZERO_POINT = 0x109,
    NPU_SET_IFM_WIDTH0_M1  = 0x10a,
    NPU_SET_IFM_HEIGHT0_M1 = 0x10b,
    NPU_SET_IFM_HEIGHT1_M1 = 0x10c,
    NPU_SET_IFM_REGION     = 0x10f,

    NPU_SET_OFM_WIDTH_M1   = 0x111,
    NPU_SET_OFM_HEIGHT_M1  = 0x112,
    NPU_SET_OFM_DEPTH_M1   = 0x113,
    NPU_SET_OFM_PRECISION  = 0x114,
    NPU_SET_OFM_BLK_WIDTH_M1  = 0x115,
    NPU_SET_OFM_BLK_HEIGHT_M1 = 0x116,
    NPU_SET_OFM_BLK_DEPTH_M1  = 0x117,
    NPU_SET_OFM_ZERO_POINT = 0x118,
    NPU_SET_OFM_WIDTH0_M1  = 0x11a,
    NPU_SET_OFM_HEIGHT0_M1 = 0x11b,
    NPU_SET_OFM_HEIGHT1_M1 = 0x11c,
    NPU_SET_OFM_REGION     = 0x11f,

    NPU_SET_KERNEL_WIDTH_M1  = 0x120,
    NPU_SET_KERNEL_HEIGHT_M1 = 0x121,
    NPU_SET_KERNEL_STRIDE    = 0x122,
    NPU_SET_ACC_FORMAT       = 0x124,
    NPU_SET_ACTIVATION       = 0x125,
    NPU_SET_ACTIVATION_MIN   = 0x126,
    NPU_SET_ACTIVATION_MAX   = 0x127,
    NPU_SET_WEIGHT_REGION    = 0x128,
    NPU_SET_SCALE_REGION     = 0x129,

    NPU_SET_DMA0_SRC_REGION  = 0x130,
    NPU_SET_DMA0_DST_REGION  = 0x131,

    NPU_SET_IFM2_BROADCAST   = 0x180,
    NPU_SET_IFM2_SCALAR      = 0x181,
    NPU_SET_IFM2_ZERO_POINT  = 0x189,
    NPU_SET_IFM2_REGION      = 0x18f,
} EthosUCmd0;

/* cmd1 opcodes (32-bit payload: address/length/stride). */
typedef enum {
    NPU_SET_IFM_BASE0     = 0x000,
    NPU_SET_IFM_BASE1     = 0x001,
    NPU_SET_IFM_BASE2     = 0x002,
    NPU_SET_IFM_BASE3     = 0x003,
    NPU_SET_IFM_STRIDE_X  = 0x004,
    NPU_SET_IFM_STRIDE_Y  = 0x005,
    NPU_SET_IFM_STRIDE_C  = 0x006,

    NPU_SET_OFM_BASE0     = 0x010,
    NPU_SET_OFM_BASE1     = 0x011,
    NPU_SET_OFM_BASE2     = 0x012,
    NPU_SET_OFM_BASE3     = 0x013,
    NPU_SET_OFM_STRIDE_X  = 0x014,
    NPU_SET_OFM_STRIDE_Y  = 0x015,
    NPU_SET_OFM_STRIDE_C  = 0x016,

    NPU_SET_WEIGHT_BASE   = 0x020,
    NPU_SET_WEIGHT_LENGTH = 0x021,
    NPU_SET_SCALE_BASE    = 0x022,
    NPU_SET_SCALE_LENGTH  = 0x023,
    NPU_SET_OFM_SCALE     = 0x024,
    NPU_SET_OPA_SCALE     = 0x025,
    NPU_SET_OPB_SCALE     = 0x026,

    NPU_SET_DMA0_SRC      = 0x030,
    NPU_SET_DMA0_DST      = 0x031,
    NPU_SET_DMA0_LEN      = 0x032,

    NPU_SET_IFM2_BASE0    = 0x080,
    NPU_SET_IFM2_STRIDE_X = 0x084,
    NPU_SET_IFM2_STRIDE_Y = 0x085,
    NPU_SET_IFM2_STRIDE_C = 0x086,
} EthosUCmd1;

typedef enum {
    ETHOS_U_LAYOUT_NHWC = 0,
    ETHOS_U_LAYOUT_NHCWB16 = 1,
} EthosULayout;

/*
 * A fully-resolved operation, accumulated from NPU_SET_* commands and executed
 * on the following NPU_OP_*. Register semantics are "sticky" across ops, as on
 * the hardware, so the parser keeps one descriptor for the whole stream.
 */
typedef struct EthosUOpDesc {
    /* IFM */
    int32_t ifm_w, ifm_h, ifm_c;
    uint32_t ifm_base[4];
    int32_t ifm_stride_x, ifm_stride_y, ifm_stride_c;
    int32_t ifm_zp;
    int ifm_region;
    EthosULayout ifm_layout;
    int32_t pad_top, pad_left, pad_right, pad_bottom;

    /* IFM2 (second elementwise input) */
    uint32_t ifm2_base[4];
    int32_t ifm2_stride_x, ifm2_stride_y, ifm2_stride_c;
    int32_t ifm2_zp;
    int ifm2_region;
    uint64_t ifm2_addr;
    /* IFM2 broadcast control (NPU_SET_IFM2_BROADCAST): bit0/1/2 broadcast
     * H/W/C, bit6 reverse operands, bit7 use the scalar register below. */
    uint32_t ifm2_broadcast;
    int32_t ifm2_scalar;

    /* OFM */
    int32_t ofm_w, ofm_h, ofm_c;
    uint32_t ofm_base[4];
    int32_t ofm_stride_x, ofm_stride_y, ofm_stride_c;
    int32_t ofm_zp;
    int ofm_region;
    EthosULayout ofm_layout;

    /* elementwise scaling: OFM_SCALE (mul + add output), OPA/OPB_SCALE (add
     * operands). Each is a Q31 multiplier + a (right) shift, gemmlowp style. */
    int32_t ofm_scale, ofm_scale_shift;
    int32_t opa_scale, opa_scale_shift;
    int32_t opb_scale, opb_scale_shift;
    /* elementwise add/sub: which operand is rescaled (0=none/same-scale,
     * 1=OPa/ifm, 2=OPb/ifm2), from IFM_PRECISION bits [9:8]. */
    int op_to_scale;

    /* kernel */
    int32_t kw, kh;
    int32_t stride_x, stride_y, dilation_x, dilation_y;
    bool part_kernel_first;

    /* block config / precision (needed to invert the weight reorder) */
    int32_t ofm_block_depth;
    int ifm_bitdepth;       /* 8, 16 or 32 */
    int ofm_bitdepth;       /* 8, 16 or 32 */
    /* PRECISION bit 6 selects signed (int8); clear => unsigned (uint8). The
     * compute path rebiases unsigned activations to int8 (XOR 0x80) so the
     * int8 kernels apply unchanged. */
    bool ifm_unsigned, ofm_unsigned;

    /* immediate param of the executing NPU_OP_* (e.g. pooling mode) */
    uint16_t op_param;

    /* weights / scales */
    int weight_region, scale_region;
    uint32_t weight_base, weight_len;
    uint32_t scale_base, scale_len;

    /* absolute guest addresses, resolved (basep[region] + base) at op fire */
    uint64_t ifm_addr, ofm_addr, weight_addr, scale_addr;

    /* activation clamp (in OFM int domain) */
    int32_t act_min, act_max;
    uint32_t act_type;

    /* DMA - dma_src/dma_dst are the raw (region-relative) registers; the
     * resolved absolute addresses go in dma_src_addr/dma_dst_addr so the
     * sticky raw values are never clobbered (a reused DMA reg must not be
     * region-resolved twice). */
    int dma_src_region, dma_dst_region;
    uint32_t dma_src, dma_dst, dma_len;
    uint64_t dma_src_addr, dma_dst_addr;
} EthosUOpDesc;

/*
 * Per-operation callback, invoked for each NPU_OP_{CONV,DEPTHWISE,POOL,
 * ELEMENTWISE,DMA_START} with the fully-resolved descriptor (absolute guest
 * addresses already filled in). The handler performs the actual compute/DMA.
 */
typedef void (*EthosUOpHandler)(void *ctx, uint16_t opcode,
                                const EthosUOpDesc *op);

/*
 * Pure command-stream decoder: parse [cms, cms+qsize), accumulating NPU_SET_*
 * into a descriptor and calling @handler on each NPU_OP_*. @basep gives the
 * eight region base addresses used to resolve absolute IFM/OFM/weight/scale
 * addresses. No guest-memory or QOM access - unit-testable in isolation.
 * Returns false on a malformed stream.
 */
bool ethos_u_cmdstream_decode(const uint8_t *cms, uint32_t qsize,
                              const uint64_t basep[ETHOS_U_NUM_BASEP],
                              EthosUOpHandler handler, void *ctx);

/*
 * Device entry point: fetch the command stream at [qbase, qbase+qsize) over the
 * device DMA address space and run it. May run on a worker thread; performs
 * guest memory access via dma_memory_* and must NOT touch QOM/IRQ state.
 */
bool ethos_u_cmdstream_run(EthosUState *s, hwaddr qbase, uint32_t qsize);

/* Resolve BASEP[region] from the register file (low/high pair). */
uint64_t ethos_u_region_base(EthosUState *s, int region);

/*
 * Per-operation execution handler (the EthosUOpHandler passed to the decoder).
 * @ctx is the EthosUState. Runs DMA copies and (later) the compute kernels over
 * the device DMA address space; may run on a worker thread.
 */
void ethos_u_exec_op(void *ctx, uint16_t opcode, const EthosUOpDesc *op);

#endif /* HW_NPU_ETHOS_U_INTERNAL_H */
