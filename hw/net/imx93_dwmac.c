/*
 * NXP i.MX 93 eQOS — Synopsys DesignWare MAC (dwmac4)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout, descriptor format and HW-feature encoding follow the
 * Linux stmmac/dwmac4 driver. One DMA channel + one internal PHY (addr 1,
 * link up at 100/full) are modeled. See imx93_dwmac.h.
 */

#include "qemu/osdep.h"
#include "hw/net/imx93_dwmac.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* MAC core registers. */
#define GMAC_CONFIG         0x0000
#define GMAC_VERSION        0x0020
#define GMAC_INT_STATUS     0x00b0
#define GMAC_INT_EN         0x00b4
#define GMAC_HW_FEATURE0    0x011c
#define GMAC_HW_FEATURE1    0x0120
#define GMAC_HW_FEATURE2    0x0124
#define GMAC_HW_FEATURE3    0x0128
#define GMAC_MDIO_ADDR      0x0200
#define GMAC_MDIO_DATA      0x0204
#define GMAC_ADDR_HIGH      0x0300
#define GMAC_ADDR_LOW       0x0304

/* DMA registers (one channel at 0x1100). */
#define DMA_BUS_MODE        0x1000
#define DMA_SYS_BUS_MODE    0x1004
#define DMA_CH0_CONTROL     0x1100
#define DMA_CH0_TX_CONTROL  0x1104
#define DMA_CH0_RX_CONTROL  0x1108
#define DMA_CH0_TX_BASE_HI  0x1110
#define DMA_CH0_TX_BASE     0x1114
#define DMA_CH0_RX_BASE_HI  0x1118
#define DMA_CH0_RX_BASE     0x111c
#define DMA_CH0_TX_TAIL     0x1120
#define DMA_CH0_RX_TAIL     0x1128
#define DMA_CH0_TX_RING_LEN 0x112c
#define DMA_CH0_RX_RING_LEN 0x1130
#define DMA_CH0_INTR_ENA    0x1134
#define DMA_CH0_CUR_TX_DESC 0x1144
#define DMA_CH0_CUR_RX_DESC 0x114c
#define DMA_CH0_STATUS      0x1160

#define DMA_BUS_MODE_SWR    BIT(0)      /* software reset (self-clearing) */
#define DMA_CH_TX_START     BIT(0)      /* TX_CONTROL.ST */
#define DMA_CH_RX_START     BIT(0)      /* RX_CONTROL.SR */

/* Channel status / interrupt-enable bits. */
#define DMA_STATUS_TI       BIT(0)
#define DMA_STATUS_RI       BIT(6)
#define DMA_STATUS_NIS      BIT(15)

/* MDIO address register. */
#define MDIO_BUSY           BIT(0)
#define MDIO_GOC_WRITE      (1u << 2)
#define MDIO_GOC_READ       (3u << 2)
#define MDIO_GOC_MASK       (3u << 2)
#define MDIO_REG_SHIFT      16
#define MDIO_PHY_SHIFT      21

/* dwmac4 descriptor (4 x u32) bits. */
#define TDES2_IOC           BIT(31)
#define TDES2_B1L_MASK      0x3fff
#define TDES3_OWN           BIT(31)
#define TDES3_FD            BIT(29)
#define TDES3_LD            BIT(28)
#define RDES3_OWN           BIT(31)
#define RDES3_FD            BIT(29)
#define RDES3_LD            BIT(28)
#define RDES3_PL_MASK       0x7fff

static void imx93_dwmac_update_irq(IMX93DwmacState *s)
{
    qemu_set_irq(s->irq, !!(s->ch_status & s->ch_intr_ena &
                            (DMA_STATUS_TI | DMA_STATUS_RI)));
}

/* ---- internal PHY (clause 22) ---- */
static uint16_t imx93_dwmac_phy_read(IMX93DwmacState *s, unsigned reg)
{
    return s->phy[reg & 0x1f];
}

static void imx93_dwmac_phy_write(IMX93DwmacState *s, unsigned reg, uint16_t v)
{
    reg &= 0x1f;
    if (reg == 0) {                    /* BMCR: ignore reset/restart-AN bits */
        v &= ~0x8200;
    }
    if (reg == 1 || reg == 2 || reg == 3) {
        return;                        /* BMSR / ID are read-only */
    }
    s->phy[reg] = v;
}

