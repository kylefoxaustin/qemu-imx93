/*
 * NXP i.MX 93 FlexSPI controller (serial NOR flash)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two command paths drive an attached SPI-NOR flash over a QEMU SSI bus:
 *
 *  - IP commands: the driver programs a sequence into the LUT (each seqid is
 *    four 32-bit words = eight 16-bit instructions: opcode[15:10], pads[9:8],
 *    operand[7:0]), sets IPCR0 (address) / IPCR1 (seqid + data size), and
 *    triggers via IPCMD. We interpret the sequence (CMD/ADDR/DUMMY/READ/WRITE/
 *    STOP), shifting bytes to the flash with chip-select asserted, fill RFDR on
 *    reads, drain TFDR on writes, and raise IPCMDDONE.
 *
 *  - AHB reads: the memory-mapped flash window issues a normal read (03h) to
 *    the flash for the accessed offset, so memory-mapped (XIP) reads return
 *    content.
 */

#include "qemu/osdep.h"
#include "hw/ssi/imx93_flexspi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define FSPI_MCR0       0x00
#define FSPI_MCR0_SWRST (1u << 0)
#define FSPI_INTEN      0x10
#define FSPI_INTR       0x14
#define FSPI_INTR_IPCMDDONE (1u << 0)
#define FSPI_LUTKEY     0x18
#define FSPI_LUTKEY_VAL 0x5af05af0
#define FSPI_LCKCR      0x1c
#define FSPI_STS0       0xe0
#define FSPI_STS0_IDLE  0x3         /* ARB_IDLE | SEQ_IDLE */
#define FSPI_AHBRXBUF0CR0 0x20      /* first of 8 per-index RX-buffer ctrl regs */
#define FSPI_DLLACR     0xc0
#define FSPI_DLLBCR     0xc4
#define FSPI_DLLCR_DLLEN (1u << 0)
#define FSPI_STS2       0xe8
#define FSPI_STS2_RESET 0x01000100u /* SEL fields at phase 0, LOCK bits clear */
#define FSPI_STS2_A_LOCK ((1u << 1) | (1u << 0))    /* AREFLOCK | ASLVLOCK */
#define FSPI_STS2_B_LOCK ((1u << 17) | (1u << 16))  /* BREFLOCK | BSLVLOCK */
#define FSPI_IPCR0      0xa0
#define FSPI_IPCR1      0xa4
#define FSPI_IPCMD      0xb0
#define FSPI_IPCMD_TRG  (1u << 0)
#define FSPI_IPRXFCR    0xb8
#define FSPI_IPTXFCR    0xbc
#define FSPI_FIFO_CLR   (1u << 0)
#define FSPI_RFDR       0x100
#define FSPI_TFDR       0x180
#define FSPI_LUT        0x200
#define FSPI_LUT_END    0x300

/* LUT instruction opcodes. */
#define LUT_STOP        0x00
#define LUT_CMD         0x01
#define LUT_ADDR        0x02
#define LUT_MODE        0x04
#define LUT_NXP_WRITE   0x08
#define LUT_NXP_READ    0x09
#define LUT_DUMMY       0x0c

#define FLASH_CMD_READ  0x03

static void flexspi_update_irq(IMX93FlexSpiState *s)
{
    uint32_t active = s->regs[FSPI_INTR >> 2] & s->regs[FSPI_INTEN >> 2];

    qemu_set_irq(s->irq, !!active);
}

static void flexspi_run_seq(IMX93FlexSpiState *s, int seqid)
{
    uint32_t addr = s->regs[FSPI_IPCR0 >> 2];
    uint32_t ipcr1 = s->regs[FSPI_IPCR1 >> 2];
    uint32_t datasz = ipcr1 & 0xffff;
    int i;

    qemu_set_irq(s->cs[0], 0);      /* assert chip-select */

    for (i = 0; i < 8; i++) {
        uint32_t word = s->regs[(FSPI_LUT + seqid * 16 + (i / 2) * 4) >> 2];
        uint16_t instr = (i & 1) ? (word >> 16) : (word & 0xffff);
        uint8_t opcode = (instr >> 10) & 0x3f;
        uint8_t operand = instr & 0xff;
        int n, b;

        switch (opcode) {
        case LUT_STOP:
            i = 8;
            break;
        case LUT_CMD:
            ssi_transfer(s->bus, operand);
            break;
        case LUT_ADDR:
            for (b = operand / 8 - 1; b >= 0; b--) {
                ssi_transfer(s->bus, (addr >> (b * 8)) & 0xff);
            }
            break;
        case LUT_DUMMY:
            for (n = 0; n < (operand + 7) / 8; n++) {
                ssi_transfer(s->bus, 0);
            }
            break;
        case LUT_MODE:
            ssi_transfer(s->bus, operand);
            break;
        case LUT_NXP_READ:
            for (n = 0; n < datasz; n++) {
                uint8_t rx = ssi_transfer(s->bus, 0);

                if (!fifo8_is_full(&s->rx)) {
                    fifo8_push(&s->rx, rx);
                }
            }
            break;
        case LUT_NXP_WRITE:
            for (n = 0; n < datasz; n++) {
                uint8_t tx = fifo8_is_empty(&s->tx) ? 0 : fifo8_pop(&s->tx);

                ssi_transfer(s->bus, tx);
            }
            break;
        default:
            break;
        }
    }

    qemu_set_irq(s->cs[0], 1);      /* deassert chip-select */

    s->regs[FSPI_INTR >> 2] |= FSPI_INTR_IPCMDDONE;
    flexspi_update_irq(s);
}

