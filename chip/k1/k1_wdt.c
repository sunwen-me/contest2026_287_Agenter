/****************************************************************************
 * vendor/spacemit/chips/k1/k1_wdt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_WATCHDOG) && defined(CONFIG_WATCHDOG)

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/spinlock.h>
#include <nuttx/timers/watchdog.h>

#include "hardware/k1_wdt.h"
#include "k1_wdt.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_WDT_DEFAULT_TIMEOUT_MSEC         10000ul
#define K1_WDT_MIN_TIMEOUT_MSEC                 1ul

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_wdt_lowerhalf_s
{
  struct watchdog_lowerhalf_s lower;
  uint32_t timeout;
  clock_t lastreset;
  bool started;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int k1_wdt_start(FAR struct watchdog_lowerhalf_s *lower);
static int k1_wdt_stop(FAR struct watchdog_lowerhalf_s *lower);
static int k1_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower);
static int k1_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                            FAR struct watchdog_status_s *status);
static int k1_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                             uint32_t timeout);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct watchdog_ops_s g_k1_wdt_ops =
{
  .start      = k1_wdt_start,
  .stop       = k1_wdt_stop,
  .keepalive  = k1_wdt_keepalive,
  .getstatus  = k1_wdt_getstatus,
  .settimeout = k1_wdt_settimeout,
  .capture    = NULL,
  .ioctl      = NULL,
};

static struct k1_wdt_lowerhalf_s g_k1_wdt =
{
  .lower =
  {
    .ops = &g_k1_wdt_ops,
  },
  .timeout = K1_WDT_DEFAULT_TIMEOUT_MSEC,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR struct k1_wdt_lowerhalf_s *
k1_wdt_priv(FAR struct watchdog_lowerhalf_s *lower)
{
  return (FAR struct k1_wdt_lowerhalf_s *)lower;
}

static uint32_t k1_wdt_timeout_to_counter(uint32_t timeout)
{
  return (uint32_t)(((uint64_t)timeout * K1_WDT_COUNTER_HZ + 999ul) /
                    1000ul);
}

static uint32_t k1_wdt_counter_to_timeout(uint32_t counter)
{
  return (uint32_t)(((uint64_t)counter * 1000ul +
                     K1_WDT_COUNTER_HZ - 1) / K1_WDT_COUNTER_HZ);
}

static void k1_wdt_clock_enable(void)
{
  uint32_t value;

  value = getreg32(K1_MPMU_WDTPCR);
  value |= K1_WDT_CLOCK_BUS_ENABLE | K1_WDT_CLOCK_FUNCTION_ENABLE;
  value &= ~K1_WDT_CLOCK_RESET;
  putreg32(value, K1_MPMU_WDTPCR);
}

static void k1_wdt_unlock(void)
{
  putreg32(K1_WDT_WFAR_KEY, K1_WDT_BASE + K1_WDT_WFAR_OFFSET);
  putreg32(K1_WDT_WSAR_KEY, K1_WDT_BASE + K1_WDT_WSAR_OFFSET);
}

static void k1_wdt_write(uint32_t value, uintptr_t offset)
{
  k1_wdt_unlock();
  putreg32(value, K1_WDT_BASE + offset);
}

static void k1_wdt_ping(FAR struct k1_wdt_lowerhalf_s *priv)
{
  k1_wdt_write(K1_WDT_STATUS_CLEAR, K1_WDT_STATUS_OFFSET);
  k1_wdt_write(K1_WDT_RESET_ENABLE, K1_WDT_RESET_OFFSET);
  priv->lastreset = clock_systime_ticks();
}

static void k1_wdt_program(FAR struct k1_wdt_lowerhalf_s *priv)
{
  uint32_t counter;

  counter = k1_wdt_timeout_to_counter(priv->timeout);
  k1_wdt_clock_enable();
  k1_wdt_write(K1_WDT_STATUS_CLEAR, K1_WDT_STATUS_OFFSET);
  k1_wdt_write(counter, K1_WDT_TIMEOUT_OFFSET);
  k1_wdt_write(K1_WDT_ENABLE, K1_WDT_ENABLE_OFFSET);
  k1_wdt_write(K1_WDT_RESET_ENABLE, K1_WDT_RESET_OFFSET);
  modifyreg32(K1_WDT_START_REG, 0, K1_WDT_START_ENABLE);
  priv->lastreset = clock_systime_ticks();
}

/****************************************************************************
 * Name: k1_wdt_start
 ****************************************************************************/

