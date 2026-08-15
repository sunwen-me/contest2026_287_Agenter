/****************************************************************************
 * vendor/spacemit/chips/k1/k1_i2c.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_I2C) && defined(CONFIG_K1_I2C2)

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_i2c.h"
#include "k1_i2c.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_I2C_BUS                       2
#define K1_I2C_POLL_INTERVAL_USEC        10u
#define K1_I2C_BUS_RESET_CLOCKS_MAX      9u

#define K1_I2C_SUPPORTED_FLAGS           \
  (I2C_M_READ | I2C_M_TEN | I2C_M_NOSTOP | I2C_M_NOSTART)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_i2c_priv_s
{
  struct i2c_master_s dev;
  uintptr_t base;
  uintptr_t clock;
  uintptr_t scl_mfpr;
  uintptr_t sda_mfpr;
  mutex_t lock;
  uint32_t frequency;
  bool configured;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int k1_i2c_transfer(FAR struct i2c_master_s *dev,
                           FAR struct i2c_msg_s *msgs, int count);
#ifdef CONFIG_I2C_RESET
static int k1_i2c_reset(FAR struct i2c_master_s *dev);
#endif
static int k1_i2c_setup(FAR struct i2c_master_s *dev);
static int k1_i2c_shutdown(FAR struct i2c_master_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2c_ops_s g_k1_i2c_ops =
{
  .transfer = k1_i2c_transfer,
#ifdef CONFIG_I2C_RESET
  .reset    = k1_i2c_reset,
#endif
  .setup    = k1_i2c_setup,
  .shutdown = k1_i2c_shutdown,
};

static struct k1_i2c_priv_s g_k1_i2c2 =
{
  .dev =
  {
    .ops = &g_k1_i2c_ops,
  },
  .base     = K1_I2C2_BASE,
  .clock    = K1_APBC_I2C2_CLK_RST,
  .scl_mfpr = K1_MFPR_GPIO84,
  .sda_mfpr = K1_MFPR_GPIO85,
  .lock     = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct k1_i2c_priv_s *
k1_i2c_priv(FAR struct i2c_master_s *dev)
{
  return (FAR struct k1_i2c_priv_s *)dev;
}

static inline uintptr_t k1_i2c_reg(FAR const struct k1_i2c_priv_s *priv,
                                   uintptr_t offset)
{
  return priv->base + offset;
}

static uint32_t k1_i2c_control(FAR const struct k1_i2c_priv_s *priv,
                               bool enable, bool start, bool stop,
                               bool nack)
{
  uint32_t control;

  control = K1_I2C_ICR_GCD | K1_I2C_ICR_SCLE | K1_I2C_ICR_MSDE;

  if (priv->frequency == I2C_SPEED_FAST)
    {
      control |= K1_I2C_ICR_FAST;
    }

  if (enable)
    {
      control |= K1_I2C_ICR_IUE;
    }

  if (start)
    {
      control |= K1_I2C_ICR_START;
    }

  if (stop)
    {
      control |= K1_I2C_ICR_STOP;
    }

  if (nack)
    {
      control |= K1_I2C_ICR_ACKNAK;
    }

  return control;
}

static void k1_i2c_controller_reset(FAR struct k1_i2c_priv_s *priv)
{
  putreg32(K1_I2C_ICR_UR, k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));
  up_udelay(5);
  putreg32(0, k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));
}

static void k1_i2c_configure_controller(FAR struct k1_i2c_priv_s *priv)
{
  uint32_t reset;

  priv->frequency = 0;

  putreg32(k1_i2c_control(priv, false, false, false, false),
           k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));

  /* K1 delays repeated START when its SDA glitch fix is enabled. */

  reset = getreg32(k1_i2c_reg(priv, K1_I2C_IRCR_OFFSET));
  reset |= K1_I2C_IRCR_SDA_GLITCH_DISABLE;
  putreg32(reset, k1_i2c_reg(priv, K1_I2C_IRCR_OFFSET));

  putreg32(K1_I2C_ISR_INT_STATUS_MASK,
           k1_i2c_reg(priv, K1_I2C_ISR_OFFSET));
}