static uint64_t flexspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93FlexSpiState *s = opaque;

    if (offset >= FSPI_RFDR && offset < FSPI_RFDR + 0x80) {
        uint32_t word = 0;
        int b;

        for (b = 0; b < 4; b++) {       /* RFDR packs 4 bytes, LSB first */
            if (!fifo8_is_empty(&s->rx)) {
                word |= (uint32_t)fifo8_pop(&s->rx) << (b * 8);
            }
        }
        return word;
    }
    switch (offset) {
    case FSPI_STS0:
        return FSPI_STS0_IDLE;      /* always idle (commands complete inline) */
    default:
        if ((offset >> 2) >= IMX93_FLEXSPI_NUM_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void flexspi_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    IMX93FlexSpiState *s = opaque;

    if (offset >= FSPI_TFDR && offset < FSPI_TFDR + 0x80) {
        int b;

        for (b = 0; b < 4; b++) {       /* TFDR unpacks 4 bytes, LSB first */
            if (!fifo8_is_full(&s->tx)) {
                fifo8_push(&s->tx, (value >> (b * 8)) & 0xff);
            }
        }
        return;
    }
    if (offset >= FSPI_LUT && offset < FSPI_LUT_END) {
        if (s->lut_unlocked) {
            s->regs[offset >> 2] = value;
        }
        return;
    }

    switch (offset) {
    case FSPI_MCR0:
        if (value & FSPI_MCR0_SWRST) {
            fifo8_reset(&s->rx);
            fifo8_reset(&s->tx);
            value &= ~FSPI_MCR0_SWRST;
        }
        s->regs[FSPI_MCR0 >> 2] = value;
        break;
    case FSPI_LUTKEY:
        s->regs[FSPI_LUTKEY >> 2] = value;
        break;
    case FSPI_LCKCR:
        /* Unlock requires the key in LUTKEY then LCKCR=2 (unlock)/1 (lock). */
        if (value == 2 && s->regs[FSPI_LUTKEY >> 2] == FSPI_LUTKEY_VAL) {
            s->lut_unlocked = true;
        } else if (value == 1) {
            s->lut_unlocked = false;
        }
        break;
    case FSPI_INTR:
        s->regs[FSPI_INTR >> 2] &= ~value;      /* write-1-to-clear */
        flexspi_update_irq(s);
        break;
    case FSPI_IPRXFCR:
        if (value & FSPI_FIFO_CLR) {
            fifo8_reset(&s->rx);
        }
        break;
    case FSPI_IPTXFCR:
        if (value & FSPI_FIFO_CLR) {
            fifo8_reset(&s->tx);
        }
        break;
    case FSPI_IPCMD:
        if (value & FSPI_IPCMD_TRG) {
            int seqid = (s->regs[FSPI_IPCR1 >> 2] >> 16) & 0xff;

            flexspi_run_seq(s, seqid);
        }
        break;
    case FSPI_DLLACR:
        /*
         * Enabling the Flash A DLL earns its lock in STS2 (silicon: the delay
         * line locks shortly after DLLEN; the driver polls STS2 for it).
         * Disabling clears it. Modelling the lock as immediate lets the poll
         * succeed instead of timing out.
         */
        s->regs[FSPI_DLLACR >> 2] = value;
        if (value & FSPI_DLLCR_DLLEN) {
            s->regs[FSPI_STS2 >> 2] |= FSPI_STS2_A_LOCK;
        } else {
            s->regs[FSPI_STS2 >> 2] &= ~FSPI_STS2_A_LOCK;
        }
        break;
    case FSPI_DLLBCR:
        s->regs[FSPI_DLLBCR >> 2] = value;
        if (value & FSPI_DLLCR_DLLEN) {
            s->regs[FSPI_STS2 >> 2] |= FSPI_STS2_B_LOCK;
        } else {
            s->regs[FSPI_STS2 >> 2] &= ~FSPI_STS2_B_LOCK;
        }
        break;
    default:
        if ((offset >> 2) < IMX93_FLEXSPI_NUM_REGS) {
            s->regs[offset >> 2] = value;
        }
        break;
    }
}

static const MemoryRegionOps flexspi_ops = {
    .read = flexspi_read,
    .write = flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* AHB-mapped flash window: a memory-mapped read issues a 03h read. */
static uint64_t flexspi_ahb_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93FlexSpiState *s = opaque;
    uint64_t val = 0;
    int b;

    qemu_set_irq(s->cs[0], 0);
    ssi_transfer(s->bus, FLASH_CMD_READ);
    for (b = 2; b >= 0; b--) {
        ssi_transfer(s->bus, (offset >> (b * 8)) & 0xff);
    }
    for (b = 0; b < size; b++) {
        val |= (uint64_t)ssi_transfer(s->bus, 0) << (b * 8);
    }
    qemu_set_irq(s->cs[0], 1);
    return val;
}

static void flexspi_ahb_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    /* XIP window is read-only; programming goes through the IP path. */
}

