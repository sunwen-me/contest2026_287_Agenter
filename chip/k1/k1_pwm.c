/****************************************************************************
 * vendor/spacemit/chips/k1/k1_pwm.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_PWM) && defined(CONFIG_K1_PWM11)

#if defined(CONFIG_PWM_FREQUENCY_FIXED) || defined(CONFIG_PWM_PULSECOUNT) || \
    defined(CONFIG_PWM_MULTICHAN)
#  error K1 PWM11 supports only integer-frequency single-channel PWM
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/timers/pwm.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_pwm.h"
#include "k1_pwm.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_PWM11_NUMBER                   11
#define K1_PWM11_CLOCK_HZ           12800000ul
#define K1_PWM11_GPIO_BANK           K1_GPIO_BANK1_BASE
#define K1_PWM11_GPIO_MASK           (1ul << 9)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_pwm_priv_s
{
  struct pwm_lowerhalf_s lower;
  uintptr_t base;
  uintptr_t clock;
  uintptr_t pad;
  bool started;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int k1_pwm_setup(FAR struct pwm_lowerhalf_s *lower);
static int k1_pwm_shutdown(FAR struct pwm_lowerhalf_s *lower);
static int k1_pwm_start(FAR struct pwm_lowerhalf_s *lower,
                        FAR const struct pwm_info_s *info);
static int k1_pwm_stop(FAR struct pwm_lowerhalf_s *lower);
static int k1_pwm_ioctl(FAR struct pwm_lowerhalf_s *lower, int cmd,
                        unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pwm_ops_s g_k1_pwm_ops =
{
  .setup    = k1_pwm_setup,
  .shutdown = k1_pwm_shutdown,
  .start    = k1_pwm_start,
  .stop     = k1_pwm_stop,
  .ioctl    = k1_pwm_ioctl,
};

static struct k1_pwm_priv_s g_k1_pwm11 =
{
  .lower =
  {
    .ops = &g_k1_pwm_ops,
  },
  .base  = K1_PWM11_BASE,
  .clock = K1_APBC_PWM11_CLK_RST,
  .pad   = K1_MFPR_GPIO41,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct k1_pwm_priv_s *
k1_pwm_priv(FAR struct pwm_lowerhalf_s *lower)
{
  return (FAR struct k1_pwm_priv_s *)lower;
}

static inline uintptr_t k1_pwm_reg(FAR const struct k1_pwm_priv_s *priv,
                                   uintptr_t offset)
{
  return priv->base + offset;
}

static void k1_pwm_clock_enable(FAR struct k1_pwm_priv_s *priv)
{
  /* U-Boot assigns PWM11 the PLL1/192 12.8 MHz parent, enables APB and
   * function clocks, and releases the active-high reset bit.
   */

  putreg32(K1_CLK_RESET, priv->clock);
  putreg32(K1_CLK_RESET | K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE |
           K1_PWM_CLK_PARENT_PLL1_D192, priv->clock);
  putreg32(K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE |
           K1_PWM_CLK_PARENT_PLL1_D192, priv->clock);
}

static void k1_pwm_clock_disable(FAR struct k1_pwm_priv_s *priv)
{
  uint32_t clock;

  clock = getreg32(priv->clock);
  clock &= ~(K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE);
  putreg32(clock, priv->clock);
}

static void k1_pwm_release_pin(FAR struct k1_pwm_priv_s *priv)
{
  /* Restore the same GPIO input pad state used by the default board GPIO
   * lower-half for Pin 3.  Switching the MFPR alone does not change GPIO
   * direction, so explicitly release GPIO41 before returning it to GPIO.
   */

  putreg32(K1_PWM11_GPIO_MASK,
           K1_PWM11_GPIO_BANK + K1_GPIO_GCDR_OFFSET);
  putreg32(K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR | K1_MFPR_PULL_UP |
           K1_MFPR_DRIVE_1V8_DS2, priv->pad);
}

