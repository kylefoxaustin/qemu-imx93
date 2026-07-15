/*
 * Trivial SMBus register-file I2C slave (i.MX 93 board bring-up helper)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A minimal I2C target that implements the standard "write register pointer,
 * then read/write data" SMBus access pattern over a 256-byte read-what-you-
 * write register file. It is not a faithful model of any specific chip; it
 * exists so the board's I2C client drivers probe and register their
 * resources instead of deferring forever:
 *   - the PCA9451A PMIC (0x25): reg 0x00 (DEV_ID) must read with high nibble
 *     0x9, set via the "reg0" property; the regulator driver then registers
 *     its BUCK/LDO regulators (unblocking uSDHC, etc.);
 *   - the PCAL6524 GPIO expander (0x22): the pca953x driver registers a
 *     gpiochip, which resolves the FEC PHY reset-gpio.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX93_I2C_REGDEV "imx93.i2c-regdev"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93I2CRegdevState, IMX93_I2C_REGDEV)

struct IMX93I2CRegdevState {
    I2CSlave parent_obj;

    uint8_t regs[256];
    uint8_t ptr;
    bool    have_ptr;
    uint8_t reg0;       /* reset value of register 0 (e.g. a device id) */
    bool    pca9450;    /* preset PCA9450/51 BUCK/LDO vsel registers */
    bool    pcal6524;   /* preset PCAL6524 config regs to all-input */
    bool    pca9538;    /* preset PCA9538 output+config regs to POR */
};

static int imx93_i2c_regdev_event(I2CSlave *i2c, enum i2c_event event)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;    /* next byte is the register pointer */
    }
    return 0;
}

static int imx93_i2c_regdev_send(I2CSlave *i2c, uint8_t data)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        s->regs[s->ptr++] = data;
    }
    return 0;
}

static uint8_t imx93_i2c_regdev_recv(I2CSlave *i2c)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(i2c);
    uint8_t val = s->regs[s->ptr];

    s->ptr++;
    return val;
}

