/****************************************************************************
 * vendor/spacemit/chips/k1/k1_usb.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_USB_PROBE) || defined(CONFIG_K1_USB_HOST_GLUE)

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_usb.h"
#include "k1_usb.h"
#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void k1_usb_logreg(FAR const char *name, uintptr_t address)
{
  k1_early_puts(name);
  k1_early_puthex(getreg32(address));
  k1_early_puts("\r\n");
}

static bool k1_usb_wait_set(uintptr_t address, uint32_t mask,
                            unsigned int timeout)
{
  while ((getreg32(address) & mask) == 0)
    {
      if (timeout-- == 0)
        {
          return false;
        }

      up_udelay(500);
    }

  return true;
}

/* The USB3 lane is provided by the PCIe0 combo PHY.  Linux selects USB3
 * mode, releases the combo PHY global reset, and starts its private PLL
 * before DWC3 host mode is initialized.  U-Boot's PCIe probe can leave the
 * lane in PCIe mode, so the NuttX handoff must repeat this selection.
 */

static int k1_usb_combo_phy_initialize(void)
{
  uintptr_t phy = K1_USB_COMBO_PHY_BASE;
  uint32_t value;

  k1_early_puts("K1 USB: combo PHY release\r\n");
  modifyreg32(K1_APMU_PCIE0_CLK_RST,
              K1_APMU_PCIE0_GLOBAL_RESET |
              K1_APMU_PCIE0_APP_HOLD_PHY_RESET, 0);
  modifyreg32(K1_APMU_USB_PHY_CTRL0, 0, K1_USB_COMBO_PHY_USB3_MODE);
  putreg32(0, phy + K1_USB_COMBO_PHY_TEST_CTRL);

  value = getreg32(phy + K1_USB_COMBO_PHY_PLL_CFG);
  value &= ~K1_USB_COMBO_PHY_PLL_TIMER_MASK;
  value |= K1_USB_COMBO_PHY_PLL_TIMER_USB;
  putreg32(value, phy + K1_USB_COMBO_PHY_PLL_CFG);

  value = getreg32(phy + K1_USB_COMBO_PHY_PLL_CFG1);
  value &= ~(K1_USB_COMBO_PHY_REF100_WSSC |
             K1_USB_COMBO_PHY_REFSEL_MASK |
             K1_USB_COMBO_PHY_SSC_DEPTH_MASK);
  value |= K1_USB_COMBO_PHY_REFSEL_24MHZ |
           K1_USB_COMBO_PHY_SSC_DEPTH_5000PPM;
  putreg32(value, phy + K1_USB_COMBO_PHY_PLL_CFG1);

  value = getreg32(phy + K1_USB_COMBO_PHY_PLL_CFG);
  value |= K1_USB_COMBO_PHY_PLL_INIT_DONE;
  putreg32(value, phy + K1_USB_COMBO_PHY_PLL_CFG);

  if (!k1_usb_wait_set(phy + K1_USB_COMBO_PHY_PLL_CFG,
                       K1_USB_COMBO_PHY_PLL_READY, 1000))
    {
      k1_early_puts("K1 USB: combo PHY PLL timeout\r\n");
      return -ETIMEDOUT;
    }

  k1_early_puts("K1 USB: combo PHY USB3 PLL ready\r\n");
  return OK;
}

static void k1_usb_dwc3_phy_setup(void)
{
  uintptr_t base = K1_USB_DWC3_BASE;
  uint32_t value;

  /* Match the K1 DTS: UTMI 8-bit, no low-power or suspend PHY gating, no
   * fabricated USB2 free clock, and the K1 USB3 RX-detect workaround.
   */

  value = getreg32(base + K1_USB_DWC3_GUSB2PHYCFG0);
  value &= ~(K1_USB_DWC3_GUSB2PHYCFG_ULPI_UTMI |
             K1_USB_DWC3_GUSB2PHYCFG_SUSPHY |
             K1_USB_DWC3_GUSB2PHYCFG_ENBLSLPM |
             K1_USB_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK |
             K1_USB_DWC3_GUSB2PHYCFG_PHYIF_MASK |
             K1_USB_DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS);
  value |= K1_USB_DWC3_GUSB2PHYCFG_USBTRDTIM_UTMI8;
  putreg32(value, base + K1_USB_DWC3_GUSB2PHYCFG0);

  value = getreg32(base + K1_USB_DWC3_GUSB3PIPECTL0);
  value &= ~(K1_USB_DWC3_GUSB3PIPECTL_UX_EXIT_PX |
             K1_USB_DWC3_GUSB3PIPECTL_DEPOCHANGE |
             K1_USB_DWC3_GUSB3PIPECTL_SUSPHY);
  value |= K1_USB_DWC3_GUSB3PIPECTL_DISRXDETINP3;
  putreg32(value, base + K1_USB_DWC3_GUSB3PIPECTL0);

  k1_usb_logreg("K1 USB: host USB2PHYCFG=", base +
                K1_USB_DWC3_GUSB2PHYCFG0);
  k1_usb_logreg("K1 USB: host USB3PIPECTL=", base +
                K1_USB_DWC3_GUSB3PIPECTL0);
}