static void imx93_dwmac_mdio(IMX93DwmacState *s, uint32_t val)
{
    unsigned phy = (val >> MDIO_PHY_SHIFT) & 0x1f;
    unsigned reg = (val >> MDIO_REG_SHIFT) & 0x1f;

    if (phy == IMX93_DWMAC_PHY_ADDR) {
        if ((val & MDIO_GOC_MASK) == MDIO_GOC_READ) {
            s->mdio_data = imx93_dwmac_phy_read(s, reg);
        } else if ((val & MDIO_GOC_MASK) == MDIO_GOC_WRITE) {
            imx93_dwmac_phy_write(s, reg, s->mdio_data & 0xffff);
        }
    } else {
        s->mdio_data = 0xffff;         /* no device at this address */
    }
    s->mdio_addr = val & ~MDIO_BUSY;   /* operation completes immediately */
}

/* ---- TX ring ---- */
static void imx93_dwmac_tx(IMX93DwmacState *s)
{
    if (!(s->tx_control & DMA_CH_TX_START)) {
        return;
    }

    while (s->cur_tx_desc != s->tx_tail) {
        uint32_t d[4];
        uint64_t buf;
        uint32_t blen;

        dma_memory_read(&address_space_memory, s->cur_tx_desc, d, sizeof(d),
                        MEMTXATTRS_UNSPECIFIED);
        if (!(d[3] & TDES3_OWN)) {
            break;
        }
        if (d[3] & TDES3_FD) {
            s->frame_len = 0;
        }
        buf = d[0] | ((uint64_t)d[1] << 32);
        blen = d[2] & TDES2_B1L_MASK;
        if (blen && s->frame_len + blen <= sizeof(s->frame)) {
            dma_memory_read(&address_space_memory, buf,
                            s->frame + s->frame_len, blen,
                            MEMTXATTRS_UNSPECIFIED);
            s->frame_len += blen;
        }
        if (d[3] & TDES3_LD) {
            qemu_send_packet(qemu_get_queue(s->nic), s->frame, s->frame_len);
            s->frame_len = 0;
        }
        d[3] &= ~TDES3_OWN;            /* hand the descriptor back */
        dma_memory_write(&address_space_memory, s->cur_tx_desc, d, sizeof(d),
                         MEMTXATTRS_UNSPECIFIED);

        s->cur_tx_desc += 16;
        if (s->cur_tx_desc > s->tx_base + s->tx_ring_len * 16) {
            s->cur_tx_desc = s->tx_base;
        }
    }

    s->ch_status |= DMA_STATUS_TI | DMA_STATUS_NIS;
    imx93_dwmac_update_irq(s);
}

/* ---- RX ---- */
static bool imx93_dwmac_can_receive(NetClientState *nc)
{
    IMX93DwmacState *s = qemu_get_nic_opaque(nc);

    return s->rx_control & DMA_CH_RX_START;
}

static ssize_t imx93_dwmac_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t size)
{
    IMX93DwmacState *s = qemu_get_nic_opaque(nc);
    uint32_t d[4];
    uint64_t dst;

    if (!(s->rx_control & DMA_CH_RX_START)) {
        return -1;
    }

    dma_memory_read(&address_space_memory, s->cur_rx_desc, d, sizeof(d),
                    MEMTXATTRS_UNSPECIFIED);
    if (!(d[3] & RDES3_OWN)) {
        return 0;                       /* no buffer available; drop */
    }
    dst = d[0] | ((uint64_t)d[1] << 32);
    dma_memory_write(&address_space_memory, dst, buf, size,
                     MEMTXATTRS_UNSPECIFIED);

    d[0] = d[1] = d[2] = 0;
    /*
     * The stmmac driver strips ETH_FCS_LEN (4) from the reported length (it
     * does not enable ACS), so advertise size + 4; the 4 notional CRC bytes
     * past the frame are never read after the strip.
     */
    /* OWN cleared: hand the descriptor back to the driver. */
    d[3] = RDES3_FD | RDES3_LD | ((size + 4) & RDES3_PL_MASK);
    dma_memory_write(&address_space_memory, s->cur_rx_desc, d, sizeof(d),
                     MEMTXATTRS_UNSPECIFIED);

    s->cur_rx_desc += 16;
    if (s->cur_rx_desc > s->rx_base + s->rx_ring_len * 16) {
        s->cur_rx_desc = s->rx_base;
    }

    s->ch_status |= DMA_STATUS_RI | DMA_STATUS_NIS;
    imx93_dwmac_update_irq(s);
    return size;
}

