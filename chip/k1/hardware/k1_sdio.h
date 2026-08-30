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
#define K1_APMU_SDH1_CLK_RST                 0xd4282858ul
#define K1_APMU_SDH2_CLK_RST                 0xd42828e0ul

#define K1_APMU_SDH_AXI_RESET_DEASSERT       (1ul << 0)
#define K1_APMU_SDH_AXI_CLOCK_ENABLE         (1ul << 3)
#define K1_APMU_SDH_RESET_DEASSERT           (1ul << 1)
#define K1_APMU_SDH_CLOCK_ENABLE             (1ul << 4)
#define K1_APMU_SDH_CLOCK_SOURCE_SHIFT        5
#define K1_APMU_SDH_CLOCK_SOURCE_MASK         (7ul << K1_APMU_SDH_CLOCK_SOURCE_SHIFT)
#define K1_APMU_SDH_CLOCK_SOURCE_PLL2_D8      (2ul << K1_APMU_SDH_CLOCK_SOURCE_SHIFT)
#define K1_APMU_SDH_CLOCK_DIVIDER_SHIFT       8
#define K1_APMU_SDH_CLOCK_DIVIDER_MASK        (7ul << K1_APMU_SDH_CLOCK_DIVIDER_SHIFT)
#define K1_APMU_SDH_CLOCK_FREQUENCY_CHANGE    (1ul << 11)

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
#define K1_SDHC_HOST_CONTROL2_OFFSET          0x03eul
#define K1_SDHC_ADMA_ERROR_OFFSET              0x054ul
#define K1_SDHC_ADMA_ADDRESS_OFFSET            0x058ul

/* SDHCI places the SDMA boundary selector above the 12-bit transfer block
 * length.  Linux initializes every K1 data request with the standard 512 KiB
 * boundary, including ADMA2 requests.
 */

#define K1_SDHC_BLOCK_SIZE_LENGTH_MASK       0x0fffu
#define K1_SDHC_BLOCK_SIZE_BOUNDARY_512K     0x7000u

/* SDHCI transfer and command fields. */

#define K1_SDHC_TRNS_DMA                       (1u << 0)
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
#define K1_SDHC_HOST_CONTROL_HIGH_SPEED       (1u << 2)
#define K1_SDHC_HOST_CONTROL_DMA_MASK        (3u << 3)
#define K1_SDHC_HOST_CONTROL_ADMA32           (2u << 3)
#define K1_SDHC_HOST_CONTROL_8BIT            (1u << 5)
#define K1_SDHC_HOST_CONTROL2_UHS_MASK         0x0007u
#define K1_SDHC_HOST_CONTROL2_UHS_SDR104       0x0003u
#define K1_SDHC_HOST_CONTROL2_180V           (1u << 3)
#define K1_SDHC_HOST_CONTROL2_EXEC_TUNING     (1u << 6)
#define K1_SDHC_HOST_CONTROL2_TUNED_CLOCK     (1u << 7)

/* ADMA2 32-bit descriptor attributes.  K1 must not use the SDHCI 64-bit
 * descriptor format; the vendor Linux host driver marks it broken.
 */

#define K1_SDHC_ADMA2_VALID                   (1u << 0)
#define K1_SDHC_ADMA2_END                     (1u << 1)
#define K1_SDHC_ADMA2_TRANSFER                (2u << 4)
#define K1_SDHC_ADMA2_TRAN_VALID              \
  (K1_SDHC_ADMA2_TRANSFER | K1_SDHC_ADMA2_VALID)

#define K1_SDHC_POWER_ON                     (1u << 0)
#define K1_SDHC_POWER_180                    0x0au
#define K1_SDHC_POWER_330                    0x0eu

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
#define K1_SDHC_PRESENT_DATA0_LEVEL           (1ul << 20)

