/*
 * NXP i.MX 93 eQOS — Synopsys DesignWare MAC (dwmac4) Ethernet
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the dwmac4/eQOS controller (compatible "nxp,imx93-dwmac-eqos",
 * "snps,dwmac-5.10a") driven by the Linux stmmac driver: GMAC core regs,
 * MDIO with an internal PHY, and a single DMA channel with dwmac4 TX/RX
 * descriptor rings. Enough for the driver to bring the link up and move
 * packets (DHCP, ping). One channel/queue is modeled.
 */

#ifndef IMX93_DWMAC_H
#define IMX93_DWMAC_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_IMX93_DWMAC "imx93.dwmac"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93DwmacState, IMX93_DWMAC)

#define IMX93_DWMAC_REG_SIZE    0x10000
#define IMX93_DWMAC_PHY_ADDR    1       /* ethphy1 on the 11x11 EVK */

struct IMX93DwmacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq;
    NICState    *nic;
    NICConf      conf;

    /* MAC core */
    uint32_t mac_config;
    uint32_t mac_int_en;
    uint32_t mdio_addr;
    uint32_t mdio_data;
    uint32_t addr_hi, addr_lo;

    /* DMA (one channel) */
    uint32_t dma_bus_mode;
    uint32_t dma_sysbus_mode;
    uint32_t ch_control;
    uint32_t tx_control;
    uint32_t rx_control;
    uint32_t tx_base_hi, tx_base;
    uint32_t rx_base_hi, rx_base;
    uint32_t tx_tail;
    uint32_t rx_tail;
    uint32_t tx_ring_len;
    uint32_t rx_ring_len;
    uint32_t ch_intr_ena;
    uint32_t ch_status;
    uint32_t cur_tx_desc;
    uint32_t cur_rx_desc;

    uint16_t phy[32];           /* internal PHY register file */

    uint8_t  frame[4096];       /* TX frame assembly buffer */
    uint32_t frame_len;
};

#endif /* IMX93_DWMAC_H */
