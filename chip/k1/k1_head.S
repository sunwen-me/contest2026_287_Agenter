/****************************************************************************
 * vendor/spacemit/chips/k1/k1_head.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/arch.h>
#include <arch/irq.h>

#include "chip.h"
#include "riscv_internal.h"
#include "riscv_macros.S"

/****************************************************************************
 * Public Symbols
 ****************************************************************************/

  .section .text
  .global __start

__start:
  /* The first milestone supports the K1 boot hart (hart 0) only. Preserve
   * a1 because U-Boot/OpenSBI normally passes the DTB pointer there.
   */

  bnez a0, 1f

  /* The C ABI permits linker relaxation to generate gp-relative accesses.
   * Initialize gp explicitly before entering any C code.
   */

  .option push
  .option norelax
  la   gp, __global_pointer$
  .option pop

  riscv_set_inital_sp K1_IDLESTACK_BASE, SMP_STACK_SIZE, a0

  csrw CSR_IE, zero
  csrw CSR_SCRATCH, zero
  la   t0, __trap_vec
  csrw CSR_TVEC, t0

  jal  x1, k1_start

1:
  csrw CSR_IE, zero
2:
  wfi
  j 2b

  .global _init
  .global _fini

_init:
_fini:
  ret
