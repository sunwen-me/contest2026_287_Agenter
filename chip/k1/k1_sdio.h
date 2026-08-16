/****************************************************************************
 * vendor/spacemit/chips/k1/k1_sdio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_K1_SDIO_H
#define __CHIP_K1_K1_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/sdio.h>

#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

FAR struct sdio_dev_s *sdio_initialize(int slotno);

#ifdef CONFIG_K1_SDIO_WIFI
#  define K1_SDIO_WIFI_MAX_FUNCTIONS 7

struct k1_sdio_wifi_info_s
{
  uint32_t ocr;
  uint8_t cccr_revision;
  uint8_t sd_spec_revision;
  uint8_t io_enable;
  uint8_t io_ready;
  uint8_t bus_interface;
  uint8_t card_capability;
  uint8_t function_count;
  uint8_t function_interface[K1_SDIO_WIFI_MAX_FUNCTIONS];
};

int k1_sdio_wifi_probe(FAR struct k1_sdio_wifi_info_s *info);
#endif

#endif /* __CHIP_K1_K1_SDIO_H */
