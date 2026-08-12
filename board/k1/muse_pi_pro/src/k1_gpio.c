/****************************************************************************
 * vendor/spacemit/boards/k1/muse_pi_pro/src/k1_gpio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/ioexpander/gpio.h>

#include "riscv_internal.h"
#include "hardware/k1_gpio.h"

#if defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_GPIO_PIN_COUNT 26

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_gpio_dev_s
{
  struct gpio_dev_s gpio;
  uintptr_t mfpr;
  uintptr_t bank;
  uint32_t mask;
  uint32_t pad;
  uint8_t gpio_number;
  uint8_t header_pin;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int k1_gpio_read(FAR struct gpio_dev_s *dev, FAR bool *value);
static int k1_gpio_write(FAR struct gpio_dev_s *dev, bool value);
static int k1_gpio_attach(FAR struct gpio_dev_s *dev,
                          pin_interrupt_t callback);
static int k1_gpio_enable(FAR struct gpio_dev_s *dev, bool enable);
static int k1_gpio_setpintype(FAR struct gpio_dev_s *dev,
                              enum gpio_pintype_e pintype);
static int k1_gpio_setdebounce(FAR struct gpio_dev_s *dev,
                               unsigned long duration);
static int k1_gpio_setmask(FAR struct gpio_dev_s *dev, bool enable);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gpio_operations_s g_k1_gpio_ops =
{
  .go_read        = k1_gpio_read,
  .go_write       = k1_gpio_write,
  .go_attach      = k1_gpio_attach,
  .go_enable      = k1_gpio_enable,
  .go_setpintype  = k1_gpio_setpintype,
  .go_setdebounce = k1_gpio_setdebounce,
  .go_setmask     = k1_gpio_setmask,
};

/* Keep GPIO49 first: /dev/gpio0 was validated on the real board and is part
 * of the existing bring-up interface.  The remaining entries follow the
 * physical 40-pin header order.
 */

#define K1_GPIO_DESC(_gpio, _header, _mfpr, _bank, _bit, _mux, _drive) \
  {                                                                     \
    .mfpr        = (_mfpr),                                             \
    .bank        = (_bank),                                             \
    .mask        = (1ul << (_bit)),                                    \
    .pad         = ((_mux) | K1_MFPR_EDGE_CLEAR | (_drive)),           \
    .gpio_number = (_gpio),                                             \
    .header_pin  = (_header),                                           \
  }

