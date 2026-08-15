/****************************************************************************
 * board/k1/muse_pi_pro/src/k1_boot.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/board.h>
#include <nuttx/fs/fs.h>
#if defined(CONFIG_K1_I2C2) && defined(CONFIG_I2C_DRIVER)
#  include <nuttx/i2c/i2c_master.h>
#endif
#if defined(CONFIG_K1_SPI3) && defined(CONFIG_SPI_DRIVER)
#  include <nuttx/spi/spi_transfer.h>
#endif
#if defined(CONFIG_K1_PWM11) && defined(CONFIG_PWM)
#  include <nuttx/timers/pwm.h>
#endif
#if defined(CONFIG_K1_WATCHDOG) && defined(CONFIG_WATCHDOG)
#  include <nuttx/timers/watchdog.h>
#endif
#if defined(CONFIG_K1_SDIO) && defined(CONFIG_MMCSD) && \
    defined(CONFIG_MMCSD_SDIO)
#  include <nuttx/mmcsd.h>
#  include <nuttx/sdio.h>
#endif
#include <syslog.h>

#include <arch/board/board.h>

#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

#if defined(CONFIG_K1_I2C2) && defined(CONFIG_I2C_DRIVER)
#  include "k1_i2c.h"
#endif
#if defined(CONFIG_K1_SPI3) && defined(CONFIG_SPI_DRIVER)
#  include "k1_spi.h"
#endif
#if defined(CONFIG_K1_PWM11) && defined(CONFIG_PWM)
#  include "k1_pwm.h"
#endif
#if defined(CONFIG_K1_WATCHDOG) && defined(CONFIG_WATCHDOG)
#  include "k1_wdt.h"
#endif
#if defined(CONFIG_K1_SDIO) && defined(CONFIG_MMCSD) && \
    defined(CONFIG_MMCSD_SDIO)
#  include "k1_sdio.h"
#endif
#if defined(CONFIG_K1_RTL8852BS2_WIFI) || defined(CONFIG_K1_RTL8852BS2_BT)
#  include "k1_wireless.h"
#endif
#if defined(CONFIG_K1_FB)
#  include "k1_fb.h"
#endif
#if defined(CONFIG_K1_USBDEV)
#  include "k1_usbdev.h"
#endif
#if defined(CONFIG_K1_USBDEV) && defined(CONFIG_CDCACM) && \
    !defined(CONFIG_CDCACM_COMPOSITE)
#  include <nuttx/usb/cdcacm.h>
#endif
#if defined(CONFIG_K1_USB_PROBE)
#  include "k1_usb.h"
#endif
#if defined(CONFIG_K1_USB_HOST_GLUE)
#  include "k1_usb.h"
#endif
#if defined(CONFIG_USBHOST_XHCI_K1)
#  include <nuttx/usb/xhci_k1.h>
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#if defined(CONFIG_K1_USB_HOST_GLUE)
static void k1_muse_pi_pro_usbhost_initialize(void)
{
  int ret;

  k1_early_puts("K1 USB host: board late entry\r\n");

  ret = k1_usb_host_glue_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: K1 USB host glue failed: %d\n", ret);
      return;
    }

#if defined(CONFIG_USBHOST_XHCI_K1)
#  if defined(CONFIG_K1_USB_HOST_POWER)
  ret = k1_usb_host_power_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: K1 USB host power setup failed: %d\n", ret);
      return;
    }
#  endif

  ret = k1_xhci_initialize(k1_usb_dwc3_base(),
                           RISCV_IRQ_EXT + CONFIG_K1_USB_IRQ);
  if (ret < 0)
    {
      /* Keep the first xHCI failure on the inherited polling console.  This
       * path is also valid before a waiter thread or a USB class driver is
       * available, and it preserves the return value for board diagnosis.
       */

      k1_early_puts("K1 USB host: xHCI initialization failed ret=");
      k1_early_puthex((uintreg_t)ret);
      k1_early_puts("\r\n");
    }
  else
    {
      k1_early_puts("K1 USB host: xHCI started\r\n");
    }
#endif
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k1_muse_pi_pro_board_initialize(void)
{
  /* Keep early board initialization deliberately empty.
   *
   * U-Boot already configured DRAM and the serial console. Reprogramming the
   * K1 UART before a safe lower-half driver exists can hang the APB bus.
   */
}

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
void board_early_initialize(void)
{
  k1_muse_pi_pro_board_initialize();
}
#endif

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
#if defined(CONFIG_FS_PROCFS) || \
    (defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)) || \
    (defined(CONFIG_K1_I2C2) && defined(CONFIG_I2C_DRIVER)) || \
    (defined(CONFIG_K1_SPI3) && defined(CONFIG_SPI_DRIVER)) || \
    (defined(CONFIG_K1_PWM11) && defined(CONFIG_PWM)) || \
    (defined(CONFIG_K1_WATCHDOG) && defined(CONFIG_WATCHDOG)) || \
    (defined(CONFIG_K1_SDIO) && defined(CONFIG_MMCSD) && \
     defined(CONFIG_MMCSD_SDIO)) || defined(CONFIG_K1_FB) || \
    defined(CONFIG_K1_RTL8852BS2_WIFI) || \
    defined(CONFIG_K1_RTL8852BS2_BT) || \
    defined(CONFIG_K1_USB_PROBE) || defined(CONFIG_K1_USBDEV)
  int ret;
