/*
 * Arm Ethos-U55/U65 microNPU - fixed-point requantization
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bit-exact reimplementation of the gemmlowp / TensorFlow Lite Micro reference
 * requantization the Ethos-U datapath matches: an int32 accumulator is scaled
 * by a per-channel Q31 multiplier and right shift, rounded, offset by the OFM
 * zero point and clamped. Kept standalone so it can gate the compute kernels in
 * isolation (tests/unit/test-ethos-u-requant.c).
 */

#ifndef HW_NPU_ETHOS_U_REQUANT_H
#define HW_NPU_ETHOS_U_REQUANT_H

/*
 * gemmlowp SaturatingRoundingDoublingHighMul: the high 32 bits of (a*b)*2,
 * i.e. nint(a*b / 2^31), saturating only the INT32_MIN*INT32_MIN case.
 */
int32_t ethos_u_srdhm(int32_t a, int32_t b);

/*
 * gemmlowp RoundingDivideByPOT: round(x / 2^exp), ties away from zero.
 * @exp must be in [0, 31].
 */
int32_t ethos_u_rdpot(int32_t x, int exp);

/*
 * TFLM MultiplyByQuantizedMultiplier: scale @x by the Q31 @multiplier with a
 * gemmlowp-style @shift (positive shifts left before the multiply, negative
 * shifts right after).
 */
int32_t ethos_u_mul_by_quant_mult(int32_t x, int32_t multiplier, int shift);

/*
 * Convenience wrapper taking Vela's encoded (multiplier, shift): Vela stores
 * shift = 31 - gemmlowp_exponent, so gemmlowp_shift = 31 - @vela_shift.
 */
int32_t ethos_u_requant_vela(int32_t x, uint32_t multiplier, int vela_shift);

#endif /* HW_NPU_ETHOS_U_REQUANT_H */
