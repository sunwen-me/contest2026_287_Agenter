/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_pwm.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_PWM_H
#define __CHIP_K1_HARDWARE_K1_PWM_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MUSE Pi Pro exposes PWM11 on GPIO41 / 40-pin header Pin 3. */

#define K1_PWM11_BASE                    0xd4020c00ul
#define K1_APBC_PWM11_CLK_RST            0xd40150c4ul

/* The K1 PWM APBC clock has PLL1/192 (12.8 MHz) at parent index zero. */

#define K1_PWM_CLK_PARENT_SHIFT           4u
#define K1_PWM_CLK_PARENT_MASK            (7ul << K1_PWM_CLK_PARENT_SHIFT)
#define K1_PWM_CLK_PARENT_PLL1_D192       0ul

/* PWM register offsets. */

#define K1_PWM_CR_OFFSET                  0x000ul
#define K1_PWM_DCR_OFFSET                 0x004ul
#define K1_PWM_PCR_OFFSET                 0x008ul

/* PWMCR fields. */

#define K1_PWM_CR_PRESCALE_MASK           0x3ful
#define K1_PWM_CR_ABRUPT_SHUTDOWN         (1ul << 6)
#define K1_PWM_PERIOD_MAX                 1024ul
#define K1_PWM_PRESCALE_MAX               63ul

#endif /* __CHIP_K1_HARDWARE_K1_PWM_H */
