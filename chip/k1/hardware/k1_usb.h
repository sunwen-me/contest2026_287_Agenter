/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_usb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_USB_H
#define __CHIP_K1_HARDWARE_K1_USB_H

#include "k1_memorymap.h"

/* K1 DTS: spacemit,k1-dwc3 and the two PHY blocks. */

#define K1_USB_DWC3_BASE                  0xc0a00000ul
#define K1_USB2_PHY_BASE                  0xc0a30000ul
#define K1_USB_COMBO_PHY_BASE             0xc0b10000ul

/* APMU USB clock and reset control.  These fields match the mainline K1
 * syscon clock/reset binding: clock bit 8, followed by USB30 AHB/VCC/PHY
 * reset release bits 9..11. */

#define K1_APMU_USB_CLK_RST               (K1_APMU_BASE + 0x05cul)
#define K1_APMU_PCIE0_CLK_RST             (K1_APMU_BASE + 0x3ccul)
#define K1_APMU_USB30_CLOCK_ENABLE        (1ul << 8)
#define K1_APMU_USB30_AHB_RESET_RELEASE   (1ul << 9)
#define K1_APMU_USB30_VCC_RESET_RELEASE   (1ul << 10)
#define K1_APMU_USB30_PHY_RESET_RELEASE  (1ul << 11)
#define K1_APMU_USB30_RESET_RELEASE       \
  (K1_APMU_USB30_AHB_RESET_RELEASE |     \
   K1_APMU_USB30_VCC_RESET_RELEASE |     \
   K1_APMU_USB30_PHY_RESET_RELEASE)

/* DWC3 global register offsets. */

#define K1_USB_DWC3_GCTL                  0xc110ul
#define K1_USB_DWC3_GSTS                  0xc118ul
#define K1_USB_DWC3_GSNPSID                0xc120ul
#define K1_USB_DWC3_GHWPARAMS0             0xc140ul
#define K1_USB_DWC3_GHWPARAMS1             0xc144ul
#define K1_USB_DWC3_GHWPARAMS2             0xc148ul
#define K1_USB_DWC3_GHWPARAMS3             0xc14cul
#define K1_USB_DWC3_GUSB2PHYCFG0          0xc200ul
#define K1_USB_DWC3_GUSB3PIPECTL0         0xc2c0ul
#define K1_USB_DWC3_DCFG                   0xc700ul
#define K1_USB_DWC3_DSTS                   0xc70cul

#define K1_USB_DWC3_GCTL_PRTCAPDIR_MASK    (3ul << 12)
#define K1_USB_DWC3_GCTL_PRTCAP_HOST       (1ul << 12)

/* DWC3 PHY settings required by the MUSE Pi Pro mainline DTS. */

#define K1_USB_DWC3_GUSB2PHYCFG_ULPI_UTMI  (1ul << 4)
#define K1_USB_DWC3_GUSB2PHYCFG_SUSPHY     (1ul << 6)
#define K1_USB_DWC3_GUSB2PHYCFG_ENBLSLPM   (1ul << 8)
#define K1_USB_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK (0xful << 10)
#define K1_USB_DWC3_GUSB2PHYCFG_USBTRDTIM_UTMI8 (9ul << 10)
#define K1_USB_DWC3_GUSB2PHYCFG_PHYIF_MASK (1ul << 3)
#define K1_USB_DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS (1ul << 30)

#define K1_USB_DWC3_GUSB3PIPECTL_DISRXDETINP3 (1ul << 28)
#define K1_USB_DWC3_GUSB3PIPECTL_UX_EXIT_PX (1ul << 27)
#define K1_USB_DWC3_GUSB3PIPECTL_DEPOCHANGE (1ul << 18)
#define K1_USB_DWC3_GUSB3PIPECTL_SUSPHY   (1ul << 17)

/* xHCI capability registers are exposed at the beginning of the DWC3
 * resource, as described by the Linux DWC3 host glue.
 */

