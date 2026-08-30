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
#include <nuttx/kmalloc.h>
#include <nuttx/wqueue.h>

#include "hardware/k1_gpio.h"
#include "k1_bt_uart.h"
#include "k1_rtl8852bs_gpl.h"
#include "k1_rtl8852bs_netdev.h"
#include "k1_sdio.h"
#include "k1_wireless.h"
#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

#define K1_RTL8852BS2_SDIO_LOCAL_HISR 0x1104u
#define K1_WIRELESS_POWER_OFF_DELAY_MSEC 100u

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RX_WORKER_DIAGNOSTIC
#  define K1_WIRELESS_RX_BUFFER_SIZE       12288u
#  define K1_WIRELESS_RX_POLL_MSEC         5u
#  define K1_WIRELESS_RX_READS_PER_TICK    4u
#  define K1_WIRELESS_RX_MAX_POLLS          100u
#  define K1_WIRELESS_RX_TYPE_DATA         0u
#  define K1_WIRELESS_RX_TYPE_C2H          10u

struct k1_wireless_rx_worker_s
{
  struct work_s work;
  FAR uint8_t *buffer;
  uint32_t transfers;
  uint32_t frames;
  uint32_t data_frames;
  uint32_t c2h_frames;
  uint32_t c2h_dispatched;
  uint32_t c2h_dispatch_errors;
  uint32_t other_frames;
  uint32_t dropped_frames;
  uint32_t parse_errors;
  uint16_t last_c2h_content_length;
  uint8_t last_c2h_category;
  uint8_t last_c2h_class;
  uint8_t last_c2h_function;
  uint8_t last_c2h_sequence;
  uint16_t polls;
};
#endif

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

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RX_WORKER_DIAGNOSTIC
static int k1_wireless_runtime_c2h_dispatch(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg)
{
  FAR struct k1_wireless_rx_worker_s *priv = arg;

  if (c2h == NULL || priv == NULL)
    {
      return -EINVAL;
    }

  priv->c2h_dispatched++;
  priv->last_c2h_content_length = c2h->content_length;
  priv->last_c2h_category = c2h->category;
  priv->last_c2h_class = c2h->class_id;
  priv->last_c2h_function = c2h->function;
  priv->last_c2h_sequence = c2h->sequence;
  return OK;
}

static void k1_wireless_runtime_rx_worker(FAR void *arg)
{
  FAR struct k1_wireless_rx_worker_s *priv = arg;
  struct k1_rtl8852bs_rx_frame_s frame;
  size_t length;
  size_t offset;
  unsigned int read_count;
  int ret;

  priv->polls++;

  for (read_count = 0; read_count < K1_WIRELESS_RX_READS_PER_TICK;
       read_count++)
    {
      ret = k1_rtl8852bs_runtime_rx_read(priv->buffer,
                                         K1_WIRELESS_RX_BUFFER_SIZE,
                                         &length);
      if (ret == -EAGAIN)
        {
          break;
        }

      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi: runtime RX read error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts("\r\n");
          break;
        }

      priv->transfers++;
      offset = 0;
      while (offset < length)
        {
          ret = k1_rtl8852bs_runtime_rx_parse(priv->buffer, length, offset,
                                               &frame);
          if (ret < 0 || frame.next_offset <= offset)
            {
              priv->parse_errors++;
              k1_early_puts("K1 Wi-Fi: runtime RX parse error=");
              k1_early_puthex((uintreg_t)(ret < 0 ? -ret : EPROTO));
              k1_early_puts("\r\n");
              break;
            }

          priv->frames++;
          if (frame.crc_error || frame.icv_error)
            {
              priv->dropped_frames++;
            }
          else if (frame.packet_type == K1_WIRELESS_RX_TYPE_DATA)
            {
              priv->data_frames++;
            }
          else if (frame.packet_type == K1_WIRELESS_RX_TYPE_C2H)
            {
              ret = k1_rtl8852bs_runtime_c2h_dispatch(
                priv->buffer + frame.payload_offset, frame.payload_length,
                k1_wireless_runtime_c2h_dispatch, priv);
              if (ret < 0)
                {
                  priv->c2h_dispatch_errors++;
                  k1_early_puts("K1 Wi-Fi: runtime C2H dispatch error=");
                  k1_early_puthex((uintreg_t)-ret);
                  k1_early_puts("\r\n");
                }
              else
                {
                  priv->c2h_frames++;
                }
            }
          else
            {
              priv->other_frames++;
            }

          offset = frame.next_offset;
        }

      k1_early_puts("K1 Wi-Fi: runtime RX transfer=");
      k1_early_puthex(length);
      k1_early_puts(" frames=");
      k1_early_puthex(priv->frames);
      k1_early_puts(" data=");
      k1_early_puthex(priv->data_frames);
      k1_early_puts(" C2H=");
      k1_early_puthex(priv->c2h_frames);
      k1_early_puts(" dispatched=");
      k1_early_puthex(priv->c2h_dispatched);
      k1_early_puts(" dropped=");
      k1_early_puthex(priv->dropped_frames);
      k1_early_puts("\r\n");

      if (priv->c2h_frames != 0)
        {
          k1_early_puts("K1 Wi-Fi: runtime RX worker complete\r\n");
          return;
        }
    }

  if (priv->polls >= K1_WIRELESS_RX_MAX_POLLS)
    {
      k1_early_puts("K1 Wi-Fi: runtime RX worker timeout polls=");
      k1_early_puthex(priv->polls);
      k1_early_puts("\r\n");
      return;
    }

  ret = work_queue(LPWORK, &priv->work, k1_wireless_runtime_rx_worker,
                   priv, MSEC2TICK(K1_WIRELESS_RX_POLL_MSEC));
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi: runtime RX worker schedule error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
    }
}

