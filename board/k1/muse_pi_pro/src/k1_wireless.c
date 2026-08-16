/****************************************************************************
 * vendor/spacemit/boards/k1/muse_pi_pro/src/k1_wireless.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_RTL8852BS2_WIFI) || defined(CONFIG_K1_RTL8852BS2_BT)

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>

#include "hardware/k1_gpio.h"
#include "k1_bt_uart.h"
#include "k1_sdio.h"
#include "k1_wireless.h"
#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uintptr_t k1_wireless_gpio_reg(uintptr_t bank,
                                             uintptr_t offset)
{
  return bank + offset;
}

static void k1_wireless_set_output(uintptr_t mfpr, uintptr_t bank,
                                   unsigned int bit, uint32_t pad,
                                   bool value)
{
  uint32_t mask = 1ul << bit;

  putreg32(pad, mfpr);

  /* Program the value before changing direction so reset and power lines do
   * not produce an unintended high pulse.
   */

  putreg32(mask, k1_wireless_gpio_reg(bank,
                                      value ? K1_GPIO_GPSR_OFFSET :
                                              K1_GPIO_GPCR_OFFSET));
  putreg32(mask, k1_wireless_gpio_reg(bank, K1_GPIO_GSDR_OFFSET));
}

static void k1_wireless_set_input(uintptr_t mfpr, uintptr_t bank,
                                  unsigned int bit, uint32_t pad)
{
  uint32_t mask = 1ul << bit;

  putreg32(pad, mfpr);
  putreg32(mask, k1_wireless_gpio_reg(bank, K1_GPIO_GCDR_OFFSET));
}

#ifdef CONFIG_K1_RTL8852BS2_WIFI
static void k1_wireless_configure_sdio_pins(void)
{
  uint32_t pad = K1_MFPR_MUX_MODE1 | K1_MFPR_EDGE_CLEAR |
                 K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP;

  /* Linux describes GPIO15..20 as SDH1 data[3:0], CMD, and CLK.  The same
   * 1.8 V mux setting is used for all six pins by the board pinctrl group.
   */

  putreg32(pad, K1_MFPR_GPIO15);
  putreg32(pad, K1_MFPR_GPIO16);
  putreg32(pad, K1_MFPR_GPIO17);
  putreg32(pad, K1_MFPR_GPIO18);
  putreg32(pad, K1_MFPR_GPIO19);
  putreg32(pad, K1_MFPR_GPIO20);
}
#endif

