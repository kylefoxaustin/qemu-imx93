/*
 * Arm Ethos-U55/U65 microNPU - register command-stream parser
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Decodes the Vela-compiled register command stream: a flat sequence of
 * NPU_SET_* register writes (cmd0 = 32-bit with a 16-bit immediate; cmd1 =
 * 32-bit code word + 32-bit payload) that accumulate into a "current operation"
 * descriptor, each executed by a following NPU_OP_*. The decoder is pure (no
 * guest-memory or QOM access) so it can be unit-tested in isolation.
 *
 * Phase 1: the parser fully resolves each operation but the NPU_OP_* handlers
 * are no-ops (traced only); DMA, addressing and compute land in later phases.
 */

#include "qemu/osdep.h"
#include "ethos_u_internal.h"
#include "qemu/bswap.h"

static EthosULayout precision_layout(uint32_t precision)
{
    /* IFM/OFM precision bit 6 selects NHCWB16. */
    return (precision & (1u << 6)) ? ETHOS_U_LAYOUT_NHCWB16
                                   : ETHOS_U_LAYOUT_NHWC;
}

static void decode_kernel_stride(EthosUOpDesc *op, uint32_t v)
{
    /*
     * NPU_SET_KERNEL_STRIDE bitfield:
     *   [0] stride_x lsb, [1] stride_y lsb, [2] part_kernel_first,
     *   [3] dilation_x-1, [4] dilation_y-1,
     *   [8:6] stride_x msbs, [11:9] stride_y msbs.
     */
    uint32_t sx = (v & 0x1) | (((v >> 6) & 0x7) << 1);
    uint32_t sy = ((v >> 1) & 0x1) | (((v >> 9) & 0x7) << 1);

    op->stride_x = sx + 1;
    op->stride_y = sy + 1;
    op->part_kernel_first = (v >> 2) & 0x1;
    op->dilation_x = ((v >> 3) & 0x1) + 1;
    op->dilation_y = ((v >> 4) & 0x1) + 1;
}

/* Apply one NPU_SET_* command to the running op descriptor. */
static void apply_set(EthosUOpDesc *op, bool payload, uint16_t opcode,
                      uint16_t imm, uint32_t data)
{
    uint32_t v = payload ? data : imm;

    if (payload) {
        switch (opcode) {
        case NPU_SET_IFM_BASE0:
            op->ifm_base[0] = v;
            break;
        case NPU_SET_IFM_BASE1:
            op->ifm_base[1] = v;
            break;
        case NPU_SET_IFM_BASE2:
            op->ifm_base[2] = v;
            break;
        case NPU_SET_IFM_BASE3:
            op->ifm_base[3] = v;
            break;
        case NPU_SET_IFM_STRIDE_X:
            op->ifm_stride_x = v;
            break;
        case NPU_SET_IFM_STRIDE_Y:
            op->ifm_stride_y = v;
            break;
        case NPU_SET_IFM_STRIDE_C:
            op->ifm_stride_c = v;
            break;
        case NPU_SET_OFM_BASE0:
            op->ofm_base[0] = v;
            break;
        case NPU_SET_OFM_BASE1:
            op->ofm_base[1] = v;
            break;
        case NPU_SET_OFM_BASE2:
            op->ofm_base[2] = v;
            break;
        case NPU_SET_OFM_BASE3:
            op->ofm_base[3] = v;
            break;
        case NPU_SET_OFM_STRIDE_X:
            op->ofm_stride_x = v;
            break;
        case NPU_SET_OFM_STRIDE_Y:
            op->ofm_stride_y = v;
            break;
        case NPU_SET_OFM_STRIDE_C:
            op->ofm_stride_c = v;
            break;
        case NPU_SET_WEIGHT_BASE:
            op->weight_base = v;
            break;
        case NPU_SET_WEIGHT_LENGTH:
            op->weight_len = v;
            break;
        case NPU_SET_SCALE_BASE:
            op->scale_base = v;
            break;
        case NPU_SET_SCALE_LENGTH:
            op->scale_len = v;
            break;
        case NPU_SET_DMA0_SRC:
            op->dma_src = v;
            break;
        case NPU_SET_DMA0_DST:
            op->dma_dst = v;
            break;
        case NPU_SET_DMA0_LEN:
            op->dma_len = v;
            break;
        default:
            break;
        }
        return;
    }

    switch (opcode) {
    case NPU_SET_IFM_PAD_TOP:
        op->pad_top = v;
        break;
    case NPU_SET_IFM_PAD_LEFT:
        op->pad_left = v;
        break;
    case NPU_SET_IFM_PAD_RIGHT:
        op->pad_right = v;
        break;
    case NPU_SET_IFM_PAD_BOTTOM:
        op->pad_bottom = v;
        break;
    case NPU_SET_IFM_DEPTH_M1:
        op->ifm_c = v + 1;
        break;
    case NPU_SET_IFM_PRECISION:
        op->ifm_layout = precision_layout(v);
        /* activation_precision at bits [3:2]: 0=>8-bit, 1=>16-bit, 2=>32-bit */
        op->ifm_bitdepth = 8 << ((v >> 2) & 0x3);
        /* bit 0: 1 => signed (int8), 0 => unsigned (uint8) */
        op->ifm_unsigned = !(v & 0x1);
        break;
    case NPU_SET_IFM_ZERO_POINT:
        op->ifm_zp = (int16_t)v;
        break;
    case NPU_SET_IFM_WIDTH0_M1:
        op->ifm_w = v + 1;
        break;
    case NPU_SET_IFM_HEIGHT0_M1:
        op->ifm_h = v + 1;
        break;
    case NPU_SET_IFM_REGION:
        op->ifm_region = v;
        break;
    case NPU_SET_OFM_WIDTH_M1:
        op->ofm_w = v + 1;
        break;
    case NPU_SET_OFM_HEIGHT_M1:
        op->ofm_h = v + 1;
        break;
    case NPU_SET_OFM_DEPTH_M1:
        op->ofm_c = v + 1;
        break;
    case NPU_SET_OFM_BLK_DEPTH_M1:
        op->ofm_block_depth = v + 1;
        break;
    case NPU_SET_OFM_PRECISION:
        op->ofm_layout = precision_layout(v);
        /* bit 0: 1 => signed (int8), 0 => unsigned (uint8) */
        op->ofm_unsigned = !(v & 0x1);
        break;
    case NPU_SET_OFM_ZERO_POINT:
        op->ofm_zp = (int16_t)v;
        break;
    case NPU_SET_OFM_REGION:
        op->ofm_region = v;
        break;
    case NPU_SET_KERNEL_WIDTH_M1:
        op->kw = v + 1;
        break;
    case NPU_SET_KERNEL_HEIGHT_M1:
        op->kh = v + 1;
        break;
    case NPU_SET_KERNEL_STRIDE:
        decode_kernel_stride(op, v);
        break;
    case NPU_SET_ACTIVATION:
        op->act_type = v;
        break;
    case NPU_SET_ACTIVATION_MIN:
        op->act_min = (int16_t)v;
        break;
    case NPU_SET_ACTIVATION_MAX:
        op->act_max = (int16_t)v;
        break;
    case NPU_SET_WEIGHT_REGION:
        op->weight_region = v;
        break;
    case NPU_SET_SCALE_REGION:
        op->scale_region = v;
        break;
    case NPU_SET_DMA0_SRC_REGION:
        op->dma_src_region = v;
        break;
    case NPU_SET_DMA0_DST_REGION:
        op->dma_dst_region = v;
        break;
    default:
        break;
    }
}