static void k1_pwm_stop_output(FAR struct k1_pwm_priv_s *priv)
{
  if (priv->started)
    {
      /* Setting duty to zero before gating the functional clock provides
       * the inactive-low stop path used by the vendor PWM implementation.
       */

      putreg32(0, k1_pwm_reg(priv, K1_PWM_DCR_OFFSET));
      k1_pwm_clock_disable(priv);
      priv->started = false;
    }

  k1_pwm_release_pin(priv);
}

/****************************************************************************
 * Name: k1_pwm_setup
 ****************************************************************************/

static int k1_pwm_setup(FAR struct pwm_lowerhalf_s *lower)
{
  FAR struct k1_pwm_priv_s *priv = k1_pwm_priv(lower);

  k1_pwm_stop_output(priv);
  return OK;
}

/****************************************************************************
 * Name: k1_pwm_shutdown
 ****************************************************************************/

static int k1_pwm_shutdown(FAR struct pwm_lowerhalf_s *lower)
{
  FAR struct k1_pwm_priv_s *priv = k1_pwm_priv(lower);

  k1_pwm_stop_output(priv);
  return OK;
}

/****************************************************************************
 * Name: k1_pwm_start
 ****************************************************************************/

static int k1_pwm_start(FAR struct pwm_lowerhalf_s *lower,
                        FAR const struct pwm_info_s *info)
{
  FAR struct k1_pwm_priv_s *priv = k1_pwm_priv(lower);
  uint64_t cycles;
  uint64_t duty;
  uint32_t prescale;
  uint32_t period;

  if (info == NULL || info->frequency == 0)
    {
      return -EINVAL;
    }

  if (info->frequency > K1_PWM11_CLOCK_HZ)
    {
      return -ERANGE;
    }

  cycles = K1_PWM11_CLOCK_HZ / info->frequency;

  prescale = (uint32_t)((cycles - 1) / K1_PWM_PERIOD_MAX);
  if (prescale > K1_PWM_PRESCALE_MAX)
    {
      return -ERANGE;
    }

  period = (uint32_t)(cycles / (prescale + 1));
  if (period == 0)
    {
      period = 1;
    }

  if (period > K1_PWM_PERIOD_MAX)
    {
      period = K1_PWM_PERIOD_MAX;
    }

  duty = (uint64_t)period * info->duty;
  duty >>= 16;

  /* Stop a prior waveform before changing the clock or counter state.  Keep
   * Pin 3 as a GPIO input until the new counter is fully programmed.
   */

  k1_pwm_stop_output(priv);
  k1_pwm_clock_enable(priv);
  putreg32(prescale | K1_PWM_CR_ABRUPT_SHUTDOWN,
           k1_pwm_reg(priv, K1_PWM_CR_OFFSET));
  putreg32(period - 1, k1_pwm_reg(priv, K1_PWM_PCR_OFFSET));
  putreg32((uint32_t)duty, k1_pwm_reg(priv, K1_PWM_DCR_OFFSET));
  putreg32(K1_MFPR_MUX_MODE4 | K1_MFPR_EDGE_CLEAR | K1_MFPR_PULL_UP |
           K1_MFPR_DRIVE_1V8_DS2, priv->pad);
  priv->started = true;
  return OK;
}

/****************************************************************************
 * Name: k1_pwm_stop
 ****************************************************************************/

static int k1_pwm_stop(FAR struct pwm_lowerhalf_s *lower)
{
  FAR struct k1_pwm_priv_s *priv = k1_pwm_priv(lower);

  k1_pwm_stop_output(priv);
  return OK;
}

/****************************************************************************
 * Name: k1_pwm_ioctl
 ****************************************************************************/

static int k1_pwm_ioctl(FAR struct pwm_lowerhalf_s *lower, int cmd,
                        unsigned long arg)
{
  (void)lower;
  (void)cmd;
  (void)arg;
  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: k1_pwminitialize
 ****************************************************************************/

FAR struct pwm_lowerhalf_s *k1_pwminitialize(int pwm)
{
  if (pwm != K1_PWM11_NUMBER)
    {
      return NULL;
    }

  return &g_k1_pwm11.lower;
}

#endif /* CONFIG_K1_PWM && CONFIG_K1_PWM11 */
