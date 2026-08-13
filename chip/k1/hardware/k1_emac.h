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

#define K1_EMAC_DMA_CONFIGURATION          (K1_EMAC0_BASE + 0x000ul)
#define K1_EMAC_DMA_CONTROL                (K1_EMAC0_BASE + 0x004ul)
#define K1_EMAC_DMA_STATUS                 (K1_EMAC0_BASE + 0x008ul)
#define K1_EMAC_DMA_INTERRUPT_ENABLE       (K1_EMAC0_BASE + 0x00cul)
#define K1_EMAC_DMA_TRANSMIT_AUTO_POLL     (K1_EMAC0_BASE + 0x010ul)
#define K1_EMAC_DMA_TRANSMIT_POLL_DEMAND   (K1_EMAC0_BASE + 0x014ul)
#define K1_EMAC_DMA_RECEIVE_POLL_DEMAND    (K1_EMAC0_BASE + 0x018ul)
#define K1_EMAC_DMA_TRANSMIT_BASE           (K1_EMAC0_BASE + 0x01cul)
#define K1_EMAC_DMA_RECEIVE_BASE            (K1_EMAC0_BASE + 0x020ul)

#define K1_EMAC_MAC_GLOBAL_CONTROL         (K1_EMAC0_BASE + 0x100ul)
#define K1_EMAC_MAC_TRANSMIT_CONTROL       (K1_EMAC0_BASE + 0x104ul)
#define K1_EMAC_MAC_RECEIVE_CONTROL        (K1_EMAC0_BASE + 0x108ul)
#define K1_EMAC_MAC_MAXIMUM_FRAME_SIZE      (K1_EMAC0_BASE + 0x10cul)
#define K1_EMAC_MAC_ADDRESS_CONTROL        (K1_EMAC0_BASE + 0x118ul)
#define K1_EMAC_MAC_ADDRESS1_HIGH          (K1_EMAC0_BASE + 0x120ul)
#define K1_EMAC_MAC_ADDRESS1_MED           (K1_EMAC0_BASE + 0x124ul)
#define K1_EMAC_MAC_ADDRESS1_LOW           (K1_EMAC0_BASE + 0x128ul)
#define K1_EMAC_MAC_MDIO_CONTROL           (K1_EMAC0_BASE + 0x1a0ul)
#define K1_EMAC_MAC_MDIO_DATA              (K1_EMAC0_BASE + 0x1a4ul)
#define K1_EMAC_MAC_TRANSMIT_FIFO_ALMOST_FULL \
                                            (K1_EMAC0_BASE + 0x1c0ul)
#define K1_EMAC_MAC_TRANSMIT_PACKET_START  (K1_EMAC0_BASE + 0x1c4ul)
#define K1_EMAC_MAC_RECEIVE_PACKET_START   (K1_EMAC0_BASE + 0x1c8ul)
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

/* DMA configuration and control. */

#define K1_EMAC_DMA_SOFTWARE_RESET         (1ul << 0)
#define K1_EMAC_DMA_BURST_16WORD            (1ul << 5)
#define K1_EMAC_DMA_STRICT_BURST            (1ul << 17)
#define K1_EMAC_DMA_64BIT_MODE              (1ul << 18)

#define K1_EMAC_DMA_START_TRANSMIT          (1ul << 0)
#define K1_EMAC_DMA_START_RECEIVE           (1ul << 1)

/* MAC controls. */

#define K1_EMAC_MAC_SPEED_MASK              0x3ul
#define K1_EMAC_MAC_SPEED_10                0x0ul
#define K1_EMAC_MAC_SPEED_100               (1ul << 0)
#define K1_EMAC_MAC_SPEED_1000              (1ul << 1)
#define K1_EMAC_MAC_FULL_DUPLEX             (1ul << 2)
#define K1_EMAC_MAC_RESET_RX_STATS          (1ul << 3)
#define K1_EMAC_MAC_RESET_TX_STATS          (1ul << 4)

#define K1_EMAC_MAC_TRANSMIT_ENABLE         (1ul << 0)
#define K1_EMAC_MAC_TRANSMIT_AUTO_RETRY     (1ul << 3)
#define K1_EMAC_MAC_TRANSMIT_IFG_MASK       (0x7ul << 4)

#define K1_EMAC_MAC_RECEIVE_ENABLE          (1ul << 0)
#define K1_EMAC_MAC_RECEIVE_STORE_FORWARD   (1ul << 3)

#define K1_EMAC_MAC_MAX_FRAME_SIZE_MASK     0x3ffful

#define K1_EMAC_MAC_ADDRESS1_ENABLE        (1ul << 0)

/* The K1 EMAC descriptor is 16 bytes.  Each descriptor below is allocated
 * on its own 64-byte cache line because K1 uses 64-byte cache blocks.
 */

#define K1_EMAC_DESC_OWN                    (1ul << 31)
#define K1_EMAC_DESC_END_OF_RING            (1ul << 26)
#define K1_EMAC_DESC_BUFFER1_SIZE_MASK      0xffful
#define K1_EMAC_DESC_RX_LAST                 (1ul << 29)
#define K1_EMAC_DESC_RX_FIRST                (1ul << 30)
#define K1_EMAC_DESC_TX_INTERRUPT_ON_DONE   (1ul << 31)
#define K1_EMAC_DESC_TX_FIRST                (1ul << 29)
#define K1_EMAC_DESC_TX_LAST                 (1ul << 30)
#define K1_EMAC_DESC_RX_FRAME_LENGTH_MASK   0x3ffful
#define K1_EMAC_DESC_RX_FRAME_RUNT           (1ul << 15)
#define K1_EMAC_DESC_RX_FRAME_CRC_ERROR      (1ul << 20)
#define K1_EMAC_DESC_RX_FRAME_MAX_LEN_ERROR  (1ul << 21)
#define K1_EMAC_DESC_RX_FRAME_JABBER_ERROR   (1ul << 22)
#define K1_EMAC_DESC_RX_FRAME_LENGTH_ERROR   (1ul << 23)
#define K1_EMAC_DESC_RX_ERROR_STATUS         \
  (K1_EMAC_DESC_RX_FRAME_RUNT |             \
   K1_EMAC_DESC_RX_FRAME_CRC_ERROR |        \
   K1_EMAC_DESC_RX_FRAME_MAX_LEN_ERROR |    \
   K1_EMAC_DESC_RX_FRAME_JABBER_ERROR |     \
   K1_EMAC_DESC_RX_FRAME_LENGTH_ERROR)

#define K1_EMAC_TX_FIFO_ALMOST_FULL         0x1f8ul
#define K1_EMAC_TX_STORE_FORWARD_THRESHOLD  0x5eeul
#define K1_EMAC_RX_STORE_FORWARD_THRESHOLD  0x00cul

/* MUSE Pi Pro EMAC0 board wiring. */

#define K1_EMAC_PHY_RESET_GPIO             110u
#define K1_EMAC_PHY_RESET_MFPR             (K1_MFPR_BASE + 0x1d0ul)
#define K1_EMAC_PHY_RESET_BANK             K1_GPIO_BANK3_BASE
#define K1_EMAC_PHY_RESET_MASK             (1ul << 14)

#define K1_EMAC_RGMII_TX_PHASE             60u
#define K1_EMAC_RGMII_RX_PHASE             73u

#define K1_EMAC_PLIC_SOURCE                131u

#endif /* __CHIP_K1_HARDWARE_K1_EMAC_H */