static int k1_wireless_runtime_rx_worker_start(void)
{
  FAR struct k1_wireless_rx_worker_s *priv;
  int ret;

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->buffer = kmm_zalloc(K1_WIRELESS_RX_BUFFER_SIZE);
  if (priv->buffer == NULL)
    {
      kmm_free(priv);
      return -ENOMEM;
    }

  ret = work_queue(LPWORK, &priv->work, k1_wireless_runtime_rx_worker,
                   priv, 0);
  if (ret < 0)
    {
      kmm_free(priv->buffer);
      kmm_free(priv);
      return ret;
    }

  k1_early_puts("K1 Wi-Fi: runtime RX worker started\r\n");
  return OK;
}
#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_RX_DIAGNOSTIC
static int k1_wireless_runtime_scanofld_rx_diagnostic(void)
{
  return
    k1_rtl8852bs_fwdl_runtime_scanofld_passive_rx_diagnostic();
}
#endif

#ifdef CONFIG_K1_RTL8852BS2_BT
/* H5 setup includes several automatic packet buffers.  Keep both the raw-HCI
 * and Host registrations out of the board late hook's startup stack.  A
 * board-late caller has no valid inherited environment, so kthread_create()
 * cannot be used here; LPWORK creates no child task and is already enabled by
 * the Bluetooth transport Kconfig.
 */

static struct work_s g_k1_wireless_bt_start_work;

