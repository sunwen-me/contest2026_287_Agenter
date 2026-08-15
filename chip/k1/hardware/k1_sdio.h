/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_sdio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_SDIO_H
#define __CHIP_K1_HARDWARE_K1_SDIO_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MUSE Pi Pro routes the RTL8852BS2 Wi-Fi function to SDH1 and eMMC to
 * SDH2.  Both controllers implement the same K1 SDHCI register layout.
 */

#define K1_SDHC1_BASE                        0xd4280800ul
#define K1_SDHC2_BASE                        0xd4281000ul

/* APMU clock and reset controls.  The SDH AXI clock/reset fields live in
 * the SDH0 control register and are shared by the three SDH controllers.
 */

#define K1_APMU_SDH_AXI_CLK_RST              0xd4282854ul
#define K1_APMU_SDH1_CLK_RST                 0xd42828dcul
#define K1_APMU_SDH2_CLK_RST                 0xd42828e0ul

#define K1_APMU_SDH_AXI_RESET_DEASSERT       (1ul << 0)
#define K1_APMU_SDH_AXI_CLOCK_ENABLE         (1ul << 3)
#define K1_APMU_SDH_RESET_DEASSERT           (1ul << 1)
#define K1_APMU_SDH_CLOCK_ENABLE             (1ul << 4)

/* SDHCI standard register offsets. */

#define K1_SDHC_BLOCK_SIZE_OFFSET            0x004ul
#define K1_SDHC_BLOCK_COUNT_OFFSET           0x006ul
#define K1_SDHC_ARGUMENT_OFFSET              0x008ul
#define K1_SDHC_TRANSFER_MODE_OFFSET         0x00cul
#define K1_SDHC_COMMAND_OFFSET               0x00eul
#define K1_SDHC_RESPONSE0_OFFSET             0x010ul
#define K1_SDHC_RESPONSE1_OFFSET             0x014ul
#define K1_SDHC_RESPONSE2_OFFSET             0x018ul
#define K1_SDHC_RESPONSE3_OFFSET             0x01cul
#define K1_SDHC_BUFFER_OFFSET                0x020ul
#define K1_SDHC_PRESENT_STATE_OFFSET         0x024ul
#define K1_SDHC_HOST_CONTROL_OFFSET          0x028ul
#define K1_SDHC_POWER_CONTROL_OFFSET         0x029ul
#define K1_SDHC_CLOCK_CONTROL_OFFSET         0x02cul
#define K1_SDHC_TIMEOUT_CONTROL_OFFSET       0x02eul
#define K1_SDHC_SOFTWARE_RESET_OFFSET        0x02ful
#define K1_SDHC_INT_STATUS_OFFSET            0x030ul
#define K1_SDHC_INT_STATUS_ENABLE_OFFSET     0x034ul
#define K1_SDHC_INT_SIGNAL_ENABLE_OFFSET     0x038ul

/* SDHCI transfer and command fields. */

#define K1_SDHC_TRNS_BLOCK_COUNT_ENABLE      (1u << 1)
#define K1_SDHC_TRNS_READ                    (1u << 4)
#define K1_SDHC_TRNS_MULTI                   (1u << 5)

#define K1_SDHC_CMD_RESPONSE_LONG            0x01u
#define K1_SDHC_CMD_RESPONSE_SHORT           0x02u
#define K1_SDHC_CMD_RESPONSE_SHORT_BUSY      0x03u
#define K1_SDHC_CMD_CRC                      (1u << 3)
#define K1_SDHC_CMD_INDEX                    (1u << 4)
#define K1_SDHC_CMD_DATA                     (1u << 5)
#define K1_SDHC_CMD_INDEX_SHIFT              8

/* SDHCI host-control, clock, reset, present-state, and IRQ fields. */

#define K1_SDHC_HOST_CONTROL_4BIT            (1u << 1)
#define K1_SDHC_HOST_CONTROL_DMA_MASK        (3u << 3)
#define K1_SDHC_HOST_CONTROL_8BIT            (1u << 5)

#define K1_SDHC_POWER_ON                     (1u << 0)
#define K1_SDHC_POWER_180                    0x0au

#define K1_SDHC_CLOCK_INTERNAL_ENABLE        (1u << 0)
#define K1_SDHC_CLOCK_INTERNAL_STABLE        (1u << 1)
#define K1_SDHC_CLOCK_CARD_ENABLE            (1u << 2)
#define K1_SDHC_CLOCK_DIVIDER_SHIFT          8
#define K1_SDHC_CLOCK_DIVIDER_HIGH_SHIFT     6
#define K1_SDHC_CLOCK_DIVIDER_HIGH_MASK      0x300u

#define K1_SDHC_TIMEOUT_MAX                  0x0eu

#define K1_SDHC_RESET_ALL                    (1u << 0)
#define K1_SDHC_RESET_COMMAND                (1u << 1)
#define K1_SDHC_RESET_DATA                   (1u << 2)

#define K1_SDHC_PRESENT_CMD_INHIBIT          (1ul << 0)
#define K1_SDHC_PRESENT_DATA_INHIBIT         (1ul << 1)
#define K1_SDHC_PRESENT_SPACE_AVAILABLE      (1ul << 10)
#define K1_SDHC_PRESENT_DATA_AVAILABLE       (1ul << 11)

#define K1_SDHC_INT_RESPONSE                 (1ul << 0)
#define K1_SDHC_INT_TRANSFER_COMPLETE        (1ul << 1)
#define K1_SDHC_INT_SPACE_AVAILABLE          (1ul << 4)
#define K1_SDHC_INT_DATA_AVAILABLE           (1ul << 5)
#define K1_SDHC_INT_ERROR                    (1ul << 15)
#define K1_SDHC_INT_ERROR_MASK               0xffff0000ul
#define K1_SDHC_INT_ALL                      0x117f01fful

/* K1-specific SDHCI registers, matching
 * drivers/mmc/host/sdhci-of-k1.c in Linux mainline.
 */

#define K1_SDHC_OP_EXT_OFFSET                 0x108ul
#define K1_SDHC_LEGACY_CONTROL_OFFSET         0x10cul
#define K1_SDHC_MMC_CONTROL_OFFSET            0x114ul
#define K1_SDHC_TX_CONTROL_OFFSET             0x11cul
#define K1_SDHC_PHY_CONTROL_OFFSET            0x160ul
#define K1_SDHC_PHY_PADCFG_OFFSET             0x178ul

#define K1_SDHC_LEGACY_PAD_CLOCK_ON           (1ul << 6)
#define K1_SDHC_MMC_CARD_MODE                 (1ul << 12)
#define K1_SDHC_TX_INTERNAL_CLOCK_SELECT      (1ul << 30)
#define K1_SDHC_PHY_FUNCTION_ENABLE           (1ul << 0)
#define K1_SDHC_PHY_PLL_LOCK                  (1ul << 1)
#define K1_SDHC_PHY_DRIVE_SELECT_MASK         0x7ul
#define K1_SDHC_PHY_RX_BIAS_ENABLE            (1ul << 5)
#define K1_SDHC_PHY_DRIVE_SELECT_4            4ul

#endif /* __CHIP_K1_HARDWARE_K1_SDIO_H */
