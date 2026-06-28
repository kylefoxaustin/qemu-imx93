/*
 * Arm Ethos-U55/U65 microNPU - operation execution (DMA + compute)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Executes the operations the command-stream decoder resolves, over the device
 * DMA address space: NPU_OP_DMA_START is a region copy; CONV / DEPTHWISE / POOL
 * marshal the IFM (and, for convolutions, the Vela-compiled weight+scale
 * stream) out of guest memory into plain NHWC int8 buffers, run the reference
 * kernels (ethos_u_kernels.c) and write the OFM back. Feature-map addressing
 * honours the NHWC / NHCWB16 layout via ethos_u_addr.c; weights are mlw-decoded
 * and the reorder inverted (ethos_u_weights.c); per-channel scale/bias records
 * are unpacked from the scale stream.
 *
 * Scope: single weight brick, single core (Ethos-U65-256 / U55), the
 * configuration the i.MX93 uses. Multi-core deinterleave and depth-bricked
 * weight streams are not yet handled.
 */

#include "qemu/osdep.h"
#include "hw/npu/ethos_u.h"
#include "ethos_u_internal.h"
#include "ethos_u_addr.h"
#include "ethos_u_kernels.h"
#include "ethos_u_requant.h"
#include "ethos_u_weights.h"
#include "system/dma.h"
#include "qemu/bswap.h"

#define ETHOS_U_DMA_MAX     (16 * 1024 * 1024)
#define ETHOS_U_BUF_MAX     (64 * 1024 * 1024)
#define ETHOS_U_UBLOCK      8       /* ifm/ofm micro-block depth (U55/U65) */
#define ETHOS_U_SUBKERNEL   8       /* SubKernelMax height/width */
#define ETHOS_U_BRICK       16      /* NHCWB16 channel-block depth */

static void ethos_u_dma_copy(EthosUState *s, uint64_t dst, uint64_t src,
                             uint32_t len)
{
    g_autofree uint8_t *buf = NULL;

    if (len == 0 || len > ETHOS_U_DMA_MAX) {
        return;
    }
    buf = g_malloc(len);
    if (dma_memory_read(&s->dma_as, src, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }
    dma_memory_write(&s->dma_as, dst, buf, len, MEMTXATTRS_UNSPECIFIED);
}

/* Byte span of a feature map in its layout, for a single bulk DMA read. */
static size_t ethos_u_fm_span(EthosULayout layout, int h, int w, int c,
                              uint32_t sy, uint32_t sx, uint32_t sc)
{
    if (layout == ETHOS_U_LAYOUT_NHCWB16) {
        int bricks = (c + ETHOS_U_BRICK - 1) / ETHOS_U_BRICK;
        int last = c - (bricks - 1) * ETHOS_U_BRICK;
        return (size_t)(bricks - 1) * sc + (size_t)(h - 1) * sy
               + (size_t)(w - 1) * sx + (size_t)last;
    }
    return (size_t)(h - 1) * sy + (size_t)(w - 1) * sx + (size_t)(c - 1) * sc
           + 1;
}

/* Read a feature map from guest memory into a fresh NHWC int8 buffer. */
static int8_t *ethos_u_load_fm(EthosUState *s, uint64_t addr,
                               EthosULayout layout, int h, int w, int c,
                               uint32_t sy, uint32_t sx, uint32_t sc)
{
    size_t span = ethos_u_fm_span(layout, h, w, c, sy, sx, sc);
    g_autofree uint8_t *raw = NULL;
    int8_t *nhwc;

    if ((size_t)h * w * c == 0 || span == 0 || span > ETHOS_U_BUF_MAX) {
        return NULL;
    }
    raw = g_malloc(span);
    if (dma_memory_read(&s->dma_as, addr, raw, span,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return NULL;
    }
    nhwc = g_malloc((size_t)h * w * c);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int ch = 0; ch < c; ch++) {
                uint64_t off = ethos_u_fm_offset(layout, y, x, ch,
                                                 sy, sx, sc, 1);
                nhwc[(y * w + x) * c + ch] = (int8_t)raw[off];
            }
        }
    }
    return nhwc;
}

/* Write an NHWC int8 buffer back to guest memory in the OFM layout. */
static void ethos_u_store_fm(EthosUState *s, uint64_t addr,
                             EthosULayout layout, int h, int w, int c,
                             uint32_t sy, uint32_t sx, uint32_t sc,
                             const int8_t *nhwc)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int ch = 0; ch < c; ch++) {
                uint64_t off = ethos_u_fm_offset(layout, y, x, ch,
                                                 sy, sx, sc, 1);
                uint8_t b = (uint8_t)nhwc[(y * w + x) * c + ch];
                dma_memory_write(&s->dma_as, addr + off, &b, 1,
                                 MEMTXATTRS_UNSPECIFIED);
            }
        }
    }
}

