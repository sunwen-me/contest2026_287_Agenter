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

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

FAR struct sdio_dev_s *sdio_initialize(int slotno);

#ifdef CONFIG_K1_SDIO_WIFI
int k1_sdio_wifi_probe(FAR uint32_t *ocr);
#endif

#endif /* __CHIP_K1_K1_SDIO_H */