static int k1_i2c_setup_locked(FAR struct k1_i2c_priv_s *priv)
{
  uint32_t pad;

  /* The APBC sequence is the one used by the vendor U-Boot I2C driver:
   * assert reset, enable bus/function clocks, then release reset.
   */

  putreg32(K1_CLK_RESET, priv->clock);
  putreg32(K1_CLK_RESET | K1_CLK_FUNCTION_ENABLE | K1_CLK_BUS_ENABLE,
           priv->clock);
  putreg32(K1_CLK_FUNCTION_ENABLE | K1_CLK_BUS_ENABLE, priv->clock);

  /* GPIO84/85 are the MUSE Pi Pro's 1.8 V I2C2 pins.  Their DTS pinctrl
   * entries select mux mode 4 with an internal pull-up and DS0 drive.
   */

  pad = K1_MFPR_MUX_MODE4 | K1_MFPR_EDGE_CLEAR | K1_MFPR_PULL_UP;
  putreg32(pad, priv->scl_mfpr);
  putreg32(pad, priv->sda_mfpr);

  k1_i2c_controller_reset(priv);
  k1_i2c_configure_controller(priv);
  priv->configured = true;
  return OK;
}

static void k1_i2c_disable_locked(FAR struct k1_i2c_priv_s *priv)
{
  putreg32(k1_i2c_control(priv, false, false, false, false),
           k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));
  priv->frequency = 0;
}

static int k1_i2c_recover_bus_locked(FAR struct k1_i2c_priv_s *priv)
{
  uint32_t monitor;
  uint32_t reset;
  unsigned int clocks;

  monitor = getreg32(k1_i2c_reg(priv, K1_I2C_IBMR_OFFSET));
  if ((monitor & (K1_I2C_IBMR_SDA | K1_I2C_IBMR_SCL)) ==
      (K1_I2C_IBMR_SDA | K1_I2C_IBMR_SCL))
    {
      return OK;
    }

  for (clocks = 0; clocks < K1_I2C_BUS_RESET_CLOCKS_MAX; clocks++)
    {
      reset = getreg32(k1_i2c_reg(priv, K1_I2C_IRCR_OFFSET));
      reset &= ~K1_I2C_IRCR_RESET_CLOCKS_MASK;
      reset |= 1;
      putreg32(reset, k1_i2c_reg(priv, K1_I2C_IRCR_OFFSET));
      putreg32(K1_I2C_ICR_RSTREQ,
               k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));
      up_udelay(30);

      monitor = getreg32(k1_i2c_reg(priv, K1_I2C_IBMR_OFFSET));
      if ((monitor & (K1_I2C_IBMR_SDA | K1_I2C_IBMR_SCL)) ==
          (K1_I2C_IBMR_SDA | K1_I2C_IBMR_SCL))
        {
          return OK;
        }
    }

  return -EBUSY;
}

static int k1_i2c_reset_locked(FAR struct k1_i2c_priv_s *priv)
{
  int ret;

  k1_i2c_controller_reset(priv);
  up_udelay(10);
  ret = k1_i2c_recover_bus_locked(priv);
  k1_i2c_configure_controller(priv);
  priv->configured = true;
  return ret;
}

