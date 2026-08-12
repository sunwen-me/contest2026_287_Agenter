/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_GPIO_H
#define __CHIP_K1_HARDWARE_K1_GPIO_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* K1 APB clock and reset controller. */

#define K1_APBC_BASE                    0xd4015000ul
#define K1_APBC_GPIO_CLK_RST            (K1_APBC_BASE + 0x008ul)
#define K1_APBC_AIB_CLK_RST             (K1_APBC_BASE + 0x03cul)

#define K1_CLK_BUS_ENABLE               (1ul << 0)
#define K1_CLK_FUNCTION_ENABLE          (1ul << 1)
#define K1_CLK_RESET                    (1ul << 2)

/* K1 multi-function pin register for GPIO49 (physical header pin 22). */

#define K1_MFPR_BASE                    0xd401e000ul
#define K1_GPIO49_MFPR                 (K1_MFPR_BASE + 0x0c8ul)

#define K1_MFPR_MUX_MODE0              0ul
#define K1_MFPR_EDGE_CLEAR             (1ul << 6)
#define K1_MFPR_DRIVE_3V3_DS1          (1ul << 10)
#define K1_MFPR_PULLDOWN               (1ul << 13)
#define K1_MFPR_PULLUP                 (1ul << 14)
#define K1_MFPR_PULL_ENABLE            (1ul << 15)

#define K1_MFPR_PULL_DOWN              (K1_MFPR_PULL_ENABLE | \
                                        K1_MFPR_PULLDOWN)
#define K1_MFPR_PULL_UP                (K1_MFPR_PULL_ENABLE | \
                                        K1_MFPR_PULLUP)

#define K1_GPIO49_PAD_INPUT            \
  (K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR | K1_MFPR_DRIVE_3V3_DS1)
#define K1_GPIO49_PAD_INPUT_PULLUP     \
  (K1_GPIO49_PAD_INPUT | K1_MFPR_PULL_UP)
#define K1_GPIO49_PAD_INPUT_PULLDOWN   \
  (K1_GPIO49_PAD_INPUT | K1_MFPR_PULL_DOWN)
#define K1_GPIO49_PAD_OUTPUT           K1_GPIO49_PAD_INPUT

/* GPIO register bank 1 contains GPIO32..GPIO63.  GPIO49 is bit 17. */

#define K1_GPIO_BASE                    0xd4019000ul
#define K1_GPIO49_BANK_BASE             (K1_GPIO_BASE + 0x004ul)
#define K1_GPIO49_MASK                 (1ul << 17)

#define K1_GPIO_GPLR_OFFSET             0x000ul
#define K1_GPIO_GPDR_OFFSET             0x00cul
#define K1_GPIO_GPSR_OFFSET             0x018ul
#define K1_GPIO_GPCR_OFFSET             0x024ul
#define K1_GPIO_GSDR_OFFSET             0x054ul
#define K1_GPIO_GCDR_OFFSET             0x060ul

#define K1_GPIO49_GPLR                 \
  (K1_GPIO49_BANK_BASE + K1_GPIO_GPLR_OFFSET)
#define K1_GPIO49_GPDR                 \
  (K1_GPIO49_BANK_BASE + K1_GPIO_GPDR_OFFSET)
#define K1_GPIO49_GPSR                 \
  (K1_GPIO49_BANK_BASE + K1_GPIO_GPSR_OFFSET)
#define K1_GPIO49_GPCR                 \
  (K1_GPIO49_BANK_BASE + K1_GPIO_GPCR_OFFSET)
#define K1_GPIO49_GSDR                 \
  (K1_GPIO49_BANK_BASE + K1_GPIO_GSDR_OFFSET)
#define K1_GPIO49_GCDR                 \
  (K1_GPIO49_BANK_BASE + K1_GPIO_GCDR_OFFSET)

#endif /* __CHIP_K1_HARDWARE_K1_GPIO_H */