/* Byte span of a feature map for an element size of @elem bytes. */
static size_t ethos_u_fm_byte_span(EthosULayout layout, int h, int w, int c,
                                   uint32_t sy, uint32_t sx, uint32_t sc,
                                   int elem)
{
    uint64_t last;
    if (layout == ETHOS_U_LAYOUT_NHCWB16) {
        int bricks = (c + ETHOS_U_BRICK - 1) / ETHOS_U_BRICK;
        int lastc = c - (bricks - 1) * ETHOS_U_BRICK;
        last = (uint64_t)(bricks - 1) * sc + (uint64_t)(h - 1) * sy
             + (uint64_t)(w - 1) * sx + (uint64_t)(lastc - 1) * elem;
    } else {
        last = (uint64_t)(h - 1) * sy + (uint64_t)(w - 1) * sx
             + (uint64_t)(c - 1) * sc;
    }
    return (size_t)last + elem;
}

/*
 * Read a feature map of @elem-byte signed elements (1/2/4) into a fresh int32
 * NHWC buffer, sign-extending. Generalises ethos_u_load_fm to int16/int32
 * activations (softmax intermediates run in int32).
 */
static int32_t *ethos_u_load_fm_typed(EthosUState *s, uint64_t addr,
                                      EthosULayout layout, int h, int w, int c,
                                      uint32_t sy, uint32_t sx, uint32_t sc,
                                      int elem)
{
    size_t span = ethos_u_fm_byte_span(layout, h, w, c, sy, sx, sc, elem);
    g_autofree uint8_t *raw = NULL;
    int32_t *out;

    if ((size_t)h * w * c == 0 || span == 0 || span > ETHOS_U_BUF_MAX) {
        return NULL;
    }
    raw = g_malloc(span);
    if (dma_memory_read(&s->dma_as, addr, raw, span,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return NULL;
    }
    out = g_new(int32_t, (size_t)h * w * c);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int ch = 0; ch < c; ch++) {
                uint64_t off = ethos_u_fm_offset(layout, y, x, ch,
                                                 sy, sx, sc, elem);
                int32_t v;
                if (elem == 1) {
                    v = (int8_t)raw[off];
                } else if (elem == 2) {
                    v = (int16_t)lduw_le_p(raw + off);
                } else {
                    v = (int32_t)ldl_le_p(raw + off);
                }
                out[(y * w + x) * c + ch] = v;
            }
        }
    }
    return out;
}

/* Write an int32 NHWC buffer back as @elem-byte elements (1/2/4). */
static void ethos_u_store_fm_typed(EthosUState *s, uint64_t addr,
                                   EthosULayout layout, int h, int w, int c,
                                   uint32_t sy, uint32_t sx, uint32_t sc,
                                   int elem, const int32_t *buf)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int ch = 0; ch < c; ch++) {
                uint64_t off = ethos_u_fm_offset(layout, y, x, ch,
                                                 sy, sx, sc, elem);
                int32_t v = buf[(y * w + x) * c + ch];
                uint8_t b[4];
                if (elem == 1) {
                    b[0] = (uint8_t)v;
                } else if (elem == 2) {
                    stw_le_p(b, (uint16_t)v);
                } else {
                    stl_le_p(b, (uint32_t)v);
                }
                dma_memory_write(&s->dma_as, addr + off, b, elem,
                                 MEMTXATTRS_UNSPECIFIED);
            }
        }
    }
}

/*
 * Decode the Vela weight stream into an OHWI int16 volume. Weights are kept
 * 16-bit: uint8 (legacy) models recentre weights by the weight zero-point,
 * producing 9-bit-signed values (e.g. [-151,104]) that would wrap if truncated
 * to int8. For int8 models the values are in [-127,127] and int16 is exact too.
 */
