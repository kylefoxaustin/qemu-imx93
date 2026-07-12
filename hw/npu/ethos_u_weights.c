/*
 * Arm Ethos-U55/U65 microNPU - weight and scale/bias decoding
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The guest hands the NPU a Vela-compiled weight stream: weights are first
 * reordered into the hardware's traversal order, then mlw-entropy-encoded. To
 * run the convolution in QEMU we reverse both steps - mlw decode (vendored from
 * Arm, hw/npu/mlw/) gives the flat reordered stream, and the code here walks
 * the exact same nested traversal Vela's encoder uses (weight_compressor.py's
 * reorder()) to scatter each weight back to its OHWI coordinate. Padding
 * positions the encoder inserts are consumed but not stored. Validated
 * bit-for-bit against ethosu.mlw_codec by tests/unit/test-ethos-u-mlw.c.
 */

#include "qemu/osdep.h"
#include "ethos_u_weights.h"
#include "mlw/mlw_decode.h"

void ethos_u_unpack_scale_bias(const uint8_t in[10], EthosUScaleBias *out)
{
    /* [bias:40][scale:32][shift:6], little-endian (Vela encode_bias). */
    uint64_t bias = (uint64_t)in[0]
                  | (uint64_t)in[1] << 8
                  | (uint64_t)in[2] << 16
                  | (uint64_t)in[3] << 24
                  | (uint64_t)in[4] << 32;
    /* sign-extend the 40-bit field */
    if (bias & (1ULL << 39)) {
        bias |= ~((1ULL << 40) - 1);
    }
    out->bias = (int64_t)bias;
    out->scale = (uint32_t)in[5]
               | (uint32_t)in[6] << 8
               | (uint32_t)in[7] << 16
               | (uint32_t)in[8] << 24;
    out->shift = in[9] & 0x3f;
}

static inline int round_up(int x, int n)
{
    return ((x + n - 1) / n) * n;
}

/*
 * Scatter the innermost micro-block group (the ifm_ub_i / ofm_uz / ifm_uz
 * loops of Vela's traversal) for one kernel element. Consumes one flat entry
 * per (iz, oz) pair, storing it at the OHWI coordinate when in range and
 * skipping the encoder's padding otherwise. Returns the advanced flat index,
 * or -1 if the stream is exhausted early.
 */
static int ethos_u_scatter_element(int16_t *ohwi, const int16_t *flat,
                                   int flat_len, int idx,
                                   const EthosUWeightParams *p,
                                   int ifm_block_z, int ofm_block_z,
                                   int ifm_ub_o, int ofm_ublk, int clip_ifm,
                                   int wy, int wx, int ky, int sub_h)
{
    const int ifm_ub = p->ifm_ublock_depth;
    const int ofm_ub = p->ofm_ublock_depth;
    const bool dw = p->is_depthwise;
    const int ifm_inner = p->is_partkernel ? 1 : clip_ifm;

    for (int ifm_ub_i = 0; ifm_ub_i < ifm_inner; ifm_ub_i += ifm_ub) {
        for (int ofm_uz = 0; ofm_uz < ofm_ub; ofm_uz++) {
            for (int ifm_uz = 0; ifm_uz < (dw ? 1 : ifm_ub); ifm_uz++) {
                int iz = ifm_block_z + ifm_ub_i + ifm_ub_o + ifm_uz;
                int oz = ofm_block_z + ofm_ublk + ofm_uz;

                if (idx >= flat_len) {
                    return -1;
                }
                if (iz < p->ifm_depth && oz < p->ofm_depth && ky < sub_h) {
                    size_t o = ((size_t)oz * p->kernel_h + wy) * p->kernel_w;
                    o = (o + wx) * p->ifm_depth + iz;
                    ohwi[o] = flat[idx];
                }
                idx++;
            }
        }
    }
    return idx;
}

int ethos_u_weights_reorder_inverse(int16_t *ohwi,
                                    const int16_t *flat, int flat_len,
                                    const EthosUWeightParams *p)
{
    const int ofm_d = p->ofm_depth;
    const int kh = p->kernel_h;
    const int kw = p->kernel_w;
    const int ifm_d = p->ifm_depth;
    const int ifm_ub = p->ifm_ublock_depth;
    const int ofm_ub = p->ofm_ublock_depth;
    const bool dw = p->is_depthwise;
    const bool pk = p->is_partkernel;
    /* IFM block depth: 16 for part-kernel / 16-bit IFM, else 32. */
    const int ifm_block_depth = (pk || p->ifm_bitdepth == 16) ? 16 : 32;
    int idx = 0;

    memset(ohwi, 0, (size_t)ofm_d * kh * kw * ifm_d * sizeof(int16_t));

    for (int ofm_block_z = 0; ofm_block_z < ofm_d;
         ofm_block_z += p->ofm_block_depth) {
        int clip_ofm = MIN(p->ofm_block_depth, ofm_d - ofm_block_z);

        for (int ifm_block_z = 0; ifm_block_z < (dw ? 1 : ifm_d);
             ifm_block_z += ifm_block_depth) {
            int clip_ifm;
            if (dw) {
                clip_ifm = ifm_ub;
            } else {
                clip_ifm = pk ? MIN(ifm_block_depth, ifm_d - ifm_block_z)
                              : ifm_block_depth;
            }

            for (int sk_y = 0; sk_y < kh; sk_y += p->decomp_h) {
                int sub_h = MIN(kh - sk_y, p->decomp_h);

                for (int sk_x = 0; sk_x < kw; sk_x += p->decomp_w) {
                    int sub_w = MIN(kw - sk_x, p->decomp_w);
                    int se = sub_w * sub_h;

                    if (pk) {
                        if (p->ifm_bitdepth == 16 && se % 2) {
                            se = round_up(se, 2);
                        } else if (p->ifm_bitdepth == 8 && se % 4) {
                            se = round_up(se, 4);
                        }
                    } else if (dw) {
                        se = round_up(se, 4);
                    }

                    int ifm_outer = pk ? clip_ifm : 1;

                    for (int ifm_ub_o = 0; ifm_ub_o < ifm_outer;
                         ifm_ub_o += ifm_ub) {
                        for (int ofm_ublk = 0; ofm_ublk < clip_ofm;
                             ofm_ublk += ofm_ub) {
                            for (int el = 0; el < se; el++) {
                                int kx = el % sub_w;
                                int ky = el / sub_w;

                                idx = ethos_u_scatter_element(
                                    ohwi, flat, flat_len, idx, p,
                                    ifm_block_z, ofm_block_z, ifm_ub_o,
                                    ofm_ublk, clip_ifm, sk_y + ky, sk_x + kx,
                                    ky, sub_h);
                                if (idx < 0) {
                                    return -1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return idx;
}

int ethos_u_weights_decode(const uint8_t *enc, int enc_len, int16_t **out)
{
    int16_t *raw = NULL;
    int n;

    *out = NULL;
    /* mlw_decode malloc()s raw; copy to a g_malloc buffer for QEMU callers. */
    n = mlw_decode((uint8_t *)enc, enc_len, &raw, 0);
    if (n <= 0 || !raw) {
        free(raw);
        return 0;
    }
    *out = g_memdup2(raw, (size_t)n * sizeof(int16_t));
    free(raw);
    return n;
}
