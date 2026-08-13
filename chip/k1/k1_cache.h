/****************************************************************************
 * vendor/spacemit/chips/k1/k1_cache.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_K1_CACHE_H
#define __CHIP_K1_K1_CACHE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The K1 C910 cores have 64-byte data-cache lines and implement the
 * RISC-V Zicbom cache-block operations.  These helpers deliberately remain
 * private to K1 drivers until the architecture-wide cache API is complete.
 */

void k1_dcache_clean(uintptr_t start, size_t size);
void k1_dcache_invalidate(uintptr_t start, size_t size);
void k1_dcache_flush(uintptr_t start, size_t size);

#endif /* __CHIP_K1_K1_CACHE_H */
