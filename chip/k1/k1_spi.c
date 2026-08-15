/****************************************************************************
 * vendor/spacemit/chips/k1/k1_spi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_SPI) && defined(CONFIG_K1_SPI3)

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_spi.h"
#include "k1_spi.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_SPI_BUS                       3
#define K1_SPI_FIFO_DEPTH                32u
#define K1_SPI_FIFO_THRESHOLD            (K1_SPI_FIFO_DEPTH / 2u)
#define K1_SPI_POLL_INTERVAL_USEC        1u
#define K1_SPI_DEFAULT_FREQUENCY          800000u
#define K1_SPI_DEFAULT_BITS                   8u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_spi_clock_s
{
  uint32_t frequency;
  uint8_t parent;
};

struct k1_spi_priv_s
{
  struct spi_dev_s dev;
  uintptr_t base;
  uintptr_t clock;
  mutex_t lock;
  uint32_t frequency;
  enum spi_mode_e mode;
  uint8_t nbits;
  bool configured;
  bool selected;
  bool mode_valid;
  bool bits_valid;
  bool faulted;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int k1_spi_lock(FAR struct spi_dev_s *dev, bool lock);
static void k1_spi_select(FAR struct spi_dev_s *dev, uint32_t devid,
                          bool selected);
static uint32_t k1_spi_setfrequency(FAR struct spi_dev_s *dev,
                                    uint32_t frequency);
static void k1_spi_setmode(FAR struct spi_dev_s *dev,
                           enum spi_mode_e mode);
static void k1_spi_setbits(FAR struct spi_dev_s *dev, int nbits);
static uint8_t k1_spi_status(FAR struct spi_dev_s *dev, uint32_t devid);
static uint32_t k1_spi_send(FAR struct spi_dev_s *dev, uint32_t wd);
static void k1_spi_exchange_words(FAR struct spi_dev_s *dev,
                                  FAR const void *txbuffer,
                                  FAR void *rxbuffer, size_t nwords);
#ifdef CONFIG_SPI_EXCHANGE
static void k1_spi_exchange(FAR struct spi_dev_s *dev,
                            FAR const void *txbuffer, FAR void *rxbuffer,
                            size_t nwords);
#else
static void k1_spi_sndblock(FAR struct spi_dev_s *dev,
                            FAR const void *txbuffer, size_t nwords);
static void k1_spi_recvblock(FAR struct spi_dev_s *dev,
                             FAR void *rxbuffer, size_t nwords);
#endif
static int k1_spi_registercallback(FAR struct spi_dev_s *dev,
                                   spi_mediachange_t callback,
                                   FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The K1 SSP3 has no serial-clock divider.  The APBC parent mux supplies
 * these seven exact rates, ordered from slowest to fastest.
 */

static const struct k1_spi_clock_s g_k1_spi_clocks[] =
{
  {  800000u, K1_SPI3_PARENT_800KHZ  },
  { 1600000u, K1_SPI3_PARENT_1P6MHZ  },
  { 3200000u, K1_SPI3_PARENT_3P2MHZ  },
  { 6400000u, K1_SPI3_PARENT_6P4MHZ  },
  {12800000u, K1_SPI3_PARENT_12P8MHZ },
  {25600000u, K1_SPI3_PARENT_25P6MHZ },
  {51200000u, K1_SPI3_PARENT_51P2MHZ },
};

static const struct spi_ops_s g_k1_spi_ops =
{
  .lock             = k1_spi_lock,
  .select           = k1_spi_select,
  .setfrequency     = k1_spi_setfrequency,
  .setmode          = k1_spi_setmode,
  .setbits          = k1_spi_setbits,
  .status           = k1_spi_status,
  .send             = k1_spi_send,
#ifdef CONFIG_SPI_EXCHANGE
  .exchange         = k1_spi_exchange,
#else
  .sndblock         = k1_spi_sndblock,
  .recvblock        = k1_spi_recvblock,
#endif
  .registercallback = k1_spi_registercallback,
};

