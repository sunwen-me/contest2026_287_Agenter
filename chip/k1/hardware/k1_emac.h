/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_emac.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_EMAC_H
#define __CHIP_K1_HARDWARE_K1_EMAC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "k1_memorymap.h"
#include "k1_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* EMAC0 register offsets. */

#define K1_EMAC_DMA_CONTROL                (K1_EMAC0_BASE + 0x004ul)
#define K1_EMAC_DMA_INTERRUPT_ENABLE       (K1_EMAC0_BASE + 0x00cul)

#define K1_EMAC_MAC_GLOBAL_CONTROL         (K1_EMAC0_BASE + 0x100ul)
#define K1_EMAC_MAC_TRANSMIT_CONTROL       (K1_EMAC0_BASE + 0x104ul)
#define K1_EMAC_MAC_RECEIVE_CONTROL        (K1_EMAC0_BASE + 0x108ul)
#define K1_EMAC_MAC_ADDRESS_CONTROL        (K1_EMAC0_BASE + 0x118ul)
#define K1_EMAC_MAC_ADDRESS1_HIGH          (K1_EMAC0_BASE + 0x120ul)
#define K1_EMAC_MAC_ADDRESS1_MED           (K1_EMAC0_BASE + 0x124ul)
#define K1_EMAC_MAC_ADDRESS1_LOW           (K1_EMAC0_BASE + 0x128ul)
#define K1_EMAC_MAC_MDIO_CONTROL           (K1_EMAC0_BASE + 0x1a0ul)
#define K1_EMAC_MAC_MDIO_DATA              (K1_EMAC0_BASE + 0x1a4ul)
#define K1_EMAC_MAC_INTERRUPT_ENABLE       (K1_EMAC0_BASE + 0x1e4ul)

/* APMU EMAC0 clock/reset, RGMII control, and delay-line registers. */

#define K1_APMU_EMAC0_CLK_RST              (K1_APMU_BASE + 0x3e4ul)
#define K1_APMU_EMAC0_DLINE                (K1_APMU_BASE + 0x3e8ul)

#define K1_EMAC_CLK_BUS_ENABLE             (1ul << 0)
#define K1_EMAC_RESET_DEASSERT             (1ul << 1)
#define K1_EMAC_PTP_CLK_ENABLE             (1ul << 15)

#define K1_EMAC_RGMII                      (1ul << 2)
#define K1_EMAC_RGMII_TX_CLK_FROM_SOC      (1ul << 8)
#define K1_EMAC_AXI_SINGLE_ID              (1ul << 13)

#define K1_EMAC_RX_DLINE_ENABLE            (1ul << 0)
#define K1_EMAC_RX_DLINE_CODE_SHIFT        8
#define K1_EMAC_RX_DLINE_CODE_MASK         \
  (0xfful << K1_EMAC_RX_DLINE_CODE_SHIFT)
#define K1_EMAC_TX_DLINE_ENABLE            (1ul << 16)
#define K1_EMAC_TX_DLINE_CODE_SHIFT        24
#define K1_EMAC_TX_DLINE_CODE_MASK         \
  (0xfful << K1_EMAC_TX_DLINE_CODE_SHIFT)

/* MII management interface. */

#define K1_EMAC_MDIO_PHYADDR_MASK          0x1ful
#define K1_EMAC_MDIO_REGADDR_SHIFT         5
#define K1_EMAC_MDIO_REGADDR_MASK          \
  (0x1ful << K1_EMAC_MDIO_REGADDR_SHIFT)
#define K1_EMAC_MDIO_READ                  (1ul << 10)
#define K1_EMAC_MDIO_START                 (1ul << 15)
#define K1_EMAC_MDIO_DATA_MASK             0xfffful

/* MAC controls used by the polling-only first phase. */

#define K1_EMAC_MAC_ADDRESS1_ENABLE        (1ul << 0)

/* MUSE Pi Pro EMAC0 board wiring. */

#define K1_EMAC_PHY_RESET_GPIO             110u
#define K1_EMAC_PHY_RESET_MFPR             (K1_MFPR_BASE + 0x1d0ul)
#define K1_EMAC_PHY_RESET_BANK             K1_GPIO_BANK3_BASE
#define K1_EMAC_PHY_RESET_MASK             (1ul << 14)

#define K1_EMAC_RGMII_TX_PHASE             90u
#define K1_EMAC_RGMII_RX_PHASE             73u

#define K1_EMAC_PLIC_SOURCE                131u

#endif /* __CHIP_K1_HARDWARE_K1_EMAC_H */
