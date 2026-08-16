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
#include <nuttx/sdio.h>
#include <nuttx/signal.h>

#include "hardware/k1_sdio.h"
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

#define K1_SDHC_IDMODE_DIVIDER               1023u
#define K1_SDHC_TRANSFER_DIVIDER              16u

#define K1_SDHC_RESET_TIMEOUT                 1000u
#define K1_SDHC_CLOCK_TIMEOUT                 1000u
#define K1_SDHC_COMMAND_TIMEOUT               1000u
#define K1_SDHC_DEFAULT_XFR_TIMEOUT           100u

#ifdef CONFIG_K1_SDIO_WIFI
#  define K1_SDIO_WIFI_READY_TIMEOUT_MSEC     100u
#  define K1_SDIO_R4_VOLTAGE_WINDOW_MASK      0x00fffffful
#  define K1_SDIO_R4_IO_READY                 (1ul << 27)
#  define K1_SDIO_R4_FUNCTIONS_SHIFT          28
#  define K1_SDIO_R4_FUNCTIONS_MASK           (7ul << 28)
#  define K1_SDIO_CMD52_WRITE                 (1ul << 31)
#  define K1_SDIO_CMD52_ADDRESS_SHIFT         9
#  define K1_SDIO_R5_CRC_ERROR                (1ul << 15)
#  define K1_SDIO_R5_ILLEGAL_COMMAND          (1ul << 14)
#  define K1_SDIO_R5_ERROR                    (1ul << 11)
#  define K1_SDIO_R5_FUNCTION_NUMBER          (1ul << 9)
#  define K1_SDIO_R5_OUT_OF_RANGE             (1ul << 8)
#  define K1_SDIO_FBR_INTERFACE_CODE          0x00u
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

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
};

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