static struct k1_spi_priv_s g_k1_spi3 =
{
  .dev =
  {
    .ops = &g_k1_spi_ops,
  },
  .base         = K1_SPI3_BASE,
  .clock        = K1_APBC_SPI3_CLK_RST,
  .lock         = NXMUTEX_INITIALIZER,
  .frequency    = K1_SPI_DEFAULT_FREQUENCY,
  .mode         = SPIDEV_MODE0,
  .nbits        = K1_SPI_DEFAULT_BITS,
  .mode_valid   = true,
  .bits_valid   = true,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct k1_spi_priv_s *
k1_spi_priv(FAR struct spi_dev_s *dev)
{
  return (FAR struct k1_spi_priv_s *)dev;
}

static inline uintptr_t k1_spi_reg(FAR const struct k1_spi_priv_s *priv,
                                   uintptr_t offset)
{
  return priv->base + offset;
}

static inline void k1_spi_unlock_io_power(void)
{
  /* One IO power-domain access is unlocked by each key sequence. */

  putreg32(K1_APBC_ASFAR_KEY, K1_APBC_ASFAR);
  putreg32(K1_APBC_ASSAR_KEY, K1_APBC_ASSAR);
}

static void k1_spi_set_header_power(void)
{
  /* GPIO75..78 are in the GPIO2 external IO domain.  A cleared V18EN bit
   * selects the MUSE Pi Pro 3.3 V header supply.
   */

  k1_spi_unlock_io_power();
  putreg32(0, K1_MFPR_IO_PWR_GPIO2);
}

static uint32_t k1_spi_mode_bits(enum spi_mode_e mode)
{
  switch (mode)
    {
      case SPIDEV_MODE0:
        return 0;

      case SPIDEV_MODE1:
        return K1_SPI_TOP_SPH;

      case SPIDEV_MODE2:
        return K1_SPI_TOP_SPO;

      case SPIDEV_MODE3:
        return K1_SPI_TOP_SPO | K1_SPI_TOP_SPH;

      default:
        return 0;
    }
}

static FAR const struct k1_spi_clock_s *
k1_spi_find_clock(uint32_t frequency)
{
  FAR const struct k1_spi_clock_s *choice;
  size_t index;

  choice = &g_k1_spi_clocks[0];
  for (index = 0;
       index < sizeof(g_k1_spi_clocks) / sizeof(g_k1_spi_clocks[0]);
       index++)
    {
      if (g_k1_spi_clocks[index].frequency > frequency)
        {
          break;
        }

      choice = &g_k1_spi_clocks[index];
    }

  return choice;
}

static void k1_spi_set_clock_parent(FAR struct k1_spi_priv_s *priv,
                                    uint8_t parent)
{
  uint32_t clock;

  clock = getreg32(priv->clock);
  clock &= ~K1_SPI3_CLK_PARENT_MASK;
  clock |= K1_SPI3_CLK_PARENT(parent) | K1_CLK_BUS_ENABLE |
           K1_CLK_FUNCTION_ENABLE;
  clock &= ~K1_CLK_RESET;
  putreg32(clock, priv->clock);
}

static void k1_spi_configure_pins(FAR struct k1_spi_priv_s *priv)
{
  uint32_t data_pad;
  uint32_t frm_pad;

  (void)priv;

  data_pad = K1_MFPR_MUX_MODE2 | K1_MFPR_EDGE_CLEAR |
             K1_MFPR_DRIVE_3V3_DS4;
  frm_pad = data_pad | K1_MFPR_PULL_UP;

  putreg32(data_pad, K1_MFPR_GPIO75); /* Pin 23: SCLK */
  putreg32(frm_pad, K1_MFPR_GPIO76);  /* Pin 24: FRM/CS0 */
  putreg32(data_pad, K1_MFPR_GPIO77); /* Pin 19: MOSI */
  putreg32(data_pad, K1_MFPR_GPIO78); /* Pin 21: MISO */
}

static void k1_spi_controller_reset(FAR struct k1_spi_priv_s *priv)
{
  putreg32(0, k1_spi_reg(priv, K1_SPI_TOP_CTRL_OFFSET));
  putreg32(K1_SPI_FIFO_TFT(K1_SPI_FIFO_THRESHOLD - 1u) |
           K1_SPI_FIFO_RFT(K1_SPI_FIFO_THRESHOLD - 1u),
           k1_spi_reg(priv, K1_SPI_FIFO_CTRL_OFFSET));
  putreg32(0, k1_spi_reg(priv, K1_SPI_INT_EN_OFFSET));
  putreg32(0, k1_spi_reg(priv, K1_SPI_TIMEOUT_OFFSET));
  putreg32(K1_SPI_STATUS_ERROR_MASK,
           k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
  priv->selected = false;
}

static int k1_spi_flush(FAR struct k1_spi_priv_s *priv)
{
  uint32_t status;
  unsigned int count;

  for (count = 0; count < K1_SPI_FIFO_DEPTH; count++)
    {
      status = getreg32(k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
      if ((status & K1_SPI_STATUS_RNE) == 0)
        {
          break;
        }

      (void)getreg32(k1_spi_reg(priv, K1_SPI_DATAR_OFFSET));
    }

  status = getreg32(k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
  if ((status & K1_SPI_STATUS_ERROR_MASK) != 0)
    {
      putreg32(status & K1_SPI_STATUS_ERROR_MASK,
               k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
      return -EIO;
    }

  return (status & K1_SPI_STATUS_RNE) != 0 ? -EIO : OK;
}

static int k1_spi_wait(FAR struct k1_spi_priv_s *priv, uint32_t wanted)
{
  uint32_t status;
  uint32_t elapsed;

  for (elapsed = 0; elapsed < CONFIG_K1_SPI_POLL_TIMEOUT_USEC;
       elapsed += K1_SPI_POLL_INTERVAL_USEC)
    {
      status = getreg32(k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
      if ((status & K1_SPI_STATUS_ERROR_MASK) != 0)
        {
          putreg32(status & K1_SPI_STATUS_ERROR_MASK,
                   k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
          return -EIO;
        }

      if ((status & wanted) == wanted)
        {
          return OK;
        }

      up_udelay(K1_SPI_POLL_INTERVAL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_spi_wait_idle(FAR struct k1_spi_priv_s *priv)
{
  uint32_t status;
  uint32_t elapsed;

  for (elapsed = 0; elapsed < CONFIG_K1_SPI_POLL_TIMEOUT_USEC;
       elapsed += K1_SPI_POLL_INTERVAL_USEC)
    {
      status = getreg32(k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
      if ((status & K1_SPI_STATUS_ERROR_MASK) != 0)
        {
          putreg32(status & K1_SPI_STATUS_ERROR_MASK,
                   k1_spi_reg(priv, K1_SPI_STATUS_OFFSET));
          return -EIO;
        }

      if ((status & K1_SPI_STATUS_BSY) == 0)
        {
          return OK;
        }

      up_udelay(K1_SPI_POLL_INTERVAL_USEC);
    }

  return -ETIMEDOUT;
}

static void k1_spi_abort(FAR struct k1_spi_priv_s *priv)
{
  k1_spi_controller_reset(priv);
  priv->faulted = true;
}

static int k1_spi_setup(FAR struct k1_spi_priv_s *priv)
{
  FAR const struct k1_spi_clock_s *clock;

  /* Follow the K1 APBC reset order: assert reset, select/enable the clock,
   * then release reset.
   */

  clock = k1_spi_find_clock(priv->frequency);
  putreg32(K1_CLK_RESET, priv->clock);
  putreg32(K1_CLK_RESET | K1_CLK_FUNCTION_ENABLE | K1_CLK_BUS_ENABLE |
           K1_SPI3_CLK_PARENT(clock->parent), priv->clock);
  putreg32(K1_CLK_FUNCTION_ENABLE | K1_CLK_BUS_ENABLE |
           K1_SPI3_CLK_PARENT(clock->parent), priv->clock);

  k1_spi_set_header_power();
  k1_spi_configure_pins(priv);
  k1_spi_controller_reset(priv);
  priv->configured = true;
  priv->faulted = false;
  return OK;
}

static int k1_spi_configure_transfer(FAR struct k1_spi_priv_s *priv)
{
  uint32_t control;
  int ret;

  if (!priv->configured)
    {
      ret = k1_spi_setup(priv);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (!priv->mode_valid || !priv->bits_valid)
    {
      return -ENOTSUP;
    }

  control = K1_SPI_TOP_FRF_MOTOROLA | k1_spi_mode_bits(priv->mode) |
            K1_SPI_TOP_DSS(priv->nbits);
  putreg32(control, k1_spi_reg(priv, K1_SPI_TOP_CTRL_OFFSET));
  return k1_spi_flush(priv);
}

static int k1_spi_transfer_word(FAR struct k1_spi_priv_s *priv,
                                uint8_t tx, FAR uint8_t *rx)
{
  int ret;

  ret = k1_spi_wait(priv, K1_SPI_STATUS_TNF);
  if (ret < 0)
    {
      return ret;
    }

  putreg32(tx, k1_spi_reg(priv, K1_SPI_DATAR_OFFSET));

  ret = k1_spi_wait(priv, K1_SPI_STATUS_RNE);
  if (ret < 0)
    {
      return ret;
    }

  if (rx != NULL)
    {
      *rx = (uint8_t)getreg32(k1_spi_reg(priv, K1_SPI_DATAR_OFFSET));
    }
  else
    {
      (void)getreg32(k1_spi_reg(priv, K1_SPI_DATAR_OFFSET));
    }

  return OK;
}

/****************************************************************************
 * SPI Operations
 ****************************************************************************/

static int k1_spi_lock(FAR struct spi_dev_s *dev, bool lock)
{
  FAR struct k1_spi_priv_s *priv;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_spi_priv(dev);
  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

static void k1_spi_select(FAR struct spi_dev_s *dev, uint32_t devid,
                          bool selected)
{
  FAR struct k1_spi_priv_s *priv;
  uint32_t control;
  int ret;

  (void)devid;

  if (dev == NULL)
    {
      return;
    }

  priv = k1_spi_priv(dev);

  if (selected)
    {
      if (priv->selected)
        {
          return;
        }

      if (priv->faulted)
        {
          k1_spi_controller_reset(priv);
          priv->faulted = false;
        }

      ret = k1_spi_configure_transfer(priv);
      if (ret < 0)
        {
          priv->faulted = true;
          spierr("ERROR: K1 SPI3 configuration failed: %d\n", ret);
          return;
        }

      control = getreg32(k1_spi_reg(priv, K1_SPI_TOP_CTRL_OFFSET));
      control |= K1_SPI_TOP_HOLD_FRAME_LOW | K1_SPI_TOP_SSE;
      putreg32(control, k1_spi_reg(priv, K1_SPI_TOP_CTRL_OFFSET));
      priv->selected = true;
      return;
    }

  if (!priv->selected)
    {
      return;
    }

  ret = k1_spi_wait_idle(priv);
  if (ret < 0)
    {
      spierr("ERROR: K1 SPI3 idle wait failed: %d\n", ret);
      priv->faulted = true;
    }

  control = getreg32(k1_spi_reg(priv, K1_SPI_TOP_CTRL_OFFSET));
  control &= ~(K1_SPI_TOP_HOLD_FRAME_LOW | K1_SPI_TOP_SSE);
  putreg32(control, k1_spi_reg(priv, K1_SPI_TOP_CTRL_OFFSET));
  priv->selected = false;
}

static uint32_t k1_spi_setfrequency(FAR struct spi_dev_s *dev,
                                    uint32_t frequency)
{
  FAR struct k1_spi_priv_s *priv;
  FAR const struct k1_spi_clock_s *choice;

  if (dev == NULL)
    {
      return 0;
    }

  priv = k1_spi_priv(dev);
  if (frequency == 0 || priv->selected)
    {
      return priv->frequency;
    }

  choice = k1_spi_find_clock(frequency);

  if (priv->frequency != choice->frequency)
    {
      k1_spi_set_clock_parent(priv, choice->parent);
      priv->frequency = choice->frequency;
    }

  return priv->frequency;
}

static void k1_spi_setmode(FAR struct spi_dev_s *dev,
                           enum spi_mode_e mode)
{
  FAR struct k1_spi_priv_s *priv;

  if (dev == NULL)
    {
      return;
    }

  priv = k1_spi_priv(dev);
  if (mode > SPIDEV_MODE3)
    {
      priv->mode_valid = false;
      spierr("ERROR: K1 SPI3 supports only modes 0 through 3\n");
      return;
    }

  priv->mode = mode;
  priv->mode_valid = true;
}

static void k1_spi_setbits(FAR struct spi_dev_s *dev, int nbits)
{
  FAR struct k1_spi_priv_s *priv;

  if (dev == NULL)
    {
      return;
    }

  priv = k1_spi_priv(dev);
  if (nbits != K1_SPI_DEFAULT_BITS)
    {
      priv->bits_valid = false;
      spierr("ERROR: K1 SPI3 currently supports only 8-bit words\n");
      return;
    }

  priv->nbits = nbits;
  priv->bits_valid = true;
}

static uint8_t k1_spi_status(FAR struct spi_dev_s *dev, uint32_t devid)
{
  (void)dev;
  (void)devid;
  return SPI_STATUS_PRESENT;
}

static uint32_t k1_spi_send(FAR struct spi_dev_s *dev, uint32_t wd)
{
  FAR struct k1_spi_priv_s *priv;
  uint8_t rx = 0;
  int ret;

  if (dev == NULL)
    {
      return 0;
    }

  priv = k1_spi_priv(dev);
  if (!priv->selected || !priv->mode_valid || !priv->bits_valid)
    {
      return 0;
    }

  ret = k1_spi_transfer_word(priv, (uint8_t)wd, &rx);
  if (ret < 0)
    {
      spierr("ERROR: K1 SPI3 word transfer failed: %d\n", ret);
      k1_spi_abort(priv);
      return 0;
    }

  return rx;
}

static void k1_spi_exchange_words(FAR struct spi_dev_s *dev,
                                  FAR const void *txbuffer,
                                  FAR void *rxbuffer, size_t nwords)
{
  FAR struct k1_spi_priv_s *priv;
  FAR const uint8_t *tx;
  FAR uint8_t *rx;
  size_t index;

  if (dev == NULL)
    {
      return;
    }

  priv = k1_spi_priv(dev);
  tx = (FAR const uint8_t *)txbuffer;
  rx = (FAR uint8_t *)rxbuffer;

  if (!priv->selected || !priv->mode_valid || !priv->bits_valid)
    {
      if (rx != NULL)
        {
          for (index = 0; index < nwords; index++)
            {
              rx[index] = 0;
            }
        }

      return;
    }

  for (index = 0; index < nwords; index++)
    {
      int ret = k1_spi_transfer_word(priv, tx == NULL ? 0 : tx[index],
                                     rx == NULL ? NULL : &rx[index]);

      if (ret < 0)
        {
          spierr("ERROR: K1 SPI3 exchange failed: %d\n", ret);
          k1_spi_abort(priv);

          if (rx != NULL)
            {
              for (; index < nwords; index++)
                {
                  rx[index] = 0;
                }
            }

          return;
        }
    }
}

#ifdef CONFIG_SPI_EXCHANGE
static void k1_spi_exchange(FAR struct spi_dev_s *dev,
                            FAR const void *txbuffer, FAR void *rxbuffer,
                            size_t nwords)
{
  k1_spi_exchange_words(dev, txbuffer, rxbuffer, nwords);
}
#else
static void k1_spi_sndblock(FAR struct spi_dev_s *dev,
                            FAR const void *txbuffer, size_t nwords)
{
  k1_spi_exchange_words(dev, txbuffer, NULL, nwords);
}

static void k1_spi_recvblock(FAR struct spi_dev_s *dev,
                             FAR void *rxbuffer, size_t nwords)
{
  k1_spi_exchange_words(dev, NULL, rxbuffer, nwords);
}
#endif

static int k1_spi_registercallback(FAR struct spi_dev_s *dev,
                                   spi_mediachange_t callback,
                                   FAR void *arg)
{
  (void)dev;
  (void)callback;
  (void)arg;
  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct spi_dev_s *k1_spibus_initialize(int bus)
{
  FAR struct k1_spi_priv_s *priv;
  int ret;

  if (bus != K1_SPI_BUS)
    {
      return NULL;
    }

  priv = &g_k1_spi3;
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return NULL;
    }

  if (!priv->configured)
    {
      ret = k1_spi_setup(priv);
    }

  nxmutex_unlock(&priv->lock);
  return ret < 0 ? NULL : &priv->dev;
}

#endif /* CONFIG_K1_SPI && CONFIG_K1_SPI3 */
