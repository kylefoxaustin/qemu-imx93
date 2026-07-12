/*
 * Analog Devices ADV7535 DSI-to-HDMI bridge (I2C main map)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ADV7535_H
#define ADV7535_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_ADV7535 "adv7535"
OBJECT_DECLARE_SIMPLE_TYPE(ADV7535State, ADV7535)

#define ADV7535_NUM_REGS    256

struct ADV7535State {
    I2CSlave parent_obj;

    uint8_t regs[ADV7535_NUM_REGS];
    uint8_t ptr;
    bool    firstbyte;
    bool    main;       /* true: main map (HPD/DDC overrides); false: plain */
};

#endif /* ADV7535_H */