static void k1_sdio_trace_state(FAR struct k1_sdio_dev_s *priv,
                                FAR const char *stage)
{
#ifdef CONFIG_K1_EARLY_BOOT_LOG
  k1_early_puts("K1 SDIO: state ");
  k1_early_puts(stage);
  k1_early_puts("\r\n");
  k1_sdio_trace("K1 eMMC: APMU AXI=", getreg32(K1_APMU_SDH_AXI_CLK_RST));
  k1_sdio_trace("K1 SDIO: clock gate=", getreg32(priv->clock_rst));
  k1_sdio_trace("K1 SDIO: PRESENT=",
                getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
  k1_sdio_trace("K1 SDIO: POWER=",
                getreg8(K1_SDHC_REG(priv, POWER_CONTROL)));
  k1_sdio_trace("K1 SDIO: CLOCK=",
                getreg32(K1_SDHC_REG(priv, CLOCK_CONTROL)));
  k1_sdio_trace("K1 SDIO: HOST=",
                getreg8(K1_SDHC_REG(priv, HOST_CONTROL)));
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

/****************************************************************************
 * Name: k1_sdio_reset
 ****************************************************************************/

static void k1_sdio_reset(FAR struct sdio_dev_s *dev)
{
  FAR struct k1_sdio_dev_s *priv = k1_sdio_priv(dev);
  uint32_t value;

  putreg32(0, K1_SDHC_REG(priv, INT_SIGNAL_ENABLE));
  putreg8(K1_SDHC_RESET_ALL, K1_SDHC_REG(priv, SOFTWARE_RESET));

  if (!k1_sdio_wait8_clear(K1_SDHC_REG(priv, SOFTWARE_RESET),
                           K1_SDHC_RESET_ALL,
                           K1_SDHC_RESET_TIMEOUT))
    {
      mcerr("ERROR: K1 %s reset timed out\n", priv->name);
    }

  /* Linux enables these K1-specific PHY and eMMC mode bits after every
   * complete SDHCI reset.  Pinmux and voltage state remain U-Boot-owned.
   */

  modifyreg32(K1_SDHC_REG(priv, PHY_CONTROL), 0,
              K1_SDHC_PHY_FUNCTION_ENABLE | K1_SDHC_PHY_PLL_LOCK);
  modifyreg32(K1_SDHC_REG(priv, PHY_PADCFG), K1_SDHC_PHY_DRIVE_SELECT_MASK,
              K1_SDHC_PHY_RX_BIAS_ENABLE | K1_SDHC_PHY_DRIVE_SELECT_4);
  if (priv->mmc_mode)
    {
      modifyreg32(K1_SDHC_REG(priv, MMC_CONTROL), 0,
                  K1_SDHC_MMC_CARD_MODE);
    }

  modifyreg32(K1_SDHC_REG(priv, LEGACY_CONTROL), 0,
              K1_SDHC_LEGACY_PAD_CLOCK_ON);
  modifyreg32(K1_SDHC_REG(priv, TX_CONTROL), 0,
              K1_SDHC_TX_INTERNAL_CLOCK_SELECT);

  /* MUSE Pi Pro routes eMMC I/O at a fixed 1.8 V.  U-Boot performs the
   * equivalent SDHCI power step after RESET_ALL; repeat it here because the
   * NuttX lower-half must not depend on U-Boot's register state.
   */

  putreg8(K1_SDHC_POWER_ON | K1_SDHC_POWER_180,
          K1_SDHC_REG(priv, POWER_CONTROL));
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
  if ((cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
    {
      k1_sdio_trace("K1 eMMC: data cmd=", cmd);
      k1_sdio_trace("K1 eMMC: data arg=", arg);
      k1_sdio_trace("K1 eMMC: data mode=", transfer);
      k1_sdio_trace("K1 eMMC: data command=", command);
      k1_sdio_trace("K1 eMMC: data block size=",
                    getreg16(K1_SDHC_REG(priv, BLOCK_SIZE)));
      k1_sdio_trace("K1 eMMC: data block count=",
                    getreg16(K1_SDHC_REG(priv, BLOCK_COUNT)));
      k1_sdio_trace("K1 eMMC: data present=",
                    getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
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

  putreg8(K1_SDHC_RESET_DATA, K1_SDHC_REG(priv, SOFTWARE_RESET));
  if (!k1_sdio_wait8_clear(K1_SDHC_REG(priv, SOFTWARE_RESET),
                           K1_SDHC_RESET_DATA,
                           K1_SDHC_RESET_TIMEOUT))
    {
      return -ETIMEDOUT;
    }

  priv->buffer = NULL;
  priv->remaining = 0;
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
          k1_sdio_trace("K1 eMMC: command error cmd=", cmd);
          k1_sdio_trace("K1 eMMC: command error status=", status);
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

  k1_sdio_trace("K1 eMMC: command timeout cmd=", cmd);
  k1_sdio_trace("K1 eMMC: command timeout status=",
                getreg32(K1_SDHC_REG(priv, INT_STATUS)));
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
            priv->timeout : K1_SDHC_DEFAULT_XFR_TIMEOUT;
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
          k1_sdio_trace("K1 eMMC: data error status=", status);
          putreg32(status, K1_SDHC_REG(priv, INT_STATUS));
          priv->waitevents = 0;
          return SDIOWAIT_ERROR;
        }

      if ((status & (K1_SDHC_INT_SPACE_AVAILABLE |
                     K1_SDHC_INT_DATA_AVAILABLE)) != 0)
        {
          if (!traced_ready)
            {
              k1_sdio_trace("K1 eMMC: data ready status=", status);
              k1_sdio_trace("K1 eMMC: data ready present=",
                            getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
              traced_ready = true;
            }

          putreg32(status & (K1_SDHC_INT_SPACE_AVAILABLE |
                             K1_SDHC_INT_DATA_AVAILABLE),
                   K1_SDHC_REG(priv, INT_STATUS));
        }

      k1_sdio_pio(priv, getreg32(K1_SDHC_REG(priv, PRESENT_STATE)));
      if ((status & K1_SDHC_INT_TRANSFER_COMPLETE) != 0)
        {
          putreg32(K1_SDHC_INT_TRANSFER_COMPLETE,
                   K1_SDHC_REG(priv, INT_STATUS));
          priv->waitevents = 0;
          return priv->remaining == 0 ? SDIOWAIT_TRANSFERDONE :
                                        SDIOWAIT_ERROR;
        }

      nxsig_usleep(1000);
    }

  priv->waitevents = 0;
  k1_sdio_trace("K1 eMMC: data timeout status=",
                getreg32(K1_SDHC_REG(priv, INT_STATUS)));
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
      /* Preserve U-Boot's SDH source selection and pinmux.  Only release the
       * documented APMU reset gates before resetting the selected SDHCI block.
       */

      modifyreg32(K1_APMU_SDH_AXI_CLK_RST, 0,
                  K1_APMU_SDH_AXI_RESET_DEASSERT |
                  K1_APMU_SDH_AXI_CLOCK_ENABLE);
      modifyreg32(priv->clock_rst, 0,
                  K1_APMU_SDH_RESET_DEASSERT | K1_APMU_SDH_CLOCK_ENABLE);
      k1_sdio_reset(&priv->dev);
      priv->initialized = true;
    }

  return &priv->dev;
}

#ifdef CONFIG_K1_SDIO_WIFI
static int k1_sdio_wifi_command(FAR struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t arg)
{
  int ret;

  ret = SDIO_SENDCMD(dev, cmd, arg);
  if (ret < 0)
    {
      return ret;
    }

  return SDIO_WAITRESPONSE(dev, cmd);
}

static int k1_sdio_wifi_cmd52(FAR struct sdio_dev_s *dev, bool write,
                               uint32_t address, uint8_t inb,
                               FAR uint8_t *outb)
{
  uint32_t arg;
  uint32_t response;
  int ret;

  arg = (address & 0x1fffful) << K1_SDIO_CMD52_ADDRESS_SHIFT;
  if (write)
    {
      arg |= K1_SDIO_CMD52_WRITE | inb;
    }

  ret = k1_sdio_wifi_command(dev, SDIO_CMD52, arg);
  if (ret < 0)
    {
      return ret;
    }

  ret = SDIO_RECVR5(dev, SDIO_CMD52, &response);
  if (ret < 0)
    {
      return ret;
    }

  if ((response & (K1_SDIO_R5_CRC_ERROR |
                   K1_SDIO_R5_ILLEGAL_COMMAND)) != 0)
    {
      return -EILSEQ;
    }

  if ((response & K1_SDIO_R5_ERROR) != 0)
    {
      return -EIO;
    }

  if ((response & (K1_SDIO_R5_FUNCTION_NUMBER |
                   K1_SDIO_R5_OUT_OF_RANGE)) != 0)
    {
      return -EINVAL;
    }

  if (outb != NULL)
    {
      *outb = response & UINT8_MAX;
    }

  return OK;
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

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_BUS_IF, 0, &value);
  if (ret < 0)
    {
      return ret;
    }

  value &= ~SDIO_CCCR_BUS_IF_WIDTH_MASK;
  value |= SDIO_CCCR_BUS_IF_4_BITS;
  ret = k1_sdio_wifi_cmd52(dev, true, SDIO_CCCR_BUS_IF, value, NULL);
  if (ret < 0)
    {
      return ret;
    }

  SDIO_WIDEBUS(dev, true);
  SDIO_CLOCK(dev, CLOCK_SD_TRANSFER_4BIT);

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_REV, 0,
                            &info->cccr_revision);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_SD_SPEC_REV, 0,
                            &info->sd_spec_revision);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_IOEN, 0,
                            &info->io_enable);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_IORDY, 0,
                            &info->io_ready);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_BUS_IF, 0,
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

  ret = k1_sdio_wifi_cmd52(dev, false, SDIO_CCCR_CARD_CAP, 0,
                            &info->card_capability);
  if (ret < 0)
    {
      return ret;
    }

  for (function = 1; function <= info->function_count; function++)
    {
      ret = k1_sdio_wifi_cmd52(dev, false,
                                (function << SDIO_FBR_SHIFT) +
                                K1_SDIO_FBR_INTERFACE_CODE, 0,
                                &info->function_interface[function - 1]);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}
#endif

#endif /* CONFIG_K1_SDIO */
