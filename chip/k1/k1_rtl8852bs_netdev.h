/****************************************************************************
 * vendor/spacemit/chips/k1/k1_rtl8852bs_netdev.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The openvela contributors.
 ****************************************************************************/

#ifndef __CHIP_K1_K1_RTL8852BS_NETDEV_H
#define __CHIP_K1_K1_RTL8852BS_NETDEV_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_K1_RTL8852BS2_WLAN_NETDEV

/****************************************************************************
 * Name: k1_rtl8852bs_netdev_register
 *
 * Description:
 *   Register the RTL8852BS2 wireless network device.  The device answers
 *   the two scan requests of the wireless extensions and nothing else: it
 *   has no transmit or receive data path and performs no association.
 *
 * Input Parameters:
 *   mac - The six-byte station address read from the device eFuse.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.
 *
 ****************************************************************************/

int k1_rtl8852bs_netdev_register(FAR const uint8_t *mac);

#endif /* CONFIG_K1_RTL8852BS2_WLAN_NETDEV */

#endif /* __CHIP_K1_K1_RTL8852BS_NETDEV_H */