static const MemoryRegionOps flexspi_ahb_ops = {
    .read = flexspi_ahb_read,
    .write = flexspi_ahb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void flexspi_reset(DeviceState *dev)
{
    IMX93FlexSpiState *s = IMX93_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));

    /*
     * Per-index AHB RX buffer control-0 defaults (RM: AHBRXBUFnCR0 resets to
     * 0x800n_0020 - PREFETCH set, MSTRID = the buffer's own index n, BUFSZ =
     * 0x20). The blanket memset-0 above would leave every buffer with MSTRID 0
     * and BUFSZ 0, mis-describing all eight; the index carries the MID, so a
     * seed-one-into-all is wrong per-buffer.
     */
    for (int n = 0; n < 8; n++) {
        s->regs[(FSPI_AHBRXBUF0CR0 + n * 4) >> 2] = 0x80000020u | (n << 16);
    }

    /* DLL control registers reset with OVRDEN set (RM: DLLxCR = 0x0000_0100). */
    s->regs[FSPI_DLLACR >> 2] = 0x00000100u;
    s->regs[FSPI_DLLBCR >> 2] = 0x00000100u;

    /*
     * STS2 reset (RM 0x0100_0100): the reference-delay SEL fields come up at
     * phase 0, but the four DLL-lock bits (AREFLOCK/ASLVLOCK/BREFLOCK/BSLVLOCK)
     * are CLEAR. The lock is EARNED when firmware enables the DLL (writes
     * DLLxCR[DLLEN]), never seeded - silicon reports not-locked until the DLL
     * is configured, and nxp-fspi polls STS2 for lock after DLLEN (it would
     * time out against a memset-0 STS2).
     */
    s->regs[FSPI_STS2 >> 2] = FSPI_STS2_RESET;

    s->lut_unlocked = false;
    fifo8_reset(&s->rx);
    fifo8_reset(&s->tx);
}

static void flexspi_realize(DeviceState *dev, Error **errp)
{
    IMX93FlexSpiState *s = IMX93_FLEXSPI(dev);

    s->bus = ssi_create_bus(dev, "spi");
    qdev_init_gpio_out_named(dev, s->cs, "cs", IMX93_FLEXSPI_NUM_CS);
    fifo8_create(&s->rx, 512);
    fifo8_create(&s->tx, 512);

    memory_region_init_io(&s->iomem, OBJECT(dev), &flexspi_ops, s,
                          TYPE_IMX93_FLEXSPI, IMX93_FLEXSPI_REG_SIZE);
    memory_region_init_io(&s->ahb, OBJECT(dev), &flexspi_ahb_ops, s,
                          "flexspi-ahb", IMX93_FLEXSPI_AHB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ahb);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_flexspi = {
    .name = TYPE_IMX93_FLEXSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93FlexSpiState, IMX93_FLEXSPI_NUM_REGS),
        VMSTATE_BOOL(lut_unlocked, IMX93FlexSpiState),
        VMSTATE_FIFO8(rx, IMX93FlexSpiState),
        VMSTATE_FIFO8(tx, IMX93FlexSpiState),
        VMSTATE_END_OF_LIST()
    },
};

static void flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flexspi_realize;
    dc->vmsd = &vmstate_flexspi;
    device_class_set_legacy_reset(dc, flexspi_reset);
    dc->desc = "i.MX93 FlexSPI controller";
}

static const TypeInfo flexspi_types[] = {
    {
        .name = TYPE_IMX93_FLEXSPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93FlexSpiState),
        .class_init = flexspi_class_init,
    },
};

DEFINE_TYPES(flexspi_types)
