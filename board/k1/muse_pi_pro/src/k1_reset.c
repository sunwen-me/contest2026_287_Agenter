/****************************************************************************
 * board/k1/muse_pi_pro/src/k1_reset.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_BOARDCTL_RESET)

#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/spinlock.h>

#include "hardware/k1_wdt.h"
#include "riscv_internal.h"
#include "riscv_sbi.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void k1_reset_wdt_write(uint32_t value, uintptr_t offset)
{
  putreg32(K1_WDT_WFAR_KEY, K1_WDT_BASE + K1_WDT_WFAR_OFFSET);
  putreg32(K1_WDT_WSAR_KEY, K1_WDT_BASE + K1_WDT_WSAR_OFFSET);
  putreg32(value, K1_WDT_BASE + offset);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_reset
 ****************************************************************************/

int board_reset(int status)
{
  (void)status;

  irqstate_t flags;
  uint32_t value;

  /* U-Boot entered NuttX in S-mode with OpenSBI underneath it.  Try the
   * standard SBI reset extension first.  The K1 firmware currently returns
   * SBI_ERR_NOT_SUPPORTED, so the on-chip watchdog below is the deterministic
   * board-local fallback.
   */

  (void)riscv_sbi_system_reset(SBI_SRST_TYPE_REBOOT_WARM,
                               SBI_SRST_REASON_NONE);

  flags = enter_critical_section();
  value = getreg32(K1_MPMU_WDTPCR);
  value |= K1_WDT_CLOCK_BUS_ENABLE | K1_WDT_CLOCK_FUNCTION_ENABLE;
  value &= ~K1_WDT_CLOCK_RESET;
  putreg32(value, K1_MPMU_WDTPCR);
  k1_reset_wdt_write(K1_WDT_STATUS_CLEAR, K1_WDT_STATUS_OFFSET);
  k1_reset_wdt_write(1, K1_WDT_TIMEOUT_OFFSET);
  k1_reset_wdt_write(K1_WDT_ENABLE, K1_WDT_ENABLE_OFFSET);
  k1_reset_wdt_write(K1_WDT_RESET_ENABLE, K1_WDT_RESET_OFFSET);
  modifyreg32(K1_WDT_START_REG, 0, K1_WDT_START_ENABLE);

  /* A successful watchdog reset does not return to boardctl(). */

  (void)flags;
  for (;;)
    {
      asm volatile("wfi");
    }
}

#endif /* CONFIG_BOARDCTL_RESET */
