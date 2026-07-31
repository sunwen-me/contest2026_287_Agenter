/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_plic.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_PLIC_H
#define __CHIP_K1_HARDWARE_K1_PLIC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "k1_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The K1 device tree lists two PLIC contexts for each hart: M-mode external
 * interrupt first, then S-mode external interrupt. Hart 0 S-mode is
 * therefore context index 1.
 */

#define K1_PLIC_HART0_S_CONTEXT       1u
#define K1_PLIC_CONTEXT_ENABLE_STRIDE  0x80u
#define K1_PLIC_CONTEXT_CTRL_STRIDE    0x1000u

#define K1_PLIC_PRIORITY_BASE         (K1_PLIC_BASE + 0x000000u)
#define K1_PLIC_ENABLE_BASE           (K1_PLIC_BASE + 0x002000u)
#define K1_PLIC_CONTEXT_BASE          (K1_PLIC_BASE + 0x200000u)

#define K1_PLIC_HART0_S_ENABLE        \
  (K1_PLIC_ENABLE_BASE + \
   K1_PLIC_HART0_S_CONTEXT * K1_PLIC_CONTEXT_ENABLE_STRIDE)
#define K1_PLIC_HART0_S_THRESHOLD     \
  (K1_PLIC_CONTEXT_BASE + \
   K1_PLIC_HART0_S_CONTEXT * K1_PLIC_CONTEXT_CTRL_STRIDE)
#define K1_PLIC_HART0_S_CLAIM         (K1_PLIC_HART0_S_THRESHOLD + 4u)

#define K1_PLIC_PRIORITY(source)      \
  (K1_PLIC_PRIORITY_BASE + 4u * (source))
#define K1_PLIC_ENABLE(source)        \
  (K1_PLIC_HART0_S_ENABLE + 4u * ((source) / 32u))
#define K1_PLIC_ENABLE_BIT(source)    (1u << ((source) % 32u))

#define K1_PLIC_PRIORITY_DISABLED     0u
#define K1_PLIC_PRIORITY_DEFAULT      1u

#endif /* __CHIP_K1_HARDWARE_K1_PLIC_H */
