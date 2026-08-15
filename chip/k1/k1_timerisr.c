/****************************************************************************
 * vendor/spacemit/chips/k1/k1_timerisr.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>

#include <nuttx/timers/arch_alarm.h>

#include "chip.h"
#include "riscv_internal.h"
#include "riscv_mtimer.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void up_timer_initialize(void)
{
  struct oneshot_lowerhalf_s *lower;

  /* In S-mode the common RISC-V timer lower half uses the SSTC stimecmp CSR
   * when CONFIG_ARCH_RV_EXT_SSTC is enabled. K1 advertises SSTC in its CPU
   * ISA and Linux confirms that S-mode timer interrupts are available. The
   * common lower half retains SBI TIME as the fallback when SSTC is disabled.
   * The MMIO arguments are deliberately zero because CLINT is owned by
   * firmware.
   */

  lower = riscv_mtimer_initialize(0, 0, RISCV_IRQ_STIMER,
                                  CONFIG_K1_SBI_TIMEBASE_FREQUENCY);
  DEBUGASSERT(lower != NULL);

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 timer: lower-half initialized\r\n");
#endif

  up_alarm_set_lowerhalf(lower);
}
