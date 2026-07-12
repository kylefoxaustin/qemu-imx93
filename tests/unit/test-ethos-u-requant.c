/*
 * Unit tests for the Arm Ethos-U fixed-point requantization.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Gates the compute kernels: the requant must be bit-exact with the gemmlowp /
 * TFLite-Micro reference. Vectors are committed by
 * tests/data/ethos-u/gen_requant.py (-> test-ethos-u-requant-vectors.h) from a
 * faithful Python reimplementation of that reference arithmetic.
 */

#include "qemu/osdep.h"
#include "../../hw/npu/ethos_u_requant.h"
#include "test-ethos-u-requant-vectors.h"

static void test_srdhm(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(srdhm_vecs); i++) {
        const SrdhmVec *v = &srdhm_vecs[i];
        g_assert_cmpint(ethos_u_srdhm(v->a, v->b), ==, v->expected);
    }
}

static void test_rdpot(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(rdpot_vecs); i++) {
        const RdpotVec *v = &rdpot_vecs[i];
        g_assert_cmpint(ethos_u_rdpot(v->x, v->exp), ==, v->expected);
    }
}

static void test_mul_by_quant_mult(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(mqm_vecs); i++) {
        const MqmVec *v = &mqm_vecs[i];
        g_assert_cmpint(ethos_u_mul_by_quant_mult(v->x, v->mult, v->shift),
                        ==, v->expected);
    }
}

static void test_requant_vela(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(vela_requant_vecs); i++) {
        const VelaRequantVec *v = &vela_requant_vecs[i];
        g_assert_cmpint(ethos_u_requant_vela(v->x, v->mult, v->vela_shift),
                        ==, v->expected);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ethos-u/requant/srdhm", test_srdhm);
    g_test_add_func("/ethos-u/requant/rdpot", test_rdpot);
    g_test_add_func("/ethos-u/requant/mul-by-quant-mult",
                    test_mul_by_quant_mult);
    g_test_add_func("/ethos-u/requant/vela", test_requant_vela);
    return g_test_run();
}
