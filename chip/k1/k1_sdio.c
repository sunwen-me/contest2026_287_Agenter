/****************************************************************************
 * vendor/spacemit/chips/k1/k1_sdio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_SDIO)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <debug.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/compiler.h>
#include <nuttx/sdio.h>
#include <nuttx/signal.h>

#include <arch/barriers.h>

#include "hardware/k1_sdio.h"
#include "hardware/k1_plic.h"
#include "k1_cache.h"
#include "k1_sdio.h"
#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* U-Boot owns the SDH source-clock muxes and their parent rates.  These
 * dividers are intentionally conservative for the initial PIO-only
 * implementation.
 */

#define K1_SDHC_IDMODE_DIVIDER                469u
#define K1_SDHC_TRANSFER_DIVIDER              16u
#define K1_SDHC_WIFI_HIGHSPEED_DIVIDER         4u
#define K1_SDHC_WIFI_SDR104_DIVIDER             1u

#define K1_SDHC_RESET_TIMEOUT                 1000u
#define K1_SDHC_CLOCK_TIMEOUT                 1000u
#define K1_SDHC_COMMAND_TIMEOUT               10000u

#if defined(CONFIG_K1_RTL8852BS2_FW_SECTION_PACKET_DIAGNOSTIC) || \
    defined(CONFIG_K1_RTL8852BS2_FW_FULL_DOWNLOAD_DIAGNOSTIC)
#  define K1_SDIO_WIFI_FWDL_BLOCK_MODE          1
#endif

/* RTL8852BS2 FWDL profiles send a 2048-byte packet as four 512-byte SDIO
 * blocks.  Other profiles retain the original byte-mode-only bounce-buffer
 * limit.
 */

#ifdef K1_SDIO_WIFI_FWDL_BLOCK_MODE
#  define K1_SDHC_ADMA_BOUNCE_SIZE            2048u
#else
#  define K1_SDHC_ADMA_BOUNCE_SIZE             512u
#endif
/* The K1 host has a data timeout derived from SDCLK.  Linux protects an
 * SDIO request with a multi-second timer; retain a one-second bounded PIO
 * wait here so the bring-up path cannot mistake a delayed first CMD53 data
 * phase for a command failure.
 */

#define K1_SDHC_DEFAULT_XFR_TIMEOUT_MSEC      1000u

#ifdef CONFIG_K1_SDIO_WIFI
#  define K1_SDIO_WIFI_READY_TIMEOUT_MSEC     100u
#  define K1_SDIO_R4_VOLTAGE_WINDOW_MASK      0x00fffffful
/* SDIO R4 bit 31 is I/O-ready; bit 27 instead reports memory presence. */
#  define K1_SDIO_R4_IO_READY                 (1ul << 31)
#  define K1_SDIO_R4_FUNCTIONS_SHIFT          28
#  define K1_SDIO_R4_FUNCTIONS_MASK           (7ul << 28)
#  define K1_SDIO_CMD52_WRITE                 (1ul << 31)
#  define K1_SDIO_CMD52_FUNCTION_SHIFT         28
#  define K1_SDIO_CMD52_ADDRESS_SHIFT         9
#  define K1_SDIO_CMD53_WRITE                  (1ul << 31)
#  define K1_SDIO_CMD53_FUNCTION_SHIFT         28
#  define K1_SDIO_CMD53_BLOCK_MODE              (1ul << 27)
#  define K1_SDIO_CMD53_INCREMENT              (1ul << 26)
#  define K1_SDIO_CMD53_ADDRESS_SHIFT          9
#  define K1_SDIO_CMD53_COUNT_MASK             0x1fful
#  define K1_SDIO_CMD53_MAX_BYTE_COUNT         512u
#  define K1_SDIO_CMD53_BLOCK_SIZE              512u
#  define K1_SDIO_WIFI_FWDL_FIFO_BASE        0x0001c000u
#  define K1_SDIO_WIFI_FWDL_FIFO_MASK        0x00000fffu
#  define K1_SDIO_WIFI_FWDL_UNIT_SIZE                  8u
/* Fixed RX FIFO address of the Wi-Fi function.  The device announces the
 * length of each pending aggregate in its own RX_REQ_LEN register, and that
 * length regularly exceeds the 512-byte CMD53 byte-mode limit, so this
 * address is the one place where the bounded CMD53 interface accepts a
 * block-mode read.
 */
#  define K1_SDIO_WIFI_RX_FIFO_ADDRESS       0x00001f00u
#  define K1_SDIO_WIFI_CMD52_READ_CRC_RETRIES     3u
#  define K1_SDIO_WIFI_CMD52_READ_RETRY_USEC     10u
#ifdef K1_SDIO_WIFI_FWDL_BLOCK_MODE
#  define K1_SDIO_CMD53_MAX_BLOCK_COUNT           4u
#endif
#  define K1_SDIO_R5_CRC_ERROR                (1ul << 15)
#  define K1_SDIO_R5_ILLEGAL_COMMAND          (1ul << 14)
#  define K1_SDIO_R5_ERROR                    (1ul << 11)
#  define K1_SDIO_R5_FUNCTION_NUMBER          (1ul << 9)
#  define K1_SDIO_R5_OUT_OF_RANGE             (1ul << 8)
#  define K1_SDIO_CCCR_ABORT                   0x06u
#  define K1_SDIO_CCCR_ABORT_RESET             (1u << 3)
#  define K1_SDIO_CCCR_SPEED                   0x13u
#  define K1_SDIO_CCCR_UHS                     0x14u
#  define K1_SDIO_SPEED_BUS_MASK               0x0eu
#  define K1_SDIO_SPEED_SDR104                 0x06u
#  define K1_SDIO_UHS_SDR104                   0x02u
#  define K1_SDIO_MUSEPI_TX_DELAYCODE          0xa8u
#  define K1_SDIO_MUSEPI_RX_DELAYCODE          184u
#  define K1_SDIO_MUSEPI_HISR_RX_DELAYCODE      0xb9u
#  define K1_SDIO_TUNING_BYTES                   64u
#  define K1_SDIO_TUNING_TIMEOUT_MSEC           150u
#  define K1_SDIO_RX_TUNE_DELAY_MIN                0u
#  define K1_SDIO_RX_TUNE_DELAY_MAX              255u
#  define K1_SDIO_RX_TUNE_WINDOW_MIN              50u
#  define K1_SDIO_FBR_INTERFACE_CODE          0x00u
#  define K1_SDIO_FBR_BLOCK_SIZE_LOW           0x10u
#  define K1_SDIO_FBR_BLOCK_SIZE_HIGH          0x11u
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* ADMA2 uses a little-endian 32-bit address descriptor.  The K1 Linux host
 * driver requires an END flag on the transfer descriptor rather than a
 * trailing NOP descriptor, and the descriptor table itself needs 8-byte
 * alignment.
 */

struct aligned_data(8) k1_sdio_adma_desc_s
{
  uint16_t command;
  uint16_t length;
  uint32_t address;
};

#ifdef CONFIG_K1_SDIO_WIFI

/* CMD53 is deliberately not offered as a generic large-transfer API.  Each
 * route is one fixed request shape with its own guard: the generic route
 * stays byte-mode-only, the FWDL FIFO route carries the firmware download
 * packets, and the RX FIFO route carries one announced receive aggregate.
 * A caller cannot widen a route by passing a different address or length.
 */

enum k1_sdio_wifi_cmd53_route_e
{
  K1_SDIO_CMD53_ROUTE_GENERIC = 0,
  K1_SDIO_CMD53_ROUTE_FWDL_FIFO,
  K1_SDIO_CMD53_ROUTE_RX_FIFO
};

#endif

struct k1_sdio_dev_s
{
  struct sdio_dev_s dev;
  uintptr_t base;
  uintptr_t clock_rst;
  FAR const char *name;
  FAR uint8_t *buffer;
  size_t remaining;
  sdio_capset_t caps;
  sdio_eventset_t waitevents;
  uint32_t timeout;
  bool write;
  bool mmc_mode;
  bool initialized;
  bool adma_active;
  struct k1_sdio_adma_desc_s adma_desc;
  uint8_t adma_bounce[K1_SDHC_ADMA_BOUNCE_SIZE] aligned_data(64);
#ifdef CONFIG_K1_SDIO_WIFI
  bool wifi_selected;
  bool tuning_sweep;
  bool command_trace_suppressed;
  uint8_t wifi_functions;
#endif
};

#ifdef CONFIG_K1_SDIO_WIFI
/* SD 4-bit CMD19 tuning block, represented as the 16 words consumed by the
 * K1 Linux and U-Boot host drivers.  CMD19 is always transferred with PIO;
 * it must not depend on the CMD53 ADMA diagnostic path.
 */

static const uint32_t g_k1_sdio_tuning_pattern4[16] =
{
  0x00ff0ffful, 0xccc3ccfful, 0xffcc3cc3ul, 0xeffefffeul,
  0xddffdffful, 0xfbfffbfful, 0xff7fffbful, 0xefbdf777ul,
  0xf0fff0fful, 0x3cccfc0ful, 0xcfcc33ccul, 0xeeffeffful,
  0xfdfffdfful, 0xffbfffdful, 0xfff7ffbbul, 0xde7b7ff7ul,
};
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void k1_sdio_reset(FAR struct sdio_dev_s *dev);
static sdio_capset_t k1_sdio_capabilities(FAR struct sdio_dev_s *dev);
static sdio_statset_t k1_sdio_status(FAR struct sdio_dev_s *dev);
static void k1_sdio_widebus(FAR struct sdio_dev_s *dev, bool enable);
static void k1_sdio_clock(FAR struct sdio_dev_s *dev,
                          enum sdio_clock_e rate);
static int k1_sdio_attach(FAR struct sdio_dev_s *dev);
static int k1_sdio_sendcmd(FAR struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t arg);
#ifdef CONFIG_SDIO_BLOCKSETUP
static void k1_sdio_blocksetup(FAR struct sdio_dev_s *dev,
                               unsigned int blocklen, unsigned int nblocks);
#endif
static int k1_sdio_recvsetup(FAR struct sdio_dev_s *dev,
                             FAR uint8_t *buffer, size_t nbytes);
static int k1_sdio_sendsetup(FAR struct sdio_dev_s *dev,
                             FAR const uint8_t *buffer, size_t nbytes);
static int k1_sdio_cancel(FAR struct sdio_dev_s *dev);
static int k1_sdio_waitresponse(FAR struct sdio_dev_s *dev, uint32_t cmd);
static int k1_sdio_recvshort(FAR struct sdio_dev_s *dev, uint32_t cmd,
                             FAR uint32_t *response);
static int k1_sdio_recvlong(FAR struct sdio_dev_s *dev, uint32_t cmd,
                            FAR uint32_t response[4]);
static void k1_sdio_waitenable(FAR struct sdio_dev_s *dev,
                               sdio_eventset_t eventset, uint32_t timeout);
