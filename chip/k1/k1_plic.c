/****************************************************************************
 * vendor/spacemit/chips/k1/k1_plic.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "chip.h"
#include "hardware/k1_plic.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k1_plic_initialize(void)
{
  unsigned int source;

  /* Start with every source disabled for the hart 0 S-mode context and with
   * every global priority at zero. Device drivers opt in one source at a
   * time through up_enable_irq().
   */

  for (source = 0; source <= K1_PLIC_NDEV; source += 32)
    {
      putreg32(0, K1_PLIC_ENABLE(source));
    }

  for (source = 1; source <= K1_PLIC_NDEV; source++)
    {
      putreg32(K1_PLIC_PRIORITY_DISABLED, K1_PLIC_PRIORITY(source));
    }

  putreg32(0, K1_PLIC_HART0_S_THRESHOLD);
}

void k1_plic_disable_irq(int irq)
{
  unsigned int source;
  irqstate_t flags;

  source = (unsigned int)(irq - RISCV_IRQ_EXT);
  if (source == 0 || source > K1_PLIC_NDEV)
    {
      return;
    }

  flags = enter_critical_section();
  modifyreg32(K1_PLIC_ENABLE(source), K1_PLIC_ENABLE_BIT(source), 0);
  putreg32(K1_PLIC_PRIORITY_DISABLED, K1_PLIC_PRIORITY(source));
  leave_critical_section(flags);
}

void k1_plic_enable_irq(int irq)
{
  unsigned int source;
  irqstate_t flags;

  source = (unsigned int)(irq - RISCV_IRQ_EXT);
  if (source == 0 || source > K1_PLIC_NDEV)
    {
      return;
    }

  flags = enter_critical_section();
  putreg32(K1_PLIC_PRIORITY_DEFAULT, K1_PLIC_PRIORITY(source));
  modifyreg32(K1_PLIC_ENABLE(source), 0, K1_PLIC_ENABLE_BIT(source));
  leave_critical_section(flags);
}

unsigned int k1_plic_claim(void)
{
  return getreg32(K1_PLIC_HART0_S_CLAIM);
}

void k1_plic_complete(unsigned int source)
{
  if (source > 0 && source <= K1_PLIC_NDEV)
    {
      putreg32(source, K1_PLIC_HART0_S_CLAIM);
    }
}
