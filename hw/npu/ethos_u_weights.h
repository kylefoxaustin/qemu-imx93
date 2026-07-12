/*
 * Arm Ethos-U55/U65 microNPU - weight and scale/bias decoding
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Decodes the Vela-compiled weight stream the guest hands to the NPU: mlw
 * entropy decode (vendored, hw/npu/mlw/) followed by the inverse of Vela's
 * NPU traversal reorder (our own code), plus the 10-byte scale/bias unpack.
 */

#ifndef HW_NPU_ETHOS_U_WEIGHTS_H
#define HW_NPU_ETHOS_U_WEIGHTS_H

/* One unpacked scale/bias record (per OFM channel). */
typedef struct EthosUScaleBias {
    int64_t bias;       /* sign-extended 40-bit */
    uint32_t scale;     /* unsigned 32-bit quantised multiplier */
    uint8_t shift;      /* 6-bit right shift */
} EthosUScaleBias;

/*
 * Reorder parameters for one weight tensor, as recovered from the command
 * stream (block depth, traversal flags) plus fixed accelerator geometry
 * (micro-block depths, sub-kernel decomposition). Mirrors the arguments to
 * Vela's mlw_codec.reorder_encode().
 */
typedef struct EthosUWeightParams {
    int ifm_ublock_depth;   /* accelerator ifm micro-block depth (U65: 8) */
    int ofm_ublock_depth;   /* accelerator ofm micro-block depth (U65: 8) */
    int ofm_depth;          /* output channels */
    int kernel_h;
    int kernel_w;
    int ifm_depth;          /* input channels (1 for depthwise) */
    int ofm_block_depth;    /* scheduler ofm block depth (from cmd stream) */
    bool is_depthwise;
    bool is_partkernel;     /* PART_KERNEL_FIRST traversal */
    int ifm_bitdepth;       /* 8 or 16 */
    int decomp_h;           /* sub-kernel decomp (SubKernelMax/dilation) */
    int decomp_w;
} EthosUWeightParams;

/* Unpack one 10-byte scale/bias record: [bias:40][scale:32][shift:6] LE. */
void ethos_u_unpack_scale_bias(const uint8_t in[10], EthosUScaleBias *out);

/*
 * Reconstruct OHWI weights from @flat (the NPU traversal-ordered stream emitted
 * by mlw decode). Writes @ohwi as a contiguous [ofm][kh][kw][ifm] int16 volume
 * (caller-allocated, ofm_depth*kh*kw*ifm_depth entries). Returns the number of
 * flat entries consumed (the full padded traversal length), or -1 if @flat is
 * shorter than the traversal requires.
 */
int ethos_u_weights_reorder_inverse(int16_t *ohwi,
                                     const int16_t *flat, int flat_len,
                                     const EthosUWeightParams *p);

/*
 * Convenience: mlw-decode @enc into a freshly g_malloc()'d int16 array of
 * reordered weights. Returns the weight count (0 on failure); on success the
 * caller must g_free(*out).
 */
int ethos_u_weights_decode(const uint8_t *enc, int enc_len, int16_t **out);

#endif /* HW_NPU_ETHOS_U_WEIGHTS_H */
