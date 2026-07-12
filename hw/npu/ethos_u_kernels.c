/*
 * Arm Ethos-U55/U65 microNPU - int8 compute kernels
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * int8 reference kernels matching the TFLite-Micro math the Ethos-U implements.
 * See ethos_u_kernels.h. Validated against tflite_runtime goldens by
 * tests/unit/test-ethos-u-kernels.c.
 */

#include "qemu/osdep.h"
#include "ethos_u_kernels.h"
#include "ethos_u_requant.h"

static inline int8_t sat_i8(int32_t v, int act_min, int act_max)
{
    v = MAX(v, act_min);
    v = MIN(v, act_max);
    return (int8_t)v;
}

void ethos_u_conv2d_int8(int8_t *ofm, const int8_t *ifm,
                         const int16_t *weights, const EthosUScaleBias *sb,
                         const EthosUConvParams *p)
{
    for (int oy = 0; oy < p->ofm_h; oy++) {
        for (int ox = 0; ox < p->ofm_w; ox++) {
            for (int oc = 0; oc < p->ofm_c; oc++) {
                int64_t acc = sb[oc].bias;

                for (int ky = 0; ky < p->kh; ky++) {
                    int iy = oy * p->stride_y - p->pad_top
                             + ky * p->dilation_y;
                    if (iy < 0 || iy >= p->ifm_h) {
                        continue;
                    }
                    for (int kx = 0; kx < p->kw; kx++) {
                        int ix = ox * p->stride_x - p->pad_left
                                 + kx * p->dilation_x;
                        if (ix < 0 || ix >= p->ifm_w) {
                            continue;
                        }
                        const int8_t *ip = ifm
                            + (iy * p->ifm_w + ix) * p->ifm_c;
                        const int16_t *wp = weights
                            + ((oc * p->kh + ky) * p->kw + kx) * p->ifm_c;
                        for (int ic = 0; ic < p->ifm_c; ic++) {
                            acc += (int32_t)(ip[ic] - p->ifm_zp) * wp[ic];
                        }
                    }
                }

                int32_t v = ethos_u_requant_vela((int32_t)acc, sb[oc].scale,
                                                 sb[oc].shift);
                v += p->ofm_zp;
                ofm[(oy * p->ofm_w + ox) * p->ofm_c + oc] =
                    sat_i8(v, p->act_min, p->act_max);
            }
        }
    }
}

void ethos_u_depthwise_int8(int8_t *ofm, const int8_t *ifm,
                            const int16_t *weights, const EthosUScaleBias *sb,
                            const EthosUConvParams *p)
{
    for (int oy = 0; oy < p->ofm_h; oy++) {
        for (int ox = 0; ox < p->ofm_w; ox++) {
            for (int c = 0; c < p->ofm_c; c++) {
                int64_t acc = sb[c].bias;

                for (int ky = 0; ky < p->kh; ky++) {
                    int iy = oy * p->stride_y - p->pad_top
                             + ky * p->dilation_y;
                    if (iy < 0 || iy >= p->ifm_h) {
                        continue;
                    }
                    for (int kx = 0; kx < p->kw; kx++) {
                        int ix = ox * p->stride_x - p->pad_left
                                 + kx * p->dilation_x;
                        if (ix < 0 || ix >= p->ifm_w) {
                            continue;
                        }
                        int8_t in = ifm[(iy * p->ifm_w + ix) * p->ifm_c + c];
                        int16_t w = weights[(ky * p->kw + kx) * p->ofm_c + c];
                        acc += (int32_t)(in - p->ifm_zp) * w;
                    }
                }

                int32_t v = ethos_u_requant_vela((int32_t)acc, sb[c].scale,
                                                 sb[c].shift);
                v += p->ofm_zp;
                ofm[(oy * p->ofm_w + ox) * p->ofm_c + c] =
                    sat_i8(v, p->act_min, p->act_max);
            }
        }
    }
}

static inline int32_t sat_i32_range(int64_t v, int act_min, int act_max)
{
    if (v < act_min) {
        v = act_min;
    } else if (v > act_max) {
        v = act_max;
    }
    return (int32_t)v;
}

/*
 * int32-output convolution / depthwise: write the raw post-bias accumulator
 * (no OFM requant, no output zero-point), clamped to the activation range.
 * Vela lowers global average pooling to a depthwise that sums the window into
 * an int32 OFM, then a separate elementwise multiply applies 1/N; the int8
 * kernels above would requantise and truncate that sum to a byte.
 */
