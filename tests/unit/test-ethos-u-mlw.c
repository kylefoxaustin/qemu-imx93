/*
 * Unit tests for the Arm Ethos-U weight / scale-bias decoders.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Validates the vendored mlw entropy decoder (hw/npu/mlw/mlw_decode.c), the
 * reorder-inverse and the 10-byte scale/bias unpack (hw/npu/ethos_u_weights.c)
 * against reference vectors produced by Arm Vela's ethosu.mlw_codec and
 * encode_bias() (tests/data/ethos-u/gen.py -> test-ethos-u-mlw-vectors.h).
 */

#include "qemu/osdep.h"
#include "../../hw/npu/ethos_u_weights.h"
#include "../../hw/npu/mlw/mlw_decode.h"
#include "test-ethos-u-mlw-vectors.h"

/* The vendored decoder turns the encoded bytes back into the flat stream. */
static void test_mlw_decode(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(mlw_weight_vecs); i++) {
        const MlwWeightVec *v = &mlw_weight_vecs[i];
        int16_t *got = NULL;
        int n;

        n = mlw_decode((uint8_t *)v->enc, v->enc_len, &got, 0);
        g_assert_cmpint(n, ==, v->flat_len);
        g_assert_nonnull(got);
        for (int k = 0; k < n; k++) {
            g_assert_cmpint(got[k], ==, v->flat[k]);
        }
        free(got);
    }
}

/* reorder-inverse(flat) must recover the original OHWI weight volume. */
static void test_reorder_inverse(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(mlw_weight_vecs); i++) {
        const MlwWeightVec *v = &mlw_weight_vecs[i];
        EthosUWeightParams p = {
            .ifm_ublock_depth = v->ifm_ublock,
            .ofm_ublock_depth = v->ofm_ublock,
            .ofm_depth = v->ofm_depth,
            .kernel_h = v->kh,
            .kernel_w = v->kw,
            .ifm_depth = v->ifm_depth,
            .ofm_block_depth = v->ofm_block_depth,
            .is_depthwise = v->is_depthwise,
            .is_partkernel = v->is_partkernel,
            .ifm_bitdepth = v->ifm_bitdepth,
            .decomp_h = v->decomp_h,
            .decomp_w = v->decomp_w,
        };
        int16_t *ohwi = g_new0(int16_t, v->ohwi_len);
        int consumed;

        consumed = ethos_u_weights_reorder_inverse(ohwi, v->flat, v->flat_len,
                                                   &p);
        g_assert_cmpint(consumed, ==, v->flat_len);
        for (int k = 0; k < v->ohwi_len; k++) {
            g_assert_cmpint(ohwi[k], ==, v->ohwi[k]);
        }
        g_free(ohwi);
    }
}

/* End-to-end convenience wrapper: encoded bytes -> reordered flat -> OHWI. */
static void test_decode_then_inverse(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(mlw_weight_vecs); i++) {
        const MlwWeightVec *v = &mlw_weight_vecs[i];
        EthosUWeightParams p = {
            .ifm_ublock_depth = v->ifm_ublock,
            .ofm_ublock_depth = v->ofm_ublock,
            .ofm_depth = v->ofm_depth,
            .kernel_h = v->kh,
            .kernel_w = v->kw,
            .ifm_depth = v->ifm_depth,
            .ofm_block_depth = v->ofm_block_depth,
            .is_depthwise = v->is_depthwise,
            .is_partkernel = v->is_partkernel,
            .ifm_bitdepth = v->ifm_bitdepth,
            .decomp_h = v->decomp_h,
            .decomp_w = v->decomp_w,
        };
        int16_t *flat = NULL;
        int n = ethos_u_weights_decode(v->enc, v->enc_len, &flat);
        int16_t *ohwi = g_new0(int16_t, v->ohwi_len);

        g_assert_cmpint(n, ==, v->flat_len);
        g_assert_cmpint(ethos_u_weights_reorder_inverse(ohwi, flat, n, &p),
                        ==, v->flat_len);
        for (int k = 0; k < v->ohwi_len; k++) {
            g_assert_cmpint(ohwi[k], ==, v->ohwi[k]);
        }
        g_free(ohwi);
        g_free(flat);
    }
}

/* A short stream must be rejected, not over-read. */
static void test_reorder_truncated(void)
{
    const MlwWeightVec *v = &mlw_weight_vecs[0];
    EthosUWeightParams p = {
        .ifm_ublock_depth = v->ifm_ublock,
        .ofm_ublock_depth = v->ofm_ublock,
        .ofm_depth = v->ofm_depth,
        .kernel_h = v->kh,
        .kernel_w = v->kw,
        .ifm_depth = v->ifm_depth,
        .ofm_block_depth = v->ofm_block_depth,
        .is_depthwise = v->is_depthwise,
        .is_partkernel = v->is_partkernel,
        .ifm_bitdepth = v->ifm_bitdepth,
        .decomp_h = v->decomp_h,
        .decomp_w = v->decomp_w,
    };
    int16_t *ohwi = g_new0(int16_t, v->ohwi_len);

    g_assert_cmpint(ethos_u_weights_reorder_inverse(ohwi, v->flat,
                                                    v->flat_len - 1, &p),
                    ==, -1);
    g_free(ohwi);
}

/* 10-byte [bias:40][scale:32][shift:6] unpack. */
static void test_scale_bias(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(scale_bias_vecs); i++) {
        const ScaleBiasVec *v = &scale_bias_vecs[i];
        EthosUScaleBias sb;

        ethos_u_unpack_scale_bias(v->packed, &sb);
        g_assert_cmpint(sb.bias, ==, v->bias);
        g_assert_cmpuint(sb.scale, ==, v->scale);
        g_assert_cmpuint(sb.shift, ==, v->shift);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ethos-u/mlw/decode", test_mlw_decode);
    g_test_add_func("/ethos-u/mlw/reorder-inverse", test_reorder_inverse);
    g_test_add_func("/ethos-u/mlw/decode-then-inverse",
                    test_decode_then_inverse);
    g_test_add_func("/ethos-u/mlw/reorder-truncated", test_reorder_truncated);
    g_test_add_func("/ethos-u/mlw/scale-bias", test_scale_bias);
    return g_test_run();
}