#ifdef CONFIG_K1_RTL8852BS2_BT
static void k1_wireless_configure_uart_pins(void)
{
  uint32_t pad = K1_MFPR_MUX_MODE1 | K1_MFPR_EDGE_CLEAR |
                 K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP;

  /* UART2 TXD, RXD, CTS_N, RTS_N. */

  putreg32(pad, K1_MFPR_GPIO21);
  putreg32(pad, K1_MFPR_GPIO22);
  putreg32(pad, K1_MFPR_GPIO23);
  putreg32(pad, K1_MFPR_GPIO24);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_wireless_initialize(void)
{
#ifdef CONFIG_K1_RTL8852BS2_WIFI
  struct k1_sdio_wifi_info_s wifi_info;
  int probe_ret;
#endif
#ifdef CONFIG_K1_RTL8852BS2_BT
  struct k1_bt_h5_info_s bt_info;
  int bt_ret;
#endif
  int ret = OK;

  /* Both the pinmux and the RF control lines are owned by Linux before the
   * RAM-only NuttX handoff.  Establish a known disabled state first.
   */

  modifyreg32(K1_APBC_GPIO_CLK_RST, K1_CLK_RESET,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE);
  modifyreg32(K1_APBC_AIB_CLK_RST, K1_CLK_RESET,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE);

#ifdef CONFIG_K1_RTL8852BS2_WIFI
  k1_wireless_configure_sdio_pins();
#endif
#ifdef CONFIG_K1_RTL8852BS2_BT
  k1_wireless_configure_uart_pins();
#endif

  k1_wireless_set_output(K1_MFPR_GPIO63, K1_GPIO_BANK1_BASE, 31,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, false);
  k1_wireless_set_input(K1_MFPR_GPIO66, K1_GPIO_BANK2_BASE, 2,
                        K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_FALL |
                        K1_MFPR_DRIVE_3V3_DS2 | K1_MFPR_PULL_DOWN);
  k1_wireless_set_output(K1_MFPR_GPIO67, K1_GPIO_BANK2_BASE, 3,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_3V3_DS4 | K1_MFPR_PULL_UP, false);
  k1_wireless_set_output(K1_MFPR_GPIO116, K1_GPIO_BANK3_BASE, 20,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, false);
  up_mdelay(10);

  /* This follows the MUSE Pi Pro rf-pwrseq wiring: shared RF power, Wi-Fi
   * REG_ON, then Bluetooth RESET_N.  GPIO66 is the WLAN wake input and is
   * intentionally not driven by this first implementation.
   */

  k1_wireless_set_output(K1_MFPR_GPIO67, K1_GPIO_BANK2_BASE, 3,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_3V3_DS4 | K1_MFPR_PULL_UP, true);
  up_mdelay(10);

#ifdef CONFIG_K1_RTL8852BS2_WIFI
  k1_wireless_set_output(K1_MFPR_GPIO116, K1_GPIO_BANK3_BASE, 20,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, true);
  up_mdelay(30);

  probe_ret = k1_sdio_wifi_probe(&wifi_info);
  if (probe_ret < 0)
    {
      ret = probe_ret;
    }
  else
    {
      k1_early_puts("K1 Wi-Fi: CMD5 OCR=");
      k1_early_puthex(wifi_info.ocr);
      k1_early_puts(" CCCR=");
      k1_early_puthex(wifi_info.cccr_revision);
      k1_early_puts(" SD revision=");
      k1_early_puthex(wifi_info.sd_spec_revision);
      k1_early_puts(" functions=");
      k1_early_puthex(wifi_info.function_count);
      k1_early_puts(" F1 interface=");
      k1_early_puthex(wifi_info.function_interface[0]);
      k1_early_puts("\r\n");
    }
#endif

#ifdef CONFIG_K1_RTL8852BS2_BT
  k1_wireless_set_output(K1_MFPR_GPIO63, K1_GPIO_BANK1_BASE, 31,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, true);
  up_mdelay(50);

  bt_ret = k1_bt_uart_initialize(&bt_info);
  if (bt_ret < 0)
    {
      if (ret >= 0)
        {
          ret = bt_ret;
        }
    }
  else
    {
      k1_early_puts("K1 Bluetooth: H5 local version HCI=");
      k1_early_puthex(bt_info.hci_version);
      k1_early_puts(" revision=");
      k1_early_puthex(bt_info.hci_revision);
      k1_early_puts(" LMP=");
      k1_early_puthex(bt_info.lmp_version);
      k1_early_puts(" manufacturer=");
      k1_early_puthex(bt_info.manufacturer);
      k1_early_puts(" subversion=");
      k1_early_puthex(bt_info.lmp_subversion);
      k1_early_puts(" CRC=");
      k1_early_puthex(bt_info.crc_enabled ? 1 : 0);
      k1_early_puts("\r\n");
    }
#endif

  /* SDIO card enumeration only selects the card, switches to the board's
   * four-bit bus, and reads common/function registers.  It never enables an
   * I/O function or accesses RTL8852BS2 vendor registers.
   */

#ifdef CONFIG_K1_RTL8852BS2_WIFI
  (void)wifi_info;
#endif
  return ret;
}

#endif /* CONFIG_K1_RTL8852BS2_WIFI || CONFIG_K1_RTL8852BS2_BT */
