/****************************************************************************
 * vendor/spacemit/chips/k1/k1_irq.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "chip.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void up_irqinitialize(void)
{
  up_irq_save();
  riscv_exception_attach();

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 IRQ: exception vectors attached\r\n");
#endif

#ifdef CONFIG_K1_PLIC
  k1_plic_initialize();
#  ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 IRQ: PLIC sources masked\r\n");
#  endif
#endif

#ifndef CONFIG_SUPPRESS_INTERRUPTS
  riscv_color_intstack();
#  ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 IRQ: enabling global interrupts\r\n");
#  endif
  up_irq_enable();
#endif
}

void up_disable_irq(int irq)
{
  if (irq == RISCV_IRQ_SOFT)
    {
      CLEAR_CSR(CSR_IE, IE_SIE);
    }
  else if (irq == RISCV_IRQ_TIMER)
    {
      CLEAR_CSR(CSR_IE, IE_TIE);
    }
  else if (irq == RISCV_IRQ_EXT)
    {
      CLEAR_CSR(CSR_IE, IE_EIE);
    }
#ifdef CONFIG_K1_PLIC
  else if (irq > RISCV_IRQ_EXT)
    {
      k1_plic_disable_irq(irq);
    }
#endif
}

void up_enable_irq(int irq)
{
  if (irq == RISCV_IRQ_SOFT)
    {
      SET_CSR(CSR_IE, IE_SIE);
    }
  else if (irq == RISCV_IRQ_TIMER)
    {
      SET_CSR(CSR_IE, IE_TIE);
    }
#ifdef CONFIG_K1_PLIC
  else if (irq == RISCV_IRQ_EXT)
    {
      SET_CSR(CSR_IE, IE_EIE);
    }
  else if (irq > RISCV_IRQ_EXT)
    {
      k1_plic_enable_irq(irq);
    }
#endif
}

irqstate_t up_irq_enable(void)
{
#ifdef CONFIG_K1_PLIC
  up_enable_irq(RISCV_IRQ_EXT);
#endif

  return READ_AND_SET_CSR(CSR_STATUS, STATUS_IE);
}
