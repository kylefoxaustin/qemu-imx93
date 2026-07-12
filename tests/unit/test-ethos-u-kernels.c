/*
 * Unit tests for the Arm Ethos-U int8 compute kernels.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Each kernel is checked bit-for-bit against a TensorFlow Lite int8 reference
 * golden (tests/data/ethos-u/gen_kernels.py -> test-ethos-u-kernels-vectors.h).
 */

#include "qemu/osdep.h"
#include "../../hw/npu/ethos_u_kernels.h"
#include "test-ethos-u-kernels-vectors.h"

static void test_conv(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(conv_vecs); i++) {
        const ConvVec *v = &conv_vecs[i];
        const EthosUConvParams *p = v->p;
        int n = p->ofm_h * p->ofm_w * p->ofm_c;
        g_autofree int8_t *ofm = g_new0(int8_t, n);

        if (v->depthwise) {
            ethos_u_depthwise_int8(ofm, v->ifm, v->w, v->sb, p);
        } else {
            ethos_u_conv2d_int8(ofm, v->ifm, v->w, v->sb, p);
        }
        for (int k = 0; k < n; k++) {
            if (ofm[k] != v->golden[k]) {
                g_test_message("%s mismatch at %d: got %d want %d",
                               v->name, k, ofm[k], v->golden[k]);
            }
            g_assert_cmpint(ofm[k], ==, v->golden[k]);
        }
    }
}

static void test_pool(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(pool_vecs); i++) {
        const PoolVec *v = &pool_vecs[i];
        const EthosUPoolParams *p = v->p;
        int n = p->ofm_h * p->ofm_w * p->c;
        g_autofree int8_t *ofm = g_new0(int8_t, n);

        ethos_u_pool_int8(ofm, v->ifm,
                          v->is_max ? ETHOS_U_POOL_MAX : ETHOS_U_POOL_AVG, p);
        for (int k = 0; k < n; k++) {
            g_assert_cmpint(ofm[k], ==, v->golden[k]);
        }
    }
}

static void test_add(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(add_vecs); i++) {
        const AddVec *v = &add_vecs[i];
        g_autofree int8_t *out = g_new0(int8_t, v->p->n);

        ethos_u_ew_add_int8(out, v->a, v->b, v->p);
        for (int k = 0; k < v->p->n; k++) {
            g_assert_cmpint(out[k], ==, v->golden[k]);
        }
    }
}

static void test_mul(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(mul_vecs); i++) {
        const MulVec *v = &mul_vecs[i];
        g_autofree int8_t *out = g_new0(int8_t, v->p->n);

        ethos_u_ew_mul_int8(out, v->a, v->b, v->p);
        for (int k = 0; k < v->p->n; k++) {
            g_assert_cmpint(out[k], ==, v->golden[k]);
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ethos-u/kernels/conv", test_conv);
    g_test_add_func("/ethos-u/kernels/pool", test_pool);
    g_test_add_func("/ethos-u/kernels/add", test_add);
    g_test_add_func("/ethos-u/kernels/mul", test_mul);
    return g_test_run();
}
