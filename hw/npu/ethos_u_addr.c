/*
 * Arm Ethos-U55/U65 microNPU - feature-map addressing
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ethos_u_addr.h"

#define ETHOS_U_BRICK 16        /* NHCWB16 channel-block depth */

uint64_t ethos_u_fm_offset(EthosULayout layout, int y, int x, int c,
                           int stride_y, int stride_x, int stride_c, int elem)
{
    if (layout == ETHOS_U_LAYOUT_NHCWB16) {
        return (uint64_t)(c / ETHOS_U_BRICK) * stride_c +
               (uint64_t)y * stride_y +
               (uint64_t)x * stride_x +
               (uint64_t)(c % ETHOS_U_BRICK) * elem;
    }
    /* NHWC */
    return (uint64_t)y * stride_y +
           (uint64_t)x * stride_x +
           (uint64_t)c * stride_c;
}
