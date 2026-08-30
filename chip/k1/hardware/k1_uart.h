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
#define K1_UART_DLL_OFFSET    (0u << K1_UART_REG_SHIFT)
#define K1_UART_IER_OFFSET    (1u << K1_UART_REG_SHIFT)
#define K1_UART_DLM_OFFSET    (1u << K1_UART_REG_SHIFT)
#define K1_UART_IIR_OFFSET    (2u << K1_UART_REG_SHIFT)
#define K1_UART_FCR_OFFSET    (2u << K1_UART_REG_SHIFT)
#define K1_UART_LCR_OFFSET    (3u << K1_UART_REG_SHIFT)
#define K1_UART_MCR_OFFSET    (4u << K1_UART_REG_SHIFT)
#define K1_UART_LSR_OFFSET    (5u << K1_UART_REG_SHIFT)

#define K1_UART_IER_RDA       (1u << 0)
#define K1_UART_IER_UUE       (1u << 6)

#define K1_UART_IIR_NO_INT    (1u << 0)

#define K1_UART_FCR_FIFO_EN   (1u << 0)
#define K1_UART_FCR_RXRST     (1u << 1)
#define K1_UART_FCR_TXRST     (1u << 2)
#define K1_UART_FCR_TRIG_14   (3u << 6)

#define K1_UART_LCR_WLS_8     (3u << 0)
#define K1_UART_LCR_PEN       (1u << 3)
#define K1_UART_LCR_EPS       (1u << 4)
#define K1_UART_LCR_DLAB      (1u << 7)

#define K1_UART_MCR_DTR       (1u << 0)
#define K1_UART_MCR_RTS       (1u << 1)
#define K1_UART_MCR_OUT2      (1u << 3)
#define K1_UART_MCR_AFCE      (1u << 5)

#define K1_UART_LSR_DR        (1u << 0)
#define K1_UART_LSR_THRE      (1u << 5)
#define K1_UART_LSR_TEMT      (1u << 6)

#define K1_UART2_SLOW_14M_CLOCK_HZ  14745600ul
#define K1_UART2_SLOW_48M_CLOCK_HZ  48000000ul

#endif /* __CHIP_K1_HARDWARE_K1_UART_H */
