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
#include <nuttx/irq.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/spinlock.h>

#include <arch/irq.h>

#include "riscv_internal.h"
#include "hardware/k1_gpio.h"

#if defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_GPIO_PIN_COUNT 26

#ifdef CONFIG_K1_GPIO_IRQ
#  define K1_GPIO_BANK_COUNT 4
#endif

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
#ifdef CONFIG_K1_GPIO_IRQ
  pin_interrupt_t callback;
  bool irq_enabled;
  bool irq_masked;
#endif
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

#ifdef CONFIG_K1_GPIO_IRQ
static int k1_gpio_interrupt(int irq, FAR void *context, FAR void *arg);
static void k1_gpio_irq_program(FAR struct k1_gpio_dev_s *priv);
static void k1_gpio_irq_update_source(void);
#endif

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

#ifdef CONFIG_K1_GPIO_IRQ
static bool g_k1gpio_irq_attached;
static bool g_k1gpio_irq_source_enabled;
#endif

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

#ifdef CONFIG_K1_GPIO_IRQ
static bool k1_gpio_is_irq_pintype(enum gpio_pintype_e pintype)
{
  switch (pintype)
    {
      case GPIO_INTERRUPT_PIN:
      case GPIO_INTERRUPT_RISING_PIN:
      case GPIO_INTERRUPT_FALLING_PIN:
      case GPIO_INTERRUPT_BOTH_PIN:
        return true;

      default:
        return false;
    }
}

static void k1_gpio_irq_update_source(void)
{
  bool active = false;
  int i;

  for (i = 0; i < K1_GPIO_PIN_COUNT; i++)
    {
      if (k1_gpio_is_irq_pintype(g_k1gpio[i].gpio.gp_pintype) &&
          g_k1gpio[i].irq_enabled && !g_k1gpio[i].irq_masked &&
          g_k1gpio[i].callback != NULL)
        {
          active = true;
          break;
        }
    }

  if (active && !g_k1gpio_irq_source_enabled)
    {
      up_enable_irq(K1_IRQ_GPIO);
      g_k1gpio_irq_source_enabled = true;
    }
  else if (!active && g_k1gpio_irq_source_enabled)
    {
      up_disable_irq(K1_IRQ_GPIO);
      g_k1gpio_irq_source_enabled = false;
    }
}

static void k1_gpio_irq_program(FAR struct k1_gpio_dev_s *priv)
{
  uint32_t rising = 0;
  uint32_t falling = 0;

  /* GAPMASK bit one allows the GPIO interrupt to reach the shared GPIO
   * controller output.  Mask the pin while changing edge configuration and
   * clear any stale status before unmasking it.
   */

  modifyreg32(k1_gpio_reg(priv, K1_GPIO_GAPMASK_OFFSET), priv->mask, 0);
  putreg32(priv->mask, k1_gpio_reg(priv, K1_GPIO_GCRER_OFFSET));
  putreg32(priv->mask, k1_gpio_reg(priv, K1_GPIO_GCFER_OFFSET));
  putreg32(priv->mask, k1_gpio_reg(priv, K1_GPIO_GEDR_OFFSET));

  if (k1_gpio_is_irq_pintype(priv->gpio.gp_pintype) &&
      priv->irq_enabled && priv->callback != NULL &&
      !priv->irq_masked)
    {
      switch (priv->gpio.gp_pintype)
        {
          case GPIO_INTERRUPT_PIN:
          case GPIO_INTERRUPT_BOTH_PIN:
            rising = priv->mask;
            falling = priv->mask;
            break;

          case GPIO_INTERRUPT_RISING_PIN:
            rising = priv->mask;
            break;

          case GPIO_INTERRUPT_FALLING_PIN:
            falling = priv->mask;
            break;

          default:
            break;
        }

      if (rising != 0)
        {
          putreg32(rising,
                   k1_gpio_reg(priv, K1_GPIO_GSRER_OFFSET));
        }

      if (falling != 0)
        {
          putreg32(falling,
                   k1_gpio_reg(priv, K1_GPIO_GSFER_OFFSET));
        }

      if (rising != 0 || falling != 0)
        {
          modifyreg32(k1_gpio_reg(priv, K1_GPIO_GAPMASK_OFFSET), 0,
                      priv->mask);
        }
    }
}

static int k1_gpio_interrupt(int irq, FAR void *context, FAR void *arg)
{
  int i;
  unsigned int bank;

  (void)irq;
  (void)context;
  (void)arg;

  /* GPIO0..GPIO127 share one PLIC source.  Clear every reported status bit
   * first, then dispatch only pins that are currently enabled and unmasked.
   */

  for (bank = 0; bank < K1_GPIO_BANK_COUNT; bank++)
    {
      uintptr_t base;
      uint32_t pending;

      switch (bank)
        {
          case 0:
            base = K1_GPIO_BANK0_BASE;
            break;
          case 1:
            base = K1_GPIO_BANK1_BASE;
            break;
          case 2:
            base = K1_GPIO_BANK2_BASE;
            break;
          default:
            base = K1_GPIO_BANK3_BASE;
            break;
        }

      pending = getreg32(base + K1_GPIO_GEDR_OFFSET);
      if (pending != 0)
        {
          putreg32(pending, base + K1_GPIO_GEDR_OFFSET);

          for (i = 0; i < K1_GPIO_PIN_COUNT; i++)
            {
              FAR struct k1_gpio_dev_s *priv = &g_k1gpio[i];

              if (priv->bank == base && (pending & priv->mask) != 0 &&
                  k1_gpio_is_irq_pintype(priv->gpio.gp_pintype) &&
                  priv->irq_enabled && !priv->irq_masked &&
                  priv->callback != NULL)
                {
                  priv->callback(&priv->gpio, priv->gpio_number);
                }
            }
        }
    }

  return OK;
}
#endif

