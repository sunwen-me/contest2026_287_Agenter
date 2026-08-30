/****************************************************************************
 * vendor/spacemit/chips/k1/k1_bt_uart.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_K1_BT_UART_H
#define __CHIP_K1_K1_BT_UART_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_K1_RTL8852BS2_BT
struct k1_bt_h5_info_s
{
  uint8_t hci_version;
  uint16_t hci_revision;
  uint8_t lmp_version;
  uint16_t manufacturer;
  uint16_t lmp_subversion;
  uint8_t rom_version;
  uint8_t chip_type;
  uint8_t chip_version;
  uint16_t patch_packets;
  bool crc_enabled;
  bool vendor_firmware_loaded;
};

int k1_bt_uart_initialize(FAR struct k1_bt_h5_info_s *info);

/****************************************************************************
 * Name: k1_bt_uart_register
 *
 * Description:
 *   Register the polled UART2 H5 transport through NuttX's Bluetooth
 *   driver and SLIP layers.  The resulting controller is exposed as
 *   /dev/ttyHCI0 when CONFIG_UART_BTH4 is selected.
 *
 ****************************************************************************/

int k1_bt_uart_register(void);
#endif

#endif /* __CHIP_K1_K1_BT_UART_H */
