/****************************************************************************
 * vendor/spacemit/chips/k1/k1_start.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/init.h>

#include "chip.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void k1_clear_bss(void)
{
  uint32_t *dest;

#ifndef CONFIG_ARCH_SKIP_ZERO_BSS
  for (dest = (uint32_t *)_sbss; dest < (uint32_t *)_ebss; )
    {
      *dest++ = 0;
    }
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k1_start(int mhartid, const void *dtb)
{
  /* The first bring-up is intentionally single-core. */

  if (mhartid != 0)
    {
      goto halt;
    }

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("\r\nK1: entry\r\n");
  k1_early_puts("K1: hart=");
  k1_early_puthex((uintreg_t)mhartid);
  k1_early_puts(" dtb=");
  k1_early_puthex((uintreg_t)(uintptr_t)dtb);
  k1_early_puts("\r\nK1: initial sstatus=");
  k1_early_puthex(READ_CSR(CSR_STATUS));
  k1_early_puts(" satp=");
  k1_early_puthex(READ_CSR(CSR_SATP));
  k1_early_puts(" stvec=");
  k1_early_puthex(READ_CSR(CSR_TVEC));
  k1_early_puts("\r\n");
#endif

  k1_clear_bss();

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1: bss-clear\r\n");
#endif

  riscv_percpu_add_hart((uintptr_t)mhartid);

  /* U-Boot enters the payload in S-mode with bare addressing. Make that
   * assumption explicit before enabling any NuttX interrupt handling.
   */

  WRITE_CSR(CSR_SATP, 0);
  asm volatile("sfence.vma" ::: "memory");

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1: s-mode bare\r\n");
#endif

  riscv_fpuconfig();

#ifdef USE_EARLYSERIALINIT
  riscv_earlyserialinit();
#endif

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1: nx_start\r\n");
#endif

  nx_start();

halt:
  for (; ; )
    {
      asm volatile("wfi");
    }
}