static uint64_t imx93_dwmac_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93DwmacState *s = opaque;

    switch (offset) {
    case GMAC_VERSION:      return 0x00003251;   /* snps 5.10a */
    case GMAC_HW_FEATURE0:  return 0x00000023;   /* MII | GMII | MDIO(SMA) */
    case GMAC_HW_FEATURE1:  return 0x00000145;   /* 4 KiB tx/rx FIFO */
    case GMAC_HW_FEATURE2:  return 0x00000000;   /* 1 queue, 1 channel each */
    case GMAC_HW_FEATURE3:  return 0x00000000;
    case GMAC_CONFIG:       return s->mac_config;
    case GMAC_INT_EN:       return s->mac_int_en;
    case GMAC_INT_STATUS:   return 0;
    case GMAC_MDIO_ADDR:    return s->mdio_addr;
    case GMAC_MDIO_DATA:    return s->mdio_data;
    case GMAC_ADDR_HIGH:    return s->addr_hi;
    case GMAC_ADDR_LOW:     return s->addr_lo;
    case DMA_BUS_MODE:      return s->dma_bus_mode;
    case DMA_SYS_BUS_MODE:  return s->dma_sysbus_mode;
    case DMA_CH0_CONTROL:   return s->ch_control;
    case DMA_CH0_TX_CONTROL: return s->tx_control;
    case DMA_CH0_RX_CONTROL: return s->rx_control;
    case DMA_CH0_TX_BASE_HI: return s->tx_base_hi;
    case DMA_CH0_TX_BASE:   return s->tx_base;
    case DMA_CH0_RX_BASE_HI: return s->rx_base_hi;
    case DMA_CH0_RX_BASE:   return s->rx_base;
    case DMA_CH0_TX_RING_LEN: return s->tx_ring_len;
    case DMA_CH0_RX_RING_LEN: return s->rx_ring_len;
    case DMA_CH0_INTR_ENA:  return s->ch_intr_ena;
    case DMA_CH0_CUR_TX_DESC: return s->cur_tx_desc;
    case DMA_CH0_CUR_RX_DESC: return s->cur_rx_desc;
    case DMA_CH0_STATUS:    return s->ch_status;
    default:                return 0;
    }
}

