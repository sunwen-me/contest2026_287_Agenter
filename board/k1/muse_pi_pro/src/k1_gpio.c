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
 * Private Types
 ****************************************************************************/

struct k1_gpio_dev_s
{
  struct gpio_dev_s gpio;
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
  .go_read       = k1_gpio_read,
  .go_write      = k1_gpio_write,
  .go_attach     = k1_gpio_attach,
  .go_enable     = k1_gpio_enable,
  .go_setpintype = k1_gpio_setpintype,
  .go_setdebounce = k1_gpio_setdebounce,
  .go_setmask    = k1_gpio_setmask,
};

static struct k1_gpio_dev_s g_k1gpio;
static bool g_k1gpio_registered;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void k1_gpio_set_pad(uint32_t value)
{
  putreg32(value, K1_GPIO49_MFPR);
}

static inline void k1_gpio_set_input(void)
{
  putreg32(K1_GPIO49_MASK, K1_GPIO49_GCDR);
}

static inline void k1_gpio_set_output(bool value)
{
  putreg32(K1_GPIO49_MASK, K1_GPIO49_GSDR);

  if (value)
    {
      putreg32(K1_GPIO49_MASK, K1_GPIO49_GPSR);
    }
  else
    {
      putreg32(K1_GPIO49_MASK, K1_GPIO49_GPCR);
    }
}

static int k1_gpio_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  (void)dev;

  if (value == NULL)
    {
      return -EINVAL;
    }

  *value = (getreg32(K1_GPIO49_GPLR) & K1_GPIO49_MASK) != 0;
  return OK;
}

static int k1_gpio_write(FAR struct gpio_dev_s *dev, bool value)
{
  if (dev == NULL)
    {
      return -EINVAL;
    }

  if (dev->gp_pintype == GPIO_OUTPUT_PIN)
    {
      if (value)
        {
          putreg32(K1_GPIO49_MASK, K1_GPIO49_GPSR);
        }
      else
        {
          putreg32(K1_GPIO49_MASK, K1_GPIO49_GPCR);
        }
    }
  else if (dev->gp_pintype == GPIO_OUTPUT_PIN_OPENDRAIN)
    {
      if (value)
        {
          /* A high open-drain value releases the line. */

          k1_gpio_set_pad(K1_GPIO49_PAD_OUTPUT);
          k1_gpio_set_input();
        }
      else
        {
          /* A low open-drain value actively pulls the line down. */

          k1_gpio_set_pad(K1_GPIO49_PAD_OUTPUT);
          k1_gpio_set_output(false);
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
  if (dev == NULL)
    {
      return -EINVAL;
    }

  switch (pintype)
    {
      case GPIO_INPUT_PIN:
        k1_gpio_set_pad(K1_GPIO49_PAD_INPUT);
        k1_gpio_set_input();
        break;

      case GPIO_INPUT_PIN_PULLUP:
        k1_gpio_set_pad(K1_GPIO49_PAD_INPUT_PULLUP);
        k1_gpio_set_input();
        break;

      case GPIO_INPUT_PIN_PULLDOWN:
        k1_gpio_set_pad(K1_GPIO49_PAD_INPUT_PULLDOWN);
        k1_gpio_set_input();
        break;

      case GPIO_OUTPUT_PIN:
        k1_gpio_set_pad(K1_GPIO49_PAD_OUTPUT);
        k1_gpio_set_output(false);
        break;

      case GPIO_OUTPUT_PIN_OPENDRAIN:
        k1_gpio_set_pad(K1_GPIO49_PAD_OUTPUT);
        k1_gpio_set_input();
        break;

      default:

        /* GPIO49 has no PLIC GPIO interrupt support in this bring-up. */

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
 *   Register GPIO49 (40-pin header Pin 22) as /dev/gpio0.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int k1_gpio_initialize(void)
{
  int ret;

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

  g_k1gpio.gpio.gp_pintype = GPIO_INPUT_PIN_PULLUP;
  g_k1gpio.gpio.gp_ops     = &g_k1_gpio_ops;

  k1_gpio_set_pad(K1_GPIO49_PAD_INPUT_PULLUP);
  k1_gpio_set_input();

  ret = gpio_pin_register(&g_k1gpio.gpio, 0);
  if (ret < 0)
    {
      return ret;
    }

  g_k1gpio_registered = true;
  return OK;
}

#endif /* CONFIG_DEV_GPIO && !CONFIG_GPIO_LOWER_HALF */
