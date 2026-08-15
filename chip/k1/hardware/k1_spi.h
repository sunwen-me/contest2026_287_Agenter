/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_spi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_SPI_H
#define __CHIP_K1_HARDWARE_K1_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MUSE Pi Pro exposes SSP3 on the 40-pin header.  This is distinct from
 * the board's boot QSPI controller and its connected flash.
 */

#define K1_SPI3_BASE                     0xd401c000ul
#define K1_APBC_SPI3_CLK_RST             0xd401507cul

/* APBC SSP3 clock-parent selector. */

#define K1_SPI3_CLK_PARENT_SHIFT         4u
#define K1_SPI3_CLK_PARENT_MASK          (7ul << K1_SPI3_CLK_PARENT_SHIFT)
#define K1_SPI3_CLK_PARENT(n)            \
  ((uint32_t)(n) << K1_SPI3_CLK_PARENT_SHIFT)

#define K1_SPI3_PARENT_6P4MHZ            0u
#define K1_SPI3_PARENT_12P8MHZ           1u
#define K1_SPI3_PARENT_25P6MHZ           2u
#define K1_SPI3_PARENT_51P2MHZ           3u
#define K1_SPI3_PARENT_3P2MHZ            4u
#define K1_SPI3_PARENT_1P6MHZ            5u
#define K1_SPI3_PARENT_800KHZ            6u

/* SSP register offsets. */

#define K1_SPI_TOP_CTRL_OFFSET           0x000ul
#define K1_SPI_FIFO_CTRL_OFFSET          0x004ul
#define K1_SPI_INT_EN_OFFSET             0x008ul
#define K1_SPI_TIMEOUT_OFFSET            0x00cul
#define K1_SPI_DATAR_OFFSET              0x010ul
#define K1_SPI_STATUS_OFFSET             0x014ul
#define K1_SPI_PSP_CTRL_OFFSET           0x018ul

/* SSP Top Control Register fields. */

#define K1_SPI_TOP_SSE                   (1ul << 0)
#define K1_SPI_TOP_FRF_MOTOROLA          0ul
#define K1_SPI_TOP_DSS_SHIFT             5u
#define K1_SPI_TOP_DSS_MASK              (0x1ful << K1_SPI_TOP_DSS_SHIFT)
#define K1_SPI_TOP_DSS(nbits)            \
  ((uint32_t)((nbits) - 1u) << K1_SPI_TOP_DSS_SHIFT)
#define K1_SPI_TOP_SPO                   (1ul << 10)
#define K1_SPI_TOP_SPH                   (1ul << 11)
#define K1_SPI_TOP_LBM                   (1ul << 12)
#define K1_SPI_TOP_HOLD_FRAME_LOW        (1ul << 14)

/* SSP FIFO Control Register fields. */

#define K1_SPI_FIFO_TFT_MASK             0x1ful
#define K1_SPI_FIFO_RFT_SHIFT            5u
#define K1_SPI_FIFO_RFT_MASK             (0x1ful << K1_SPI_FIFO_RFT_SHIFT)
#define K1_SPI_FIFO_TFT(level)           ((uint32_t)(level) & 0x1fu)
#define K1_SPI_FIFO_RFT(level)           \
  ((uint32_t)(level) << K1_SPI_FIFO_RFT_SHIFT)

/* SSP Status Register fields.  Error bits are write-one-to-clear. */

#define K1_SPI_STATUS_BSY                (1ul << 0)
#define K1_SPI_STATUS_TNF                (1ul << 6)
#define K1_SPI_STATUS_TUR                (1ul << 12)
#define K1_SPI_STATUS_RNE                (1ul << 14)
#define K1_SPI_STATUS_ROR                (1ul << 20)
#define K1_SPI_STATUS_BCE                (1ul << 21)
#define K1_SPI_STATUS_ERROR_MASK         \
  (K1_SPI_STATUS_TUR | K1_SPI_STATUS_ROR | K1_SPI_STATUS_BCE)

#endif /* __CHIP_K1_HARDWARE_K1_SPI_H */