static void imx93_dwmac_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    IMX93DwmacState *s = opaque;

    switch (offset) {
    case GMAC_CONFIG:
        s->mac_config = value;
        break;
    case GMAC_INT_EN:
        s->mac_int_en = value;
        break;
    case GMAC_ADDR_HIGH:
        s->addr_hi = value;
        break;
    case GMAC_ADDR_LOW:
        s->addr_lo = value;
        break;
    case GMAC_MDIO_DATA:
        s->mdio_data = value;
        break;
    case GMAC_MDIO_ADDR:
        if (value & MDIO_BUSY) {
            imx93_dwmac_mdio(s, value);
        } else {
            s->mdio_addr = value;
        }
        break;
    case DMA_BUS_MODE:
        s->dma_bus_mode = value & ~DMA_BUS_MODE_SWR;   /* reset self-clears */
        break;
    case DMA_SYS_BUS_MODE:
        s->dma_sysbus_mode = value;
        break;
    case DMA_CH0_CONTROL:
        s->ch_control = value;
        break;
    case DMA_CH0_TX_CONTROL:
        s->tx_control = value;
        break;
    case DMA_CH0_RX_CONTROL:
        s->rx_control = value;
        break;
    case DMA_CH0_TX_BASE_HI:
        s->tx_base_hi = value;
        break;
    case DMA_CH0_TX_BASE:
        s->tx_base = value;
        s->cur_tx_desc = value;
        break;
    case DMA_CH0_RX_BASE_HI:
        s->rx_base_hi = value;
        break;
    case DMA_CH0_RX_BASE:
        s->rx_base = value;
        s->cur_rx_desc = value;
        break;
    case DMA_CH0_TX_TAIL:
        s->tx_tail = value;
        imx93_dwmac_tx(s);              /* tail write is the TX doorbell */
        break;
    case DMA_CH0_RX_TAIL:
        s->rx_tail = value;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    case DMA_CH0_TX_RING_LEN:
        s->tx_ring_len = value;
        break;
    case DMA_CH0_RX_RING_LEN:
        s->rx_ring_len = value;
        break;
    case DMA_CH0_INTR_ENA:
        s->ch_intr_ena = value;
        imx93_dwmac_update_irq(s);
        break;
    case DMA_CH0_STATUS:
        s->ch_status &= ~(uint32_t)value;      /* W1C */
        imx93_dwmac_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx93_dwmac_ops = {
    .read = imx93_dwmac_read,
    .write = imx93_dwmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_dwmac_phy_reset(IMX93DwmacState *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[0] = 0x1140;        /* BMCR: AN enable, full duplex */
    s->phy[1] = 0x796d;        /* BMSR: link up, AN complete, 10/100 caps */
    s->phy[2] = 0x0007;        /* PHY ID1 (generic) */
    s->phy[3] = 0xc0f0;        /* PHY ID2 (generic -> genphy) */
    s->phy[4] = 0x01e1;        /* ANAR: advertise 10/100 */
    s->phy[5] = 0x45e1;        /* ANLPAR: partner 100/full */
}

static void imx93_dwmac_reset(DeviceState *dev)
{
    IMX93DwmacState *s = IMX93_DWMAC(dev);

    s->mac_config = s->mac_int_en = 0;
    s->mdio_addr = s->mdio_data = 0;
    s->dma_bus_mode = s->dma_sysbus_mode = 0;
    s->ch_control = s->tx_control = s->rx_control = 0;
    s->tx_base = s->tx_base_hi = s->rx_base = s->rx_base_hi = 0;
    s->tx_tail = s->rx_tail = s->tx_ring_len = s->rx_ring_len = 0;
    s->ch_intr_ena = s->ch_status = 0;
    s->cur_tx_desc = s->cur_rx_desc = 0;
    s->frame_len = 0;
    imx93_dwmac_phy_reset(s);
    qemu_set_irq(s->irq, 0);
}

static NetClientInfo net_imx93_dwmac_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = imx93_dwmac_can_receive,
    .receive = imx93_dwmac_receive,
};

static void imx93_dwmac_realize(DeviceState *dev, Error **errp)
{
    IMX93DwmacState *s = IMX93_DWMAC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx93_dwmac_ops, s,
                          TYPE_IMX93_DWMAC, IMX93_DWMAC_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_imx93_dwmac_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const VMStateDescription vmstate_imx93_dwmac = {
    .name = TYPE_IMX93_DWMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mac_config, IMX93DwmacState),
        VMSTATE_UINT32(mac_int_en, IMX93DwmacState),
        VMSTATE_UINT32(addr_hi, IMX93DwmacState),
        VMSTATE_UINT32(addr_lo, IMX93DwmacState),
        VMSTATE_UINT32(mdio_addr, IMX93DwmacState),
        VMSTATE_UINT32(mdio_data, IMX93DwmacState),
        VMSTATE_UINT32(dma_bus_mode, IMX93DwmacState),
        VMSTATE_UINT32(dma_sysbus_mode, IMX93DwmacState),
        VMSTATE_UINT32(tx_control, IMX93DwmacState),
        VMSTATE_UINT32(rx_control, IMX93DwmacState),
        VMSTATE_UINT32(ch_control, IMX93DwmacState),
        VMSTATE_UINT32(tx_base_hi, IMX93DwmacState),
        VMSTATE_UINT32(tx_base, IMX93DwmacState),
        VMSTATE_UINT32(rx_base_hi, IMX93DwmacState),
        VMSTATE_UINT32(rx_base, IMX93DwmacState),
        VMSTATE_UINT32(tx_tail, IMX93DwmacState),
        VMSTATE_UINT32(rx_tail, IMX93DwmacState),
        VMSTATE_UINT32(tx_ring_len, IMX93DwmacState),
        VMSTATE_UINT32(rx_ring_len, IMX93DwmacState),
        VMSTATE_UINT32(ch_intr_ena, IMX93DwmacState),
        VMSTATE_UINT32(ch_status, IMX93DwmacState),
        VMSTATE_UINT32(cur_tx_desc, IMX93DwmacState),
        VMSTATE_UINT32(cur_rx_desc, IMX93DwmacState),
        VMSTATE_UINT16_ARRAY(phy, IMX93DwmacState, 32),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx93_dwmac_props[] = {
    DEFINE_NIC_PROPERTIES(IMX93DwmacState, conf),
};

static void imx93_dwmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 eQOS (dwmac4) Ethernet";
    dc->realize = imx93_dwmac_realize;
    device_class_set_legacy_reset(dc, imx93_dwmac_reset);
    dc->vmsd = &vmstate_imx93_dwmac;
    device_class_set_props(dc, imx93_dwmac_props);
}

static const TypeInfo imx93_dwmac_types[] = {
    {
        .name           = TYPE_IMX93_DWMAC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93DwmacState),
        .class_init     = imx93_dwmac_class_init,
    },
};

DEFINE_TYPES(imx93_dwmac_types)
