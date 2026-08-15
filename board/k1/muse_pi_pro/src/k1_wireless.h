/****************************************************************************
 * vendor/spacemit/boards/k1/muse_pi_pro/src/k1_wireless.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_K1_MUSE_PI_PRO_SRC_K1_WIRELESS_H
#define __BOARD_K1_MUSE_PI_PRO_SRC_K1_WIRELESS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_K1_RTL8852BS2_WIFI) || defined(CONFIG_K1_RTL8852BS2_BT)
int k1_wireless_initialize(void);
#endif

#endif /* __BOARD_K1_MUSE_PI_PRO_SRC_K1_WIRELESS_H */