/* The K1 interconnect faults accesses to the DWC3 window while USB30 is
 * clock-gated.  Complete the clock, reset, and USB2 PHY sequence before any
 * DWC3 register read, including an otherwise read-only probe.
 */

static void k1_usb_stop_inherited_xhci(void)
{
  uintptr_t base = K1_USB_DWC3_BASE;
  uint32_t caplength;
  unsigned int i;

  /* The xHCI operational registers follow the capability length.  Linux may
   * leave the controller running across reboot, so stop it before the delay
   * below gives the inherited DMA engine time to touch stale ring memory.
   */

  k1_early_puts("K1 USB: stop inherited xHCI\r\n");
  caplength = getreg32(base + K1_USB_XHCI_CAPLENGTH) & 0xffu;
  if (caplength < 0x20u || caplength >= 0x1000u || (caplength & 3u) != 0)
    {
      k1_early_puts("K1 USB: invalid xHCI caplength\r\n");
      return;
    }

  putreg32(0, base + caplength + K1_USB_XHCI_USBCMD);
  for (i = 0; i < 10; i++)
    {
      if ((getreg32(base + caplength + K1_USB_XHCI_USBSTS) &
           K1_USB_XHCI_USBSTS_HCH) != 0)
        {
          k1_early_puts("K1 USB: inherited xHCI halted\r\n");
          return;
        }

      up_udelay(100);
    }

  k1_early_puts("K1 USB: inherited xHCI halt pending\r\n");
}

static int k1_usb_prepare(bool stop_controller)
{
  uintptr_t phy = K1_USB2_PHY_BASE;
  uint32_t value;

  /* These markers intentionally surround every first-touch MMIO operation.
   * The K1 can fault USB accesses before its APMU clock and reset sequence
   * has completed, so early polling-UART output is the only reliable way to
   * identify the unsafe access during board bring-up.
   */

  k1_early_puts("K1 USB: prepare APMU\r\n");
  modifyreg32(K1_APMU_USB_CLK_RST, 0, K1_APMU_USB30_CLOCK_ENABLE);

  if (stop_controller)
    {
      /* Linux describes these three fields as active-low resets.  A
       * release-only write leaves an inherited xHCI instance alive across
       * the NuttX handoff, so assert the complete USB30 block first.
       */

      k1_early_puts("K1 USB: assert USB30 reset\r\n");
      modifyreg32(K1_APMU_USB_CLK_RST, K1_APMU_USB30_RESET_RELEASE, 0);
      up_mdelay(2);
      k1_early_puts("K1 USB: release USB30 reset\r\n");
      modifyreg32(K1_APMU_USB_CLK_RST, 0,
                  K1_APMU_USB30_RESET_RELEASE);
      up_mdelay(2);
    }
  else
    {
      /* Probe is intentionally read-only with respect to the controller
       * reset state.  It only makes the block accessible for register reads.
       */

      modifyreg32(K1_APMU_USB_CLK_RST, 0,
                  K1_APMU_USB30_RESET_RELEASE);
    }

  k1_early_puts("K1 USB: prepare clock/reset done\r\n");

  if (stop_controller)
    {
      k1_usb_stop_inherited_xhci();
    }

  up_mdelay(2);

  if (stop_controller)
    {
      int ret = k1_usb_combo_phy_initialize();
      if (ret < 0)
        {
          return ret;
        }
    }

  value = K1_USB2_PHY_PLL_DIV_VALUE |
          K1_USB2_PHY_PLL_FREQ_24MHZ |
          K1_USB2_PHY_PLL_DIV_LOCAL_ENABLE;
  k1_early_puts("K1 USB: prepare PLL config\r\n");
  putreg32(value, phy + K1_USB2_PHY_PLL_DIV_CFG);
  k1_early_puts("K1 USB: prepare PLL wait\r\n");
  if (!k1_usb_wait_set(phy + K1_USB2_PHY_RST_MODE_CTRL,
                       K1_USB2_PHY_PLL_READY, 100))
    {
      k1_early_puts("K1 USB: USB2 PHY PLL timeout\r\n");
      return -ETIMEDOUT;
    }

  k1_early_puts("K1 USB: prepare PHY release\r\n");
  value = K1_USB2_PHY_HS_LINE_TX_MODE |
          K1_USB2_PHY_FS_LINE_TX_MODE |
          K1_USB2_PHY_CLK_PLL_EN |
          K1_USB2_PHY_CLK_CDR_EN |
          K1_USB2_PHY_CLK_MAC_EN |
          K1_USB2_PHY_PLL_RSTN |
          K1_USB2_PHY_CDR_RSTN |
          K1_USB2_PHY_MAC_RSTN;
  putreg32(value, phy + K1_USB2_PHY_RST_MODE_CTRL);
  k1_early_puts("K1 USB: prepare HSTXP\r\n");
  putreg32(K1_USB2_PHY_HSTXP_RSTN |
           K1_USB2_PHY_CLK_HSTXP_EN |
           K1_USB2_PHY_HSTXP_MODE,
           phy + K1_USB2_PHY_HSTXP_HW_CTRL);
  k1_early_puts("K1 USB: prepare host-disconnect\r\n");
  modifyreg32(phy + K1_USB2_PHY_TX_HOST_CTRL, 0,
              K1_USB2_PHY_HST_DISC_AUTO_CLR);
  k1_early_puts("K1 USB: prepare complete\r\n");
  return OK;
}