static int k1_i2c_wait_event(FAR struct k1_i2c_priv_s *priv,
                             uint32_t event, FAR uint32_t *status)
{
  uint32_t value;
  uint32_t elapsed;

  for (elapsed = 0; elapsed < CONFIG_K1_I2C_POLL_TIMEOUT_USEC;
       elapsed += K1_I2C_POLL_INTERVAL_USEC)
    {
      value = getreg32(k1_i2c_reg(priv, K1_I2C_ISR_OFFSET));
      if ((value & K1_I2C_ISR_INT_STATUS_MASK) != 0)
        {
          putreg32(value & K1_I2C_ISR_INT_STATUS_MASK,
                   k1_i2c_reg(priv, K1_I2C_ISR_OFFSET));
        }

      if (status != NULL)
        {
          *status = value;
        }

      if ((value & K1_I2C_ISR_ALD) != 0)
        {
          return -EAGAIN;
        }

      if ((value & (K1_I2C_ISR_BED | K1_I2C_ISR_RXOV)) != 0)
        {
          return -EIO;
        }

      if ((value & event) != 0)
        {
          return OK;
        }

      up_udelay(K1_I2C_POLL_INTERVAL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_i2c_wait_bus_idle(FAR struct k1_i2c_priv_s *priv)
{
  uint32_t status;
  uint32_t elapsed;

  for (elapsed = 0; elapsed < CONFIG_K1_I2C_POLL_TIMEOUT_USEC;
       elapsed += K1_I2C_POLL_INTERVAL_USEC)
    {
      status = getreg32(k1_i2c_reg(priv, K1_I2C_ISR_OFFSET));
      if ((status & (K1_I2C_ISR_UB | K1_I2C_ISR_IBB)) == 0)
        {
          if ((status & K1_I2C_ISR_INT_STATUS_MASK) != 0)
            {
              putreg32(status & K1_I2C_ISR_INT_STATUS_MASK,
                       k1_i2c_reg(priv, K1_I2C_ISR_OFFSET));
            }

          return OK;
        }

      up_udelay(K1_I2C_POLL_INTERVAL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_i2c_set_frequency_locked(FAR struct k1_i2c_priv_s *priv,
                                        uint32_t frequency)
{
  if (frequency != I2C_SPEED_STANDARD && frequency != I2C_SPEED_FAST)
    {
      return -EINVAL;
    }

  if (priv->frequency != frequency)
    {
      priv->frequency = frequency;
      putreg32(k1_i2c_control(priv, true, false, false, false),
               k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));
    }

  return OK;
}

static int k1_i2c_send_byte(FAR struct k1_i2c_priv_s *priv, uint8_t byte,
                            bool start, bool stop)
{
  uint32_t status;
  int ret;

  putreg32(byte, k1_i2c_reg(priv, K1_I2C_IDBR_OFFSET));
  putreg32(k1_i2c_control(priv, true, start, stop, false) |
           K1_I2C_ICR_TB,
           k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));

  ret = k1_i2c_wait_event(priv, K1_I2C_ISR_ITE, &status);
  if (ret < 0)
    {
      return ret;
    }

  return (status & K1_I2C_ISR_ACKNAK) != 0 ? -ENXIO : OK;
}

static int k1_i2c_read_byte(FAR struct k1_i2c_priv_s *priv,
                            FAR uint8_t *byte, bool stop, bool nack)
{
  int ret;

  putreg32(k1_i2c_control(priv, true, false, stop, nack) |
           K1_I2C_ICR_TB,
           k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));

  ret = k1_i2c_wait_event(priv, K1_I2C_ISR_IRF, NULL);
  if (ret < 0)
    {
      return ret;
    }

  *byte = (uint8_t)getreg32(k1_i2c_reg(priv, K1_I2C_IDBR_OFFSET));
  return OK;
}

static int k1_i2c_transfer_message(FAR struct k1_i2c_priv_s *priv,
                                   FAR struct i2c_msg_s *msg)
{
  bool read;
  bool nostop;
  ssize_t index;
  int ret;

  read = (msg->flags & I2C_M_READ) != 0;
  nostop = (msg->flags & I2C_M_NOSTOP) != 0;

  ret = k1_i2c_send_byte(priv, (msg->addr << 1) | (read ? 1 : 0), true,
                          false);
  if (ret < 0)
    {
      return ret;
    }

  if (read)
    {
      for (index = 0; index < msg->length; index++)
        {
          bool last = index == msg->length - 1;

          ret = k1_i2c_read_byte(priv, &msg->buffer[index], last && !nostop,
                                 last);
          if (ret < 0)
            {
              return ret;
            }
        }
    }
  else
    {
      for (index = 0; index < msg->length; index++)
        {
          bool last = index == msg->length - 1;

          ret = k1_i2c_send_byte(priv, msg->buffer[index], false,
                                 last && !nostop);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return OK;
}

static int k1_i2c_validate_messages(FAR struct i2c_msg_s *msgs, int count)
{
  int index;

  if (msgs == NULL || count <= 0)
    {
      return -EINVAL;
    }

  for (index = 0; index < count; index++)
    {
      FAR struct i2c_msg_s *msg = &msgs[index];

      if ((msg->flags & ~K1_I2C_SUPPORTED_FLAGS) != 0 ||
          (msg->flags & (I2C_M_TEN | I2C_M_NOSTART)) != 0)
        {
          return -ENOTSUP;
        }

      if (msg->addr > 0x7f || msg->buffer == NULL || msg->length <= 0)
        {
          return -EINVAL;
        }

      if (msg->frequency != I2C_SPEED_STANDARD &&
          msg->frequency != I2C_SPEED_FAST)
        {
          return -EINVAL;
        }

      if ((msg->flags & I2C_M_NOSTOP) != 0)
        {
          if (index == count - 1 ||
              msgs[index + 1].frequency != msg->frequency)
            {
              return -EINVAL;
            }
        }
    }

  return OK;
}

/****************************************************************************
 * I2C Operations
 ****************************************************************************/

static int k1_i2c_transfer(FAR struct i2c_master_s *dev,
                           FAR struct i2c_msg_s *msgs, int count)
{
  FAR struct k1_i2c_priv_s *priv;
  int index;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_i2c_priv(dev);

  ret = k1_i2c_validate_messages(msgs, count);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->configured)
    {
      ret = k1_i2c_setup_locked(priv);
      if (ret < 0)
        {
          goto out;
        }
    }

  ret = k1_i2c_set_frequency_locked(priv, msgs[0].frequency);
  if (ret < 0)
    {
      goto recover;
    }

  ret = k1_i2c_wait_bus_idle(priv);
  if (ret < 0)
    {
      goto recover;
    }

  for (index = 0; index < count; index++)
    {
      ret = k1_i2c_set_frequency_locked(priv, msgs[index].frequency);
      if (ret < 0)
        {
          goto recover;
        }

      ret = k1_i2c_transfer_message(priv, &msgs[index]);
      if (ret < 0)
        {
          goto recover;
        }

      if ((msgs[index].flags & I2C_M_NOSTOP) == 0)
        {
          ret = k1_i2c_wait_bus_idle(priv);
          if (ret < 0)
            {
              goto recover;
            }
        }
    }

  k1_i2c_disable_locked(priv);
  goto out;

recover:
  k1_i2c_reset_locked(priv);
out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

#ifdef CONFIG_I2C_RESET
static int k1_i2c_reset(FAR struct i2c_master_s *dev)
{
  FAR struct k1_i2c_priv_s *priv;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_i2c_priv(dev);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_i2c_reset_locked(priv);
  nxmutex_unlock(&priv->lock);
  return ret;
}
#endif

static int k1_i2c_setup(FAR struct i2c_master_s *dev)
{
  FAR struct k1_i2c_priv_s *priv;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_i2c_priv(dev);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_i2c_setup_locked(priv);
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int k1_i2c_shutdown(FAR struct i2c_master_s *dev)
{
  FAR struct k1_i2c_priv_s *priv;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_i2c_priv(dev);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  putreg32(0, k1_i2c_reg(priv, K1_I2C_ICR_OFFSET));
  priv->frequency = 0;
  priv->configured = false;
  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct i2c_master_s *k1_i2cbus_initialize(int bus)
{
  FAR struct k1_i2c_priv_s *priv;
  int ret;

  if (bus != K1_I2C_BUS)
    {
      return NULL;
    }

  priv = &g_k1_i2c2;
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return NULL;
    }

  if (!priv->configured)
    {
      ret = k1_i2c_setup_locked(priv);
    }

  nxmutex_unlock(&priv->lock);
  return ret < 0 ? NULL : &priv->dev;
}

#endif /* CONFIG_K1_I2C && CONFIG_K1_I2C2 */
