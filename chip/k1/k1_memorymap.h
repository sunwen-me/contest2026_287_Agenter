/****************************************************************************
 * vendor/spacemit/chips/k1/k1_memorymap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_K1_MEMORYMAP_H
#define __CHIP_K1_K1_MEMORYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "riscv_common_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef __ASSEMBLY__
#  define K1_IDLESTACK_BASE  (uintptr_t)_ebss
#else
#  define K1_IDLESTACK_BASE  _ebss
#endif

#endif /* __CHIP_K1_K1_MEMORYMAP_H */