static void k1_wireless_bt_start(FAR void *arg)
{
#ifdef CONFIG_K1_BT_H5_LOCAL_VERSION_PROBE
  struct k1_bt_h5_info_s info;
#endif
  int ret;

  UNUSED(arg);

  k1_early_puts("K1 Bluetooth: H5 bring-up work start\r\n");

#ifdef CONFIG_K1_BT_H5_LOCAL_VERSION_PROBE
  ret = k1_bt_uart_initialize(&info);
  if (ret < 0)
    {
      k1_early_puts("K1 Bluetooth: H5 local-version error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return;
    }

  k1_early_puts("K1 Bluetooth: H5 local version HCI=");
  k1_early_puthex(info.hci_version);
  k1_early_puts(" revision=");
  k1_early_puthex(info.hci_revision);
  k1_early_puts(" LMP=");
  k1_early_puthex(info.lmp_version);
  k1_early_puts(" manufacturer=");
  k1_early_puthex(info.manufacturer);
  k1_early_puts(" subversion=");
  k1_early_puthex(info.lmp_subversion);
  k1_early_puts(" CRC=");
  k1_early_puthex(info.crc_enabled ? 1 : 0);
  k1_early_puts("\r\n");
#endif

  ret = k1_bt_uart_register();
  if (ret < 0)
    {
      k1_early_puts("K1 Bluetooth: H5 stack registration error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
    }
  else
    {
      k1_early_puts("K1 Bluetooth: H5 bring-up complete\r\n");
    }

}

static int k1_wireless_bt_start_async(void)
{
  int ret;

  ret = work_queue(LPWORK, &g_k1_wireless_bt_start_work,
                   k1_wireless_bt_start, NULL, 0);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Bluetooth: H5 bring-up work queued\r\n");
  return OK;
}
#endif

#ifdef CONFIG_K1_RTL8852BS2_WIFI
static void k1_wireless_configure_sdio_pins(void)
{
  uint32_t pad = K1_MFPR_MUX_MODE1 | K1_MFPR_EDGE_CLEAR |
                 K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP;

  /* pinctrl_mmc2 in the MUSE Pi Pro DTS applies the same 1.8 V DS2/up pad
   * setting to all four data pins, CMD, and CLK.  The DS3/down CLK setting
   * belongs to the unrelated pinctrl_mmc1_fast group.
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

  /* Linux mainline UART2 pinctrl uses GPIO21--24 in TX/RX/CTS/RTS order. */

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
  uint8_t wifi_local[sizeof(wifi_info.function1_local)];
#  if defined(CONFIG_K1_SDIO_WIFI_CMD53_COMMON_DIAGNOSTIC) || \
      defined(CONFIG_K1_SDIO_WIFI_CMD53_PIO_DIAGNOSTIC)
  uint8_t wifi_cccr[4];
#  endif
#  ifdef CONFIG_K1_SDIO_WIFI_CMD53_HISR_READ_DIAGNOSTIC
  uint8_t wifi_hisr_cmd53[sizeof(wifi_info.function1_local)];
#  endif
#  ifdef CONFIG_K1_RTL8852BS2_FW_RUNTIME_DIAGNOSTIC
  uint8_t wifi_mac[6];
#  endif
  unsigned int byte;
  int probe_ret;
#endif
#ifdef CONFIG_K1_RTL8852BS2_BT
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
  /* Match the vendor RF pwrseq's 100 ms power-on settling window while the
   * controls are all inactive.  This also gives a warm-reset card a bounded
   * power-off interval before SDH1 is prepared again.
   */

  up_mdelay(K1_WIRELESS_POWER_OFF_DELAY_MSEC);

#ifdef CONFIG_K1_RTL8852BS2_WIFI
  /* Linux initializes mmc1 before WLAN power sequence asserts RF_PWR and
   * REG_ON.  Keep that order so an SDH1 reset cannot disturb a powered card.
   */

  probe_ret = k1_sdio_wifi_prepare();
  if (probe_ret < 0)
    {
      ret = probe_ret;
    }
  else
    {
      k1_early_puts("K1 Wi-Fi: SDH1 prepared while reset held\r\n");
    }
#endif

  /* This follows the MUSE Pi Pro rf-pwrseq wiring: shared RF power, Wi-Fi
   * REG_ON, then Bluetooth RESET_N.  GPIO66 is the WLAN wake input and is
   * intentionally not driven by this first implementation.
   */

  k1_wireless_set_output(K1_MFPR_GPIO67, K1_GPIO_BANK2_BASE, 3,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_3V3_DS4 | K1_MFPR_PULL_UP, true);

  /* The vendor rf-pwrseq uses its default 100 ms power-on delay after
   * RF_PWR before the WLAN child asserts REG_ON.  Keep that electrical
   * settling interval even though this implementation does not own DCDC3.
   */

  up_mdelay(100);

#ifdef CONFIG_K1_RTL8852BS2_WIFI
  /* The Linux bt-pwrseq turns RF_PWR on before it releases RESET_N, then
   * waits its default 10 ms.  Linux dmesg confirms this happens before the
   * RTL8852BS2 Wi-Fi module starts SDIO enumeration.
   */

  k1_wireless_set_output(K1_MFPR_GPIO63, K1_GPIO_BANK1_BASE, 31,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, true);
  up_mdelay(10);
#endif

#ifdef CONFIG_K1_RTL8852BS2_WIFI
  k1_wireless_set_output(K1_MFPR_GPIO116, K1_GPIO_BANK3_BASE, 20,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, true);

  /* The wlan-pwrseq child has no power-on-delay-ms property in the MUSE Pi
   * Pro DTS, so its Linux driver uses its 10 ms default after REG_ON.
   */

  up_mdelay(10);

  k1_early_puts("K1 Wi-Fi: RF GPIO level=");
  k1_early_puthex(getreg32(k1_wireless_gpio_reg(K1_GPIO_BANK2_BASE,
                                                 K1_GPIO_GPLR_OFFSET)));
  k1_early_puts(" REG_ON GPIO level=");
  k1_early_puthex(getreg32(k1_wireless_gpio_reg(K1_GPIO_BANK3_BASE,
                                                 K1_GPIO_GPLR_OFFSET)));
  k1_early_puts("\r\n");

  if (probe_ret >= 0)
    {
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
          probe_ret = k1_sdio_wifi_enable_function(1, 512, &wifi_info);
          if (probe_ret < 0)
            {
              ret = probe_ret;
            }
          else
            {
              k1_early_puts("K1 Wi-Fi: F1 IOEN=");
              k1_early_puthex(wifi_info.io_enable);
              k1_early_puts(" IORDY=");
              k1_early_puthex(wifi_info.io_ready);
              k1_early_puts(" CCCR speed=");
              k1_early_puthex(wifi_info.speed_control);
              k1_early_puts(" high-speed=");
              k1_early_puthex(wifi_info.high_speed_enabled);
              k1_early_puts(" UHS=");
              k1_early_puthex(wifi_info.uhs_support);
              k1_early_puts(" SDR104=");
              k1_early_puthex(wifi_info.sdr104_enabled);
              k1_early_puts(" RX delay=");
              k1_early_puthex(wifi_info.rx_delaycode);
              k1_early_puts("\r\n");
#ifdef CONFIG_K1_RTL8852BS2_GPL_BOOTSTRAP
              probe_ret = k1_rtl8852bs_bootstrap();
              if (probe_ret < 0)
                {
                  ret = probe_ret;
                }
              else
                {
#ifdef CONFIG_K1_SDIO_WIFI_FIRST_VENDOR_CMD53_DIAGNOSTIC
                  probe_ret = k1_rtl8852bs_first_cmd53_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
#endif

                  if (probe_ret >= 0)
                    {
                      probe_ret = k1_rtl8852bs_hci_dmac_pre_init();
                      if (probe_ret < 0)
                        {
                          ret = probe_ret;
                        }
                    }
                }

#ifdef CONFIG_K1_RTL8852BS2_DLE_SCC_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_dle_scc_init();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_RTL8852BS2_HCI_FC_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_hci_fc_init();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_sdio_pre_init();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }

#ifdef CONFIG_K1_RTL8852BS2_FW_IMAGE_LAYOUT_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_image_layout_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_RTL8852BS2_FW_MSS_EFUSE_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_mss_efuse_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_RTL8852BS2_FW_MSS_LEGACY_SIGNATURE_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_mss_legacy_signature_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_RTL8852BS2_FW_FULL_DOWNLOAD_DIAGNOSTIC
              if (probe_ret >= 0)
                {
#  ifdef CONFIG_K1_RTL8852BS2_FW_RUNTIME_DIAGNOSTIC
                  probe_ret = k1_rtl8852bs_efuse_read_mac(wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
#  endif
                }

              /* The RF context has to be captured before the firmware
               * download: the eFuse read sequence drives the power-cut and
               * isolation registers, and the host indirect register window
               * stops returning live values once the WCPU owns the chip.
               */

#  ifdef CONFIG_K1_RTL8852BS2_RF_CONTEXT_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_rf_context_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_full_download();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }

#  ifdef CONFIG_K1_RTL8852BS2_FW_RUNTIME_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_runtime_status_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_TRANSPORT_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_transport_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_H2C_LOOPBACK_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_h2c_loopback_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_MAC_CORE_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_mac_core_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_BB_RF_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_bb_rf_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_phy_cr_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

              /* rtw_hal_init_bb_reg() is halbb_init_reg() followed by
               * halbb_reset_bb(), so the BB reset pulse belongs between the
               * PHY CR image and anything that follows it.
               */

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_BB_RESET_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_bb_reset_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

              /* hal_start_8852b() runs init_rf_reg right after
               * init_bb_reg, so the radio A/B image follows the BB PHY CR
               * image.
               */

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_rf_cr_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_CONTROL_PLANE_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_control_plane_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_ADDRESS_CAM_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_runtime_addr_cam_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_ROLE_CAM_DONE_ACK_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_role_cam_done_ack_diagnostic(
                      wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_CHANNEL_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_scanofld_ch_done_ack_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_PASSIVE_DIAGNOSTIC
              if (probe_ret >= 0)
                {
#    ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_RX_DIAGNOSTIC
                  probe_ret =
                    k1_wireless_runtime_scanofld_rx_diagnostic();
#    else
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic();
#    endif
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_WLAN_NETDEV

              /* The boot sweep above has already proved that this firmware
               * receives Beacons on this board, so wlan0 is registered only
               * after it succeeded: the device exists to repeat that same
               * sweep on request, and registering it after a failed sweep
               * would advertise a scan that is known not to work.
               */

              if (probe_ret >= 0)
                {
                  int netdev_ret = k1_rtl8852bs_netdev_register(wifi_mac);

                  if (netdev_ret < 0)
                    {
                      k1_early_puts("K1 Wi-Fi: wlan0 register error=");
                      k1_early_puthex((uintreg_t)-netdev_ret);
                      k1_early_puts("\r\n");
                      ret = netdev_ret;
                    }
                  else
                    {
                      k1_early_puts("K1 Wi-Fi: wlan0 scan device "
                                    "registered\r\n");
                    }
                }

#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_DATA_TX_DIAGNOSTIC
              /* Keep this preflight before the optional LPWORK RX worker so
               * its one TXPG_WP CMD53 snapshot cannot race another SDIO I/O.
               */

              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_runtime_data_tx_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RX_WORKER_DIAGNOSTIC
              /* Queue one C2H response before the worker is scheduled.  The
               * SDIO controller is therefore never used concurrently with
               * board initialization, and the worker proves its own RXFF
               * read/descriptor classification path on real hardware.
               */

              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_runtime_h2c_loopback_submit();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }

              if (probe_ret >= 0)
                {
                  probe_ret = k1_wireless_runtime_rx_worker_start();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_ACTIVE_DIAGNOSTIC

              /* Transmit is attempted last of all, after every step that does
               * not need it has run, wlan0 and the transmit descriptor
               * preflight included.  A radio that cannot transmit yet then
               * reports itself alone: it cannot suppress a receive-side
               * result that was already proven, because there is nothing
               * after it left to gate.
               */

              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_scanofld_active_diagnostic(
                      wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#  endif

#  ifdef CONFIG_K1_RTL8852BS2_RUNTIME_AUTH_DIAGNOSTIC

              /* The directed exchange runs after the broadcast one, because it
               * depends on it: the target it aims at comes from a sweep, and
               * the channel it transmits on is the channel the firmware parks
               * the radio on during that sweep's dwell.  It is also the only
               * step that addresses a single access point, so it runs last and
               * gates nothing after it.
               */

              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_auth_diagnostic(wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }

#    ifdef CONFIG_K1_RTL8852BS2_RUNTIME_JOIN_DIAGNOSTIC

              /* The join runs after the directed exchange rather than before
               * it, so that one run shows both: the exchange against a
               * no-link address CAM first, then the same exchange once the
               * firmware and the address CAM have been told which BSS this
               * host belongs to.  Reversing them would leave no evidence that
               * programming the peer's BSSID changed nothing it should not.
               */

              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_join_diagnostic(wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }

#      ifdef CONFIG_K1_RTL8852BS2_RUNTIME_ASSOC_DIAGNOSTIC

              /* The association runs last, after the join has told the
               * firmware and the address CAM which BSS this host belongs to,
               * because that is the order IEEE 802.11 and the vendor connect
               * path both use.  It re-runs the authentication exchange itself
               * inside each of its own sweeps, since an Association Request is
               * only accepted while the access point still holds this host in
               * state 2 from that answer.
               */

#        ifdef CONFIG_K1_RTL8852BS2_RUNTIME_WPA_DIAGNOSTIC

              /* The handshake replaces the association step rather than
               * following it, because it is the same exchange carried further:
               * the access point sends its first EAPOL-Key frame within
               * milliseconds of granting the association, so the only place it
               * can be answered is the receive loop of the sweep the
               * association happened in.  Running the association step first
               * and the handshake after would spend a whole extra sweep to
               * arrive too late for the frame it exists to answer.
               */

              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_wpa_diagnostic(wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#        else
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_runtime_assoc_diagnostic(wifi_mac);
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#        endif
#      endif
#    endif
#  endif
#endif

#ifdef CONFIG_K1_RTL8852BS2_FW_PREBOOT_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_preboot_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_RTL8852BS2_FW_H2C_TX_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_fwdl_h2c_tx_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#if defined(CONFIG_K1_RTL8852BS2_FW_HEADER_PACKET_DIAGNOSTIC) && \
    !defined(CONFIG_K1_RTL8852BS2_FW_SECTION_PACKET_DIAGNOSTIC)
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_fw_header_packet_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#if defined(CONFIG_K1_RTL8852BS2_FW_SECTION_PACKET_DIAGNOSTIC) && \
    !defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_TAIL_PACKET_DIAGNOSTIC) && \
    !defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_SECOND_PACKET_DIAGNOSTIC) && \
    !defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_THIRD_PACKET_DIAGNOSTIC) && \
    !defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_FOURTH_PACKET_DIAGNOSTIC)
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_fw_section0_packet_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_RTL8852BS2_FW_SECTION0_TAIL_PACKET_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_fw_section0_tail_packet_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#elif defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_FOURTH_PACKET_DIAGNOSTIC)
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_fw_section0_fourth_packet_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#elif defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_THIRD_PACKET_DIAGNOSTIC)
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_fw_section0_third_packet_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#elif defined(CONFIG_K1_RTL8852BS2_FW_SECTION0_SECOND_PACKET_DIAGNOSTIC)
              if (probe_ret >= 0)
                {
                  probe_ret =
                    k1_rtl8852bs_fwdl_fw_section0_second_packet_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_SDIO_WIFI_CMD53_WRITE_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  probe_ret = k1_rtl8852bs_cmd53_write_diagnostic();
                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                }
#endif

#ifdef CONFIG_K1_SDIO_WIFI_CMD53_HISR_READ_DIAGNOSTIC
              if (probe_ret >= 0)
                {
                  /* This is the Function 1 CMD53 request captured from the
                   * vendor RTL8852BS2 driver.  HISR is read-only here, and
                   * the incrementing four-byte request encodes as
                   * 0x14220804.
                   */

                  probe_ret = k1_sdio_wifi_read(
                    1, K1_RTL8852BS2_SDIO_LOCAL_HISR, true,
                    wifi_hisr_cmd53, sizeof(wifi_hisr_cmd53));
                  if (probe_ret < 0)
                    {
                      k1_early_puts("K1 Wi-Fi: F1 HISR CMD53 error=");
                      k1_early_puthex((uintreg_t)-probe_ret);
                      k1_early_puts("\r\n");
                      ret = probe_ret;
                    }
                  else
                    {
                      wifi_info.function1_local =
                        (uint32_t)wifi_hisr_cmd53[0] |
                        ((uint32_t)wifi_hisr_cmd53[1] << 8) |
                        ((uint32_t)wifi_hisr_cmd53[2] << 16) |
                        ((uint32_t)wifi_hisr_cmd53[3] << 24);
                      k1_early_puts("K1 Wi-Fi: F1 HISR CMD53=");
                      k1_early_puthex(wifi_info.function1_local);
                      k1_early_puts("\r\n");
                    }
                }
#endif

              if (probe_ret >= 0)
#endif
                {
                  /* HISR is a Function 1 register within CMD52's 17-bit
                   * address space.  Read it through the proven command-only
                   * path; K1 CMD53/ADMA remains a separate diagnostic.
                   */

                  for (byte = 0; byte < sizeof(wifi_local); byte++)
                    {
                      probe_ret = k1_sdio_wifi_f1_read_byte(
                        K1_RTL8852BS2_SDIO_LOCAL_HISR + byte,
                        &wifi_local[byte]);
                      if (probe_ret < 0)
                        {
                          break;
                        }
                    }

                  if (probe_ret < 0)
                    {
                      ret = probe_ret;
                    }
                  else
                    {
                      wifi_info.function1_local =
                        (uint32_t)wifi_local[0] |
                        ((uint32_t)wifi_local[1] << 8) |
                        ((uint32_t)wifi_local[2] << 16) |
                        ((uint32_t)wifi_local[3] << 24);
                      k1_early_puts("K1 Wi-Fi: F1 HISR CMD52=");
                      k1_early_puthex(wifi_info.function1_local);
                      k1_early_puts("\r\n");
#if defined(CONFIG_K1_SDIO_WIFI_CMD53_COMMON_DIAGNOSTIC) || \
    defined(CONFIG_K1_SDIO_WIFI_CMD53_PIO_DIAGNOSTIC)
                      /* Function 0 CMD53 is read-only and has no Realtek
                       * vendor-register dependency.  It distinguishes a
                       * controller-wide CMD53 data failure from one that is
                       * specific to enabled Function 1.  The PIO profile
                       * additionally clears the host DMA selector.
                       */

                      probe_ret = k1_sdio_wifi_read(0, 0, true, wifi_cccr,
                                                    sizeof(wifi_cccr));
                      if (probe_ret < 0)
                        {
                          k1_early_puts("K1 Wi-Fi: F0 CCCR CMD53 error=");
                          k1_early_puthex((uintreg_t)-probe_ret);
                          k1_early_puts("\r\n");
#ifdef CONFIG_K1_SDIO_WIFI_CMD53_PIO_RETRY_DIAGNOSTIC
                          /* k1_sdio_wifi_read() has already reset the failed
                           * command and data state machines.  Retry exactly
                           * the same read once.  This mirrors MMC request
                           * recovery without broadening this RAM-only test.
                           */

                          probe_ret = k1_sdio_wifi_read(
                            0, 0, true, wifi_cccr, sizeof(wifi_cccr));
                          if (probe_ret < 0)
                            {
                              k1_early_puts(
                                "K1 Wi-Fi: F0 CCCR CMD53 retry error=");
                              k1_early_puthex((uintreg_t)-probe_ret);
                              k1_early_puts("\r\n");
                              ret = probe_ret;
                            }
                          else
                            {
                              k1_early_puts(
                                "K1 Wi-Fi: F0 CCCR CMD53 retry=");
                              k1_early_puthex(
                                (uint32_t)wifi_cccr[0] |
                                ((uint32_t)wifi_cccr[1] << 8) |
                                ((uint32_t)wifi_cccr[2] << 16) |
                                ((uint32_t)wifi_cccr[3] << 24));
                              k1_early_puts("\r\n");
                            }
#else
                          ret = probe_ret;
#endif
                        }
                      else
                        {
                          k1_early_puts("K1 Wi-Fi: F0 CCCR CMD53=");
                          k1_early_puthex((uint32_t)wifi_cccr[0] |
                                          ((uint32_t)wifi_cccr[1] << 8) |
                                          ((uint32_t)wifi_cccr[2] << 16) |
                                          ((uint32_t)wifi_cccr[3] << 24));
                          k1_early_puts("\r\n");
                        }
#endif
                    }
                }
            }
        }
    }
#endif

#ifdef CONFIG_K1_RTL8852BS2_BT
  if (probe_ret >= 0)
    {
#  if !defined(CONFIG_K1_RTL8852BS2_WIFI)
  k1_wireless_set_output(K1_MFPR_GPIO63, K1_GPIO_BANK1_BASE, 31,
                         K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
                         K1_MFPR_DRIVE_1V8_DS2 | K1_MFPR_PULL_UP, true);
  up_mdelay(50);
#  endif

  bt_ret = k1_wireless_bt_start_async();
  if (bt_ret < 0)
    {
      k1_early_puts("K1 Bluetooth: H5 thread creation error=");
      k1_early_puthex((uintreg_t)-bt_ret);
      k1_early_puts("\r\n");
      if (ret >= 0)
        {
          ret = bt_ret;
        }
    }
#endif
    }

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