static void k1_usb_gpio_output(uintptr_t mfpr, uintptr_t bank,
                               uint32_t mask, uint32_t pad, bool value)
{
  uintptr_t setclear = bank +
    (value ? K1_GPIO_GPSR_OFFSET : K1_GPIO_GPCR_OFFSET);

  /* Program the output latch before its direction to avoid a transient on
   * VBUS, hub power, or reset when software takes the pin from U-Boot.
   */

  putreg32(pad, mfpr);
  putreg32(mask, setclear);
  putreg32(mask, bank + K1_GPIO_GSDR_OFFSET);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_usb_probe(void)
{
  uintptr_t base = K1_USB_DWC3_BASE;
  uint32_t id;
  uint32_t caplength;
  uint32_t hcsparams1;
  int ret;

  ret = k1_usb_prepare(false);
  if (ret < 0)
    {
      return ret;
    }

  id = getreg32(base + K1_USB_DWC3_GSNPSID);
  if (id == 0 || id == UINT32_MAX)
    {
      k1_early_puts("K1 USB: DWC3 unavailable after power-up\r\n");
      return -ENODEV;
    }

  k1_early_puts("K1 USB: DWC3 GSNPSID=");
  k1_early_puthex(id);
  k1_early_puts("\r\n");
  k1_usb_logreg("K1 USB: GCTL=", base + K1_USB_DWC3_GCTL);
  k1_usb_logreg("K1 USB: GSTS=", base + K1_USB_DWC3_GSTS);
  k1_usb_logreg("K1 USB: GHWPARAMS0=",
                base + K1_USB_DWC3_GHWPARAMS0);
  k1_usb_logreg("K1 USB: GHWPARAMS1=",
                base + K1_USB_DWC3_GHWPARAMS1);
  k1_usb_logreg("K1 USB: GHWPARAMS2=",
                base + K1_USB_DWC3_GHWPARAMS2);
  k1_usb_logreg("K1 USB: GHWPARAMS3=",
                base + K1_USB_DWC3_GHWPARAMS3);
  k1_usb_logreg("K1 USB: USB2PHYCFG=", base + K1_USB_DWC3_GUSB2PHYCFG0);
  k1_usb_logreg("K1 USB: USB3PIPECTL=",
                base + K1_USB_DWC3_GUSB3PIPECTL0);
  k1_usb_logreg("K1 USB: DCFG=", base + K1_USB_DWC3_DCFG);
  k1_usb_logreg("K1 USB: DSTS=", base + K1_USB_DWC3_DSTS);
  k1_usb_logreg("K1 USB: USB2 PHY REG0=", K1_USB2_PHY_BASE);
  k1_usb_logreg("K1 USB: combo PHY REG0=", K1_USB_COMBO_PHY_BASE);

  caplength = getreg32(base + K1_USB_XHCI_CAPLENGTH) & 0xffu;
  hcsparams1 = getreg32(base + K1_USB_XHCI_HCSPARAMS1);
  k1_early_puts("K1 USB: xHCI caplen=");
  k1_early_puthex(caplength);
  k1_early_puts(" hcsparams1=");
  k1_early_puthex(hcsparams1);
  k1_early_puts("\r\n");

  if ((id & K1_USB_DWC3_ID_MASK) != K1_USB_DWC3_ID)
    {
      k1_early_puts("K1 USB: unexpected DWC3 signature\r\n");
      return -ENODEV;
    }

  return OK;
}

int k1_usb_host_glue_initialize(void)
{
  uintptr_t base = K1_USB_DWC3_BASE;
  uint32_t id;
  uint32_t value;
  uint32_t caplength;
  uint32_t hcsparams1;
  uint32_t hccparams1;
  int ret;

  ret = k1_usb_prepare(true);
  if (ret < 0)
    {
      return ret;
    }

  id = getreg32(base + K1_USB_DWC3_GSNPSID);
  if ((id & K1_USB_DWC3_ID_MASK) != K1_USB_DWC3_ID)
    {
      k1_early_puts("K1 USB host: invalid DWC3 signature\r\n");
      return -ENODEV;
    }

  /* Select host role but leave xHCI stopped.  Rings and event handling are
   * intentionally not enabled until the non-PCI xHCI adapter is available.
   */

  value = getreg32(base + K1_USB_DWC3_GCTL);
  value &= ~K1_USB_DWC3_GCTL_PRTCAPDIR_MASK;
  value |= K1_USB_DWC3_GCTL_PRTCAP_HOST;
  putreg32(value, base + K1_USB_DWC3_GCTL);

  k1_usb_dwc3_phy_setup();

  /* Selecting host mode can restart an inherited xHCI instance.  Stop it
   * again after the role transition and before the shared HCD touches it.
   */

  k1_usb_stop_inherited_xhci();

  caplength = getreg32(base + K1_USB_XHCI_CAPLENGTH) & 0xffu;
  hcsparams1 = getreg32(base + K1_USB_XHCI_HCSPARAMS1);
  hccparams1 = getreg32(base + K1_USB_XHCI_HCCPARAMS1);
  k1_early_puts("K1 USB host: glue ready, xHCI caplen=");
  k1_early_puthex(caplength);
  k1_early_puts(" hcsparams1=");
  k1_early_puthex(hcsparams1);
  k1_early_puts(" hccparams1=");
  k1_early_puthex(hccparams1);
  k1_early_puts(" (controller stopped)\r\n");
  k1_early_puts("K1 USB host: GPIO79/127/123 power path prepared\r\n");

  return OK;
}

int k1_usb_host_power_initialize(void)
{
  /* Match the K1 onboard-hub driver: initialize all GPIOs inactive, enable
   * the ordered hub GPIO array (GPIO127 then GPIO123), wait for the MUSE Pi
   * Pro vbus_delay_ms=200 setting, then enable GPIO79 VBUS.  GPIO123 is the
   * USB2817 active-low reset, so its active-high hub-array value deasserts
   * reset after GPIO127 has enabled the hub supply.
   */

  k1_usb_gpio_output(K1_MFPR_GPIO79, K1_GPIO_BANK2_BASE, 1ul << 15,
                     K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                     K1_MFPR_DRIVE_3V3_DS4, false);
  k1_usb_gpio_output(K1_MFPR_GPIO127, K1_GPIO_BANK3_BASE, 1ul << 31,
                     K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                     K1_MFPR_DRIVE_1V8_DS2, false);
  k1_usb_gpio_output(K1_MFPR_GPIO123, K1_GPIO_BANK3_BASE, 1ul << 27,
                     K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                     K1_MFPR_DRIVE_1V8_DS2, false);
  k1_usb_gpio_output(K1_MFPR_GPIO127, K1_GPIO_BANK3_BASE, 1ul << 31,
                     K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                     K1_MFPR_DRIVE_1V8_DS2, true);
  k1_usb_gpio_output(K1_MFPR_GPIO123, K1_GPIO_BANK3_BASE, 1ul << 27,
                     K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                     K1_MFPR_DRIVE_1V8_DS2, true);
  up_mdelay(200);
  k1_usb_gpio_output(K1_MFPR_GPIO79, K1_GPIO_BANK2_BASE, 1ul << 15,
                     K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                     K1_MFPR_DRIVE_3V3_DS4, true);

  k1_early_puts("K1 USB host: VBUS and USB2817 hub enabled\r\n");
  return OK;
}

uintptr_t k1_usb_dwc3_base(void)
{
  return K1_USB_DWC3_BASE;
}

#endif /* CONFIG_K1_USB_PROBE || CONFIG_K1_USB_HOST_GLUE */
