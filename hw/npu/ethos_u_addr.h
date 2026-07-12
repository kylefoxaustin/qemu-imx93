/*
 * Arm Ethos-U55/U65 microNPU - feature-map addressing
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Element addressing for the two NPU feature-map layouts. Pure arithmetic (no
 * guest-memory access), so it is unit-testable in isolation.
 */

#ifndef HW_NPU_ETHOS_U_ADDR_H
#define HW_NPU_ETHOS_U_ADDR_H

#include "ethos_u_internal.h"

/*
 * Byte offset (from the tensor base) of element (y, x, c).
 *
 *   NHWC:     off = y*stride_y + x*stride_x + c*stride_c
 *   NHCWB16:  channels are grouped into bricks of 16; off =
 *             (c/16)*stride_c + y*stride_y + x*stride_x + (c%16)*elem
 *
 * Strides are the byte strides programmed into the NPU_SET_*_STRIDE_{X,Y,C}
 * registers; elem is the element size in bytes (1 for int8).
 */
uint64_t ethos_u_fm_offset(EthosULayout layout, int y, int x, int c,
                           int stride_y, int stride_x, int stride_c, int elem);

#endif /* HW_NPU_ETHOS_U_ADDR_H */