static int k1_wdt_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct k1_wdt_lowerhalf_s *priv = k1_wdt_priv(lower);
  irqstate_t flags;

  flags = enter_critical_section();
  k1_wdt_program(priv);
  priv->started = true;
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: k1_wdt_stop
 ****************************************************************************/

static int k1_wdt_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct k1_wdt_lowerhalf_s *priv = k1_wdt_priv(lower);
  irqstate_t flags;

  flags = enter_critical_section();
  k1_wdt_clock_enable();
  k1_wdt_write(K1_WDT_DISABLE, K1_WDT_ENABLE_OFFSET);
  priv->started = false;
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: k1_wdt_keepalive
 ****************************************************************************/

static int k1_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct k1_wdt_lowerhalf_s *priv = k1_wdt_priv(lower);
  irqstate_t flags;

  if (!priv->started)
    {
      return -EPERM;
    }

  flags = enter_critical_section();
  k1_wdt_ping(priv);
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: k1_wdt_getstatus
 ****************************************************************************/

static int k1_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                            FAR struct watchdog_status_s *status)
{
  FAR struct k1_wdt_lowerhalf_s *priv = k1_wdt_priv(lower);
  uint32_t elapsed;

  if (status == NULL)
    {
      return -EINVAL;
    }

  status->flags = WDFLAGS_RESET;
  status->timeout = priv->timeout;
  status->timeleft = 0;

  if (priv->started)
    {
      elapsed = (uint32_t)TICK2MSEC(clock_systime_ticks() -
                                     priv->lastreset);
      if (elapsed > priv->timeout)
        {
          elapsed = priv->timeout;
        }

      status->flags |= WDFLAGS_ACTIVE;
      status->timeleft = priv->timeout - elapsed;
    }

  return OK;
}

/****************************************************************************
 * Name: k1_wdt_settimeout
 ****************************************************************************/

static int k1_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                             uint32_t timeout)
{
  FAR struct k1_wdt_lowerhalf_s *priv = k1_wdt_priv(lower);
  irqstate_t flags;
  uint32_t counter;

  counter = k1_wdt_timeout_to_counter(timeout);
  if (timeout < K1_WDT_MIN_TIMEOUT_MSEC || counter == 0)
    {
      return -ERANGE;
    }

  /* The controller has 3.90625 ms granularity.  Round both conversions up
   * so that the hardware never resets the board before the timeout reported
   * to the NuttX upper-half.
   */

  flags = enter_critical_section();
  priv->timeout = k1_wdt_counter_to_timeout(counter);
  if (priv->started)
    {
      k1_wdt_write(counter, K1_WDT_TIMEOUT_OFFSET);
      k1_wdt_ping(priv);
    }

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: k1_wdt_initialize
 ****************************************************************************/

int k1_wdt_initialize(FAR const char *devpath)
{
  FAR void *handle;

  if (devpath == NULL)
    {
      return -EINVAL;
    }

  /* U-Boot may have used this same controller before the RAM payload.  Leave
   * the device in the disabled state required by watchdog_register().
   */

  k1_wdt_stop(&g_k1_wdt.lower);
  handle = watchdog_register(devpath, &g_k1_wdt.lower);
  return handle != NULL ? OK : -ENODEV;
}

#endif /* CONFIG_K1_WATCHDOG && CONFIG_WATCHDOG */