static int k1_gpio_attach(FAR struct gpio_dev_s *dev,
                          pin_interrupt_t callback)
{
#ifdef CONFIG_K1_GPIO_IRQ
  FAR struct k1_gpio_dev_s *priv;
  irqstate_t flags;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_gpio_priv(dev);
  flags = enter_critical_section();
  priv->callback = callback;
  k1_gpio_irq_program(priv);
  k1_gpio_irq_update_source();
  leave_critical_section(flags);
  return OK;
#else
  (void)dev;
  (void)callback;
  return -ENOTSUP;
#endif
}

static int k1_gpio_enable(FAR struct gpio_dev_s *dev, bool enable)
{
#ifdef CONFIG_K1_GPIO_IRQ
  FAR struct k1_gpio_dev_s *priv;
  irqstate_t flags;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_gpio_priv(dev);
  if (enable && priv->callback == NULL)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();
  priv->irq_enabled = enable;
  k1_gpio_irq_program(priv);
  k1_gpio_irq_update_source();
  leave_critical_section(flags);
  return OK;
#else
  (void)dev;
  (void)enable;
  return -ENOTSUP;
#endif
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

  if ((unsigned int)pintype >= GPIO_NPINTYPES)
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

#ifdef CONFIG_K1_GPIO_IRQ
      case GPIO_INTERRUPT_PIN:
      case GPIO_INTERRUPT_RISING_PIN:
      case GPIO_INTERRUPT_FALLING_PIN:
      case GPIO_INTERRUPT_BOTH_PIN:
        k1_gpio_set_pad(priv, pad);
        k1_gpio_set_input(priv);
        break;
#endif

      default:
        return -ENOTSUP;
    }

#ifdef CONFIG_K1_GPIO_IRQ
  if (k1_gpio_is_irq_pintype(dev->gp_pintype))
    {
      irqstate_t flags = enter_critical_section();

      priv->irq_enabled = false;
      priv->callback = NULL;
      k1_gpio_irq_program(priv);
      k1_gpio_irq_update_source();
      leave_critical_section(flags);
    }
#endif

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
#ifdef CONFIG_K1_GPIO_IRQ
  FAR struct k1_gpio_dev_s *priv;
  irqstate_t flags;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  priv = k1_gpio_priv(dev);
  flags = enter_critical_section();
  priv->irq_masked = enable;

  if (k1_gpio_is_irq_pintype(dev->gp_pintype))
    {
      k1_gpio_irq_program(priv);
      k1_gpio_irq_update_source();
    }

  leave_critical_section(flags);
  return OK;
#else
  (void)dev;
  (void)enable;
  return -ENOTSUP;
#endif
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
#ifdef CONFIG_K1_GPIO_IRQ
  unsigned int bank;
#endif

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

#ifdef CONFIG_K1_GPIO_IRQ
  /* Disable all GPIO edge sources and clear stale status before attaching
   * the shared PLIC handler.  The PLIC source remains disabled until a GPIO
   * pin has a callback, is enabled, and is not masked.
   */

  for (bank = 0; bank < K1_GPIO_BANK_COUNT; bank++)
    {
      uintptr_t base;

      switch (bank)
        {
          case 0:
            base = K1_GPIO_BANK0_BASE;
            break;
          case 1:
            base = K1_GPIO_BANK1_BASE;
            break;
          case 2:
            base = K1_GPIO_BANK2_BASE;
            break;
          default:
            base = K1_GPIO_BANK3_BASE;
            break;
        }

      putreg32(0, base + K1_GPIO_GRER_OFFSET);
      putreg32(0, base + K1_GPIO_GFER_OFFSET);
      putreg32(UINT32_MAX, base + K1_GPIO_GCRER_OFFSET);
      putreg32(UINT32_MAX, base + K1_GPIO_GCFER_OFFSET);
      putreg32(UINT32_MAX, base + K1_GPIO_GEDR_OFFSET);
      putreg32(0, base + K1_GPIO_GAPMASK_OFFSET);
    }

  ret = irq_attach(K1_IRQ_GPIO, k1_gpio_interrupt, NULL);
  if (ret < 0)
    {
      goto err_unregister;
    }

  g_k1gpio_irq_attached = true;
  up_disable_irq(K1_IRQ_GPIO);
#endif

  g_k1gpio_registered = true;
  return OK;

err_unregister:
#ifdef CONFIG_K1_GPIO_IRQ
  if (g_k1gpio_irq_attached)
    {
      up_disable_irq(K1_IRQ_GPIO);
      irq_detach(K1_IRQ_GPIO);
      g_k1gpio_irq_attached = false;
      g_k1gpio_irq_source_enabled = false;
    }
#endif

  while (i-- > 0)
    {
      gpio_pin_unregister(&g_k1gpio[i].gpio, i);
    }

  return ret;
}

#endif /* CONFIG_DEV_GPIO && !CONFIG_GPIO_LOWER_HALF */
