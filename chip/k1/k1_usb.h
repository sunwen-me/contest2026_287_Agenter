/****************************************************************************
 * vendor/spacemit/chips/k1/k1_usb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_K1_USB_H
#define __CHIP_K1_K1_USB_H

#include <stdint.h>

int k1_usb_probe(void);

int k1_usb_host_glue_initialize(void);

int k1_usb_host_power_initialize(void);

uintptr_t k1_usb_dwc3_base(void);

#endif /* __CHIP_K1_K1_USB_H */