static void imx93_i2c_regdev_reset(DeviceState *dev)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = s->reg0;
    if (s->pca9450) {
        /*
         * Full PCA9451A power-on register file, BIT-EXACT to the datasheet OTP
         * defaults (93_docs/PCA9451A-datasheet.pdf rev 2.1, Table 17 register
         * overview, cross-checked against the per-register bit-field tables).
         *
         * The model is a dumb read-what-you-write regfile, so seeding a register
         * only changes what the pca9450 driver READS at probe - it never makes
         * the model DO anything.  Seeding the true OTP defaults means every rail
         * the driver registers reports its real silicon voltage/enable state
         * instead of the memset-0 value.  Only the non-zero defaults are listed;
         * every unlisted register resets to 0 (already done by the memset).
         *
         * Register 0x00 (Device_ID) is left to the reg0 property, which the
         * board sets to 0x90 - the datasheet's own Device_ID reset value.
         *
         * Voltage spot-checks (pca9451a encoding: DVS bucks 0.65V @0x00 +12.5mV;
         * BUCK4/5/6 0.60V @0x00 +25mV; LDO1 1.6V @0x00 +100mV): BUCK3 DVS0=0x10
         * -> 0.85V, BUCK4=0x6C -> 3.3V (EVK SD supply), BUCK5=0x30 -> 1.8V,
         * BUCK6=0x14 -> 1.1V, LDO1=0xC2 -> ENMODE=11 (always on) + 1.8V.
         *
         * The driver reads these live (REGCACHE_MAPLE, no reg_defaults, so no
         * "update_bits skips the write" hazard).  Boot-verified: pca9451a
         * probes, all rails register (0 failures), guest regulator sysfs reads
         * the datasheet voltages, mmc0-2 come up.
         */
        static const struct { uint8_t reg, val; } pca9451a_otp[] = {
            { 0x02, 0xFF },   /* INT1_MSK      - all sources masked            */
            { 0x07, 0x6C },   /* PWR_CTRL      - debounce/step timing          */
            { 0x08, 0x21 },   /* RESET_CTRL                                    */
            { 0x09, 0x50 },   /* CONFIG1       - LOW_VSYS / VSYS_UVLO          */
            { 0x0C, 0xA8 },   /* BUCK123_DVS   - DVS preset config             */
            { 0x0D, 0x1C },   /* BUCK1OUT_LIMIT                                */
            { 0x0E, 0x28 },   /* BUCK2OUT_LIMIT                                */
            { 0x0F, 0x1C },   /* BUCK3OUT_LIMIT                                */
            { 0x10, 0x49 },   /* BUCK1CTRL     - ramp + B1_ENMODE              */
            { 0x11, 0x10 },   /* BUCK1OUT_DVS0 - 0.85V                         */
            { 0x12, 0x10 },   /* BUCK1OUT_DVS1 - 0.85V                         */
            { 0x13, 0x49 },   /* BUCK2CTRL                                     */
            /* 0x14/0x15 BUCK2OUT_DVS0/1 = 0x00 (0.65V) -> memset              */
            { 0x16, 0x49 },   /* BUCK3CTRL                                     */
            { 0x17, 0x10 },   /* BUCK3OUT_DVS0 - 0.85V                         */
            { 0x18, 0x10 },   /* BUCK3OUT_DVS1 - 0.85V                         */
            { 0x19, 0x09 },   /* BUCK4CTRL     - B4_ENMODE                     */
            { 0x1A, 0x6C },   /* BUCK4OUT      - 3.3V (EVK SD supply)          */
            { 0x1B, 0x09 },   /* BUCK5CTRL                                     */
            { 0x1C, 0x30 },   /* BUCK5OUT      - 1.8V                          */
            { 0x1D, 0x09 },   /* BUCK6CTRL                                     */
            { 0x1E, 0x14 },   /* BUCK6OUT      - 1.1V                          */
            { 0x20, 0xF8 },   /* LDO_AD_CTRL   - active-discharge enables      */
            { 0x21, 0xC2 },   /* LDO1CTRL      - ENMODE=11 (always on) + 1.8V  */
            { 0x23, 0x4A },   /* (reserved, non-zero OTP)                      */
            { 0x24, 0x40 },   /* LDO4CTRL                                      */
            { 0x25, 0x4F },   /* LDO5CTRL_L                                    */
            { 0x2A, 0x85 },   /* LOADSW_CTRL                                   */
            { 0x2D, 0x3F },   /* VRFLT1_MASK                                   */
            { 0x2E, 0x1F },   /* VRFLT2_MASK                                   */
        };
        for (int i = 0; i < ARRAY_SIZE(pca9451a_otp); i++) {
            s->regs[pca9451a_otp[i].reg] = pca9451a_otp[i].val;
        }
    }
    if (s->pcal6524) {
        /*
         * PCAL6524 powers up with all pins configured as inputs (the
         * pca953x DIRECTION registers default to 0xFF). Without this the
         * pins read back as outputs and the pca953x driver refuses to use
         * any of them as an IRQ ("tried to flag a GPIO set as output for
         * IRQ"), which breaks the PMIC interrupt that hangs off line 11.
         * Direction regs for a 24-pin part are at 0x0c/0x0d/0x0e; the pca953x
         * driver accesses them with the auto-increment bit (0x80) set, so the
         * I2C register pointer this slave sees is 0x8c/0x8d/0x8e. Preset both
         * so the read returns all-input regardless of addressing mode.
         */
        s->regs[0x0c] = s->regs[0x0d] = s->regs[0x0e] = 0xff;
        s->regs[0x8c] = s->regs[0x8d] = s->regs[0x8e] = 0xff;
    }
    if (s->pca9538) {
        /*
         * PCA9538 power-on reset defaults, from the datasheet register table
         * (TI PCA9538 rev, 93_docs/PCA9538-datasheet-TI.pdf: "at power-on reset
         * all registers return to default values"):
         *   0x00 Input Port     - X (reflects pin levels) -> leave memset 0
         *   0x01 Output Port    - 1111 1111 = 0xFF
         *   0x02 Polarity Inv   - 0000 0000 = 0x00        -> memset already 0
         *   0x03 Configuration  - 1111 1111 = 0xFF (all pins INPUTS)
         * The memset-0 default got 0x03 wrong as all-OUTPUTS, which is not a
         * physical POR and is the same shape as the PCAL6524 direction bug: the
         * pca953x driver refuses to flag an output pin for IRQ. Unlike the
         * 24-bit PCAL6524, the 8-bit PCA9538 is single-bank and addressed
         * directly (no 0x80 auto-increment), so seed the plain register offsets.
         * The driver still drives the camera reset pin to output when it claims
         * it - this only fixes what an unclaimed pin and a config read-back show.
         */
        s->regs[0x01] = 0xff;   /* Output Port:   all high (POR)   */
        s->regs[0x03] = 0xff;   /* Configuration: all inputs (POR) */
    }
    s->ptr = 0;
    s->have_ptr = false;
}

static const Property imx93_i2c_regdev_props[] = {
    DEFINE_PROP_UINT8("reg0", IMX93I2CRegdevState, reg0, 0),
    DEFINE_PROP_BOOL("pca9450", IMX93I2CRegdevState, pca9450, false),
    DEFINE_PROP_BOOL("pcal6524", IMX93I2CRegdevState, pcal6524, false),
    DEFINE_PROP_BOOL("pca9538", IMX93I2CRegdevState, pca9538, false),
};

static const VMStateDescription vmstate_imx93_i2c_regdev = {
    .name = TYPE_IMX93_I2C_REGDEV,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, IMX93I2CRegdevState),
        VMSTATE_UINT8_ARRAY(regs, IMX93I2CRegdevState, 256),
        VMSTATE_UINT8(ptr, IMX93I2CRegdevState),
        VMSTATE_BOOL(have_ptr, IMX93I2CRegdevState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_i2c_regdev_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "i.MX 93 trivial I2C register device";
    device_class_set_legacy_reset(dc, imx93_i2c_regdev_reset);
    dc->vmsd = &vmstate_imx93_i2c_regdev;
    device_class_set_props(dc, imx93_i2c_regdev_props);
    sc->event = imx93_i2c_regdev_event;
    sc->recv = imx93_i2c_regdev_recv;
    sc->send = imx93_i2c_regdev_send;
}

static const TypeInfo imx93_i2c_regdev_types[] = {
    {
        .name           = TYPE_IMX93_I2C_REGDEV,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(IMX93I2CRegdevState),
        .class_init     = imx93_i2c_regdev_class_init,
    },
};

DEFINE_TYPES(imx93_i2c_regdev_types)