void ethos_u_conv2d_int32(int32_t *ofm, const int8_t *ifm,
                          const int16_t *weights, const EthosUScaleBias *sb,
                          const EthosUConvParams *p)
{
    for (int oy = 0; oy < p->ofm_h; oy++) {
        for (int ox = 0; ox < p->ofm_w; ox++) {
            for (int oc = 0; oc < p->ofm_c; oc++) {
                int64_t acc = sb[oc].bias;

                for (int ky = 0; ky < p->kh; ky++) {
                    int iy = oy * p->stride_y - p->pad_top
                             + ky * p->dilation_y;
                    if (iy < 0 || iy >= p->ifm_h) {
                        continue;
                    }
                    for (int kx = 0; kx < p->kw; kx++) {
                        int ix = ox * p->stride_x - p->pad_left
                                 + kx * p->dilation_x;
                        if (ix < 0 || ix >= p->ifm_w) {
                            continue;
                        }
                        const int8_t *ip = ifm
                            + (iy * p->ifm_w + ix) * p->ifm_c;
                        const int16_t *wp = weights
                            + ((oc * p->kh + ky) * p->kw + kx) * p->ifm_c;
                        for (int ic = 0; ic < p->ifm_c; ic++) {
                            acc += (int32_t)(ip[ic] - p->ifm_zp) * wp[ic];
                        }
                    }
                }
                ofm[(oy * p->ofm_w + ox) * p->ofm_c + oc] =
                    sat_i32_range(acc, p->act_min, p->act_max);
            }
        }
    }
}

void ethos_u_depthwise_int32(int32_t *ofm, const int8_t *ifm,
                             const int16_t *weights, const EthosUScaleBias *sb,
                             const EthosUConvParams *p)
{
    for (int oy = 0; oy < p->ofm_h; oy++) {
        for (int ox = 0; ox < p->ofm_w; ox++) {
            for (int c = 0; c < p->ofm_c; c++) {
                int64_t acc = sb[c].bias;

                for (int ky = 0; ky < p->kh; ky++) {
                    int iy = oy * p->stride_y - p->pad_top
                             + ky * p->dilation_y;
                    if (iy < 0 || iy >= p->ifm_h) {
                        continue;
                    }
                    for (int kx = 0; kx < p->kw; kx++) {
                        int ix = ox * p->stride_x - p->pad_left
                                 + kx * p->dilation_x;
                        if (ix < 0 || ix >= p->ifm_w) {
                            continue;
                        }
                        int8_t in = ifm[(iy * p->ifm_w + ix) * p->ifm_c + c];
                        int16_t w = weights[(ky * p->kw + kx) * p->ofm_c + c];
                        acc += (int32_t)(in - p->ifm_zp) * w;
                    }
                }
                ofm[(oy * p->ofm_w + ox) * p->ofm_c + c] =
                    sat_i32_range(acc, p->act_min, p->act_max);
            }
        }
    }
}

void ethos_u_pool_int8(int8_t *ofm, const int8_t *ifm, EthosUPoolType type,
                       const EthosUPoolParams *p)
{
    for (int oy = 0; oy < p->ofm_h; oy++) {
        for (int ox = 0; ox < p->ofm_w; ox++) {
            for (int c = 0; c < p->ofm_c; c++) {
                int32_t acc = (type == ETHOS_U_POOL_MAX) ? INT32_MIN : 0;
                int count = 0;

                for (int ky = 0; ky < p->kh; ky++) {
                    int iy = oy * p->stride_y - p->pad_top + ky;
                    if (iy < 0 || iy >= p->ifm_h) {
                        continue;
                    }
                    for (int kx = 0; kx < p->kw; kx++) {
                        int ix = ox * p->stride_x - p->pad_left + kx;
                        if (ix < 0 || ix >= p->ifm_w) {
                            continue;
                        }
                        int8_t in = ifm[(iy * p->ifm_w + ix) * p->c + c];
                        if (type == ETHOS_U_POOL_MAX) {
                            acc = MAX(acc, in);
                        } else {
                            acc += in;
                        }
                        count++;
                    }
                }

                int32_t v;
                if (type == ETHOS_U_POOL_MAX) {
                    v = acc;
                } else {
                    /* round half away from zero, dividing by the valid count */
                    v = acc > 0 ? (acc + count / 2) / count
                                : (acc - count / 2) / count;
                }
                ofm[(oy * p->ofm_w + ox) * p->ofm_c + c] =
                    sat_i8(v, p->act_min, p->act_max);
            }
        }
    }
}

void ethos_u_ew_add_int8(int8_t *out, const int8_t *a, const int8_t *b,
                         const EthosUAddParams *p)
{
    for (int i = 0; i < p->n; i++) {
        int32_t a_val = (a[i] - p->in1_zp) * (1 << p->left_shift);
        int32_t b_val = (b[i] - p->in2_zp) * (1 << p->left_shift);
        int32_t sa = ethos_u_mul_by_quant_mult(a_val, p->in1_mult,
                                               p->in1_shift);
        int32_t sb = ethos_u_mul_by_quant_mult(b_val, p->in2_mult,
                                               p->in2_shift);
        int32_t raw = ethos_u_mul_by_quant_mult(sa + sb, p->out_mult,
                                                p->out_shift);
        raw += p->out_zp;
        out[i] = sat_i8(raw, p->act_min, p->act_max);
    }
}

void ethos_u_ew_mul_int8(int8_t *out, const int8_t *a, const int8_t *b,
                         const EthosUMulParams *p)
{
    for (int i = 0; i < p->n; i++) {
        int32_t prod = (a[i] - p->in1_zp) * (b[i] - p->in2_zp);
        int32_t v = ethos_u_mul_by_quant_mult(prod, p->out_mult, p->out_shift);
        v += p->out_zp;
        out[i] = sat_i8(v, p->act_min, p->act_max);
    }
}
