/****************************************************************************
 * vendor/spacemit/chips/k1/k1_irq_dispatch.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>

#include "chip.h"
#include "riscv_internal.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_K1_EARLY_BOOT_LOG
static bool k1_exception_needs_log(uintreg_t cause)
{
  return cause < RISCV_IRQ_ECALLU || cause > RISCV_IRQ_ECALLM;
}

static void k1_log_exception(uintreg_t cause, uintreg_t *regs)
{
  k1_early_puts("\r\nK1 EXCEPTION\r\n  scause=");
  k1_early_puthex(cause);
  k1_early_puts("\r\n  sepc=");
  k1_early_puthex(regs[REG_EPC]);
  k1_early_puts("\r\n  stval=");
  k1_early_puthex(READ_CSR(CSR_TVAL));
  k1_early_puts("\r\n  sstatus=");
  k1_early_puthex(regs[REG_INT_CTX]);
  k1_early_puts("\r\n  satp=");
  k1_early_puthex(READ_CSR(CSR_SATP));
  k1_early_puts("\r\n  sp=");
  k1_early_puthex(regs[REG_SP]);
  k1_early_puts("\r\n");
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void *riscv_dispatch_irq(uintreg_t vector, uintreg_t *regs)
{
  int irq = vector & RISCV_IRQ_MASK;

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  if ((vector & RISCV_IRQ_BIT) == 0 && regs != NULL &&
      k1_exception_needs_log((uintreg_t)irq))
    {
      k1_log_exception((uintreg_t)irq, regs);
    }
#endif

  if ((vector & RISCV_IRQ_BIT) != 0)
    {
      irq += RISCV_IRQ_ASYNC;

#ifdef CONFIG_K1_PLIC
      if (irq == RISCV_IRQ_EXT)
        {
          unsigned int source;

          while ((source = k1_plic_claim()) != 0)
            {
              if (source <= K1_PLIC_NDEV)
                {
                  regs = riscv_doirq(RISCV_IRQ_EXT + source, regs);
                  k1_plic_complete(source);
                }
              else
                {
                  break;
                }
            }

          return regs;
        }
#endif
    }

  return riscv_doirq(irq, regs);
}
