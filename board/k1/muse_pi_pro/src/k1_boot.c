/****************************************************************************
 * board/k1/muse_pi_pro/src/k1_boot.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/board.h>
#include <syslog.h>

#include <arch/board/board.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k1_muse_pi_pro_board_initialize(void)
{
  /* Keep early board initialization deliberately empty.
   *
   * U-Boot already configured DRAM and the serial console. Reprogramming the
   * K1 UART before a safe lower-half driver exists can hang the APB bus.
   */
}

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
void board_early_initialize(void)
{
  k1_muse_pi_pro_board_initialize();
}
#endif

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
#if defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)
  int ret;

  ret = k1_gpio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: k1_gpio_initialize failed: %d\n", ret);
    }
#endif
}
#endif

int board_app_initialize(uintptr_t arg)
{
  (void)arg;
  return 0;
}
