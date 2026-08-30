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

#include <stdbool.h>
#include <stddef.h>
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
  uint32_t function1_local;
  uint8_t cccr_revision;
  uint8_t sd_spec_revision;
  uint8_t io_enable;
  uint8_t io_ready;
  uint8_t bus_interface;
  uint8_t card_capability;
  uint8_t speed_control;
  uint8_t high_speed_enabled;
  uint8_t uhs_support;
  uint8_t sdr104_enabled;
  uint8_t rx_delaycode;
  uint8_t function_count;
  uint8_t function_interface[K1_SDIO_WIFI_MAX_FUNCTIONS];
};

int k1_sdio_wifi_prepare(void);
int k1_sdio_wifi_probe(FAR struct k1_sdio_wifi_info_s *info);
int k1_sdio_wifi_enable_function(uint8_t function, uint16_t blocksize,
                                 FAR struct k1_sdio_wifi_info_s *info);
int k1_sdio_wifi_recover_after_crc(void);
void k1_sdio_wifi_suppress_command_trace(bool suppress);
int k1_sdio_wifi_f1_read_byte(uint32_t address, FAR uint8_t *value);
int k1_sdio_wifi_f1_write_byte(uint32_t address, uint8_t value);
int k1_sdio_wifi_read(uint8_t function, uint32_t address, bool increment,
                      FAR uint8_t *buffer, size_t length);
int k1_sdio_wifi_write(uint8_t function, uint32_t address, bool increment,
                       FAR const uint8_t *buffer, size_t length);
int k1_sdio_wifi_fwdl_write(uint32_t address, FAR const uint8_t *buffer,
                             size_t length);
int k1_sdio_wifi_rxfifo_read(uint32_t address, FAR uint8_t *buffer,
                             size_t length);
#endif

#endif /* __CHIP_K1_K1_SDIO_H */