static uint64_t resolve(const uint64_t basep[ETHOS_U_NUM_BASEP], int region,
                        uint32_t off)
{
    if (region < 0 || region >= ETHOS_U_NUM_BASEP) {
        return 0;
    }
    return basep[region] + off;
}

bool ethos_u_cmdstream_decode(const uint8_t *cms, uint32_t qsize,
                              const uint64_t basep[ETHOS_U_NUM_BASEP],
                              EthosUOpHandler handler, void *ctx)
{
    EthosUOpDesc op = { 0 };
    uint32_t pc = 0;

    if (qsize == 0 || qsize > ETHOS_U_CMS_MAX || (qsize & 3)) {
        return false;
    }

    while (pc + 4 <= qsize) {
        uint16_t code = lduw_le_p(cms + pc);
        uint16_t imm = lduw_le_p(cms + pc + 2);
        uint16_t opcode = code & ETHOS_U_CMD_OPCODE_MASK;
        bool payload = code & ETHOS_U_CMD_PAYLOAD_BIT;
        uint32_t data = 0;

        if (payload) {
            if (pc + 8 > qsize) {
                return false;
            }
            data = ldl_le_p(cms + pc + 4);
            pc += 8;
        } else {
            pc += 4;
        }

        /* Any command carrying a payload is a cmd1 register write. */
        if (payload || opcode >= 0x100) {
            apply_set(&op, payload, opcode, imm, data);
            continue;
        }

        switch (opcode) {
        case NPU_OP_STOP:
            return true;
        case NPU_OP_IRQ:
        case NPU_OP_DMA_WAIT:
        case NPU_OP_KERNEL_WAIT:
            /* ordering/barriers - nothing to do in a functional model */
            break;
        case NPU_OP_CONV:
        case NPU_OP_DEPTHWISE:
        case NPU_OP_POOL:
        case NPU_OP_ELEMENTWISE:
            op.ifm_addr = resolve(basep, op.ifm_region, op.ifm_base[0]);
            op.ofm_addr = resolve(basep, op.ofm_region, op.ofm_base[0]);
            op.weight_addr = resolve(basep, op.weight_region, op.weight_base);
            op.scale_addr = resolve(basep, op.scale_region, op.scale_base);
            op.op_param = imm;
            if (handler) {
                handler(ctx, opcode, &op);
            }
            break;
        case NPU_OP_DMA_START:
            op.dma_src_addr = resolve(basep, op.dma_src_region, op.dma_src);
            op.dma_dst_addr = resolve(basep, op.dma_dst_region, op.dma_dst);
            if (handler) {
                handler(ctx, opcode, &op);
            }
            break;
        default:
            break;
        }
    }

    return true;
}