#define K1_USB_XHCI_CAPLENGTH             0x00ul
#define K1_USB_XHCI_HCSPARAMS1            0x04ul
#define K1_USB_XHCI_HCCPARAMS1            0x10ul

#define K1_USB_XHCI_USBCMD                0x00ul
#define K1_USB_XHCI_USBSTS                0x04ul
#define K1_USB_XHCI_USBSTS_HCH            (1ul << 0)

/* K1 USB2 PHY registers. */

#define K1_USB2_PHY_RST_MODE_CTRL          0x04ul
#define K1_USB2_PHY_TX_HOST_CTRL           0x10ul
#define K1_USB2_PHY_HSTXP_HW_CTRL          0x34ul
#define K1_USB2_PHY_PLL_DIV_CFG            0x98ul
#define K1_USB2_PHY_PLL_READY              (1ul << 0)
#define K1_USB2_PHY_CLK_CDR_EN             (1ul << 1)
#define K1_USB2_PHY_CLK_PLL_EN             (1ul << 2)
#define K1_USB2_PHY_CLK_MAC_EN             (1ul << 3)
#define K1_USB2_PHY_MAC_RSTN               (1ul << 5)
#define K1_USB2_PHY_CDR_RSTN               (1ul << 6)
#define K1_USB2_PHY_PLL_RSTN               (1ul << 7)
#define K1_USB2_PHY_HS_LINE_TX_MODE        (1ul << 13)
#define K1_USB2_PHY_FS_LINE_TX_MODE        (1ul << 14)
#define K1_USB2_PHY_HSTXP_RSTN             (1ul << 2)
#define K1_USB2_PHY_CLK_HSTXP_EN           (1ul << 3)
#define K1_USB2_PHY_HSTXP_MODE             (1ul << 4)
#define K1_USB2_PHY_HST_DISC_AUTO_CLR     (1ul << 2)
#define K1_USB2_PHY_PLL_DIV_VALUE          0x1ec4ul
#define K1_USB2_PHY_PLL_FREQ_24MHZ         (1ul << 13)
#define K1_USB2_PHY_PLL_DIV_LOCAL_ENABLE   (1ul << 15)

/* K1 combo PHY USB3 mode selection. */

#define K1_USB_COMBO_PHY_TEST_CTRL         0x68ul
#define K1_USB_COMBO_PHY_PLL_CFG           0x08ul
#define K1_USB_COMBO_PHY_PLL_INIT_DONE     (1ul << 11)
#define K1_USB_COMBO_PHY_PLL_READY         (1ul << 0)
#define K1_USB_COMBO_PHY_PLL_TIMER_MASK    (0xful << 7)
#define K1_USB_COMBO_PHY_PLL_TIMER_USB     (0x2ul << 7)
#define K1_USB_COMBO_PHY_PLL_CFG1          0x48ul
#define K1_USB_COMBO_PHY_REF100_WSSC       (1ul << 12)
#define K1_USB_COMBO_PHY_REFSEL_MASK       (0x7ul << 13)
#define K1_USB_COMBO_PHY_REFSEL_24MHZ      (0x1ul << 13)
#define K1_USB_COMBO_PHY_SSC_DEPTH_MASK    (0xful << 16)
#define K1_USB_COMBO_PHY_SSC_DEPTH_5000PPM (0xau << 16)
#define K1_APMU_USB_PHY_CTRL0              (K1_APMU_BASE + 0x110ul)
#define K1_USB_COMBO_PHY_USB3_MODE         (1ul << 3)
#define K1_APMU_PCIE0_GLOBAL_RESET         (1ul << 8)
#define K1_APMU_PCIE0_APP_HOLD_PHY_RESET  (1ul << 30)

#define K1_USB_DWC3_ID_MASK               0xffff0000ul
#define K1_USB_DWC3_ID                     0x55330000ul

#endif /* __CHIP_K1_HARDWARE_K1_USB_H */
