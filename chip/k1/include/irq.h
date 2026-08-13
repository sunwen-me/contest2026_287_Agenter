/****************************************************************************
 * vendor/spacemit/chips/k1/include/irq.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_INCLUDE_IRQ_H
#define __CHIP_K1_INCLUDE_IRQ_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The K1 device tree reports 159 PLIC interrupt sources. Source zero is
 * reserved by the PLIC specification.
 */

#define K1_PLIC_NDEV  159
#define NR_IRQS       (RISCV_IRQ_EXT + K1_PLIC_NDEV + 1)

#define K1_IRQ_UART0  (RISCV_IRQ_EXT + 42)
#define K1_IRQ_GPIO   (RISCV_IRQ_EXT + 58)

#endif /* __CHIP_K1_INCLUDE_IRQ_H */