static sdio_eventset_t k1_sdio_eventwait(FAR struct sdio_dev_s *dev);
static void k1_sdio_callbackenable(FAR struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int k1_sdio_registercallback(FAR struct sdio_dev_s *dev,
                                    worker_t callback, FAR void *arg);
#endif

#ifdef CONFIG_K1_SDIO_WIFI
static void k1_sdio_configure_wifi_clock(FAR struct k1_sdio_dev_s *priv);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_SDIO_BLOCKSETUP
#  define K1_SDIO_BLOCKSETUP_INITIALIZER \
    .blocksetup     = k1_sdio_blocksetup,
#else
#  define K1_SDIO_BLOCKSETUP_INITIALIZER
#endif

#define K1_SDIO_DEVICE(_base, _clock, _name, _caps, _mmc)                 \
  {                                                                         \
    .dev =                                                                   \
      {                                                                       \
        .reset          = k1_sdio_reset,                                    \
        .capabilities   = k1_sdio_capabilities,                             \
        .status         = k1_sdio_status,                                   \
        .widebus        = k1_sdio_widebus,                                  \
        .clock          = k1_sdio_clock,                                    \
        .attach         = k1_sdio_attach,                                   \
        .sendcmd        = k1_sdio_sendcmd,                                  \
        K1_SDIO_BLOCKSETUP_INITIALIZER                                       \
        .recvsetup      = k1_sdio_recvsetup,                                \
        .sendsetup      = k1_sdio_sendsetup,                                \
        .cancel         = k1_sdio_cancel,                                   \
        .waitresponse   = k1_sdio_waitresponse,                             \
        .recv_r1        = k1_sdio_recvshort,                                \
        .recv_r2        = k1_sdio_recvlong,                                 \
        .recv_r3        = k1_sdio_recvshort,                                \
        .recv_r4        = k1_sdio_recvshort,                                \
        .recv_r5        = k1_sdio_recvshort,                                \
        .recv_r6        = k1_sdio_recvshort,                                \
        .recv_r7        = k1_sdio_recvshort,                                \
        .waitenable     = k1_sdio_waitenable,                               \
        .eventwait      = k1_sdio_eventwait,                                \
        .callbackenable = k1_sdio_callbackenable,                           \
      },                                                                      \
    .base      = (_base),                                                    \
    .clock_rst = (_clock),                                                   \
    .name      = (_name),                                                    \
    .caps      = (_caps),                                                    \
    .mmc_mode  = (_mmc),                                                     \
  }

static struct k1_sdio_dev_s g_k1_sdio_emmc =
  K1_SDIO_DEVICE(K1_SDHC2_BASE, K1_APMU_SDH2_CLK_RST, "eMMC",
                 SDIO_CAPS_1BIT_ONLY, true);

#ifdef CONFIG_K1_SDIO_WIFI
static struct k1_sdio_dev_s g_k1_sdio_wifi =
  K1_SDIO_DEVICE(K1_SDHC1_BASE, K1_APMU_SDH1_CLK_RST, "Wi-Fi SDIO",
                 SDIO_CAPS_4BIT, false);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct k1_sdio_dev_s *
k1_sdio_priv(FAR struct sdio_dev_s *dev)
{
  return (FAR struct k1_sdio_dev_s *)dev;
}

static inline uintptr_t k1_sdio_reg(FAR const struct k1_sdio_dev_s *priv,
                                    uintptr_t offset)
{
  return priv->base + offset;
}

#define K1_SDHC_REG(priv, _reg) \
  k1_sdio_reg((priv), K1_SDHC_##_reg##_OFFSET)

static bool k1_sdio_wait_clear(uintptr_t address, uint32_t mask,
                               unsigned int timeout)
{
  while ((getreg32(address) & mask) != 0)
    {
      if (timeout-- == 0)
        {
          return false;
        }

      up_udelay(10);
    }

  return true;
}

static bool k1_sdio_wait8_clear(uintptr_t address, uint8_t mask,
                                unsigned int timeout)
{
  while ((getreg8(address) & mask) != 0)
    {
      if (timeout-- == 0)
        {
          return false;
        }

      up_udelay(10);
    }

  return true;
}

static bool k1_sdio_wait_set(uintptr_t address, uint32_t mask,
                             unsigned int timeout)
{
  while ((getreg32(address) & mask) == 0)
    {
      if (timeout-- == 0)
        {
          return false;
        }

      up_udelay(10);
    }

  return true;
}

static int k1_sdio_error(uint32_t status)
{
  if ((status & K1_SDHC_INT_ERROR_MASK) == 0)
    {
      return OK;
    }

  if ((status & (1ul << 16)) != 0 || (status & (1ul << 20)) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((status & ((1ul << 17) | (1ul << 21))) != 0)
    {
      return -EILSEQ;
    }

  return -EIO;
}

static void k1_sdio_trace(FAR const char *label, uint32_t value)
{
#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts(label);
  k1_early_puthex((uintreg_t)value);
  k1_early_puts("\r\n");
#else
  (void)label;
  (void)value;
#endif
}

static void k1_sdio_trace_device(FAR const struct k1_sdio_dev_s *priv,
                                 FAR const char *label, uint32_t value)
{
#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 ");
  k1_early_puts(priv->name);
  k1_early_puts(": ");
  k1_early_puts(label);
  k1_early_puthex((uintreg_t)value);
  k1_early_puts("\r\n");
#else
  (void)priv;
  (void)label;
  (void)value;
#endif
}

static void k1_sdio_trace_state(FAR struct k1_sdio_dev_s *priv,
                                FAR const char *stage)
{
#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 SDIO: state ");
  k1_early_puts(stage);
  k1_early_puts("\r\n");
  k1_sdio_trace_device(priv, "APMU AXI=",
                       getreg32(K1_APMU_SDH_AXI_CLK_RST));
  k1_sdio_trace("K1 SDIO: clock gate=", getreg32(priv->clock_rst));
  k1_sdio_trace("K1 SDIO: PRESENT=",
                getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
  k1_sdio_trace("K1 SDIO: POWER=",
                getreg8(K1_SDHC_REG(priv, POWER_CONTROL)));
  k1_sdio_trace("K1 SDIO: CLOCK=",
                getreg32(K1_SDHC_REG(priv, CLOCK_CONTROL)));
  k1_sdio_trace("K1 SDIO: HOST=",
                getreg8(K1_SDHC_REG(priv, HOST_CONTROL)));
  k1_sdio_trace("K1 SDIO: HOST2=",
                getreg16(K1_SDHC_REG(priv, HOST_CONTROL2)));
  k1_sdio_trace("K1 SDIO: INT=",
                getreg32(K1_SDHC_REG(priv, INT_STATUS)));
  k1_sdio_trace("K1 SDIO: OP_EXT=",
                getreg32(K1_SDHC_REG(priv, OP_EXT)));
  k1_sdio_trace("K1 SDIO: MMC_CTRL=",
                getreg32(K1_SDHC_REG(priv, MMC_CONTROL)));
  k1_sdio_trace("K1 SDIO: TX_CFG=",
                getreg32(K1_SDHC_REG(priv, TX_CONTROL)));
  k1_sdio_trace("K1 SDIO: PHY=",
                getreg32(K1_SDHC_REG(priv, PHY_CONTROL)));
  k1_sdio_trace("K1 SDIO: PADCFG=",
                getreg32(K1_SDHC_REG(priv, PHY_PADCFG)));
#else
  (void)priv;
  (void)stage;
#endif
}

#ifdef CONFIG_K1_SDIO_WIFI
/****************************************************************************
 * Name: k1_sdio_configure_wifi_clock
 *
 * Description:
 *   Program SDH1 to the same 375 MHz pll2_d8 source used by the vendor
 *   Linux DTS before the SDHCI divider produces the 400 kHz identification
 *   clock.  The source and divider fields only take effect after the
 *   hardware clears the frequency-change request bit.
 ****************************************************************************/

static void k1_sdio_configure_wifi_clock(FAR struct k1_sdio_dev_s *priv)
{
  uint32_t value;

  value = getreg32(priv->clock_rst);
  value &= ~(K1_APMU_SDH_CLOCK_SOURCE_MASK |
             K1_APMU_SDH_CLOCK_DIVIDER_MASK);
  value |= K1_APMU_SDH_CLOCK_SOURCE_PLL2_D8;
  putreg32(value, priv->clock_rst);

  modifyreg32(priv->clock_rst, 0, K1_APMU_SDH_CLOCK_FREQUENCY_CHANGE);
  if (!k1_sdio_wait_clear(priv->clock_rst,
                           K1_APMU_SDH_CLOCK_FREQUENCY_CHANGE,
                           K1_SDHC_CLOCK_TIMEOUT))
    {
      mcerr("ERROR: K1 Wi-Fi SDH1 clock change timed out\n");
    }
}
#endif

static void k1_sdio_pio(FAR struct k1_sdio_dev_s *priv, uint32_t state)
{
  uint32_t word;
  size_t ncopy;

  while (priv->remaining != 0)
    {
      if (priv->write)
        {
          if ((state & K1_SDHC_PRESENT_SPACE_AVAILABLE) == 0)
            {
              break;
            }

          word = 0;
          ncopy = priv->remaining < sizeof(word) ? priv->remaining :
                                                   sizeof(word);
          memcpy(&word, priv->buffer, ncopy);
          putreg32(word, K1_SDHC_REG(priv, BUFFER));
        }
      else
        {
          if ((state & K1_SDHC_PRESENT_DATA_AVAILABLE) == 0)
            {
              break;
            }

          word = getreg32(K1_SDHC_REG(priv, BUFFER));
          ncopy = priv->remaining < sizeof(word) ? priv->remaining :
                                                   sizeof(word);
          memcpy(priv->buffer, &word, ncopy);
        }

      priv->buffer += ncopy;
      priv->remaining -= ncopy;
      state = getreg32(K1_SDHC_REG(priv, PRESENT_STATE));
    }
}

#ifdef CONFIG_K1_SDIO_WIFI
/****************************************************************************
 * Name: k1_sdio_wifi_tuning_read
 *
 * Description:
 *   Consume one CMD19 pattern through the SDHCI buffer register.  K1 signals
 *   Buffer Read Ready for CMD19 but, unlike a normal block read, does not
 *   signal Transfer Complete after the 64-byte tuning block is drained.
 ****************************************************************************/

static int k1_sdio_wifi_tuning_read(FAR struct k1_sdio_dev_s *priv,
                                    bool trace)
{
  uint32_t status;
  unsigned int timeout;

  for (timeout = K1_SDIO_TUNING_TIMEOUT_MSEC; timeout > 0; timeout--)
    {
      status = getreg32(K1_SDHC_REG(priv, INT_STATUS));
      if ((status & (K1_SDHC_INT_ERROR | K1_SDHC_INT_ERROR_MASK)) != 0)
        {
          if (trace)
            {
              k1_sdio_trace_device(priv, "CMD19 data error=", status);
            }

          putreg32(status, K1_SDHC_REG(priv, INT_STATUS));
          priv->waitevents = 0;
          return k1_sdio_error(status);
        }

      if ((status & K1_SDHC_INT_DATA_AVAILABLE) != 0)
        {
          if (trace)
            {
              k1_sdio_trace_device(priv, "CMD19 data ready=", status);
            }

          k1_sdio_pio(priv, getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
          putreg32(K1_SDHC_INT_DATA_AVAILABLE,
                   K1_SDHC_REG(priv, INT_STATUS));
          if (priv->remaining == 0)
            {
              priv->waitevents = 0;
              return OK;
            }
        }

      nxsig_usleep(1000);
    }

  priv->waitevents = 0;
  if (trace)
    {
      k1_sdio_trace_device(priv, "CMD19 data remaining=", priv->remaining);
    }

  return -ETIMEDOUT;
}
#endif

/****************************************************************************
 * Name: k1_sdio_adma_prepare
 *
 * Description:
 *   Set up the single ADMA2 descriptor used by the bounded Wi-Fi CMD53
 *   transfer interface.  A cache-line-aligned bounce buffer keeps arbitrary
 *   caller buffers out of the 32-bit DMA and cache-coherency contract.
 ****************************************************************************/

#ifdef CONFIG_K1_SDIO_WIFI
static int k1_sdio_adma_prepare(FAR struct k1_sdio_dev_s *priv,
                                size_t source_length)
{
  uint8_t control;
  uintptr_t desc_address;
  uintptr_t data_address;

  if (priv->buffer == NULL || source_length == 0 ||
      source_length > priv->remaining ||
      priv->remaining > sizeof(priv->adma_bounce))
    {
      return -EINVAL;
    }

  desc_address = (uintptr_t)&priv->adma_desc;
  data_address = (uintptr_t)priv->adma_bounce;
  if (desc_address > UINT32_MAX || data_address > UINT32_MAX)
    {
      return -EOVERFLOW;
    }

  if (priv->write)
    {
      memcpy(priv->adma_bounce, priv->buffer, source_length);
      memset(priv->adma_bounce + source_length, 0,
             priv->remaining - source_length);
    }

  /* Clean first for both directions.  For a read this writes back any stale
   * cache line before the controller overwrites DRAM; completion invalidates
   * it before the CPU consumes the received bytes.
   */

  k1_dcache_clean(data_address, priv->remaining);

  priv->adma_desc.command = K1_SDHC_ADMA2_TRAN_VALID |
                            K1_SDHC_ADMA2_END;
  priv->adma_desc.length = (uint16_t)priv->remaining;
  priv->adma_desc.address = (uint32_t)data_address;
  k1_dcache_clean(desc_address, sizeof(priv->adma_desc));

  /* The SDHCI master is an I/O observer.  The cache helper already orders
   * normal memory, but this barrier also orders the descriptor before the
   * ADMA address and command registers are visible to the controller.
   */

  UP_DSB();

  control = getreg8(K1_SDHC_REG(priv, HOST_CONTROL));
  control &= ~K1_SDHC_HOST_CONTROL_DMA_MASK;
  control |= K1_SDHC_HOST_CONTROL_ADMA32;
  putreg8(control, K1_SDHC_REG(priv, HOST_CONTROL));
  putreg32((uint32_t)desc_address, K1_SDHC_REG(priv, ADMA_ADDRESS));
  UP_DSB();
  priv->adma_active = true;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_adma_complete
 ****************************************************************************/

static void k1_sdio_adma_complete(FAR struct k1_sdio_dev_s *priv)
{
  DEBUGASSERT(priv->adma_active);

  if (!priv->write)
    {
      k1_dcache_invalidate((uintptr_t)priv->adma_bounce, priv->remaining);
      memcpy(priv->buffer, priv->adma_bounce, priv->remaining);
    }

  priv->remaining = 0;
  priv->adma_active = false;
}
#endif

/****************************************************************************
 * Name: k1_sdio_reset
 ****************************************************************************/

static void k1_sdio_reset(FAR struct sdio_dev_s *dev)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t value;
  uint16_t control2;

  putreg32(0, K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
  putreg8(K1_SDHC_RESET_ALL, K1_SDHC_REG(priv, SOFTWARE_RESET));

  if (!k1_sdio_wait8_clear(K1_SDHC_REG(priv, SOFTWARE_RESET),
                           K1_SDHC_RESET_ALL,
                           K1_SDHC_RESET_TIMEOUT))
    {
      mcerr("ERROR: K1 %s reset timed out\n", priv->name);
    }

  /* Linux uses different reset follow-up sequences for eMMC and SDH1.
   * SDH1 has BROKEN_PHY_MODULE in the board DTS, so it must not receive the
   * eMMC PHY or legacy-pad writes.
   */

  if (priv->mmc_mode)
    {
      modifyreg32(K1_SDHC_REG(priv, PHY_CONTROL), 0,
                  K1_SDHC_PHY_FUNCTION_ENABLE | K1_SDHC_PHY_PLL_LOCK);
      modifyreg32(K1_SDHC_REG(priv, PHY_PADCFG),
                  K1_SDHC_PHY_DRIVE_SELECT_MASK,
                  K1_SDHC_PHY_RX_BIAS_ENABLE | K1_SDHC_PHY_DRIVE_SELECT_4);
      modifyreg32(K1_SDHC_REG(priv, MMC_CONTROL), 0,
                  K1_SDHC_MMC_CARD_MODE);
      modifyreg32(K1_SDHC_REG(priv, LEGACY_CONTROL), 0,
                  K1_SDHC_LEGACY_PAD_CLOCK_ON);
      putreg8(K1_SDHC_POWER_ON | K1_SDHC_POWER_180,
              K1_SDHC_REG(priv, POWER_CONTROL));
    }
  else
    {
      /* The SDIO card's VDD OCR is 3.3 V while its board-level signalling is
       * fixed at 1.8 V.  This matches Linux mmc1 ios before CMD0.
       */

#ifdef CONFIG_K1_SDIO_WIFI_MAINLINE_RESET_DIAGNOSTIC
      /* Linux mainline pairs these writes for an SDIO-only K1 host.  The
       * prior OP_EXT-only trial changed CMD5 behavior, so retain this exact
       * reset sequence behind the PIO-only transport diagnostic.
       */

      modifyreg32(K1_SDHC_REG(priv, PHY_CONTROL), 0,
                  K1_SDHC_PHY_FUNCTION_ENABLE | K1_SDHC_PHY_PLL_LOCK);
      modifyreg32(K1_SDHC_REG(priv, PHY_PADCFG),
                  K1_SDHC_PHY_DRIVE_SELECT_MASK,
                  K1_SDHC_PHY_RX_BIAS_ENABLE | K1_SDHC_PHY_DRIVE_SELECT_4);
      modifyreg32(K1_SDHC_REG(priv, LEGACY_CONTROL), 0,
                  K1_SDHC_LEGACY_PAD_CLOCK_ON);
      modifyreg32(K1_SDHC_REG(priv, OP_EXT), 0,
                  K1_SDHC_OP_EXT_OVERRIDE_CLOCK_ENABLE |
                  K1_SDHC_OP_EXT_FORCE_CLOCK_ON);
#endif

      modifyreg32(K1_SDHC_REG(priv, TX_CONTROL), 0,
                  K1_SDHC_TX_INTERNAL_CLOCK_SELECT);
      putreg8(K1_SDHC_POWER_ON | K1_SDHC_POWER_330,
              K1_SDHC_REG(priv, POWER_CONTROL));
      control2 = getreg16(K1_SDHC_REG(priv, HOST_CONTROL2));
      putreg16(control2 | K1_SDHC_HOST_CONTROL2_180V,
               K1_SDHC_REG(priv, HOST_CONTROL2));
    }

  up_mdelay(5);

  value = getreg8(K1_SDHC_REG(priv, HOST_CONTROL));
  value &= ~(K1_SDHC_HOST_CONTROL_DMA_MASK |
             K1_SDHC_HOST_CONTROL_4BIT |
             K1_SDHC_HOST_CONTROL_8BIT);
  putreg8(value, K1_SDHC_REG(priv, HOST_CONTROL));

  putreg32(K1_SDHC_INT_ALL, K1_SDHC_REG(priv, INT_STATUS));
  putreg32(K1_SDHC_INT_ALL, K1_SDHC_REG(priv, INT_STATUS_ENABLE));

  priv->buffer = NULL;
  priv->remaining = 0;
  priv->waitevents = 0;
  priv->timeout = 0;
  priv->write = false;
  priv->adma_active = false;

  k1_sdio_trace_state(priv, "after reset");
}

/****************************************************************************
 * Name: k1_sdio_capabilities
 ****************************************************************************/

static sdio_capset_t k1_sdio_capabilities(FAR struct sdio_dev_s *dev)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  return priv->caps;
}

/****************************************************************************
 * Name: k1_sdio_status
 ****************************************************************************/

static sdio_statset_t k1_sdio_status(FAR struct sdio_dev_s *dev)
{
  (void)dev;
  return SDIO_STATUS_PRESENT;
}

/****************************************************************************
 * Name: k1_sdio_widebus
 ****************************************************************************/

static void k1_sdio_widebus(FAR struct sdio_dev_s *dev, bool enable)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t value;

  value = getreg8(K1_SDHC_REG(priv, HOST_CONTROL));
  if (enable && !priv->mmc_mode)
    {
      value |= K1_SDHC_HOST_CONTROL_4BIT;
    }
  else
    {
      value &= ~K1_SDHC_HOST_CONTROL_4BIT;
    }

  putreg8(value, K1_SDHC_REG(priv, HOST_CONTROL));
}

/****************************************************************************
 * Name: k1_sdio_clock
 ****************************************************************************/

static void k1_sdio_clock(FAR struct sdio_dev_s *dev,
                          enum sdio_clock_e rate)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint16_t clock;
  uint16_t divider;

  if (rate == CLOCK_SDIO_DISABLED)
    {
      clock = getreg16(K1_SDHC_REG(priv, CLOCK_CONTROL));
      clock &= ~(K1_SDHC_CLOCK_CARD_ENABLE | K1_SDHC_CLOCK_INTERNAL_ENABLE);
      putreg16(clock, K1_SDHC_REG(priv, CLOCK_CONTROL));
      return;
    }

  divider = rate == CLOCK_IDMODE ? K1_SDHC_IDMODE_DIVIDER :
                                   K1_SDHC_TRANSFER_DIVIDER;
  clock = (uint16_t)((divider & 0xffu) << K1_SDHC_CLOCK_DIVIDER_SHIFT);
  clock |= (uint16_t)(((divider & K1_SDHC_CLOCK_DIVIDER_HIGH_MASK) >> 8) <<
                      K1_SDHC_CLOCK_DIVIDER_HIGH_SHIFT);
  clock |= K1_SDHC_CLOCK_INTERNAL_ENABLE;
  putreg16(clock, K1_SDHC_REG(priv, CLOCK_CONTROL));

  if (!k1_sdio_wait_set(K1_SDHC_REG(priv, CLOCK_CONTROL),
                        K1_SDHC_CLOCK_INTERNAL_STABLE,
                        K1_SDHC_CLOCK_TIMEOUT))
    {
      mcerr("ERROR: K1 %s clock did not stabilize\n", priv->name);
      return;
    }

  putreg16(clock | K1_SDHC_CLOCK_CARD_ENABLE,
           K1_SDHC_REG(priv, CLOCK_CONTROL));

  k1_sdio_trace_state(priv, "after clock");
}

/****************************************************************************
 * Name: k1_sdio_set_clock_divider
 ****************************************************************************/

static int k1_sdio_set_clock_divider(FAR struct k1_sdio_dev_s *priv,
                                     uint16_t divider)
{
  uint16_t clock;

  clock = (uint16_t)((divider & 0xffu) << K1_SDHC_CLOCK_DIVIDER_SHIFT);
  clock |= (uint16_t)(((divider & K1_SDHC_CLOCK_DIVIDER_HIGH_MASK) >> 8) <<
                      K1_SDHC_CLOCK_DIVIDER_HIGH_SHIFT);
  clock |= K1_SDHC_CLOCK_INTERNAL_ENABLE;
  putreg16(clock, K1_SDHC_REG(priv, CLOCK_CONTROL));

  if (!k1_sdio_wait_set(K1_SDHC_REG(priv, CLOCK_CONTROL),
                        K1_SDHC_CLOCK_INTERNAL_STABLE,
                        K1_SDHC_CLOCK_TIMEOUT))
    {
      return -ETIMEDOUT;
    }

  putreg16(clock | K1_SDHC_CLOCK_CARD_ENABLE,
           K1_SDHC_REG(priv, CLOCK_CONTROL));

  k1_sdio_trace_state(priv, "after clock");
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_attach
 ****************************************************************************/

static int k1_sdio_attach(FAR struct sdio_dev_s *dev)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);

  /* The initial driver deliberately uses status polling instead of PLIC.
   * Leave all SDHCI interrupt signals masked.
   */

  putreg32(0, K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_sendcmd
 ****************************************************************************/

static int k1_sdio_sendcmd(FAR struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t arg)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint16_t command;
  uint16_t transfer = 0;
  uint32_t response;
  uint32_t state;
  bool trace;

  trace = !priv->command_trace_suppressed &&
          (!priv->tuning_sweep ||
           ((cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT) != 19);

  state = K1_SDHC_PRESENT_CMD_INHIBIT;
  if ((cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
    {
      state |= K1_SDHC_PRESENT_DATA_INHIBIT;
    }

  if (!k1_sdio_wait_clear(K1_SDHC_REG(priv, PRESENT_STATE), state,
                          K1_SDHC_COMMAND_TIMEOUT))
    {
      k1_sdio_trace("K1 SDIO: command busy state=", state);
      return -EBUSY;
    }

  command = (uint16_t)(((cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT) <<
                       K1_SDHC_CMD_INDEX_SHIFT);
  response = cmd & MMCSD_RESPONSE_MASK;
  switch (response)
    {
      case MMCSD_R2_RESPONSE:
        command |= K1_SDHC_CMD_RESPONSE_LONG | K1_SDHC_CMD_CRC;
        break;

      case MMCSD_R1B_RESPONSE:
        command |= K1_SDHC_CMD_RESPONSE_SHORT_BUSY |
                   K1_SDHC_CMD_CRC | K1_SDHC_CMD_INDEX;
        break;

      case MMCSD_R1_RESPONSE:
      case MMCSD_R5_RESPONSE:
      case MMCSD_R6_RESPONSE:
      case MMCSD_R7_RESPONSE:
        command |= K1_SDHC_CMD_RESPONSE_SHORT |
                   K1_SDHC_CMD_CRC | K1_SDHC_CMD_INDEX;
        break;

      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:
        command |= K1_SDHC_CMD_RESPONSE_SHORT;
        break;

      case MMCSD_NO_RESPONSE:
      default:
        break;
    }

  if ((cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
    {
      command |= K1_SDHC_CMD_DATA;
      putreg8(K1_SDHC_TIMEOUT_MAX,
              K1_SDHC_REG(priv, TIMEOUT_CONTROL));
      transfer |= K1_SDHC_TRNS_BLOCK_COUNT_ENABLE;
      if (priv->adma_active)
        {
          transfer |= K1_SDHC_TRNS_DMA;
        }

      if ((cmd & MMCSD_DATAXFR_MASK) == MMCSD_RDDATAXFR ||
          (cmd & MMCSD_DATAXFR_MASK) == MMCSD_RDSTREAM)
        {
          transfer |= K1_SDHC_TRNS_READ;
        }

      if ((cmd & MMCSD_MULTIBLOCK) != 0)
        {
          transfer |= K1_SDHC_TRNS_MULTI;
        }
    }
  else if (response == MMCSD_R1B_RESPONSE)
    {
      putreg8(K1_SDHC_TIMEOUT_MAX,
              K1_SDHC_REG(priv, TIMEOUT_CONTROL));
    }

  putreg32(K1_SDHC_INT_ALL, K1_SDHC_REG(priv, INT_STATUS));
  putreg32(arg, K1_SDHC_REG(priv, ARGUMENT));
  putreg16(transfer, K1_SDHC_REG(priv, TRANSFER_MODE));
  putreg16(command, K1_SDHC_REG(priv, COMMAND));

  /* Retain enough state to distinguish a silent card from a command that
   * the SDHCI block never accepted during RAM-only board bring-up.
   */

  if (trace)
    {
      k1_sdio_trace_device(priv, "command launch cmd=", cmd);
      k1_sdio_trace_device(priv, "command launch register=",
                           getreg16(K1_SDHC_REG(priv, COMMAND)));
      k1_sdio_trace_device(priv, "command launch present=",
                           getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
      if ((cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
        {
          k1_sdio_trace_device(priv, "data cmd=", cmd);
          k1_sdio_trace_device(priv, "data arg=", arg);
          k1_sdio_trace_device(priv, "data mode=", transfer);
          k1_sdio_trace_device(priv, "data command=", command);
          k1_sdio_trace_device(priv, "data block size=",
                               getreg16(K1_SDHC_REG(priv, BLOCK_SIZE)));
          k1_sdio_trace_device(priv, "data block count=",
                               getreg16(K1_SDHC_REG(priv, BLOCK_COUNT)));
          k1_sdio_trace_device(priv, "data present=",
                               getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
          k1_sdio_trace_device(priv, "data host=",
                               getreg8(K1_SDHC_REG(priv, HOST_CONTROL)));
          if (priv->adma_active)
            {
              k1_sdio_trace_device(priv, "data ADMA address=",
                                   getreg32(K1_SDHC_REG(priv,
                                                         ADMA_ADDRESS)));
              k1_sdio_trace_device(priv, "data ADMA descriptor=",
                                   getreg32((uintptr_t)&priv->adma_desc));
              k1_sdio_trace_device(priv, "data ADMA data=",
                                   getreg32((uintptr_t)
                                            &priv->adma_desc.address));
            }
        }
    }

  if (((cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT) == 1)
    {
      k1_sdio_trace_state(priv, "after CMD1");
      k1_sdio_trace("K1 eMMC: CMD1 command=", command);
      k1_sdio_trace("K1 eMMC: CMD1 argument=", arg);
    }

  return OK;
}

/****************************************************************************
 * Name: k1_sdio_blocksetup
 ****************************************************************************/

#ifdef CONFIG_SDIO_BLOCKSETUP
static void k1_sdio_blocksetup(FAR struct sdio_dev_s *dev,
                               unsigned int blocklen, unsigned int nblocks)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);

  DEBUGASSERT(blocklen > 0 && blocklen <= 0x0fffu);
  DEBUGASSERT(nblocks > 0 && nblocks <= 0xffffu);

  putreg16((uint16_t)blocklen, K1_SDHC_REG(priv, BLOCK_SIZE));
  putreg16((uint16_t)nblocks, K1_SDHC_REG(priv, BLOCK_COUNT));
}
#endif

/****************************************************************************
 * Name: k1_sdio_recvsetup
 ****************************************************************************/

static int k1_sdio_recvsetup(FAR struct sdio_dev_s *dev,
                             FAR uint8_t *buffer, size_t nbytes)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);

  if (buffer == NULL || nbytes == 0)
    {
      return -EINVAL;
    }

  priv->buffer = buffer;
  priv->remaining = nbytes;
  priv->write = false;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_sendsetup
 ****************************************************************************/

static int k1_sdio_sendsetup(FAR struct sdio_dev_s *dev,
                             FAR const uint8_t *buffer, size_t nbytes)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);

  if (buffer == NULL || nbytes == 0)
    {
      return -EINVAL;
    }

  priv->buffer = (FAR uint8_t *)buffer;
  priv->remaining = nbytes;
  priv->write = true;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_cancel
 ****************************************************************************/

static int k1_sdio_cancel(FAR struct sdio_dev_s *dev)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint8_t reset;

  /* Linux uses a command reset followed by a data reset when a request
   * fails.  K1 does not declare the combined-reset quirk, so wait for the
   * command state machine to finish resetting before resetting the data
   * state machine.  A simultaneous write can leave the following CMD19
   * command timeout even though both reset bits eventually read clear.
   */

  reset = K1_SDHC_RESET_COMMAND;
  putreg8(reset, K1_SDHC_REG(priv, SOFTWARE_RESET));
  if (!k1_sdio_wait8_clear(K1_SDHC_REG(priv, SOFTWARE_RESET),
                           reset,
                           K1_SDHC_RESET_TIMEOUT))
    {
      return -ETIMEDOUT;
    }

  reset = K1_SDHC_RESET_DATA;
  putreg8(reset, K1_SDHC_REG(priv, SOFTWARE_RESET));
  if (!k1_sdio_wait8_clear(K1_SDHC_REG(priv, SOFTWARE_RESET),
                           reset,
                           K1_SDHC_RESET_TIMEOUT))
    {
      return -ETIMEDOUT;
    }

  putreg32(K1_SDHC_INT_ALL, K1_SDHC_REG(priv, INT_STATUS));
  putreg32(0, K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
  priv->buffer = NULL;
  priv->remaining = 0;
  priv->waitevents = 0;
  priv->timeout = 0;
  priv->write = false;
  priv->adma_active = false;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_waitresponse
 ****************************************************************************/

static int k1_sdio_waitresponse(FAR struct sdio_dev_s *dev, uint32_t cmd)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t status;

  for (unsigned int timeout = K1_SDHC_COMMAND_TIMEOUT; timeout > 0;
       timeout--)
    {
      status = getreg32(K1_SDHC_REG(priv, INT_STATUS));
      if ((status & (K1_SDHC_INT_ERROR | K1_SDHC_INT_ERROR_MASK)) != 0)
        {
          k1_sdio_trace_device(priv, "command error cmd=", cmd);
          k1_sdio_trace_device(priv, "command error status=", status);
          putreg32(status, K1_SDHC_REG(priv, INT_STATUS));
          return k1_sdio_error(status);
        }

      if ((status & K1_SDHC_INT_RESPONSE) != 0)
        {
          putreg32(K1_SDHC_INT_RESPONSE, K1_SDHC_REG(priv, INT_STATUS));
          return OK;
        }

      up_udelay(10);
    }

  k1_sdio_trace_device(priv, "command timeout cmd=", cmd);
  k1_sdio_trace_device(priv, "command timeout status=",
                       getreg32(K1_SDHC_REG(priv, INT_STATUS)));
  k1_sdio_trace_device(priv, "command timeout register=",
                       getreg16(K1_SDHC_REG(priv, COMMAND)));
  k1_sdio_trace_device(priv, "command timeout present=",
                       getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
  k1_sdio_trace_device(priv, "command timeout response=",
                       getreg32(K1_SDHC_REG(priv, RESPONSE0)));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: k1_sdio_recvshort
 ****************************************************************************/

static int k1_sdio_recvshort(FAR struct sdio_dev_s *dev, uint32_t cmd,
                             FAR uint32_t *response)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  (void)cmd;

  if (response != NULL)
    {
      *response = getreg32(K1_SDHC_REG(priv, RESPONSE0));
    }

  return OK;
}

/****************************************************************************
 * Name: k1_sdio_recvlong
 ****************************************************************************/

static int k1_sdio_recvlong(FAR struct sdio_dev_s *dev, uint32_t cmd,
                            FAR uint32_t response[4])
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;

  (void)cmd;

  if (response == NULL)
    {
      return OK;
    }

  /* SDHCI stores the R2 framing bits in the four response words.  Shift
   * them away so NuttX receives the 128-bit CID/CSD payload.
   */

  r0 = getreg32(K1_SDHC_REG(priv, RESPONSE3));
  r1 = getreg32(K1_SDHC_REG(priv, RESPONSE2));
  r2 = getreg32(K1_SDHC_REG(priv, RESPONSE1));
  r3 = getreg32(K1_SDHC_REG(priv, RESPONSE0));

  response[0] = (r0 << 8) | (r1 >> 24);
  response[1] = (r1 << 8) | (r2 >> 24);
  response[2] = (r2 << 8) | (r3 >> 24);
  response[3] = r3 << 8;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_waitenable
 ****************************************************************************/

static void k1_sdio_waitenable(FAR struct sdio_dev_s *dev,
                               sdio_eventset_t eventset, uint32_t timeout)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);

  putreg32(K1_SDHC_INT_ALL, K1_SDHC_REG(priv, INT_STATUS));
  priv->waitevents = eventset;
  priv->timeout = timeout;
}

/****************************************************************************
 * Name: k1_sdio_eventwait
 ****************************************************************************/

static sdio_eventset_t k1_sdio_eventwait(FAR struct sdio_dev_s *dev)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t status;
  uint32_t timeout;
  bool traced_ready = false;

  timeout = (priv->waitevents & SDIOWAIT_TIMEOUT) != 0 ?
            priv->timeout : K1_SDHC_DEFAULT_XFR_TIMEOUT_MSEC;
  if (timeout == 0)
    {
      priv->waitevents = 0;
      return SDIOWAIT_TIMEOUT;
    }

  while (timeout-- > 0)
    {
      status = getreg32(K1_SDHC_REG(priv, INT_STATUS));
      if ((status & (K1_SDHC_INT_ERROR | K1_SDHC_INT_ERROR_MASK)) != 0)
        {
          k1_sdio_trace_device(priv, "data error status=", status);
          putreg32(status, K1_SDHC_REG(priv, INT_STATUS));
          priv->waitevents = 0;
          return SDIOWAIT_ERROR;
        }

      if ((status & (K1_SDHC_INT_SPACE_AVAILABLE |
                     K1_SDHC_INT_DATA_AVAILABLE)) != 0)
        {
          if (!traced_ready)
            {
              k1_sdio_trace_device(priv, "data ready status=", status);
              k1_sdio_trace_device(priv, "data ready present=",
                                   getreg32(K1_SDHC_REG(priv,
                                                         PRESENT_STATE)));
              traced_ready = true;
            }

          /* The Buffer Ready status is write-one-to-clear.  Consume the
           * buffer first or the K1 controller can withdraw the corresponding
           * Present State bit before the PIO read/write reaches it.
           */

          if (!priv->adma_active)
            {
              k1_sdio_pio(priv,
                          getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
            }

          putreg32(status & (K1_SDHC_INT_SPACE_AVAILABLE |
                             K1_SDHC_INT_DATA_AVAILABLE),
                   K1_SDHC_REG(priv, INT_STATUS));
        }
      else if (!priv->adma_active)
        {
          k1_sdio_pio(priv, getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
        }

      if ((status & K1_SDHC_INT_TRANSFER_COMPLETE) != 0)
        {
          putreg32(K1_SDHC_INT_TRANSFER_COMPLETE,
                   K1_SDHC_REG(priv, INT_STATUS));
#ifdef CONFIG_K1_SDIO_WIFI
          if (priv->adma_active)
            {
              k1_sdio_adma_complete(priv);
            }

#endif
          priv->waitevents = 0;
          return priv->remaining == 0 ? SDIOWAIT_TRANSFERDONE :
                                        SDIOWAIT_ERROR;
        }

      nxsig_usleep(1000);
    }

  priv->waitevents = 0;
  k1_sdio_trace_device(priv, "data timeout status=",
                       getreg32(K1_SDHC_REG(priv, INT_STATUS)));
  k1_sdio_trace_device(priv, "data timeout present=",
                       getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
  k1_sdio_trace_device(priv, "data timeout transfer=",
                       getreg16(K1_SDHC_REG(priv, TRANSFER_MODE)));
  k1_sdio_trace_device(priv, "data timeout command=",
                       getreg16(K1_SDHC_REG(priv, COMMAND)));
  k1_sdio_trace_device(priv, "data timeout block size=",
                       getreg16(K1_SDHC_REG(priv, BLOCK_SIZE)));
  k1_sdio_trace_device(priv, "data timeout block count=",
                       getreg16(K1_SDHC_REG(priv, BLOCK_COUNT)));
  k1_sdio_trace_device(priv, "data timeout control=",
                       getreg8(K1_SDHC_REG(priv, TIMEOUT_CONTROL)));
  if (priv->adma_active)
    {
      k1_sdio_trace_device(priv, "data timeout ADMA error=",
                           getreg32(K1_SDHC_REG(priv, ADMA_ERROR)));
      k1_sdio_trace_device(priv, "data timeout ADMA address=",
                           getreg32(K1_SDHC_REG(priv, ADMA_ADDRESS)));
    }

  return SDIOWAIT_TIMEOUT;
}

/****************************************************************************
 * Name: k1_sdio_callbackenable
 ****************************************************************************/

static void k1_sdio_callbackenable(FAR struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset)
{
  (void)dev;
  (void)eventset;
}

/****************************************************************************
 * Name: k1_sdio_registercallback
 ****************************************************************************/

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int k1_sdio_registercallback(FAR struct sdio_dev_s *dev,
                                    worker_t callback, FAR void *arg)
{
  (void)dev;
  (void)callback;
  (void)arg;
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sdio_initialize
 ****************************************************************************/

FAR struct sdio_dev_s *sdio_initialize(int slotno)
{
  FAR struct k1_sdio_dev_s *priv;

  if (slotno == 0)
    {
      priv = &g_k1_sdio_emmc;
    }
#ifdef CONFIG_K1_SDIO_WIFI
  else if (slotno == 1)
    {
      priv = &g_k1_sdio_wifi;
    }
#endif
  else
    {
      return NULL;
    }

  if (!priv->initialized)
    {
      /* SDH1 must not inherit an arbitrary U-Boot clock source: its 400 kHz
       * identification clock assumes the MUSE Pi Pro Linux configuration.
       * Other hosts retain their handoff clock state.
       */

      modifyreg32(K1_APMU_SDH_AXI_CLK_RST, 0,
                  K1_APMU_SDH_AXI_RESET_DEASSERT |
                  K1_APMU_SDH_AXI_CLOCK_ENABLE);
#ifdef CONFIG_K1_SDIO_WIFI
      if (!priv->mmc_mode)
        {
          k1_sdio_configure_wifi_clock(priv);
        }

#endif
      modifyreg32(priv->clock_rst, 0,
                  K1_APMU_SDH_RESET_DEASSERT | K1_APMU_SDH_CLOCK_ENABLE);
      k1_sdio_reset(&priv->dev);
      priv->initialized = true;
    }

  return &priv->dev;
}

#ifdef CONFIG_K1_SDIO_WIFI
/****************************************************************************
 * Name: k1_sdio_wifi_prepare
 *
 * Description:
 *   Reset and configure SDH1 before the board powers the SDIO card.  The
 *   public SDIO initializer is idempotent, so the subsequent probe reuses
 *   this controller state without a second host reset.
 ****************************************************************************/

int k1_sdio_wifi_prepare(void)
{
  return sdio_initialize(1) == NULL ? -ENODEV : OK;
}

/****************************************************************************
 * Name: k1_sdio_wifi_suppress_command_trace
 *
 * Description:
 *   Keep failures observable while a bounded diagnostic issues many CMD52
 *   operations.  This only controls successful command-register dumps.
 ****************************************************************************/

void k1_sdio_wifi_suppress_command_trace(bool suppress)
{
  g_k1_sdio_wifi.command_trace_suppressed = suppress;
}

static int k1_sdio_wifi_command(FAR struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t arg)
{
  int ret;

  ret = SDIO_SENDCMD(dev, cmd, arg);
  if (ret < 0)
    {
      return ret;
    }

  /* SDH1 completes a no-response CMD0 without latching SDHCI's Response
   * Complete status bit.  Its command inhibit bit still provides the
   * required ordering point before CMD5.  Waiting for a response interrupt
   * here therefore turns a successfully transmitted reset into a timeout.
   */

  if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_NO_RESPONSE)
    {
      if (!k1_sdio_wait_clear(K1_SDHC_REG(k1_sdio_priv(dev), PRESENT_STATE),
                              K1_SDHC_PRESENT_CMD_INHIBIT,
                              K1_SDHC_COMMAND_TIMEOUT))
        {
          return -ETIMEDOUT;
        }

      return OK;
    }

  return SDIO_WAITRESPONSE(dev, cmd);
}

static int k1_sdio_wifi_cmd52(FAR struct sdio_dev_s *dev, bool write,
                               uint8_t function, uint32_t address,
                               uint8_t inb,
                               FAR uint8_t *outb)
{
  uint32_t arg;
  uint32_t response;
  int ret;

  arg = ((uint32_t)(function & 7u) << K1_SDIO_CMD52_FUNCTION_SHIFT) |
        ((address & 0x1fffful) << K1_SDIO_CMD52_ADDRESS_SHIFT);
  if (write)
    {
      arg |= K1_SDIO_CMD52_WRITE | inb;
    }

  ret = k1_sdio_wifi_command(dev, SDIO_CMD52, arg);
  if (ret < 0)
    {
      goto err_cancel;
    }

  ret = SDIO_RECVR5(dev, SDIO_CMD52, &response);
  if (ret < 0)
    {
      goto err_cancel;
    }

  if ((response & (K1_SDIO_R5_CRC_ERROR |
                   K1_SDIO_R5_ILLEGAL_COMMAND)) != 0)
    {
      ret = -EILSEQ;
      goto err_cancel;
    }

  if ((response & K1_SDIO_R5_ERROR) != 0)
    {
      ret = -EIO;
      goto err_cancel;
    }

  if ((response & (K1_SDIO_R5_FUNCTION_NUMBER |
                   K1_SDIO_R5_OUT_OF_RANGE)) != 0)
    {
      ret = -EINVAL;
      goto err_cancel;
    }

  if (outb != NULL)
    {
      *outb = response & UINT8_MAX;
    }

  return OK;

err_cancel:
  /* A failed CMD52 can leave SDH1's command/data state inhibited.  Recover
   * before a caller decides whether its side-effect-free read is retryable.
   */

  (void)k1_sdio_cancel(dev);
  return ret;
}

/****************************************************************************
 * Name: k1_sdio_wifi_abort_reset
 *
 * Description:
 *   Reproduce Linux sdio_reset(): retain CCCR_ABORT's existing bits and set
 *   its RES bit before the CMD0/CMD5 enumeration sequence.  A RAM-only
 *   handoff can otherwise leave the Wi-Fi function in a state where CMD5
 *   times out despite the power-control GPIOs having completed their cycle.
 ****************************************************************************/

#ifdef CONFIG_K1_SDIO_WIFI_CCCR_ABORT_RESET
static int k1_sdio_wifi_abort_reset(FAR struct sdio_dev_s *dev)
{
  uint8_t abort;
  int ret;

  ret = k1_sdio_wifi_cmd52(dev, false, 0, K1_SDIO_CCCR_ABORT, 0, &abort);
  if (ret < 0)
    {
      abort = K1_SDIO_CCCR_ABORT_RESET;
    }
  else
    {
      abort |= K1_SDIO_CCCR_ABORT_RESET;
    }

  ret = k1_sdio_wifi_cmd52(dev, true, 0, K1_SDIO_CCCR_ABORT, abort, NULL);
  if (ret == OK)
    {
      k1_sdio_trace_device(k1_sdio_priv(dev), "CCCR_ABORT reset=", abort);
    }

  return ret;
}
#endif

/****************************************************************************
 * Name: k1_sdio_wifi_enable_highspeed
 *
 * Description:
 *   Select the SDIO High-Speed fallback before a CMD53 diagnostic.  This is
 *   intentionally distinct from UHS/SDR104: it uses no tuning command and
 *   limits SDCLK to the 375 MHz parent divided by eight (about 46.9 MHz).
 ****************************************************************************/

static int
k1_sdio_wifi_enable_highspeed(FAR struct sdio_dev_s *dev,
                               FAR struct k1_sdio_dev_s *priv,
                               FAR struct k1_sdio_wifi_info_s *info)
{
  uint8_t control;
  uint8_t speed;
  uint16_t control2;
  int ret;

  ret = k1_sdio_wifi_cmd52(dev, false, 0, K1_SDIO_CCCR_SPEED, 0, &speed);
  if (ret < 0)
    {
      return ret;
    }

  info->speed_control = speed;
  if ((speed & SDIO_CCCR_HIGHSPEED_SHS) == 0)
    {
      return OK;
    }

  /* Clear a preceding UHS BSS selection before enabling legacy High-Speed.
   * Linux clears SDIO_SPEED_BSS_MASK when it changes SDIO timing; retaining
   * SDR104 here would leave the card and host at different clock rates.
   */

  speed &= ~K1_SDIO_SPEED_BUS_MASK;
  speed |= SDIO_CCCR_HIGHSPEED_EHS;
  ret = k1_sdio_wifi_cmd52(dev, true, 0, K1_SDIO_CCCR_SPEED, speed, NULL);
  if (ret < 0)
    {
      return ret;
    }

  control = getreg8(K1_SDHC_REG(priv, HOST_CONTROL));
  control |= K1_SDHC_HOST_CONTROL_HIGH_SPEED;
  putreg8(control, K1_SDHC_REG(priv, HOST_CONTROL));

  /* Mirror the K1 Linux set_clock() transition for non-UHS timing.  SDR104
   * leaves both the UHS mode and tuning flags set, and uses the DLINE TX
   * clock.  High-Speed needs the standard timing encoding and the K1
   * internal TX clock before the lower clock divider is enabled.
   */

  control2 = getreg16(K1_SDHC_REG(priv, HOST_CONTROL2));
  control2 &= ~(K1_SDHC_HOST_CONTROL2_UHS_MASK |
                K1_SDHC_HOST_CONTROL2_EXEC_TUNING |
                K1_SDHC_HOST_CONTROL2_TUNED_CLOCK);
  control2 |= K1_SDHC_HOST_CONTROL2_180V;
  putreg16(control2, K1_SDHC_REG(priv, HOST_CONTROL2));
  modifyreg32(K1_SDHC_REG(priv, TX_CONTROL), 0,
              K1_SDHC_TX_INTERNAL_CLOCK_SELECT);

  ret = k1_sdio_set_clock_divider(priv, K1_SDHC_WIFI_HIGHSPEED_DIVIDER);
  if (ret < 0)
    {
      return ret;
    }

  info->speed_control = speed;
  info->high_speed_enabled = 1;
  info->sdr104_enabled = 0;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_wifi_enable_sdr104
 *
 * Description:
 *   Apply the SDR104 clocking and delay-line values established by the stock
 *   Linux image on this exact MUSE Pi Pro/RTL8852BS2 combination.  This is a
 *   board calibration profile, not a replacement for the SDIO CMD19 tuning
 *   sweep required for a reusable host driver.
 ****************************************************************************/

static int
k1_sdio_wifi_enable_sdr104(FAR struct sdio_dev_s *dev,
                            FAR struct k1_sdio_dev_s *priv,
                            FAR struct k1_sdio_wifi_info_s *info)
{
  uint16_t control2;
  uint8_t speed;
  uint8_t uhs;
  int ret;

  ret = k1_sdio_wifi_cmd52(dev, false, 0, K1_SDIO_CCCR_UHS, 0, &uhs);
  if (ret < 0)
    {
      return ret;
    }

  info->uhs_support = uhs;
  if ((uhs & K1_SDIO_UHS_SDR104) == 0)
    {
      return -ENOTSUP;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, K1_SDIO_CCCR_SPEED, 0, &speed);
  if (ret < 0)
    {
      return ret;
    }

  speed &= ~K1_SDIO_SPEED_BUS_MASK;
  speed |= K1_SDIO_SPEED_SDR104;
  ret = k1_sdio_wifi_cmd52(dev, true, 0, K1_SDIO_CCCR_SPEED, speed, NULL);
  if (ret < 0)
    {
      return ret;
    }

  control2 = getreg16(K1_SDHC_REG(priv, HOST_CONTROL2));
  control2 &= ~K1_SDHC_HOST_CONTROL2_UHS_MASK;
  control2 |= K1_SDHC_HOST_CONTROL2_UHS_SDR104 |
              K1_SDHC_HOST_CONTROL2_180V;
  putreg16(control2, K1_SDHC_REG(priv, HOST_CONTROL2));

  /* The K1 Linux host clears TX_INT_CLK_SEL for SDR104, then routes TX and
   * RX through the DLINE path.  Its selected RX code is captured from the
   * stock image running on this board; TX=0xa8 comes from the board DTS.
   */

  modifyreg32(K1_SDHC_REG(priv, TX_CONTROL),
              K1_SDHC_TX_INTERNAL_CLOCK_SELECT,
              K1_SDHC_TX_MUX_SELECT);
  modifyreg32(K1_SDHC_REG(priv, DLINE_CONFIG),
              K1_SDHC_DLINE_RX_REGISTER_MASK | K1_SDHC_DLINE_RX_GAIN |
              K1_SDHC_DLINE_TX_REGISTER_MASK, 0);
  modifyreg32(K1_SDHC_REG(priv, DLINE_CONTROL),
              (K1_SDHC_DLINE_CODE_MASK << K1_SDHC_DLINE_RX_CODE_SHIFT) |
              (K1_SDHC_DLINE_CODE_MASK << K1_SDHC_DLINE_TX_CODE_SHIFT),
              K1_SDHC_DLINE_POWER_UP |
              ((uint32_t)K1_SDIO_MUSEPI_RX_DELAYCODE <<
               K1_SDHC_DLINE_RX_CODE_SHIFT) |
              ((uint32_t)K1_SDIO_MUSEPI_TX_DELAYCODE <<
               K1_SDHC_DLINE_TX_CODE_SHIFT));
  modifyreg32(K1_SDHC_REG(priv, RX_CONTROL),
              K1_SDHC_RX_CLOCK_SELECT1_MASK,
              K1_SDHC_RX_CLOCK_SELECT1_DLINE);
  up_udelay(5);

  ret = k1_sdio_set_clock_divider(priv, K1_SDHC_WIFI_SDR104_DIVIDER);
  if (ret < 0)
    {
      return ret;
    }

  info->speed_control = speed;
  info->sdr104_enabled = 1;
  info->rx_delaycode = K1_SDIO_MUSEPI_RX_DELAYCODE;
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_wifi_set_rx_delay
 ****************************************************************************/

static void
k1_sdio_wifi_set_rx_delay(FAR struct k1_sdio_dev_s *priv, uint8_t delay)
{
  modifyreg32(K1_SDHC_REG(priv, DLINE_CONTROL),
              K1_SDHC_DLINE_CODE_MASK << K1_SDHC_DLINE_RX_CODE_SHIFT,
              (uint32_t)delay << K1_SDHC_DLINE_RX_CODE_SHIFT);
}

/****************************************************************************
 * Name: k1_sdio_wifi_tuning_transaction
 *
 * Description:
 *   Send a single CMD19 request through K1's PIO tuning path and compare the
 *   received 4-bit block with the standard SD tuning pattern.  K1 signals
 *   data readiness without the generic SDHCI automatic-tuning completion
 *   bits, so the pattern itself is the success condition.
 ****************************************************************************/

static int
k1_sdio_wifi_tuning_transaction(FAR struct sdio_dev_s *dev,
                                 FAR struct k1_sdio_dev_s *priv,
                                 bool trace)
{
  uint8_t pattern[K1_SDIO_TUNING_BYTES];
  uint32_t received;
  uint32_t response;
  uint16_t control2;
  unsigned int index;
  unsigned int mismatches = 0;
  int ret;

  ret = k1_sdio_recvsetup(dev, pattern, sizeof(pattern));
  if (ret < 0)
    {
      goto err_tuning;
    }

  putreg16(K1_SDHC_BLOCK_SIZE_BOUNDARY_512K | sizeof(pattern),
           K1_SDHC_REG(priv, BLOCK_SIZE));
  putreg16(1, K1_SDHC_REG(priv, BLOCK_COUNT));
  SDIO_WAITENABLE(dev,
                  SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT | SDIOWAIT_ERROR,
                  K1_SDIO_TUNING_TIMEOUT_MSEC);
  ret = SDIO_SENDCMD(dev, SD_CMD19, 0);
  if (ret < 0)
    {
      goto err_cancel;
    }

  /* K1 leaves Response Complete clear for CMD19 even after the card puts
   * the tuning block in the buffer register.  Its released command-inhibit
   * bit provides the ordering point; waiting for the absent status bit would
   * discard a valid PIO tuning pattern.
   */

  if (!k1_sdio_wait_clear(K1_SDHC_REG(priv, PRESENT_STATE),
                          K1_SDHC_PRESENT_CMD_INHIBIT,
                          K1_SDHC_COMMAND_TIMEOUT))
    {
      ret = -ETIMEDOUT;
      goto err_cancel;
    }

  ret = SDIO_RECVR1(dev, SD_CMD19, &response);
  if (ret < 0)
    {
      goto err_cancel;
    }

  if (trace)
    {
      k1_sdio_trace_device(priv, "CMD19 R1=", response);
    }

  ret = k1_sdio_wifi_tuning_read(priv, trace);
  if (ret < 0)
    {
      goto err_cancel;
    }

  /* K1 never reports Transfer Complete for a drained CMD19 PIO pattern.
   * The generic SDHCI request path normally closes that data phase, but this
   * bounded tuning transaction consumes the buffer directly.  Reset DATA
   * before the next tuning point or CMD53 so DATA_INHIBIT cannot persist.
   */

  ret = k1_sdio_cancel(dev);
  if (ret < 0)
    {
      goto err_tuning;
    }

  if (trace)
    {
      k1_sdio_trace_device(priv, "CMD19 cleanup present=",
                           getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
    }

  for (index = 0;
       index < sizeof(g_k1_sdio_tuning_pattern4) /
               sizeof(g_k1_sdio_tuning_pattern4[0]);
       index++)
    {
      memcpy(&received, &pattern[index * sizeof(received)],
             sizeof(received));
      if (received != g_k1_sdio_tuning_pattern4[index])
        {
          mismatches++;
        }
    }

  if (trace)
    {
      k1_sdio_trace_device(priv, "CMD19 mismatches=", mismatches);
    }

  return mismatches == 0 ? OK : -EILSEQ;

err_cancel:
  (void)k1_sdio_cancel(dev);
err_tuning:
  if (trace)
    {
      control2 = getreg16(K1_SDHC_REG(priv, HOST_CONTROL2));
      k1_sdio_trace_device(priv, "CMD19 failed HOST2=", control2);
    }

  return ret;
}

/****************************************************************************
 * Name: k1_sdio_wifi_tune_sdr104
 *
 * Description:
 *   Follow the K1 Linux/U-Boot software tuning model.  Scan the DLINE RX
 *   delay range with CMD19, select the midpoint of the longest valid window,
 *   then repeat one traced transaction at the selected point.  The MUSE Pi
 *   Pro DTS requires a 50-point window before SDR104 is accepted.
 ****************************************************************************/

static int
k1_sdio_wifi_tune_sdr104(FAR struct sdio_dev_s *dev,
                          FAR struct k1_sdio_dev_s *priv,
                          FAR struct k1_sdio_wifi_info_s *info)
{
  uint8_t control;
  unsigned int best_start = 0;
  unsigned int best_length = 0;
  unsigned int current_start = 0;
  unsigned int current_length = 0;
  unsigned int delay;
  unsigned int selected;
  int ret;

  /* The previous CMD53 diagnostic may have selected ADMA32 in host control.
   * CMD19 follows the K1 Linux/U-Boot tuning path and reads its short block
   * through the SDHCI buffer register instead.
   */

  control = getreg8(K1_SDHC_REG(priv, HOST_CONTROL));
  control &= ~K1_SDHC_HOST_CONTROL_DMA_MASK;
  putreg8(control, K1_SDHC_REG(priv, HOST_CONTROL));

#ifdef CONFIG_K1_SDIO_WIFI_STATIC_TUNING_DIAGNOSTIC
  /* The board DTS supplies this calibrated value.  Keep the one-shot path
   * separate from the regular software sweep so it can identify whether a
   * changed DLINE value needs a settling interval before CMD19.
   */

  k1_sdio_wifi_set_rx_delay(priv, K1_SDIO_MUSEPI_RX_DELAYCODE);
  up_udelay(5);
  ret = k1_sdio_wifi_tuning_transaction(dev, priv, true);
  if (ret < 0)
    {
      goto err_restore;
    }

  info->rx_delaycode = K1_SDIO_MUSEPI_RX_DELAYCODE;
  return OK;
#endif

  priv->tuning_sweep = true;
  for (delay = K1_SDIO_RX_TUNE_DELAY_MIN;
       delay < K1_SDIO_RX_TUNE_DELAY_MAX;
       delay++)
    {
      k1_sdio_wifi_set_rx_delay(priv, (uint8_t)delay);
      ret = k1_sdio_wifi_tuning_transaction(dev, priv, false);
      if (ret == OK)
        {
          if (current_length == 0)
            {
              current_start = delay;
            }

          current_length++;
        }
      else
        {
          if (current_length > best_length)
            {
              best_start = current_start;
              best_length = current_length;
            }

          current_length = 0;
        }
    }

  priv->tuning_sweep = false;
  if (current_length > best_length)
    {
      best_start = current_start;
      best_length = current_length;
    }

  k1_sdio_trace_device(priv, "CMD19 window start=", best_start);
  k1_sdio_trace_device(priv, "CMD19 window length=", best_length);
  if (best_length < K1_SDIO_RX_TUNE_WINDOW_MIN)
    {
      ret = -EILSEQ;
      goto err_restore;
    }

  selected = best_start + (best_length - 1) / 2;
  k1_sdio_wifi_set_rx_delay(priv, (uint8_t)selected);
  k1_sdio_trace_device(priv, "CMD19 selected delay=", selected);
  ret = k1_sdio_wifi_tuning_transaction(dev, priv, true);
  if (ret < 0)
    {
      goto err_restore;
    }

  info->rx_delaycode = (uint8_t)selected;
  return OK;

err_restore:
  k1_sdio_wifi_set_rx_delay(priv, K1_SDIO_MUSEPI_RX_DELAYCODE);
  info->rx_delaycode = K1_SDIO_MUSEPI_RX_DELAYCODE;
  k1_sdio_trace_device(priv, "CMD19 tuning failed=", (uint32_t)-ret);
  return ret;
}

/****************************************************************************
 * Name: k1_sdio_wifi_cmd53
 *
 * Description:
 *   Run one byte-mode CMD53 transfer, or a bounded RTL8852BS2 FWDL FIFO
 *   packet, over the polling SDHCI lower-half using its ADMA2 data path.
 *   FWDL packets at most one block retain their logical byte-mode length;
 *   longer packets are rounded to the block boundary.  This interface
 *   supplies the descriptor and bounce buffer itself instead of depending on
 *   generic NuttX CMD53 DMA callbacks.
 ****************************************************************************/

static int k1_sdio_wifi_cmd53(FAR struct sdio_dev_s *dev, bool write,
                               uint8_t function, uint32_t address,
                               bool increment, FAR uint8_t *buffer,
                               size_t length,
                               enum k1_sdio_wifi_cmd53_route_e route)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t arg;
  uint32_t command;
  uint32_t response;
  sdio_eventset_t event;
  uint16_t block_count;
  uint16_t block_size;
  size_t transfer_length;
  bool block_mode;
  bool use_pio = false;
  int ret;

  if (buffer == NULL || length == 0 || function > priv->wifi_functions)
    {
      return -EINVAL;
    }

  transfer_length = length;
  block_mode = false;
  if (route == K1_SDIO_CMD53_ROUTE_RX_FIFO)
    {
      /* One announced receive aggregate, read from the fixed RX FIFO
       * address.  The caller has already split the aggregate into
       * transfers this bounce buffer can hold, so the only shapes accepted
       * here are a whole number of 512-byte blocks or a single sub-block
       * remainder.
       */

      if (write || function != 1 || increment ||
          address != K1_SDIO_WIFI_RX_FIFO_ADDRESS ||
          length > K1_SDHC_ADMA_BOUNCE_SIZE)
        {
          return -EINVAL;
        }

      block_size = (uint16_t)length;
      block_count = 1;
#ifdef K1_SDIO_WIFI_FWDL_BLOCK_MODE
      if (length >= K1_SDIO_CMD53_BLOCK_SIZE)
        {
          if ((length % K1_SDIO_CMD53_BLOCK_SIZE) != 0 ||
              length / K1_SDIO_CMD53_BLOCK_SIZE >
                K1_SDIO_CMD53_MAX_BLOCK_COUNT)
            {
              return -EINVAL;
            }

          block_size = K1_SDIO_CMD53_BLOCK_SIZE;
          block_count = (uint16_t)(length / K1_SDIO_CMD53_BLOCK_SIZE);
          block_mode = true;
        }
#else
      if (length > K1_SDIO_CMD53_MAX_BYTE_COUNT)
        {
          return -EINVAL;
        }
#endif
    }
  else if (route == K1_SDIO_CMD53_ROUTE_FWDL_FIFO)
    {
#ifdef CONFIG_K1_RTL8852BS2_FW_FULL_DOWNLOAD_DIAGNOSTIC
      if (!write || function != 1 || increment ||
          length > K1_SDIO_CMD53_BLOCK_SIZE *
                   K1_SDIO_CMD53_MAX_BLOCK_COUNT ||
          (length & (K1_SDIO_WIFI_FWDL_UNIT_SIZE - 1)) != 0 ||
          (address & ~K1_SDIO_WIFI_FWDL_FIFO_MASK) !=
            K1_SDIO_WIFI_FWDL_FIFO_BASE ||
          (address & K1_SDIO_WIFI_FWDL_FIFO_MASK) !=
            length / K1_SDIO_WIFI_FWDL_UNIT_SIZE)
        {
          return -EINVAL;
        }

      if (length > K1_SDIO_CMD53_MAX_BYTE_COUNT)
        {
          transfer_length = (length + K1_SDIO_CMD53_BLOCK_SIZE - 1) &
                            ~(K1_SDIO_CMD53_BLOCK_SIZE - 1);
          block_size = K1_SDIO_CMD53_BLOCK_SIZE;
          block_count = transfer_length / K1_SDIO_CMD53_BLOCK_SIZE;
          block_mode = true;
        }
      else
        {
          /* Realtek's SDIO transport retains <=512-byte FWDL packets in
           * byte mode.  In particular, the final 96-byte section-1 packet
           * encodes its logical length in the FIFO address as 0x0c.
           */

          block_size = (uint16_t)length;
          block_count = 1;
        }
#else
      return -EINVAL;
#endif
    }
  else
    {
#ifdef CONFIG_K1_RTL8852BS2_FW_SECTION_PACKET_DIAGNOSTIC
      /* Keep the older one-packet section diagnostic working exactly as
       * before.  It cannot coexist with the complete-download profile.
       */

      if (length == K1_SDIO_CMD53_BLOCK_SIZE *
                    K1_SDIO_CMD53_MAX_BLOCK_COUNT)
        {
          block_size = K1_SDIO_CMD53_BLOCK_SIZE;
          block_count = K1_SDIO_CMD53_MAX_BLOCK_COUNT;
          block_mode = true;
        }
      else if (length > K1_SDIO_CMD53_MAX_BYTE_COUNT)
        {
          return -EINVAL;
        }
      else
        {
          block_size = (uint16_t)length;
          block_count = 1;
        }
#else
      if (length > K1_SDIO_CMD53_MAX_BYTE_COUNT)
        {
          return -EINVAL;
        }

      block_size = (uint16_t)length;
      block_count = 1;
#endif
    }

#ifdef CONFIG_K1_SDIO_WIFI_CMD53_HISR_READ_RX_B9_DIAGNOSTIC
  /* The vendor trace used RX DLINE 0xb9 for this exact read.  Do not expose
   * this as a generic timing policy: it is intentionally restricted to the
   * bounded, read-only HISR diagnostic request.
   */

  if (!write && function == 1 && address == 0x1104u && increment &&
      length == sizeof(uint32_t))
    {
      k1_sdio_wifi_set_rx_delay(priv, K1_SDIO_MUSEPI_HISR_RX_DELAYCODE);
      k1_sdio_trace_device(priv, "K1 Wi-Fi: HISR RX DLINE_CTRL=",
                           getreg32(K1_SDHC_REG(priv, DLINE_CONTROL)));
    }
#endif

#ifdef CONFIG_K1_SDIO_WIFI_CMD53_PIO_DIAGNOSTIC
  /* Keep this A/B experiment deliberately narrow.  Function 0, address 0,
   * and four bytes are enough to compare the controller's PIO data path with
   * the existing ADMA path without touching a Realtek register.
   */

  if (function != 0 || address != 0 || increment == false ||
      length != 4 || write)
    {
      return -EINVAL;
    }

  use_pio = true;
#endif

#ifdef CONFIG_K1_SDIO_WIFI_CMD53_WRITE_PIO_DIAGNOSTIC
  /* This is the write-direction counterpart of the Function 0 PIO probe.
   * It accepts only the vendor-observed indirect-register request so a
   * diagnostic build cannot turn the generic CMD53 API into a PIO data path.
   */

  if (function != 1 || address != 0x1040u || increment == false ||
      length != 12 || !write)
    {
      return -EINVAL;
    }

  use_pio = true;
#endif

  arg = ((uint32_t)function << K1_SDIO_CMD53_FUNCTION_SHIFT) |
        ((address & 0x1fffful) << K1_SDIO_CMD53_ADDRESS_SHIFT) |
        ((uint32_t)(block_mode ? block_count : length) &
         K1_SDIO_CMD53_COUNT_MASK);
  if (block_mode)
    {
      arg |= K1_SDIO_CMD53_BLOCK_MODE;
    }

  if (increment)
    {
      arg |= K1_SDIO_CMD53_INCREMENT;
    }

  if (write)
    {
      arg |= K1_SDIO_CMD53_WRITE;
      ret = k1_sdio_sendsetup(dev, buffer, transfer_length);
    }
  else
    {
      ret = k1_sdio_recvsetup(dev, buffer, transfer_length);
    }

  if (ret < 0)
    {
      return ret;
    }

  if (!use_pio)
    {
      ret = k1_sdio_adma_prepare(priv, length);
      if (ret < 0)
        {
          return ret;
        }
    }
  else
    {
      uint8_t control;

      control = getreg8(K1_SDHC_REG(priv, HOST_CONTROL));
      control &= ~K1_SDHC_HOST_CONTROL_DMA_MASK;
      putreg8(control, K1_SDHC_REG(priv, HOST_CONTROL));
      priv->adma_active = false;
    }

#ifdef CONFIG_K1_SDIO_WIFI_LINUX_SIGNAL_DIAGNOSTIC
  uint32_t saved_status_enable;

  /* This experiment mirrors both Linux SDH1 interrupt gates, but leaves
   * the matching PLIC source disabled.  Do not rely on a reset handoff from
   * U-Boot to keep an unhandled external interrupt out of NuttX.
   */

  putreg32(K1_PLIC_PRIORITY_DISABLED,
           K1_PLIC_PRIORITY(K1_SDHC1_PLIC_SOURCE));
  modifyreg32(K1_PLIC_ENABLE(K1_SDHC1_PLIC_SOURCE),
              K1_PLIC_ENABLE_BIT(K1_SDHC1_PLIC_SOURCE), 0);
  saved_status_enable = getreg32(K1_SDHC_REG(priv, INT_STATUS_ENABLE));
  putreg32(K1_SDHC_INT_LINUX_SDH1,
           K1_SDHC_REG(priv, INT_STATUS_ENABLE));
  putreg32(K1_SDHC_INT_LINUX_SDH1,
           K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
  k1_sdio_trace_device(priv, "CMD53 status enable=",
                       getreg32(K1_SDHC_REG(priv, INT_STATUS_ENABLE)));
  k1_sdio_trace_device(priv, "CMD53 signal enable=",
                       getreg32(K1_SDHC_REG(priv, INT_SIGNAL_ENABLE)));
#endif

  /* The generic path remains byte-mode-only except for the existing exact
   * four-block section diagnostic.  Complete FWDL uses a separate fixed
   * FIFO route and rounds bus transactions without changing Realtek's FIFO
   * logical-length encoding, and the RX FIFO route reads whole 512-byte
   * blocks of one announced aggregate.
   */

  putreg16(K1_SDHC_BLOCK_SIZE_BOUNDARY_512K | block_size,
           K1_SDHC_REG(priv, BLOCK_SIZE));
  putreg16(block_count, K1_SDHC_REG(priv, BLOCK_COUNT));
  if (block_mode)
    {
      k1_sdio_trace_device(priv, "CMD53 block count=", block_count);
    }

  if (use_pio)
    {
      k1_sdio_trace_device(priv, "data PIO host=",
                           getreg8(K1_SDHC_REG(priv, HOST_CONTROL)));
#ifdef CONFIG_K1_SDIO_WIFI_CMD53_PIO_DIAGNOSTIC
      /* Linux enables these K1 SDIO output-clock bits after an SDH reset.
       * Enabling them at our reset point makes CMD5 time out on this board,
       * so confine the comparison to this one PIO CMD53 transaction after
       * CMD52 enumeration, SDR104 tuning, and the Realtek CMD52 bootstrap.
       */

      modifyreg32(K1_SDHC_REG(priv, OP_EXT), 0,
                  K1_SDHC_OP_EXT_OVERRIDE_CLOCK_ENABLE |
                  K1_SDHC_OP_EXT_FORCE_CLOCK_ON);
      k1_sdio_trace_device(priv, "data PIO OP_EXT=",
                           getreg32(K1_SDHC_REG(priv, OP_EXT)));
#endif
    }

  SDIO_WAITENABLE(dev,
                  SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT | SDIOWAIT_ERROR,
                  K1_SDHC_DEFAULT_XFR_TIMEOUT_MSEC);

  command = write ? SD_ACMD53WR : SD_ACMD53RD;
  if (block_mode)
    {
      command |= MMCSD_MULTIBLOCK;
    }

  ret = k1_sdio_wifi_command(dev, command, arg);
  if (ret < 0)
    {
      goto err_cancel;
    }

  ret = SDIO_RECVR5(dev, command, &response);
  if (ret < 0)
    {
      goto err_cancel;
    }

  if ((response & (K1_SDIO_R5_CRC_ERROR |
                   K1_SDIO_R5_ILLEGAL_COMMAND)) != 0)
    {
      ret = -EILSEQ;
      goto err_cancel;
    }

  if ((response & K1_SDIO_R5_ERROR) != 0)
    {
      ret = -EIO;
      goto err_cancel;
    }

  if ((response & (K1_SDIO_R5_FUNCTION_NUMBER |
                   K1_SDIO_R5_OUT_OF_RANGE)) != 0)
    {
      ret = -EINVAL;
      goto err_cancel;
    }

  event = SDIO_EVENTWAIT(dev);
  if ((event & SDIOWAIT_TIMEOUT) != 0)
    {
      ret = -ETIMEDOUT;
      goto err_cancel;
    }

  if ((event & (SDIOWAIT_ERROR | SDIOWAIT_TRANSFERDONE)) !=
      SDIOWAIT_TRANSFERDONE)
    {
      ret = -EIO;
      goto err_cancel;
    }

#ifdef CONFIG_K1_SDIO_WIFI_LINUX_SIGNAL_DIAGNOSTIC
  putreg32(saved_status_enable, K1_SDHC_REG(priv, INT_STATUS_ENABLE));
  putreg32(0, K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
#endif
  return OK;

err_cancel:
  (void)k1_sdio_cancel(dev);
#ifdef CONFIG_K1_SDIO_WIFI_LINUX_SIGNAL_DIAGNOSTIC
  putreg32(saved_status_enable, K1_SDHC_REG(priv, INT_STATUS_ENABLE));
  putreg32(0, K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
#endif
  return ret;
}

int k1_sdio_wifi_probe(FAR struct k1_sdio_wifi_info_s *info)
{
  FAR struct sdio_dev_s *dev;
  uint32_t response;
  uint32_t voltage_window;
  uint32_t rca;
  uint8_t value;
  uint8_t function;
  unsigned int attempt;
  int ret;

  if (info == NULL)
    {
      return -EINVAL;
    }

  memset(info, 0, sizeof(*info));

  dev = sdio_initialize(1);
  if (dev == NULL)
    {
      return -ENODEV;
    }

  SDIO_ATTACH(dev);
  SDIO_CLOCK(dev, CLOCK_IDMODE);

  /* SDHCI supplies the required initialization clocks when the ID-mode card
   * clock is enabled.  The K1 vendor MISC_INT sequence applies only to hosts
   * with MMC_CAP2_NO_SDIO, so SDH1 must not run it.
   */

  up_mdelay(1);

#ifdef CONFIG_K1_SDIO_WIFI_CCCR_ABORT_RESET
  ret = k1_sdio_wifi_abort_reset(dev);
  if (ret < 0)
    {
      /* Linux's mmc_sdio_pre_init() deliberately ignores sdio_reset()'s
       * return value before it issues CMD0/CMD5.  Preserve that behaviour:
       * record an unavailable early reset but still run ordinary card
       * enumeration.
       */

      k1_sdio_trace_device(k1_sdio_priv(dev), "CCCR_ABORT reset error=",
                           (uint32_t)-ret);
    }
#endif

  /* Select the card with the SDIO-prescribed CMD0/CMD5/CMD3/CMD7 sequence.
   * No I/O function is enabled here; function enable remains exclusively a
   * responsibility of a future RTL8852BS2 MAC driver.
   */

  ret = k1_sdio_wifi_command(dev, MMCSD_CMD0, 0);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(50);

  ret = k1_sdio_wifi_command(dev, SDIO_CMD5, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = SDIO_RECVR4(dev, SDIO_CMD5, &info->ocr);
  if (ret < 0)
    {
      return ret;
    }

  k1_sdio_trace_device(k1_sdio_priv(dev), "CMD5 probe R4=", info->ocr);

  info->function_count =
    (info->ocr & K1_SDIO_R4_FUNCTIONS_MASK) >>
    K1_SDIO_R4_FUNCTIONS_SHIFT;
  if (info->function_count == 0)
    {
      return -ENODEV;
    }

  voltage_window = info->ocr & K1_SDIO_R4_VOLTAGE_WINDOW_MASK;
  if (voltage_window == 0)
    {
      return -EINVAL;
    }

  /* Request the first advertised pair of supported voltage bits, matching
   * the standard NuttX SDIO probe policy.
   */

  for (attempt = 0; attempt < 24; attempt++)
    {
      if ((voltage_window & (1ul << attempt)) != 0)
        {
          voltage_window &= 3ul << attempt;
          break;
        }
    }

  k1_sdio_trace_device(k1_sdio_priv(dev), "CMD5 requested OCR=",
                       voltage_window);

  for (attempt = 0; attempt < K1_SDIO_WIFI_READY_TIMEOUT_MSEC; attempt++)
    {
      ret = k1_sdio_wifi_command(dev, SDIO_CMD5, voltage_window);
      if (ret < 0)
        {
          return ret;
        }

      ret = SDIO_RECVR4(dev, SDIO_CMD5, &response);
      if (ret < 0)
        {
          return ret;
        }

      if (attempt == 0 ||
          (response & K1_SDIO_R4_IO_READY) != 0 ||
          attempt + 1 == K1_SDIO_WIFI_READY_TIMEOUT_MSEC)
        {
          k1_sdio_trace_device(k1_sdio_priv(dev), "CMD5 ready R4=",
                               response);
        }

      if ((response & K1_SDIO_R4_IO_READY) != 0)
        {
          break;
        }

      up_mdelay(1);
    }

  if (attempt == K1_SDIO_WIFI_READY_TIMEOUT_MSEC)
    {
      return -ETIMEDOUT;
    }

  ret = k1_sdio_wifi_command(dev, SD_CMD3, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = SDIO_RECVR6(dev, SD_CMD3, &rca);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_command(dev, MMCSD_CMD7S, rca & 0xffff0000ul);
  if (ret < 0)
    {
      return ret;
    }

  ret = SDIO_RECVR1(dev, MMCSD_CMD7S, &response);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_BUS_IF, 0, &value);
  if (ret < 0)
    {
      return ret;
    }

  value &= ~SDIO_CCCR_BUS_IF_WIDTH_MASK;
  value |= SDIO_CCCR_BUS_IF_4_BITS;
  ret = k1_sdio_wifi_cmd52(dev, true, 0, SDIO_CCCR_BUS_IF, value, NULL);
  if (ret < 0)
    {
      return ret;
    }

  SDIO_WIDEBUS(dev, true);
  SDIO_CLOCK(dev, CLOCK_SD_TRANSFER_4BIT);

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_REV, 0,
                            &info->cccr_revision);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_SD_SPEC_REV, 0,
                            &info->sd_spec_revision);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_IOEN, 0,
                            &info->io_enable);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_IORDY, 0,
                            &info->io_ready);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_BUS_IF, 0,
                            &info->bus_interface);
  if (ret < 0)
    {
      return ret;
    }

  if ((info->bus_interface & SDIO_CCCR_BUS_IF_WIDTH_MASK) !=
      SDIO_CCCR_BUS_IF_4_BITS)
    {
      return -EIO;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_CARD_CAP, 0,
                            &info->card_capability);
  if (ret < 0)
    {
      return ret;
    }

  for (function = 1; function <= info->function_count; function++)
    {
      ret = k1_sdio_wifi_cmd52(dev, false, 0,
                                (function << SDIO_FBR_SHIFT) +
                                K1_SDIO_FBR_INTERFACE_CODE, 0,
                                &info->function_interface[function - 1]);
      if (ret < 0)
        {
          return ret;
        }
    }

  k1_sdio_priv(dev)->wifi_selected = true;
  k1_sdio_priv(dev)->wifi_functions = info->function_count;

#ifdef CONFIG_K1_SDIO_WIFI_TUNE_BEFORE_F1_ENABLE_DIAGNOSTIC
  /* Linux completes the card-wide timing transition before the Function 1
   * driver calls sdio_enable_func().  Keep this order restricted to the
   * HISR transport A/B so the normal wireless profile is unchanged.
   */

  ret = k1_sdio_wifi_enable_highspeed(dev, k1_sdio_priv(dev), info);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_enable_sdr104(dev, k1_sdio_priv(dev), info);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_tune_sdr104(dev, k1_sdio_priv(dev), info);
  if (ret < 0)
    {
      return ret;
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: k1_sdio_wifi_enable_function
 ****************************************************************************/

int k1_sdio_wifi_enable_function(uint8_t function, uint16_t blocksize,
                                 FAR struct k1_sdio_wifi_info_s *info)
{
  FAR struct sdio_dev_s *dev;
  FAR struct k1_sdio_dev_s *priv;
  uint8_t io_enable;
  uint8_t io_ready;
  unsigned int attempt;
  int ret;

  if (function == 0 || blocksize == 0 || info == NULL)
    {
      return -EINVAL;
    }

  dev = sdio_initialize(1);
  if (dev == NULL)
    {
      return -ENODEV;
    }

  priv = k1_sdio_priv(dev);
  if (!priv->wifi_selected || function > priv->wifi_functions)
    {
      return -EHOSTDOWN;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_IOEN, 0, &io_enable);
  if (ret < 0)
    {
      return ret;
    }

  io_enable |= 1u << function;
  ret = k1_sdio_wifi_cmd52(dev, true, 0, SDIO_CCCR_IOEN, io_enable, NULL);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < K1_SDIO_WIFI_READY_TIMEOUT_MSEC; attempt++)
    {
      ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_IORDY, 0,
                                &io_ready);
      if (ret < 0)
        {
          return ret;
        }

      if ((io_ready & (1u << function)) != 0)
        {
          info->io_enable = io_enable;
          info->io_ready = io_ready;
          break;
        }

      up_mdelay(1);
    }

  if (attempt == K1_SDIO_WIFI_READY_TIMEOUT_MSEC)
    {
      return -ETIMEDOUT;
    }

  /* The vendor driver enables the SDIO function before setting its block
   * size.  Some Realtek cards accept the FBR writes while disabled but do
   * not subsequently start a CMD53 data phase.
   */

  ret = k1_sdio_wifi_cmd52(dev, true, 0,
                            (function << SDIO_FBR_SHIFT) +
                            K1_SDIO_FBR_BLOCK_SIZE_LOW,
                            blocksize & UINT8_MAX, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, true, 0,
                            (function << SDIO_FBR_SHIFT) +
                            K1_SDIO_FBR_BLOCK_SIZE_HIGH,
                            blocksize >> 8, NULL);
  if (ret < 0)
    {
      return ret;
    }

#if defined(CONFIG_K1_SDIO_WIFI_LEGACY_CMD53_DIAGNOSTIC) || \
    defined(CONFIG_K1_SDIO_WIFI_LEGACY_CMD53_WRITE_PIO_DIAGNOSTIC) || \
    defined(CONFIG_K1_SDIO_WIFI_LEGACY_CMD53_WRITE_TX_DLINE_DIAGNOSTIC)
  /* Retain the four-bit transfer divider selected after CMD52 enumeration.
   * This profile isolates UHS timing without changing Function 1 state.
   */

#ifdef CONFIG_K1_SDIO_WIFI_LEGACY_CMD53_WRITE_TX_DLINE_DIAGNOSTIC
  /* The vendor K1 SDIO path sets this transmit delay-line state when it
   * enters its CMD53-capable tuning path.  Retain the legacy clock here so
   * this A/B isolates the output path from high-speed/UHS timing.
   */

  modifyreg32(K1_SDHC_REG(priv, TX_CONTROL), 0, K1_SDHC_TX_MUX_SELECT);
  modifyreg32(K1_SDHC_REG(priv, DLINE_CONFIG),
              K1_SDHC_DLINE_TX_REGISTER_MASK, 0);
  modifyreg32(K1_SDHC_REG(priv, DLINE_CONTROL),
              K1_SDHC_DLINE_CODE_MASK << K1_SDHC_DLINE_TX_CODE_SHIFT,
              K1_SDHC_DLINE_POWER_UP |
              ((uint32_t)K1_SDIO_MUSEPI_TX_DELAYCODE <<
               K1_SDHC_DLINE_TX_CODE_SHIFT));
  up_udelay(5);
  k1_sdio_trace_device(priv, "legacy CMD53 TX_CFG=",
                       getreg32(K1_SDHC_REG(priv, TX_CONTROL)));
  k1_sdio_trace_device(priv, "legacy CMD53 DLINE_CTRL=",
                       getreg32(K1_SDHC_REG(priv, DLINE_CONTROL)));
  k1_sdio_trace_device(priv, "legacy CMD53 DLINE_CFG=",
                       getreg32(K1_SDHC_REG(priv, DLINE_CONFIG)));
#endif

  k1_sdio_trace_device(priv, "K1 Wi-Fi: legacy CMD53 clock=",
                       getreg16(K1_SDHC_REG(priv, CLOCK_CONTROL)));
  return OK;
#endif

#ifndef CONFIG_K1_SDIO_WIFI_TUNE_BEFORE_F1_ENABLE_DIAGNOSTIC
  ret = k1_sdio_wifi_enable_highspeed(dev, priv, info);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_K1_SDIO_WIFI_HIGHSPEED_ONLY_DIAGNOSTIC
  k1_sdio_trace_device(priv, "K1 Wi-Fi: High-Speed-only clock=",
                       getreg16(K1_SDHC_REG(priv, CLOCK_CONTROL)));
  return OK;
#endif

  ret = k1_sdio_wifi_enable_sdr104(dev, priv, info);
  if (ret < 0)
    {
      return ret;
    }

  return k1_sdio_wifi_tune_sdr104(dev, priv, info);
#else
  return OK;
#endif
}

/****************************************************************************
 * Name: k1_sdio_wifi_recover_after_crc
 *
 * Description:
 *   Recover the already selected Wi-Fi card after an SDHCI data CRC error.
 *   K1 Linux withdraws SDR104 after a CRC error and selects a slower mode at
 *   the next card initialization.  A running RTL8852BS2 cannot be
 *   re-enumerated without losing its volatile firmware state, so switch its
 *   CCCR/host timing directly to High-Speed before an idempotent replay.
 ****************************************************************************/

int k1_sdio_wifi_recover_after_crc(void)
{
  FAR struct sdio_dev_s *dev;
  FAR struct k1_sdio_dev_s *priv;
  struct k1_sdio_wifi_info_s info;
  uint8_t io_ready;
  int ret;

  dev = sdio_initialize(1);
  if (dev == NULL)
    {
      return -ENODEV;
    }

  priv = k1_sdio_priv(dev);
  if (!priv->wifi_selected || priv->wifi_functions < 1)
    {
      return -EHOSTDOWN;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, 0, SDIO_CCCR_IORDY, 0, &io_ready);
  if (ret < 0)
    {
      return ret;
    }

  if ((io_ready & (1u << 1)) == 0)
    {
      return -EHOSTDOWN;
    }

  /* The K1 Linux host checks DAT0 separately from SDHCI DATA_INHIBIT.
   * A CRC recovery can release the controller state before the SDIO device
   * has released DAT0; starting CMD19 during that interval times out.
   */

  if (!k1_sdio_wait_clear(K1_SDHC_REG(priv, PRESENT_STATE),
                          K1_SDHC_PRESENT_DATA_INHIBIT,
                          K1_SDHC_COMMAND_TIMEOUT) ||
      !k1_sdio_wait_set(K1_SDHC_REG(priv, PRESENT_STATE),
                         K1_SDHC_PRESENT_DATA0_LEVEL,
                         K1_SDHC_COMMAND_TIMEOUT))
    {
      k1_sdio_trace_device(priv, "Wi-Fi CRC recovery busy=",
                           getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
      return -ETIMEDOUT;
    }

  memset(&info, 0, sizeof(info));
  k1_sdio_trace_device(priv, "Wi-Fi CRC fallback High-Speed", 0);
  ret = k1_sdio_wifi_enable_highspeed(dev, priv, &info);
  if (ret < 0)
    {
      k1_sdio_trace_device(priv, "Wi-Fi CRC fallback error=",
                           (uint32_t)-ret);
      return ret;
    }

  k1_sdio_trace_device(priv, "Wi-Fi CRC fallback speed=",
                       info.speed_control);
  return OK;
}

/****************************************************************************
 * Name: k1_sdio_wifi_f1_read_byte
 ****************************************************************************/

int k1_sdio_wifi_f1_read_byte(uint32_t address, FAR uint8_t *value)
{
  FAR struct sdio_dev_s *dev = sdio_initialize(1);
  unsigned int attempt;
  int ret;

  if (value == NULL || address > 0x1fffful)
    {
      return -EINVAL;
    }

  if (dev == NULL || !k1_sdio_priv(dev)->wifi_selected ||
      k1_sdio_priv(dev)->wifi_functions < 1)
    {
      return -EHOSTDOWN;
    }

  /* A Function 1 CMD52 read has no card-side write effect.  The K1 host can
   * report a transient command CRC/index error after a long CMD53 sequence.
   * Retry only after k1_sdio_wifi_cmd52() resets the host state.
   */

  for (attempt = 0; attempt <= K1_SDIO_WIFI_CMD52_READ_CRC_RETRIES;
       attempt++)
    {
      ret = k1_sdio_wifi_cmd52(dev, false, 1, address, 0, value);
      if (ret != -EILSEQ || attempt == K1_SDIO_WIFI_CMD52_READ_CRC_RETRIES)
        {
          return ret;
        }

      k1_sdio_trace_device(k1_sdio_priv(dev), "F1 CMD52 read CRC retry=",
                           attempt + 1);
      up_udelay(K1_SDIO_WIFI_CMD52_READ_RETRY_USEC);
    }

  return -EILSEQ;
}

/****************************************************************************
 * Name: k1_sdio_wifi_f1_write_byte
 ****************************************************************************/

int k1_sdio_wifi_f1_write_byte(uint32_t address, uint8_t value)
{
  FAR struct sdio_dev_s *dev = sdio_initialize(1);

  if (address > 0x1fffful)
    {
      return -EINVAL;
    }

  if (dev == NULL || !k1_sdio_priv(dev)->wifi_selected ||
      k1_sdio_priv(dev)->wifi_functions < 1)
    {
      return -EHOSTDOWN;
    }

  return k1_sdio_wifi_cmd52(dev, true, 1, address, value, NULL);
}

/****************************************************************************
 * Name: k1_sdio_wifi_read
 ****************************************************************************/

int k1_sdio_wifi_read(uint8_t function, uint32_t address, bool increment,
                      FAR uint8_t *buffer, size_t length)
{
  FAR struct sdio_dev_s *dev = sdio_initialize(1);

  if (dev == NULL || !k1_sdio_priv(dev)->wifi_selected)
    {
      return -EHOSTDOWN;
    }

  return k1_sdio_wifi_cmd53(dev, false, function, address, increment,
                            buffer, length,
                            K1_SDIO_CMD53_ROUTE_GENERIC);
}

/****************************************************************************
 * Name: k1_sdio_wifi_write
 ****************************************************************************/

int k1_sdio_wifi_write(uint8_t function, uint32_t address, bool increment,
                       FAR const uint8_t *buffer, size_t length)
{
  FAR struct sdio_dev_s *dev = sdio_initialize(1);

  /* Function 0 contains CCCR/FBR control registers.  The generic write
   * interface is intentionally restricted to device functions; the only
   * common-function CMD53 consumer is the read-only transport diagnostic.
   */

  if (function == 0)
    {
      return -EINVAL;
    }

  if (dev == NULL || !k1_sdio_priv(dev)->wifi_selected)
    {
      return -EHOSTDOWN;
    }

  return k1_sdio_wifi_cmd53(dev, true, function, address, increment,
                            (FAR uint8_t *)buffer, length,
                            K1_SDIO_CMD53_ROUTE_GENERIC);
}

/****************************************************************************
 * Name: k1_sdio_wifi_fwdl_write
 ****************************************************************************/

int k1_sdio_wifi_fwdl_write(uint32_t address, FAR const uint8_t *buffer,
                             size_t length)
{
  FAR struct sdio_dev_s *dev = sdio_initialize(1);

  if (dev == NULL || !k1_sdio_priv(dev)->wifi_selected)
    {
      return -EHOSTDOWN;
    }

  /* This is deliberately not a generic large CMD53 API.  The helper admits
   * only fixed-address Realtek F1 H2C FIFO packets from the full-download or
   * controlled runtime-command paths; the lower layer verifies the FIFO
   * length encoding before issuing CMD53.
   */

  return k1_sdio_wifi_cmd53(dev, true, 1, address, false,
                            (FAR uint8_t *)buffer, length,
                            K1_SDIO_CMD53_ROUTE_FWDL_FIFO);
}

/****************************************************************************
 * Name: k1_sdio_wifi_rxfifo_read
 *
 * Description:
 *   Read one announced receive aggregate out of the Wi-Fi function's fixed
 *   RX FIFO address.  The device reports the exact length to transfer in its
 *   own RX_REQ_LEN register and that length regularly exceeds the 512-byte
 *   CMD53 byte-mode limit, so the transfer is issued the way SDIO defines a
 *   large fixed-address read: block-mode CMD53s for the 512-byte-aligned
 *   part, then one byte-mode CMD53 for any remainder.  The address is held
 *   constant, so the device sees one continuous read of the aggregate it
 *   announced.
 *
 *   This is not a generic multi-block interface.  It accepts only a read of
 *   the Wi-Fi RX FIFO address on function 1, it never transfers more than
 *   the announced length, and each transaction stays within the bounce
 *   buffer the bounded CMD53 path already owns.
 *
 * Input Parameters:
 *   address - Must be the Wi-Fi RX FIFO address.
 *   buffer  - Receives the aggregate; must hold length bytes.
 *   length  - The length the device announced.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.  A failed transaction stops
 *   the read: the remainder of the aggregate is left in the device FIFO
 *   rather than retried, so a partial read stays visible to the caller.
 *
 ****************************************************************************/

int k1_sdio_wifi_rxfifo_read(uint32_t address, FAR uint8_t *buffer,
                             size_t length)
{
  FAR struct sdio_dev_s *dev = sdio_initialize(1);
  size_t offset = 0;
  size_t chunk;
  int ret;

  if (dev == NULL || !k1_sdio_priv(dev)->wifi_selected)
    {
      return -EHOSTDOWN;
    }

  if (buffer == NULL || length == 0 ||
      address != K1_SDIO_WIFI_RX_FIFO_ADDRESS)
    {
      return -EINVAL;
    }

  while (offset < length)
    {
      chunk = length - offset;
#ifdef K1_SDIO_WIFI_FWDL_BLOCK_MODE
      if (chunk >= K1_SDIO_CMD53_BLOCK_SIZE)
        {
          chunk /= K1_SDIO_CMD53_BLOCK_SIZE;
          if (chunk > K1_SDIO_CMD53_MAX_BLOCK_COUNT)
            {
              chunk = K1_SDIO_CMD53_MAX_BLOCK_COUNT;
            }

          chunk *= K1_SDIO_CMD53_BLOCK_SIZE;
        }
#else
      if (chunk > K1_SDIO_CMD53_MAX_BYTE_COUNT)
        {
          chunk = K1_SDIO_CMD53_MAX_BYTE_COUNT;
        }
#endif

      ret = k1_sdio_wifi_cmd53(dev, false, 1, address, false,
                               buffer + offset, chunk,
                               K1_SDIO_CMD53_ROUTE_RX_FIFO);
      if (ret < 0)
        {
          return ret;
        }

      offset += chunk;
    }

  return OK;
}
#endif

#endif /* CONFIG_K1_SDIO */