#endif
#if defined(CONFIG_K1_I2C2) && defined(CONFIG_I2C_DRIVER)
  FAR struct i2c_master_s *i2c;
#endif
#if defined(CONFIG_K1_SPI3) && defined(CONFIG_SPI_DRIVER)
  FAR struct spi_dev_s *spi;
#endif
#if defined(CONFIG_K1_PWM11) && defined(CONFIG_PWM)
  FAR struct pwm_lowerhalf_s *pwm;
#endif
#if defined(CONFIG_K1_SDIO) && defined(CONFIG_MMCSD) && \
    defined(CONFIG_MMCSD_SDIO)
  FAR struct sdio_dev_s *sdio;
#endif

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#if defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)
  ret = k1_gpio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: k1_gpio_initialize failed: %d\n", ret);
    }
#endif

#if defined(CONFIG_K1_RTL8852BS2_WIFI) || defined(CONFIG_K1_RTL8852BS2_BT)
  ret = k1_wireless_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: K1 RTL8852BS2 bring-up failed: %d\n", ret);
    }
#endif

#if defined(CONFIG_K1_I2C2) && defined(CONFIG_I2C_DRIVER)
  i2c = k1_i2cbus_initialize(2);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize K1 I2C2\n");
    }
  else
    {
      ret = i2c_register(i2c, 2);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to register /dev/i2c2: %d\n",
                 ret);
        }
    }
#endif

#if defined(CONFIG_K1_SPI3) && defined(CONFIG_SPI_DRIVER)
  spi = k1_spibus_initialize(3);
  if (spi == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize K1 SPI3\n");
    }
  else
    {
      ret = spi_register(spi, 3);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to register /dev/spi3: %d\n",
                 ret);
        }
    }
#endif

#if defined(CONFIG_K1_PWM11) && defined(CONFIG_PWM)
  pwm = k1_pwminitialize(11);
  if (pwm == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize K1 PWM11\n");
    }
  else
    {
      ret = pwm_register("/dev/pwm11", pwm);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to register /dev/pwm11: %d\n",
                 ret);
        }
    }
#endif

#if defined(CONFIG_K1_WATCHDOG) && defined(CONFIG_WATCHDOG)
  ret = k1_wdt_initialize(CONFIG_WATCHDOG_DEVPATH);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to register K1 watchdog: %d\n", ret);
    }
#endif

#if defined(CONFIG_K1_SDIO) && defined(CONFIG_MMCSD) && \
    defined(CONFIG_MMCSD_SDIO)
  k1_early_puts("K1 eMMC: sdio_initialize start\r\n");
  sdio = sdio_initialize(0);
  k1_early_puts("K1 eMMC: sdio_initialize done\r\n");
  if (sdio == NULL)
    {
      k1_early_puts("K1 eMMC: sdio_initialize NULL\r\n");
    }
  else
    {
      k1_early_puts("K1 eMMC: mmcsd_slotinitialize start\r\n");
      ret = mmcsd_slotinitialize(0, sdio);
      k1_early_puts("K1 eMMC: mmcsd_slotinitialize done\r\n");
      k1_early_puts("K1 eMMC: mmcsd_slotinitialize ret=");
      k1_early_puthex((uintreg_t)(int64_t)ret);
      k1_early_puts("\r\n");
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to register /dev/mmcsd0: %d\n",
                 ret);
        }
    }
#endif

#if defined(CONFIG_K1_FB)
  ret = k1_fb_initialize(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to register K1 /dev/fb0: %d\n", ret);
    }
  else
    {
      k1_early_puts("K1 display: inherited /dev/fb0 registered\r\n");
    }
#endif

#if defined(CONFIG_K1_USB_PROBE)
  ret = k1_usb_probe();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: K1 USB probe failed: %d\n", ret);
    }
#endif

#if defined(CONFIG_K1_USBDEV)
  ret = k1_usbdev_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: K1 USB device initialization failed: %d\n",
             ret);
    }
  else
    {
      k1_early_puts("K1 USB device: controller initialized\r\n");
    }
#  if defined(CONFIG_CDCACM) && !defined(CONFIG_CDCACM_COMPOSITE)
  if (ret >= 0)
    {
      ret = cdcacm_initialize(0, NULL);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: K1 CDC-ACM initialization failed: %d\n",
                 ret);
        }
      else
        {
          k1_early_puts("K1 USB device: CDC-ACM class registered\r\n");
        }
    }

#  endif
#endif

#if defined(CONFIG_K1_USB_HOST_GLUE)
  k1_muse_pi_pro_usbhost_initialize();
#endif
}
#endif

int board_app_initialize(uintptr_t arg)
{
  (void)arg;
  return 0;
}
