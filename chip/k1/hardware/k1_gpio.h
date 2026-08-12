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
#define K1_APBC_ASFAR                   (K1_APBC_BASE + 0x050ul)
#define K1_APBC_ASSAR                   (K1_APBC_BASE + 0x054ul)

/* APBC secure-access keys for the MFPR IO power-domain registers. */

#define K1_APBC_ASFAR_KEY               0xbabaul
#define K1_APBC_ASSAR_KEY               0xeb10ul

#define K1_CLK_BUS_ENABLE               (1ul << 0)
#define K1_CLK_FUNCTION_ENABLE          (1ul << 1)
#define K1_CLK_RESET                    (1ul << 2)

/* K1 multi-function pin registers.  The offsets follow the Linux mainline
 * spacemit_k1_pin_to_offset() mapping.  GPIO91 and GPIO92 are after a
 * reserved MFPR range, so their offsets are not simply GPIO + 1.
 */

#define K1_MFPR_BASE                    0xd401e000ul

/* MFPR IO power-domain registers.  These registers are protected by the
 * APBC secure-access sequence above.  A cleared V18EN bit selects the
 * board's 3.3V external IO supply; setting it selects 1.8V.
 */

#define K1_MFPR_IO_PWR_DOMAIN_OFFSET    0x800ul
#define K1_MFPR_IO_PWR_GPIO2_OFFSET     0x00cul /* GPIO75..GPIO80 */
#define K1_MFPR_IO_PWR_GPIO3_OFFSET     0x010ul /* GPIO47..GPIO52 */
#define K1_MFPR_IO_PWR_V18EN             (1ul << 2)

#define K1_MFPR_IO_PWR_GPIO2            \
  (K1_MFPR_BASE + K1_MFPR_IO_PWR_DOMAIN_OFFSET + \
   K1_MFPR_IO_PWR_GPIO2_OFFSET)
#define K1_MFPR_IO_PWR_GPIO3            \
  (K1_MFPR_BASE + K1_MFPR_IO_PWR_DOMAIN_OFFSET + \
   K1_MFPR_IO_PWR_GPIO3_OFFSET)

#define K1_MFPR_GPIO33                  (K1_MFPR_BASE + 0x088ul)
#define K1_MFPR_GPIO34                  (K1_MFPR_BASE + 0x08cul)
#define K1_MFPR_GPIO35                  (K1_MFPR_BASE + 0x090ul)
#define K1_MFPR_GPIO37                  (K1_MFPR_BASE + 0x098ul)
#define K1_MFPR_GPIO38                  (K1_MFPR_BASE + 0x09cul)
#define K1_MFPR_GPIO39                  (K1_MFPR_BASE + 0x0a0ul)
#define K1_MFPR_GPIO40                  (K1_MFPR_BASE + 0x0a4ul)
#define K1_MFPR_GPIO41                  (K1_MFPR_BASE + 0x0a8ul)
#define K1_MFPR_GPIO46                  (K1_MFPR_BASE + 0x0bcul)
#define K1_MFPR_GPIO47                  (K1_MFPR_BASE + 0x0c0ul)
#define K1_MFPR_GPIO48                  (K1_MFPR_BASE + 0x0c4ul)
#define K1_MFPR_GPIO49                  (K1_MFPR_BASE + 0x0c8ul)
#define K1_MFPR_GPIO50                  (K1_MFPR_BASE + 0x0ccul)
#define K1_MFPR_GPIO51                  (K1_MFPR_BASE + 0x0d0ul)
#define K1_MFPR_GPIO52                  (K1_MFPR_BASE + 0x0d4ul)
#define K1_MFPR_GPIO70                  (K1_MFPR_BASE + 0x11cul)
#define K1_MFPR_GPIO71                  (K1_MFPR_BASE + 0x120ul)
#define K1_MFPR_GPIO72                  (K1_MFPR_BASE + 0x124ul)
#define K1_MFPR_GPIO73                  (K1_MFPR_BASE + 0x128ul)
#define K1_MFPR_GPIO74                  (K1_MFPR_BASE + 0x12cul)
#define K1_MFPR_GPIO75                  (K1_MFPR_BASE + 0x130ul)
#define K1_MFPR_GPIO76                  (K1_MFPR_BASE + 0x134ul)
#define K1_MFPR_GPIO77                  (K1_MFPR_BASE + 0x138ul)
#define K1_MFPR_GPIO78                  (K1_MFPR_BASE + 0x13cul)
#define K1_MFPR_GPIO91                  (K1_MFPR_BASE + 0x200ul)
#define K1_MFPR_GPIO92                  (K1_MFPR_BASE + 0x204ul)

/* MFPR fields. */

#define K1_MFPR_MUX_MODE0              0ul
#define K1_MFPR_MUX_MODE1              1ul
#define K1_MFPR_EDGE_CLEAR             (1ul << 6)
#define K1_MFPR_DRIVE_1V8_DS2          (2ul << 10)
#define K1_MFPR_DRIVE_3V3_DS1          (1ul << 10)
#define K1_MFPR_PULLDOWN               (1ul << 13)
#define K1_MFPR_PULLUP                 (1ul << 14)
#define K1_MFPR_PULL_ENABLE            (1ul << 15)

#define K1_MFPR_PULL_DOWN              (K1_MFPR_PULL_ENABLE | \
                                        K1_MFPR_PULLDOWN)
#define K1_MFPR_PULL_UP                (K1_MFPR_PULL_ENABLE | \
                                        K1_MFPR_PULLUP)

/* K1 GPIO bank bases.  Linux mainline uses {0x0, 0x4, 0x8, 0x100}. */

#define K1_GPIO_BASE                    0xd4019000ul
#define K1_GPIO_BANK0_BASE              (K1_GPIO_BASE + 0x000ul)
#define K1_GPIO_BANK1_BASE              (K1_GPIO_BASE + 0x004ul)
#define K1_GPIO_BANK2_BASE              (K1_GPIO_BASE + 0x008ul)
#define K1_GPIO_BANK3_BASE              (K1_GPIO_BASE + 0x100ul)

/* GPIO register offsets. */

#define K1_GPIO_GPLR_OFFSET             0x000ul
#define K1_GPIO_GPDR_OFFSET             0x00cul
#define K1_GPIO_GPSR_OFFSET             0x018ul
#define K1_GPIO_GPCR_OFFSET             0x024ul
#define K1_GPIO_GRER_OFFSET             0x030ul
#define K1_GPIO_GFER_OFFSET             0x03cul
#define K1_GPIO_GEDR_OFFSET             0x048ul
#define K1_GPIO_GSDR_OFFSET             0x054ul
#define K1_GPIO_GCDR_OFFSET             0x060ul
#define K1_GPIO_GSRER_OFFSET            0x06cul
#define K1_GPIO_GCRER_OFFSET            0x078ul
#define K1_GPIO_GSFER_OFFSET            0x084ul
#define K1_GPIO_GCFER_OFFSET            0x090ul
#define K1_GPIO_GAPMASK_OFFSET          0x09cul

#endif /* __CHIP_K1_HARDWARE_K1_GPIO_H */
