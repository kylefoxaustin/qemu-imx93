/*
 * Arm Ethos-U55/U65 microNPU - fixed-point requantization
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * gemmlowp / TFLite-Micro reference fixed-point arithmetic, reproduced exactly
 * so the modelled NPU's output matches the int8 reference kernels Vela targets.
 * See ethos_u_requant.h.
 */

#include "qemu/osdep.h"
#include "ethos_u_requant.h"

int32_t ethos_u_srdhm(int32_t a, int32_t b)
{
    bool overflow = (a == b) && (a == INT32_MIN);
    int64_t ab = (int64_t)a * (int64_t)b;
    /* Round to nearest, ties away from zero; high word of 2*ab. */
    int32_t nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
    int32_t high = (int32_t)((ab + nudge) / (1LL << 31));

    return overflow ? INT32_MAX : high;
}

int32_t ethos_u_rdpot(int32_t x, int exp)
{
    int32_t mask, remainder, threshold;

    if (exp <= 0) {
        return x;
    }
    g_assert(exp <= 31);
    mask = ((int32_t)1 << exp) - 1;
    remainder = x & mask;
    threshold = (mask >> 1) + (x < 0 ? 1 : 0);
    return (x >> exp) + (remainder > threshold ? 1 : 0);
}

int32_t ethos_u_mul_by_quant_mult(int32_t x, int32_t multiplier, int shift)
{
    int left_shift = shift > 0 ? shift : 0;
    int right_shift = shift > 0 ? 0 : -shift;

    return ethos_u_rdpot(ethos_u_srdhm(x * (1 << left_shift), multiplier),
                         right_shift);
}

int32_t ethos_u_requant_vela(int32_t x, uint32_t multiplier, int vela_shift)
{
    return ethos_u_mul_by_quant_mult(x, (int32_t)multiplier, 31 - vela_shift);
}
