/****************************************************************************
 * vendor/spacemit/chips/k1/k1_usbdev.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_K1_USBDEV_H
#define __CHIP_K1_K1_USBDEV_H

#include <stdbool.h>

typedef enum
{
  K1_USBDEV0 = 0,
} k1_usb_id_t;

#ifdef __cplusplus
extern "C"
{
#endif

int k1_usbdev_initialize(void);
void k1_usbdev_uninitialize(void);
bool k1_vbus_detect(k1_usb_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* __CHIP_K1_K1_USBDEV_H */