static int16_t *ethos_u_load_weights(EthosUState *s, const EthosUOpDesc *op,
                                     bool depthwise)
{
    int ofm_c = op->ofm_c;
    int ifm_c = depthwise ? 1 : op->ifm_c;
    size_t n_ohwi = (size_t)ofm_c * op->kh * op->kw * ifm_c;
    int bitdepth = op->ifm_bitdepth ? op->ifm_bitdepth : 8;
    g_autofree uint8_t *enc = NULL;
    g_autofree int16_t *flat = NULL;
    int16_t *ohwi16;
    int n_flat;

    EthosUWeightParams wp = {
        .ifm_ublock_depth = ETHOS_U_UBLOCK,
        .ofm_ublock_depth = ETHOS_U_UBLOCK,
        .ofm_depth = ofm_c,
        .kernel_h = op->kh,
        .kernel_w = op->kw,
        .ifm_depth = ifm_c,
        .ofm_block_depth = op->ofm_block_depth ? op->ofm_block_depth : ofm_c,
        .is_depthwise = depthwise,
        .is_partkernel = op->part_kernel_first,
        .ifm_bitdepth = bitdepth,
        .decomp_h = ETHOS_U_SUBKERNEL / (op->dilation_y ? op->dilation_y : 1),
        .decomp_w = ETHOS_U_SUBKERNEL / (op->dilation_x ? op->dilation_x : 1),
    };

    if (op->weight_len == 0 || op->weight_len > ETHOS_U_BUF_MAX ||
        n_ohwi == 0 || n_ohwi > ETHOS_U_BUF_MAX) {
        return NULL;
    }
    enc = g_malloc(op->weight_len);
    if (dma_memory_read(&s->dma_as, op->weight_addr, enc, op->weight_len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return NULL;
    }
    n_flat = ethos_u_weights_decode(enc, op->weight_len, &flat);
    if (n_flat <= 0) {
        return NULL;
    }
    ohwi16 = g_new0(int16_t, n_ohwi);
    if (ethos_u_weights_reorder_inverse(ohwi16, flat, n_flat, &wp) < 0) {
        g_free(ohwi16);
        return NULL;
    }
    return ohwi16;
}

/* Read @n per-channel 10-byte scale/bias records from the scale stream. */
static EthosUScaleBias *ethos_u_load_scales(EthosUState *s, uint64_t addr,
                                            int n)
{
    g_autofree uint8_t *buf = NULL;
    EthosUScaleBias *sb;

    if (n <= 0 || (size_t)n * 10 > ETHOS_U_BUF_MAX) {
        return NULL;
    }
    buf = g_malloc((size_t)n * 10);
    if (dma_memory_read(&s->dma_as, addr, buf, (size_t)n * 10,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return NULL;
    }
    sb = g_new0(EthosUScaleBias, n);
    for (int i = 0; i < n; i++) {
        ethos_u_unpack_scale_bias(buf + (size_t)i * 10, &sb[i]);
    }
    return sb;
}

static void ethos_u_conv_params(EthosUConvParams *p, const EthosUOpDesc *op)
{
    p->ifm_h = op->ifm_h;
    p->ifm_w = op->ifm_w;
    p->ifm_c = op->ifm_c;
    p->ofm_h = op->ofm_h;
    p->ofm_w = op->ofm_w;
    p->ofm_c = op->ofm_c;
    p->kh = op->kh;
    p->kw = op->kw;
    p->stride_y = op->stride_y ? op->stride_y : 1;
    p->stride_x = op->stride_x ? op->stride_x : 1;
    p->dilation_y = op->dilation_y ? op->dilation_y : 1;
    p->dilation_x = op->dilation_x ? op->dilation_x : 1;
    p->pad_top = op->pad_top;
    p->pad_left = op->pad_left;
    /*
     * uint8 (unsigned) activations are rebiased to int8 by XOR 0x80 in
     * load/store; shift the matching zero-points and clamps by -128 so the
     * int8 kernels compute the identical result. int8 path: offsets are 0.
     */
    p->ifm_zp = op->ifm_zp - (op->ifm_unsigned ? 128 : 0);
    p->ofm_zp = op->ofm_zp - (op->ofm_unsigned ? 128 : 0);
    p->act_min = op->act_min - (op->ofm_unsigned ? 128 : 0);
    p->act_max = op->act_max - (op->ofm_unsigned ? 128 : 0);
}

static void ethos_u_exec_conv(EthosUState *s, const EthosUOpDesc *op,
                              bool depthwise)
{
    g_autofree int8_t *ifm = NULL;
    g_autofree int16_t *ohwi = NULL;
    g_autofree int8_t *ofm = NULL;
    g_autofree EthosUScaleBias *sb = NULL;
    EthosUConvParams p;

    ifm = ethos_u_load_fm(s, op->ifm_addr, op->ifm_layout, op->ifm_h,
                          op->ifm_w, op->ifm_c, op->ifm_stride_y,
                          op->ifm_stride_x, op->ifm_stride_c);
    ohwi = ethos_u_load_weights(s, op, depthwise);
    sb = ethos_u_load_scales(s, op->scale_addr, op->ofm_c);
    if (!ifm || !ohwi || !sb) {
        return;
    }
    /* unsigned (uint8) IFM: rebias each byte to int8 (XOR 0x80) so the
     * int8 kernels apply (see ethos_u_conv_params zero-point shift). */
    if (op->ifm_unsigned) {
        size_t nin = (size_t)op->ifm_h * op->ifm_w * op->ifm_c;
        for (size_t i = 0; i < nin; i++) {
            ifm[i] ^= (int8_t)0x80;
        }
    }
    ethos_u_conv_params(&p, op);

    /* Depthwise wants its OHWI (ifm_depth 1) kernel as HWC. */
    g_autofree int16_t *hwc = NULL;
    if (depthwise) {
        hwc = g_new0(int16_t, (size_t)op->kh * op->kw * op->ofm_c);
        for (int c = 0; c < op->ofm_c; c++) {
            for (int ky = 0; ky < op->kh; ky++) {
                for (int kx = 0; kx < op->kw; kx++) {
                    hwc[(ky * op->kw + kx) * op->ofm_c + c] =
                        ohwi[(c * op->kh + ky) * op->kw + kx];
                }
            }
        }
    }

    /*
     * int32 (or int16) OFM: Vela's reduction sum stage (global average pool
     * lowered as a depthwise that sums the window) wants the raw accumulator,
     * not a requantised byte. Emit it via the typed store; a later elementwise
     * op applies the 1/N multiply.
     */
    if (op->ofm_bitdepth > 8) {
        int elem_out = op->ofm_bitdepth / 8;
        g_autofree int32_t *ofm32 =
            g_new0(int32_t, (size_t)op->ofm_h * op->ofm_w * op->ofm_c);
        if (depthwise) {
            ethos_u_depthwise_int32(ofm32, ifm, hwc, sb, &p);
        } else {
            ethos_u_conv2d_int32(ofm32, ifm, ohwi, sb, &p);
        }
        ethos_u_store_fm_typed(s, op->ofm_addr, op->ofm_layout, op->ofm_h,
                               op->ofm_w, op->ofm_c, op->ofm_stride_y,
                               op->ofm_stride_x, op->ofm_stride_c, elem_out,
                               ofm32);
        return;
    }

    ofm = g_new0(int8_t, (size_t)op->ofm_h * op->ofm_w * op->ofm_c);
    if (depthwise) {
        ethos_u_depthwise_int8(ofm, ifm, hwc, sb, &p);
    } else {
        ethos_u_conv2d_int8(ofm, ifm, ohwi, sb, &p);
    }

    /* unsigned (uint8) OFM: rebias the int8 result back to uint8 (XOR 0x80). */
    if (op->ofm_unsigned) {
        size_t nout = (size_t)op->ofm_h * op->ofm_w * op->ofm_c;
        for (size_t i = 0; i < nout; i++) {
            ofm[i] ^= (int8_t)0x80;
        }
    }
    ethos_u_store_fm(s, op->ofm_addr, op->ofm_layout, op->ofm_h, op->ofm_w,
                     op->ofm_c, op->ofm_stride_y, op->ofm_stride_x,
                     op->ofm_stride_c, ofm);
}

/*
 * REDUCE_SUM / wide pooling: reduce the IFM channels to the (single-channel)
 * OFM, in int32. Used by the lowered softmax reduce-sum (sum of exp). MAX is
 * also supported for completeness; OFM_SCALE is applied when non-identity.
 */
static void ethos_u_exec_pool_wide(EthosUState *s, const EthosUOpDesc *op)
{
    int elem_in = op->ifm_bitdepth ? op->ifm_bitdepth / 8 : 1;
    int elem_out = op->ofm_bitdepth ? op->ofm_bitdepth / 8 : 1;
    bool reduce_sum = (op->op_param == 2);
    g_autofree int32_t *ifm = NULL;
    g_autofree int32_t *ofm = NULL;

    ifm = ethos_u_load_fm_typed(s, op->ifm_addr, op->ifm_layout, op->ifm_h,
                                op->ifm_w, op->ifm_c, op->ifm_stride_y,
                                op->ifm_stride_x, op->ifm_stride_c, elem_in);
    if (!ifm) {
        return;
    }
    bool explicit_shift = (op->ofm_scale == 1);
    bool requant = !explicit_shift && op->ofm_scale != 0 &&
                   !(op->ofm_scale == (1 << 30) &&
                     (op->ofm_scale_shift == 30 || op->ofm_scale_shift == 31));
    ofm = g_new0(int32_t, (size_t)op->ofm_h * op->ofm_w);
    for (int y = 0; y < op->ofm_h; y++) {
        for (int x = 0; x < op->ofm_w; x++) {
            int64_t acc = reduce_sum ? 0 : INT32_MIN;
            for (int c = 0; c < op->ifm_c; c++) {
                int32_t v = ifm[(y * op->ifm_w + x) * op->ifm_c + c];
                acc = reduce_sum ? acc + v : MAX(acc, v);
            }
            if (explicit_shift) {
                int sh = op->ofm_scale_shift;
                if (sh > 0) {
                    acc = (acc + ((int64_t)1 << (sh - 1))) >> sh;
                }
                if (elem_out == 1) {
                    acc += op->ofm_zp;
                }
            } else if (requant) {
                acc = ethos_u_mul_by_quant_mult((int32_t)acc, op->ofm_scale,
                                                31 - op->ofm_scale_shift);
                if (elem_out == 1) {
                    acc += op->ofm_zp;
                }
            }
            ofm[y * op->ofm_w + x] = (int32_t)acc;
        }
    }
    ethos_u_store_fm_typed(s, op->ofm_addr, op->ofm_layout, op->ofm_h,
                           op->ofm_w, 1, op->ofm_stride_y, op->ofm_stride_x,
                           op->ofm_stride_c, elem_out, ofm);
}

static void ethos_u_exec_pool(EthosUState *s, const EthosUOpDesc *op)
{
    g_autofree int8_t *ifm = NULL;
    g_autofree int8_t *ofm = NULL;
    EthosUPoolParams p;
    /* NPU_OP_POOL immediate: 0 = MAX, 1 = AVERAGE, 2 = REDUCE_SUM. */
    EthosUPoolType type = (op->op_param == 1) ? ETHOS_U_POOL_AVG
                                              : ETHOS_U_POOL_MAX;

    if (op->op_param == 2 || op->ifm_bitdepth > 8) {
        ethos_u_exec_pool_wide(s, op);
        return;
    }

    ifm = ethos_u_load_fm(s, op->ifm_addr, op->ifm_layout, op->ifm_h,
                          op->ifm_w, op->ifm_c, op->ifm_stride_y,
                          op->ifm_stride_x, op->ifm_stride_c);
    if (!ifm) {
        return;
    }
    /* unsigned (uint8) IFM: rebias to int8 (XOR 0x80). Pooling preserves the
     * quant params (out == in), so the rebias cancels on store. */
    if (op->ifm_unsigned) {
        size_t nin = (size_t)op->ifm_h * op->ifm_w * op->ifm_c;
        for (size_t i = 0; i < nin; i++) {
            ifm[i] ^= (int8_t)0x80;
        }
    }
    ofm = g_new0(int8_t, (size_t)op->ofm_h * op->ofm_w * op->ofm_c);

    p.ifm_h = op->ifm_h;
    p.ifm_w = op->ifm_w;
    p.c = op->ifm_c;
    p.ofm_h = op->ofm_h;
    p.ofm_w = op->ofm_w;
    p.ofm_c = op->ofm_c;
    p.kh = op->kh;
    p.kw = op->kw;
    p.stride_y = op->stride_y ? op->stride_y : 1;
    p.stride_x = op->stride_x ? op->stride_x : 1;
    p.pad_top = op->pad_top;
    p.pad_left = op->pad_left;
    p.act_min = op->act_min - (op->ofm_unsigned ? 128 : 0);
    p.act_max = op->act_max - (op->ofm_unsigned ? 128 : 0);

    ethos_u_pool_int8(ofm, ifm, type, &p);
    /* unsigned (uint8) OFM: rebias the int8 result back to uint8. */
    if (op->ofm_unsigned) {
        size_t nout = (size_t)op->ofm_h * op->ofm_w * op->ofm_c;
        for (size_t i = 0; i < nout; i++) {
            ofm[i] ^= (int8_t)0x80;
        }
    }
    ethos_u_store_fm(s, op->ofm_addr, op->ofm_layout, op->ofm_h, op->ofm_w,
                     op->ofm_c, op->ofm_stride_y, op->ofm_stride_x,
                     op->ofm_stride_c, ofm);
}

/* Elementwise sub-type, from the NPU_OP_ELEMENTWISE immediate (elementwise_mode). */
enum {
    ETHOS_U_EW_MUL = 0,
    ETHOS_U_EW_ADD = 1,
    ETHOS_U_EW_SUB = 2,
    ETHOS_U_EW_CLZ = 7,
    ETHOS_U_EW_SHR = 8,
    ETHOS_U_EW_SHL = 9,
};

/* Advanced (8-bit) add/sub scaling pre-shifts each operand into int32. */
#define ETHOS_U_EW_LEFT_SHIFT 20

/* NPU_SET_IFM2_BROADCAST flags. */
#define ETHOS_U_IFM2_BC_H      (1u << 0)
#define ETHOS_U_IFM2_BC_W      (1u << 1)
#define ETHOS_U_IFM2_BC_C      (1u << 2)
#define ETHOS_U_IFM2_REVERSE   (1u << 6)
#define ETHOS_U_IFM2_SCALAR    (1u << 7)

/* TABLE_0..TABLE_7 activation functions occupy activation_function 16..23. */
static bool ethos_u_act_is_lut(uint32_t act)
{
    return (act & 0x1f) >= 16 && (act & 0x1f) <= 23;
}

/*
 * Wide (int16/int32) and LUT/shift elementwise path, used for the int32
 * fixed-point chain a lowered softmax runs in (exp via the activation LUT,
 * a reduce-sum, then a Newton-Raphson reciprocal of CLZ/SHL/SHR/MUL/ADD/SUB)
 * and for the int8 SUB whose result indexes the exp LUT. Integer ops are raw;
 * OFM_SCALE is applied when it is not the identity. The plain int8 add/sub/mul
 * (with operand scaling) stays on the dedicated path below.
 */
static void ethos_u_exec_elementwise_wide(EthosUState *s,
                                          const EthosUOpDesc *op, bool is_lut)
{
    int mode = op->op_param;
    int elem_in = op->ifm_bitdepth ? op->ifm_bitdepth / 8 : 1;
    int elem_out = op->ofm_bitdepth ? op->ofm_bitdepth / 8 : 1;
    size_t n = (size_t)op->ofm_h * op->ofm_w * op->ofm_c;
    g_autofree int32_t *ifm = NULL;
    g_autofree int32_t *ifm2 = NULL;
    g_autofree int32_t *ofm = NULL;
    bool have_b = (mode != ETHOS_U_EW_CLZ);

    ifm = ethos_u_load_fm_typed(s, op->ifm_addr, op->ifm_layout, op->ifm_h,
                                op->ifm_w, op->ifm_c, op->ifm_stride_y,
                                op->ifm_stride_x, op->ifm_stride_c, elem_in);
    if (!ifm) {
        return;
    }
    if (have_b) {
        ifm2 = g_new(int32_t, n);
        if (op->ifm2_broadcast & ETHOS_U_IFM2_SCALAR) {
            /* IFM2 is a single scalar held in the IFM2_SCALAR register. */
            for (size_t i = 0; i < n; i++) {
                ifm2[i] = op->ifm2_scalar;
            }
        } else {
            /*
             * Load IFM2 with each broadcast dimension collapsed to size 1
             * (a zero stride is not enough: NHCWB16 keeps a within-brick
             * channel offset), then replicate it across the broadcast dims.
             */
            int bh = (op->ifm2_broadcast & ETHOS_U_IFM2_BC_H);
            int bw = (op->ifm2_broadcast & ETHOS_U_IFM2_BC_W);
            int bc = (op->ifm2_broadcast & ETHOS_U_IFM2_BC_C);
            int h2 = bh ? 1 : op->ifm_h;
            int w2 = bw ? 1 : op->ifm_w;
            int c2 = bc ? 1 : op->ifm_c;
            g_autofree int32_t *small =
                ethos_u_load_fm_typed(s, op->ifm2_addr, op->ifm_layout,
                                      h2, w2, c2, op->ifm2_stride_y,
                                      op->ifm2_stride_x, op->ifm2_stride_c,
                                      elem_in);
            if (!small) {
                return;
            }
            for (int y = 0; y < op->ifm_h; y++) {
                for (int x = 0; x < op->ifm_w; x++) {
                    for (int c = 0; c < op->ifm_c; c++) {
                        int sy2 = bh ? 0 : y, sx2 = bw ? 0 : x, sc2 = bc ? 0 : c;
                        ifm2[(size_t)(y * op->ifm_w + x) * op->ifm_c + c] =
                            small[(size_t)(sy2 * w2 + sx2) * c2 + sc2];
                    }
                }
            }
        }
        if (!ifm2) {
            return;
        }
    }
    bool rev = have_b && (op->ifm2_broadcast & ETHOS_U_IFM2_REVERSE);
    /* Reverse operand order (e.g. const - ifm): swap the two input buffers
     * (zero-points are swapped below). */
    if (rev) {
        int32_t *t = ifm; ifm = ifm2; ifm2 = t;
    }
    /* uint8 inputs feeding the int8-domain SUB+LUT are rebiased like elsewhere.
     * The buffer is int32 (sign-extended), so re-truncate to int8 after the XOR
     * (uint8 u -> int8 u-128). */
    if (elem_in == 1 && op->ifm_unsigned) {
        for (size_t i = 0; i < n; i++) {
            ifm[i] = (int8_t)(ifm[i] ^ 0x80);
            if (have_b) {
                ifm2[i] = (int8_t)(ifm2[i] ^ 0x80);
            }
        }
    }
    int zp_a = op->ifm_zp - (elem_in == 1 && op->ifm_unsigned ? 128 : 0);
    int zp_b = op->ifm2_zp - (elem_in == 1 && op->ifm_unsigned ? 128 : 0);
    int in1_zp = rev ? zp_b : zp_a;
    int in2_zp = rev ? zp_a : zp_b;
    int out_zp = op->ofm_zp - (elem_out == 1 && op->ofm_unsigned ? 128 : 0);
    int amin = op->act_min - (elem_out == 1 && op->ofm_unsigned ? 128 : 0);
    int amax = op->act_max - (elem_out == 1 && op->ofm_unsigned ? 128 : 0);
    /*
     * OFM_SCALE encoding for these ops: a value of 1 means "explicit scaling"
     * (the result is arithmetic-shifted right by ofm_scale_shift, rounded);
     * 2^30 is the no_scale identity for the gemmlowp SRDHM domain - the
     * command-stream encodes it with shift 31 (M0=2^30 in Q0.31 == unit), so a
     * MUL there is a bare SaturatingRoundingDoublingHighMul with no post-scale.
     * Vela's lowered softmax reciprocal (gemmlowp one_over_one_plus_x) relies on
     * this: applying a spurious requant >>1 to each FixedPoint multiply halves
     * the result. shift 30 is also accepted as identity (mul_by_quant_mult by
     * 2^30 with shift arg 1 is itself a no-op). Any other value is a real Q31
     * gemmlowp multiplier.
     */
    bool explicit_shift = (op->ofm_scale == 1);
    bool requant = !explicit_shift && op->ofm_scale != 0 &&
                   !(op->ofm_scale == (1 << 30) &&
                     (op->ofm_scale_shift == 30 || op->ofm_scale_shift == 31));

    ofm = g_new0(int32_t, n);
    for (size_t i = 0; i < n; i++) {
        int32_t a = ifm[i];
        int32_t b = have_b ? ifm2[i] : 0;
        int64_t v;

        switch (mode) {
        case ETHOS_U_EW_MUL:
            /*
             * Explicit-scaling muls (OFM_SCALE 1) are plain integer products
             * (one operand is a small constant); Q31-scaled muls use the
             * gemmlowp saturating-rounding doubling-high-mul.
             */
            v = explicit_shift ? (int64_t)a * b
                               : ethos_u_mul_by_quant_mult(a, b, 0);
            break;
        case ETHOS_U_EW_ADD:
            v = (int64_t)a + b;
            break;
        case ETHOS_U_EW_SUB:
            v = (int64_t)(a - in1_zp) - (b - in2_zp);
            break;
        case ETHOS_U_EW_SHR:
            v = b >= 0 ? (a >> (b & 31)) : ((int64_t)a << ((-b) & 31));
            break;
        case ETHOS_U_EW_SHL:
            v = (int64_t)a << (b & 31);
            break;
        case ETHOS_U_EW_CLZ:
            v = a ? __builtin_clz((uint32_t)a) : 32;
            break;
        default:
            /* Unmodelled elementwise mode: zero-fill, or honest-fault if opted in. */
            v = 0;
            if (s->honest_fault) {
                s->op_failed = true;
            }
            break;
        }

        if (is_lut) {
            /* LUT input is the result in the OFM quant domain (+ zero-point);
             * index = clamp(input, act_min, act_max) - act_min. For the
             * softmax exp table (out_zp 127) the winner (diff 0) maps to 255. */
            int in = (int)v + out_zp;
            in = in < amin ? amin : (in > amax ? amax : in);
            int idx = in - amin;
            if (idx < 0) {
                idx = 0;
            } else if (idx > 255) {
                idx = 255;
            }
            v = (int32_t)s->lut[idx];
        } else if (explicit_shift) {
            int sh = op->ofm_scale_shift;
            if (sh > 0) {
                v = (v + ((int64_t)1 << (sh - 1))) >> sh;   /* rounded */
            }
            /* int32 intermediates carry no zero-point (a stale OFM_ZERO_POINT
             * from the preceding conv must not leak in); only the final
             * quantised (int8/uint8) output is offset. */
            if (elem_out == 1) {
                v += out_zp;
            }
        } else if (requant) {
            v = ethos_u_mul_by_quant_mult((int32_t)v, op->ofm_scale,
                                          31 - op->ofm_scale_shift);
            if (elem_out == 1) {
                v += out_zp;
            }
        }
        if (elem_out == 1) {            /* int8 output: apply the clamp */
            if (v < amin) {
                v = amin;
            } else if (v > amax) {
                v = amax;
            }
        } else {                        /* int32: saturate, don't wrap */
            if (v > INT32_MAX) {
                v = INT32_MAX;
            } else if (v < INT32_MIN) {
                v = INT32_MIN;
            }
        }
        ofm[i] = (int32_t)v;
    }
    if (elem_out == 1 && op->ofm_unsigned) {
        for (size_t i = 0; i < n; i++) {
            ofm[i] ^= 0x80;
        }
    }
    ethos_u_store_fm_typed(s, op->ofm_addr, op->ofm_layout, op->ofm_h,
                           op->ofm_w, op->ofm_c, op->ofm_stride_y,
                           op->ofm_stride_x, op->ofm_stride_c, elem_out, ofm);
}

static void ethos_u_exec_elementwise(EthosUState *s, const EthosUOpDesc *op)
{
    g_autofree int8_t *ifm = NULL;
    g_autofree int8_t *ifm2 = NULL;
    g_autofree int8_t *ofm = NULL;
    int mode = op->op_param;
    size_t n = (size_t)op->ofm_h * op->ofm_w * op->ofm_c;
    bool is_lut = s->lut_loaded && ethos_u_act_is_lut(op->act_type);

    /* Wide / shift / CLZ / LUT ops go through the int32 path. */
    if (op->ifm_bitdepth > 8 || op->ofm_bitdepth > 8 || is_lut ||
        mode == ETHOS_U_EW_CLZ || mode == ETHOS_U_EW_SHR ||
        mode == ETHOS_U_EW_SHL) {
        ethos_u_exec_elementwise_wide(s, op, is_lut);
        return;
    }

    if (mode != ETHOS_U_EW_MUL && mode != ETHOS_U_EW_ADD &&
        mode != ETHOS_U_EW_SUB) {
        /* MIN/MAX/LRELU/ABS not modelled yet: leave OFM untouched, or
         * honest-fault the run if the operator opted in. */
        if (s->honest_fault) {
            s->op_failed = true;
        }
        return;
    }

    ifm = ethos_u_load_fm(s, op->ifm_addr, op->ifm_layout, op->ifm_h,
                          op->ifm_w, op->ifm_c, op->ifm_stride_y,
                          op->ifm_stride_x, op->ifm_stride_c);
    ifm2 = ethos_u_load_fm(s, op->ifm2_addr, op->ifm_layout, op->ifm_h,
                           op->ifm_w, op->ifm_c, op->ifm2_stride_y,
                           op->ifm2_stride_x, op->ifm2_stride_c);
    if (!ifm || !ifm2) {
        return;
    }
    /* unsigned (uint8): rebias both inputs to int8; zero-points shift -128. */
    if (op->ifm_unsigned) {
        for (size_t i = 0; i < n; i++) {
            ifm[i] ^= (int8_t)0x80;
            ifm2[i] ^= (int8_t)0x80;
        }
    }
    ofm = g_new0(int8_t, n);

    int in1_zp = op->ifm_zp - (op->ifm_unsigned ? 128 : 0);
    int in2_zp = op->ifm2_zp - (op->ifm_unsigned ? 128 : 0);
    int out_zp = op->ofm_zp - (op->ofm_unsigned ? 128 : 0);
    int amin = op->act_min - (op->ofm_unsigned ? 128 : 0);
    int amax = op->act_max - (op->ofm_unsigned ? 128 : 0);

    /*
     * The OFM/OPA/OPB_SCALE registers carry a Q31 multiplier and a full shift
     * such that the result is round(x * mult >> shift). The gemmlowp kernels
     * take a "vela shift" instead, where the >>31 of the doubling-high-mul is
     * implicit; convert with (31 - register_shift), as the conv path does.
     */
    if (mode == ETHOS_U_EW_MUL) {
        EthosUMulParams p = {
            .n = n, .in1_zp = in1_zp, .in2_zp = in2_zp, .out_zp = out_zp,
            .out_mult = op->ofm_scale, .out_shift = 31 - op->ofm_scale_shift,
            .act_min = amin, .act_max = amax,
        };
        ethos_u_ew_mul_int8(ofm, ifm, ifm2, &p);
    } else {
        /*
         * Add/Sub. Vela brings both operands to a common scale of
         * 2*max_input_scale / 2^lsh (lsh = 20 for 8-bit). In the "advanced"
         * case (opb_scale == 0) only the smaller-scaled operand, selected by
         * op_to_scale (OPa=ifm, OPb=ifm2), is rescaled with OPA_SCALE; the
         * larger one is exactly << (lsh-1) (its rescale is 2^(lsh-1)). In the
         * "same scale" case both operands carry their own OPA/OPB multiplier
         * with shift 0. The sum is then requantised by OFM_SCALE.
         */
        const int lsh = ETHOS_U_EW_LEFT_SHIFT;
        bool sub = (mode == ETHOS_U_EW_SUB);
        for (size_t i = 0; i < n; i++) {
            int32_t a = (int32_t)ifm[i] - in1_zp;
            int32_t b = (int32_t)ifm2[i] - in2_zp;
            int32_t sa, sb;

            if (op->opb_scale == 0) {
                int32_t scaled = ethos_u_mul_by_quant_mult(
                    (op->op_to_scale == 2) ? b : a, op->opa_scale,
                    31 - op->opa_scale_shift);
                int32_t plain = ((op->op_to_scale == 2) ? a : b) << (lsh - 1);
                sa = (op->op_to_scale == 2) ? plain : scaled;
                sb = (op->op_to_scale == 2) ? scaled : plain;
            } else {
                sa = ethos_u_mul_by_quant_mult(a << lsh, op->opa_scale,
                                               31 - op->opa_scale_shift);
                sb = ethos_u_mul_by_quant_mult(b << lsh, op->opb_scale,
                                               31 - op->opb_scale_shift);
            }
            int32_t raw = sub ? sa - sb : sa + sb;
            int32_t v = ethos_u_mul_by_quant_mult(raw, op->ofm_scale,
                                                  31 - op->ofm_scale_shift);
            v += out_zp;
            v = v < amin ? amin : (v > amax ? amax : v);
            ofm[i] = (int8_t)v;
        }
    }

    if (op->ofm_unsigned) {
        for (size_t i = 0; i < n; i++) {
            ofm[i] ^= (int8_t)0x80;
        }
    }
    ethos_u_store_fm(s, op->ofm_addr, op->ofm_layout, op->ofm_h, op->ofm_w,
                     op->ofm_c, op->ofm_stride_y, op->ofm_stride_x,
                     op->ofm_stride_c, ofm);
}

void ethos_u_exec_op(void *ctx, uint16_t opcode, const EthosUOpDesc *op)
{
    EthosUState *s = ctx;

    switch (opcode) {
    case NPU_OP_DMA_START:
        /*
         * A DMA into the on-chip LUT slot (destination address 0) loads the
         * activation lookup table (e.g. softmax exp / 1-over-1+x), not guest
         * memory; capture it into s->lut for the next TABLE activation.
         */
        if (op->dma_dst_addr == 0 && op->dma_len >= 4 &&
            op->dma_len <= sizeof(s->lut)) {
            g_autofree uint8_t *buf = g_malloc(op->dma_len);
            if (dma_memory_read(&s->dma_as, op->dma_src_addr, buf, op->dma_len,
                                MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
                for (unsigned i = 0; i < op->dma_len / 4; i++) {
                    s->lut[i] = ldl_le_p(buf + i * 4);
                }
                s->lut_loaded = true;
            }
        } else {
            ethos_u_dma_copy(s, op->dma_dst_addr, op->dma_src_addr,
                             op->dma_len);
        }
        break;
    case NPU_OP_CONV:
        ethos_u_exec_conv(s, op, false);
        break;
    case NPU_OP_DEPTHWISE:
        ethos_u_exec_conv(s, op, true);
        break;
    case NPU_OP_POOL:
        ethos_u_exec_pool(s, op);
        break;
    case NPU_OP_ELEMENTWISE:
        ethos_u_exec_elementwise(s, op);
        break;
    default:
        /* Unimplemented opcode: honest-fault the run if the operator opted in. */
        if (s->honest_fault) {
            s->op_failed = true;
        }
        break;
    }
}