static struct k1_gpio_dev_s g_k1gpio[K1_GPIO_PIN_COUNT] =
{
  K1_GPIO_DESC(49, 22, K1_MFPR_GPIO49, K1_GPIO_BANK1_BASE, 17,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(41,  3, K1_MFPR_GPIO41, K1_GPIO_BANK1_BASE,  9,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(40,  5, K1_MFPR_GPIO40, K1_GPIO_BANK1_BASE,  8,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(70,  7, K1_MFPR_GPIO70, K1_GPIO_BANK2_BASE,  6,
               K1_MFPR_MUX_MODE1, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(71, 11, K1_MFPR_GPIO71, K1_GPIO_BANK2_BASE,  7,
               K1_MFPR_MUX_MODE1, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(74, 12, K1_MFPR_GPIO74, K1_GPIO_BANK2_BASE, 10,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(72, 13, K1_MFPR_GPIO72, K1_GPIO_BANK2_BASE,  8,
               K1_MFPR_MUX_MODE1, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(73, 15, K1_MFPR_GPIO73, K1_GPIO_BANK2_BASE,  9,
               K1_MFPR_MUX_MODE1, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(91, 16, K1_MFPR_GPIO91, K1_GPIO_BANK2_BASE, 27,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(92, 18, K1_MFPR_GPIO92, K1_GPIO_BANK2_BASE, 28,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(77, 19, K1_MFPR_GPIO77, K1_GPIO_BANK2_BASE, 13,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(78, 21, K1_MFPR_GPIO78, K1_GPIO_BANK2_BASE, 14,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(75, 23, K1_MFPR_GPIO75, K1_GPIO_BANK2_BASE, 11,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(76, 24, K1_MFPR_GPIO76, K1_GPIO_BANK2_BASE, 12,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(50, 26, K1_MFPR_GPIO50, K1_GPIO_BANK1_BASE, 18,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(39, 27, K1_MFPR_GPIO39, K1_GPIO_BANK1_BASE,  7,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(38, 28, K1_MFPR_GPIO38, K1_GPIO_BANK1_BASE,  6,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(51, 29, K1_MFPR_GPIO51, K1_GPIO_BANK1_BASE, 19,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(52, 31, K1_MFPR_GPIO52, K1_GPIO_BANK1_BASE, 20,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(34, 32, K1_MFPR_GPIO34, K1_GPIO_BANK1_BASE,  2,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(47, 33, K1_MFPR_GPIO47, K1_GPIO_BANK1_BASE, 15,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(48, 35, K1_MFPR_GPIO48, K1_GPIO_BANK1_BASE, 16,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_3V3_DS1),
  K1_GPIO_DESC(35, 36, K1_MFPR_GPIO35, K1_GPIO_BANK1_BASE,  3,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(33, 37, K1_MFPR_GPIO33, K1_GPIO_BANK1_BASE,  1,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(46, 38, K1_MFPR_GPIO46, K1_GPIO_BANK1_BASE, 14,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
  K1_GPIO_DESC(37, 40, K1_MFPR_GPIO37, K1_GPIO_BANK1_BASE,  5,
               K1_MFPR_MUX_MODE0, K1_MFPR_DRIVE_1V8_DS2),
};

static bool g_k1gpio_registered;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct k1_gpio_dev_s *k1_gpio_priv(FAR struct gpio_dev_s *dev)
{
  return (FAR struct k1_gpio_dev_s *)dev;
}

static inline uintptr_t k1_gpio_reg(FAR const struct k1_gpio_dev_s *priv,
                                    uintptr_t offset)
{
  return priv->bank + offset;
}

static inline void k1_gpio_set_pad(FAR struct k1_gpio_dev_s *priv,
                                   uint32_t value)
{
  putreg32(value, priv->mfpr);
}

static inline void k1_gpio_set_input(FAR struct k1_gpio_dev_s *priv)
{
  putreg32(priv->mask,
           k1_gpio_reg(priv, K1_GPIO_GCDR_OFFSET));
}

static inline void k1_gpio_set_value(FAR struct k1_gpio_dev_s *priv,
                                     bool value)
{
  if (value)
    {
      putreg32(priv->mask,
               k1_gpio_reg(priv, K1_GPIO_GPSR_OFFSET));
    }
  else
    {
      putreg32(priv->mask,
               k1_gpio_reg(priv, K1_GPIO_GPCR_OFFSET));
    }
}

static inline void k1_gpio_set_output(FAR struct k1_gpio_dev_s *priv,
                                      bool value)
{
  /* Program the latch before changing direction to avoid a visible glitch
   * when a pin is changed from input/open-drain release to push-pull output.
   */

  k1_gpio_set_value(priv, value);
  putreg32(priv->mask,
           k1_gpio_reg(priv, K1_GPIO_GSDR_OFFSET));
}

static inline void k1_gpio_unlock_io_power(void)
{
  /* The K1 MFPR IO power-domain registers accept exactly one access after
   * this two-register unlock sequence.  Keep the key writes immediately
   * before each protected register write.
   */

  putreg32(K1_APBC_ASFAR_KEY, K1_APBC_ASFAR);
  putreg32(K1_APBC_ASSAR_KEY, K1_APBC_ASSAR);
}

static inline void k1_gpio_set_io_power(uintptr_t reg, bool v18)
{
  uint32_t value = v18 ? K1_MFPR_IO_PWR_V18EN : 0;

  k1_gpio_unlock_io_power();
  putreg32(value, reg);
}

static int k1_gpio_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  FAR struct k1_gpio_dev_s *priv;

  if (dev == NULL || value == NULL)
    {
      return -EINVAL;
    }

  priv = k1_gpio_priv(dev);
  *value = (getreg32(k1_gpio_reg(priv, K1_GPIO_GPLR_OFFSET)) &
            priv->mask) != 0;
  return OK;
}

static int k1_gpio_write(FAR struct gpio_dev_s *dev, bool value)
{
  FAR struct k1_gpio_dev_s *priv;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_gpio_priv(dev);

  if (dev->gp_pintype == GPIO_OUTPUT_PIN)
    {
      k1_gpio_set_value(priv, value);
    }
  else if (dev->gp_pintype == GPIO_OUTPUT_PIN_OPENDRAIN)
    {
      if (value)
        {
          /* A high open-drain value releases the line. */

          k1_gpio_set_pad(priv, priv->pad);
          k1_gpio_set_input(priv);
        }
      else
        {
          /* A low open-drain value actively pulls the line down. */

          k1_gpio_set_pad(priv, priv->pad);
          k1_gpio_set_output(priv, false);
        }
    }
  else
    {
      return -EACCES;
    }

  return OK;
}

static int k1_gpio_attach(FAR struct gpio_dev_s *dev,
                          pin_interrupt_t callback)
{
  (void)dev;
  (void)callback;
  return -ENOTSUP;
}

static int k1_gpio_enable(FAR struct gpio_dev_s *dev, bool enable)
{
  (void)dev;
  (void)enable;
  return -ENOTSUP;
}

static int k1_gpio_setpintype(FAR struct gpio_dev_s *dev,
                              enum gpio_pintype_e pintype)
{
  FAR struct k1_gpio_dev_s *priv;
  uint32_t pad;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_gpio_priv(dev);
  pad = priv->pad;

  switch (pintype)
    {
      case GPIO_INPUT_PIN:
        k1_gpio_set_pad(priv, pad);
        k1_gpio_set_input(priv);
        break;

      case GPIO_INPUT_PIN_PULLUP:
        k1_gpio_set_pad(priv, pad | K1_MFPR_PULL_UP);
        k1_gpio_set_input(priv);
        break;

      case GPIO_INPUT_PIN_PULLDOWN:
        k1_gpio_set_pad(priv, pad | K1_MFPR_PULL_DOWN);
        k1_gpio_set_input(priv);
        break;

      case GPIO_OUTPUT_PIN:
        k1_gpio_set_pad(priv, pad);
        k1_gpio_set_output(priv, false);
        break;

      case GPIO_OUTPUT_PIN_OPENDRAIN:
        k1_gpio_set_pad(priv, pad);
        k1_gpio_set_input(priv);
        break;

      default:

        /* GPIO interrupts require the K1 GPIO IRQ and PLIC chain, which is
         * deliberately outside this first multi-pin migration.
         */

        return -ENOTSUP;
    }

  dev->gp_pintype = pintype;
  return OK;
}

static int k1_gpio_setdebounce(FAR struct gpio_dev_s *dev,
                               unsigned long duration)
{
  (void)dev;
  (void)duration;
  return -ENOTSUP;
}

static int k1_gpio_setmask(FAR struct gpio_dev_s *dev, bool enable)
{
  (void)dev;
  (void)enable;
  return -ENOTSUP;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: k1_gpio_initialize
 *
 * Description:
 *   Register the MUSE Pi Pro 40-pin GPIO signals as /dev/gpio0..gpio25.
 *   /dev/gpio0 remains GPIO49 (physical Pin 22) for compatibility with the
 *   first real-board bring-up test.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int k1_gpio_initialize(void)
{
  int ret;
  int i;

  if (g_k1gpio_registered)
    {
      return OK;
    }

  /* Enable the GPIO and AIB clocks and release both blocks from reset.
   * APBC register 0x00 is UART0 and must not be touched here.
   */

  modifyreg32(K1_APBC_GPIO_CLK_RST, K1_CLK_RESET,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE);
  modifyreg32(K1_APBC_AIB_CLK_RST, K1_CLK_RESET,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE);

  /* Set both K1 external GPIO power domains to the MUSE Pi Pro 3.3V rail
   * before programming the individual MFPR pin settings.
   */

  /* GPIO47..GPIO52 and GPIO75..GPIO80 are external-voltage domains on K1.
   * MUSE Pi Pro routes these header signals through its 3.3V interface, so
   * select 3.3V explicitly instead of relying on U-Boot/Linux inheritance.
   */

  k1_gpio_set_io_power(K1_MFPR_IO_PWR_GPIO3, false);
  k1_gpio_set_io_power(K1_MFPR_IO_PWR_GPIO2, false);

  for (i = 0; i < K1_GPIO_PIN_COUNT; i++)
    {
      g_k1gpio[i].gpio.gp_pintype = GPIO_INPUT_PIN_PULLUP;
      g_k1gpio[i].gpio.gp_ops     = &g_k1_gpio_ops;

      ret = k1_gpio_setpintype(&g_k1gpio[i].gpio,
                               GPIO_INPUT_PIN_PULLUP);
      if (ret < 0)
        {
          goto err_unregister;
        }

      ret = gpio_pin_register(&g_k1gpio[i].gpio, i);
      if (ret < 0)
        {
          goto err_unregister;
        }
    }

  g_k1gpio_registered = true;
  return OK;

err_unregister:
  while (i-- > 0)
    {
      gpio_pin_unregister(&g_k1gpio[i].gpio, i);
    }

  return ret;
}

#endif /* CONFIG_DEV_GPIO && !CONFIG_GPIO_LOWER_HALF */
