/****************************************************************************
 * vendor/spacemit/chips/k1/k1_cache.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "k1_cache.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_DCACHE_LINE_SIZE 64u
#define K1_DCACHE_LINE_MASK (K1_DCACHE_LINE_SIZE - 1u)

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum k1_dcache_op_e
{
  K1_DCACHE_CLEAN,
  K1_DCACHE_INVALIDATE,
  K1_DCACHE_FLUSH
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void k1_dcache_op(uintptr_t start, size_t size,
                          enum k1_dcache_op_e operation)
{
  uintptr_t address;
  size_t count;

  if (size == 0)
    {
      return;
    }

  address = start & ~((uintptr_t)K1_DCACHE_LINE_MASK);
  size += start & K1_DCACHE_LINE_MASK;
  if (size < (start & K1_DCACHE_LINE_MASK))
    {
      return;
    }

  if (size > SIZE_MAX - K1_DCACHE_LINE_MASK)
    {
      return;
    }

  count = (size + K1_DCACHE_LINE_MASK) / K1_DCACHE_LINE_SIZE;
  if (count == 0)
    {
      return;
    }

  asm volatile ("fence rw, rw" : : : "memory");

  while (count-- > 0)
    {
      /* CBO instructions use opcode 0x0f/funct3 2.  Use .insn because the
       * current K1 NuttX compiler flags do not advertise zicbom even though
       * the C910 hardware supports it.
       */

      switch (operation)
        {
          case K1_DCACHE_CLEAN:
            asm volatile (".insn i 0x0f, 2, x0, %0, 1"
                          : : "r"(address) : "memory");
            break;

          case K1_DCACHE_INVALIDATE:
            asm volatile (".insn i 0x0f, 2, x0, %0, 0"
                          : : "r"(address) : "memory");
            break;

          case K1_DCACHE_FLUSH:
            asm volatile (".insn i 0x0f, 2, x0, %0, 2"
                          : : "r"(address) : "memory");
            break;
        }

      address += K1_DCACHE_LINE_SIZE;
    }

  asm volatile ("fence rw, rw" : : : "memory");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k1_dcache_clean(uintptr_t start, size_t size)
{
  k1_dcache_op(start, size, K1_DCACHE_CLEAN);
}

void k1_dcache_invalidate(uintptr_t start, size_t size)
{
  k1_dcache_op(start, size, K1_DCACHE_INVALIDATE);
}

void k1_dcache_flush(uintptr_t start, size_t size)
{
  k1_dcache_op(start, size, K1_DCACHE_FLUSH);
}
