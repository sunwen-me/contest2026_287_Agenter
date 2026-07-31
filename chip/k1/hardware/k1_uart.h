/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_uart.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_UART_H
#define __CHIP_K1_HARDWARE_K1_UART_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "k1_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* spacemit,pxa-uart uses 32-bit accesses and reg-shift = 2. */

#define K1_UART_REG_SHIFT     2
#define K1_UART_RBR_OFFSET    (0u << K1_UART_REG_SHIFT)
#define K1_UART_THR_OFFSET    (0u << K1_UART_REG_SHIFT)
#define K1_UART_IER_OFFSET    (1u << K1_UART_REG_SHIFT)
#define K1_UART_LSR_OFFSET    (5u << K1_UART_REG_SHIFT)

#define K1_UART_LSR_DR        (1u << 0)
#define K1_UART_LSR_THRE      (1u << 5)
#define K1_UART_LSR_TEMT      (1u << 6)

#endif /* __CHIP_K1_HARDWARE_K1_UART_H */