#define K1_SDHC_INT_RESPONSE                 (1ul << 0)
#define K1_SDHC_INT_TRANSFER_COMPLETE        (1ul << 1)
#define K1_SDHC_INT_SPACE_AVAILABLE          (1ul << 4)
#define K1_SDHC_INT_DATA_AVAILABLE           (1ul << 5)
#define K1_SDHC_INT_ERROR                    (1ul << 15)
#define K1_SDHC_INT_ERROR_MASK               0xffff0000ul
#define K1_SDHC_INT_ALL                      0x117f01fful

/* Linux leaves these event sources enabled on MUSE Pi Pro SDH1 while
 * servicing the controller through PLIC source 100.  The wireless polling
 * profile uses the value only in a bounded diagnostic and masks that PLIC
 * source before programming it.
 */

#define K1_SDHC1_PLIC_SOURCE                  100u
#define K1_SDHC_INT_LINUX_SDH1                0x03ff010bul

/* K1-specific SDHCI registers, matching
 * drivers/mmc/host/sdhci-of-k1.c in Linux mainline.
 */

#define K1_SDHC_OP_EXT_OFFSET                 0x108ul
#define K1_SDHC_LEGACY_CONTROL_OFFSET         0x10cul
#define K1_SDHC_MMC_CONTROL_OFFSET            0x114ul
#define K1_SDHC_RX_CONTROL_OFFSET              0x118ul
#define K1_SDHC_TX_CONTROL_OFFSET             0x11cul
#define K1_SDHC_DLINE_CONTROL_OFFSET           0x130ul
#define K1_SDHC_DLINE_CONFIG_OFFSET            0x134ul
#define K1_SDHC_PHY_CONTROL_OFFSET            0x160ul
#define K1_SDHC_PHY_PADCFG_OFFSET             0x178ul

#define K1_SDHC_LEGACY_PAD_CLOCK_ON           (1ul << 6)
#define K1_SDHC_OP_EXT_OVERRIDE_CLOCK_ENABLE  (1ul << 11)
#define K1_SDHC_OP_EXT_FORCE_CLOCK_ON         (1ul << 12)
#define K1_SDHC_MMC_CARD_MODE                 (1ul << 12)
#define K1_SDHC_RX_CLOCK_SELECT1_SHIFT         2
#define K1_SDHC_RX_CLOCK_SELECT1_MASK          (3ul << K1_SDHC_RX_CLOCK_SELECT1_SHIFT)
#define K1_SDHC_RX_CLOCK_SELECT1_DLINE         (1ul << K1_SDHC_RX_CLOCK_SELECT1_SHIFT)
#define K1_SDHC_TX_INTERNAL_CLOCK_SELECT      (1ul << 30)
#define K1_SDHC_TX_MUX_SELECT                  (1ul << 31)
#define K1_SDHC_DLINE_POWER_UP                 (1ul << 0)
#define K1_SDHC_DLINE_RX_CODE_SHIFT            16
#define K1_SDHC_DLINE_TX_CODE_SHIFT            24
#define K1_SDHC_DLINE_CODE_MASK                0xfful
#define K1_SDHC_DLINE_RX_REGISTER_MASK         0xfful
#define K1_SDHC_DLINE_RX_GAIN                  (1ul << 8)
#define K1_SDHC_DLINE_TX_REGISTER_SHIFT        16
#define K1_SDHC_DLINE_TX_REGISTER_MASK         \
  (0xfful << K1_SDHC_DLINE_TX_REGISTER_SHIFT)
#define K1_SDHC_PHY_FUNCTION_ENABLE           (1ul << 0)
#define K1_SDHC_PHY_PLL_LOCK                  (1ul << 1)
#define K1_SDHC_PHY_DRIVE_SELECT_MASK         0x7ul
#define K1_SDHC_PHY_RX_BIAS_ENABLE            (1ul << 5)
#define K1_SDHC_PHY_DRIVE_SELECT_4            4ul

#endif /* __CHIP_K1_HARDWARE_K1_SDIO_H */
