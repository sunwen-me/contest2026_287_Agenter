/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_wdt.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_WDT_H
#define __CHIP_K1_HARDWARE_K1_WDT_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The K1 watchdog is clocked from the MPMU and requires a separate start
 * latch.  These addresses and the register sequence are used by the K1
 * vendor U-Boot watchdog, clock, and reset drivers.
 */

#define K1_WDT_BASE                         0xd4080000ul
#define K1_WDT_START_REG                    0xd4051020ul
#define K1_MPMU_WDTPCR                      0xd4050200ul

/* MPMU WDTPCR fields. */

#define K1_WDT_CLOCK_BUS_ENABLE             (1ul << 0)
#define K1_WDT_CLOCK_FUNCTION_ENABLE        (1ul << 1)
#define K1_WDT_CLOCK_RESET                  (1ul << 2)

/* WDT register offsets. */

#define K1_WDT_WFAR_OFFSET                  0x0b0ul
#define K1_WDT_WSAR_OFFSET                  0x0b4ul
#define K1_WDT_ENABLE_OFFSET                0x0b8ul
#define K1_WDT_TIMEOUT_OFFSET               0x0bcul
#define K1_WDT_STATUS_OFFSET                0x0c0ul
#define K1_WDT_RESET_OFFSET                 0x0c8ul

/* WDT protected-write unlock sequence. */

#define K1_WDT_WFAR_KEY                     0xbabaul
#define K1_WDT_WSAR_KEY                     0xeb10ul

/* WDT control values. */

#define K1_WDT_ENABLE                       0x3ul
#define K1_WDT_DISABLE                      0x0ul
#define K1_WDT_RESET_ENABLE                 0x1ul
#define K1_WDT_STATUS_CLEAR                 0x0ul
#define K1_WDT_START_ENABLE                 (1ul << 4)

/* The vendor driver converts milliseconds to this controller counter rate.
 * The upstream clock tree supplies PLL1/96 (25.6 MHz); the watchdog block
 * divides it internally before consuming the programmed timeout count.
 */

#define K1_WDT_COUNTER_HZ                   256ul

#endif /* __CHIP_K1_HARDWARE_K1_WDT_H */
