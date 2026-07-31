/****************************************************************************
 * vendor/spacemit/chips/k1/chip.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_CHIP_H
#define __CHIP_K1_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <arch/chip/chip.h>

#include "k1_memorymap.h"
#include "hardware/k1_memorymap.h"
#include "hardware/k1_uart.h"

#include "riscv_internal.h"
#include "riscv_percpu.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef __ASSEMBLY__

#  if CONFIG_ARCH_INTERRUPTSTACK > 15
#    if !defined(CONFIG_SMP) && defined(CONFIG_ARCH_USE_S_MODE)
.macro setintstack tmp0, tmp1
  csrr    \tmp0, CSR_SCRATCH
  REGLOAD sp, RISCV_PERCPU_IRQSTACK(\tmp0)
.endm
#    endif
#  endif

#endif /* __ASSEMBLY__ */

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void k1_early_puts(FAR const char *str);
void k1_early_puthex(uintreg_t value);

#ifdef CONFIG_K1_PLIC
void k1_plic_initialize(void);
void k1_plic_disable_irq(int irq);
void k1_plic_enable_irq(int irq);
unsigned int k1_plic_claim(void);
void k1_plic_complete(unsigned int source);
#endif

#endif
#endif /* __CHIP_K1_CHIP_H */
