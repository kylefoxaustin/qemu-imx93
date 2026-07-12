/*
 * Arm Ethos-U55/U65 microNPU - int8 compute kernels
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure int8 reference kernels (conv, depthwise, pooling, elementwise) operating
 * on plain NHWC buffers, matching the TFLite-Micro reference math the Ethos-U
 * implements. The device layer (ethos_u_compute.c) marshals guest memory into
 * these buffers and back; keeping the math buffer-only makes it testable
 * against a committed tflite_runtime golden (test-ethos-u-kernels).
 */

#ifndef HW_NPU_ETHOS_U_KERNELS_H
#define HW_NPU_ETHOS_U_KERNELS_H

#include "ethos_u_weights.h"    /* EthosUScaleBias */

/* Convolution / depthwise geometry and quantization. */
typedef struct EthosUConvParams {
    int ifm_h, ifm_w, ifm_c;
    int ofm_h, ofm_w, ofm_c;
    int kh, kw;
    int stride_y, stride_x;
    int dilation_y, dilation_x;
    int pad_top, pad_left;
    int ifm_zp, ofm_zp;
    int act_min, act_max;       /* clamp, in the int8 output domain */
} EthosUConvParams;

/*
 * int8 2-D convolution. @ifm is NHWC [ifm_h*ifm_w*ifm_c], @weights is OHWI
 * [ofm_c*kh*kw*ifm_c], @sb is per-OFM-channel scale/bias; writes @ofm NHWC
 * [ofm_h*ofm_w*ofm_c]. Filter zero point is 0 (symmetric per-channel weights).
 * Weights are int16: uint8 (legacy) models recentre by the weight zero-point,
 * yielding 9-bit-signed values (e.g. [-151,104]) that do not fit int8.
 */
void ethos_u_conv2d_int8(int8_t *ofm, const int8_t *ifm,
                         const int16_t *weights, const EthosUScaleBias *sb,
                         const EthosUConvParams *p);

/*
 * int8 depthwise convolution, depth multiplier 1 (ifm_c == ofm_c). @weights is
 * HWC [kh*kw*ofm_c] (TFLite depthwise filter layout [1,kh,kw,C]), int16 (see
 * ethos_u_conv2d_int8 on the 9-bit-signed weight range for uint8 models).
 */
void ethos_u_depthwise_int8(int8_t *ofm, const int8_t *ifm,
                            const int16_t *weights, const EthosUScaleBias *sb,
                            const EthosUConvParams *p);

/*
 * int32-output convolution / depthwise: the raw post-bias accumulator clamped
 * to [act_min, act_max], with no OFM requant or output zero point. Used for the
 * sum stage of a Vela-lowered reduction (e.g. global average pool = depthwise
 * sum to int32 OFM, then an elementwise 1/N multiply).
 */
void ethos_u_conv2d_int32(int32_t *ofm, const int8_t *ifm,
                          const int16_t *weights, const EthosUScaleBias *sb,
                          const EthosUConvParams *p);

void ethos_u_depthwise_int32(int32_t *ofm, const int8_t *ifm,
                             const int16_t *weights, const EthosUScaleBias *sb,
                             const EthosUConvParams *p);

typedef enum {
    ETHOS_U_POOL_MAX,
    ETHOS_U_POOL_AVG,
} EthosUPoolType;

typedef struct EthosUPoolParams {
    int ifm_h, ifm_w, c;        /* c = IFM channel count (read stride) */
    int ofm_h, ofm_w, ofm_c;    /* ofm_c = OFM channel count (write stride/count) */
    int kh, kw;
    int stride_y, stride_x;
    int pad_top, pad_left;
    int act_min, act_max;
} EthosUPoolParams;

/* int8 max / average pooling (quantization preserved; padding excluded). */
void ethos_u_pool_int8(int8_t *ofm, const int8_t *ifm, EthosUPoolType type,
                       const EthosUPoolParams *p);

/* Elementwise add: per-input and output (multiplier, gemmlowp shift). */
typedef struct EthosUAddParams {
    int n;
    int in1_zp, in2_zp, out_zp;
    int32_t in1_mult, in2_mult, out_mult;
    int in1_shift, in2_shift, out_shift;
    int left_shift;
    int act_min, act_max;
} EthosUAddParams;

void ethos_u_ew_add_int8(int8_t *out, const int8_t *a, const int8_t *b,
                         const EthosUAddParams *p);

/* Elementwise multiply. */
typedef struct EthosUMulParams {
    int n;
    int in1_zp, in2_zp, out_zp;
    int32_t out_mult;
    int out_shift;
    int act_min, act_max;
} EthosUMulParams;

void ethos_u_ew_mul_int8(int8_t *out, const int8_t *a, const int8_t *b,
                         const EthosUMulParams *p);

#endif /* HW_NPU_ETHOS_U_KERNELS_H */
