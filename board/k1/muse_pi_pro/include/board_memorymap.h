/****************************************************************************
 * board/k1/muse_pi_pro/include/board_memorymap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_K1_MUSE_PI_PRO_INCLUDE_BOARD_MEMORYMAP_H
#define __BOARD_K1_MUSE_PI_PRO_INCLUDE_BOARD_MEMORYMAP_H

/* Provisional openvela load window.
 *
 * 0x11000000 was validated by the reference K1 OS as a U-Boot staging
 * address. It must still be checked against the exact contest image, DTB,
 * OpenSBI reserved memory, and U-Boot relocation ranges before release.
 */

#define K1_OPENVELA_RAM_START  0x11000000ul
#define K1_OPENVELA_RAM_SIZE   (128ul * 1024ul * 1024ul)

/* Real-board device-tree observations. */

#define K1_UART0_BASE          0xd4017000ul
#define K1_UART0_SIZE          0x00010000ul
#define K1_UART0_IRQ           42
#define K1_UART_REG_SHIFT      2
#define K1_UART_REG_WIDTH      32

#define K1_PLIC_BASE           0xe0000000ul
#define K1_PLIC_SIZE           0x04000000ul
#define K1_PLIC_SOURCES        159

#define K1_CLINT_BASE          0xe4000000ul
#define K1_CLINT_SIZE          0x00010000ul

#endif /* __BOARD_K1_MUSE_PI_PRO_INCLUDE_BOARD_MEMORYMAP_H */
