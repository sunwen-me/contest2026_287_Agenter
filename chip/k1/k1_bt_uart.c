/****************************************************************************
 * vendor/spacemit/chips/k1/k1_bt_uart.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_K1_RTL8852BS2_BT

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/wireless/bluetooth/bt_uart.h>

#include <arch/irq.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_uart.h"
#include "k1_bt_uart.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_BT_UART_RXBUFSIZE  2048

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_bt_uart_s
{
  struct btuart_lowerhalf_s lower;
  spinlock_t lock;
  mutex_t txlock;
  btuart_rxcallback_t callback;
  FAR void *callback_arg;
  uint16_t rxhead;
  uint16_t rxtail;
  uint8_t rxbuffer[K1_BT_UART_RXBUFSIZE];
  bool initialized;
  bool rxenabled;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void k1_bt_uart_rxattach(FAR const struct btuart_lowerhalf_s *lower,
                                btuart_rxcallback_t callback,
                                FAR void *arg);
static void k1_bt_uart_rxenable(FAR const struct btuart_lowerhalf_s *lower,
                                bool enable);
static int k1_bt_uart_setbaud(FAR const struct btuart_lowerhalf_s *lower,
                              uint32_t baud);
static ssize_t k1_bt_uart_read(FAR const struct btuart_lowerhalf_s *lower,
                               FAR void *buffer, size_t buflen);
static ssize_t k1_bt_uart_write(FAR const struct btuart_lowerhalf_s *lower,
                                FAR const void *buffer, size_t buflen);
static ssize_t k1_bt_uart_rxdrain(
  FAR const struct btuart_lowerhalf_s *lower);
static int k1_bt_uart_ioctl(FAR const struct btuart_lowerhalf_s *lower,
                            int cmd, unsigned long arg);
static int k1_bt_uart_interrupt(int irq, FAR void *context, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* MUSE Pi Pro exposes one Bluetooth controller on UART2.  A static single
 * instance keeps its IRQ, FIFO, and H4 endpoint ownership unambiguous.
 */

static struct k1_bt_uart_s g_k1_bt_uart =
{
  .lower =
  {
    .rxattach = k1_bt_uart_rxattach,
    .rxenable = k1_bt_uart_rxenable,
    .setbaud  = k1_bt_uart_setbaud,
    .read     = k1_bt_uart_read,
    .write    = k1_bt_uart_write,
    .rxdrain  = k1_bt_uart_rxdrain,
    .ioctl    = k1_bt_uart_ioctl,
  },
  .lock = SP_UNLOCKED,
  .txlock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t k1_bt_uart_getreg(unsigned int offset)
{
  return getreg32(K1_UART2_BASE + offset);
}

static inline void k1_bt_uart_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, K1_UART2_BASE + offset);
}

static inline uint16_t k1_bt_uart_next(uint16_t index)
{
  return (uint16_t)((index + 1u) % K1_BT_UART_RXBUFSIZE);
}

static int k1_bt_uart_configure(uint32_t baud)
{
  uint32_t divisor;
  uint32_t lcr;

  if (baud == 0)
    {
      return -EINVAL;
    }

  divisor = (K1_UART2_CLOCK_HZ + baud * 8u) / (baud * 16u);
  if (divisor == 0 || divisor > UINT16_MAX)
    {
      return -ERANGE;
    }

  lcr = k1_bt_uart_getreg(K1_UART_LCR_OFFSET);
  k1_bt_uart_putreg(K1_UART_LCR_OFFSET, lcr | K1_UART_LCR_DLAB);
  k1_bt_uart_putreg(K1_UART_DLL_OFFSET, divisor & UINT8_MAX);
  k1_bt_uart_putreg(K1_UART_DLM_OFFSET, divisor >> 8);
  k1_bt_uart_putreg(K1_UART_LCR_OFFSET, K1_UART_LCR_WLS_8);
  k1_bt_uart_putreg(K1_UART_FCR_OFFSET,
                    K1_UART_FCR_FIFO_EN | K1_UART_FCR_RXRST |
                    K1_UART_FCR_TXRST | K1_UART_FCR_TRIG_14);
  k1_bt_uart_putreg(K1_UART_MCR_OFFSET,
                    K1_UART_MCR_RTS | K1_UART_MCR_AFCE);
  return OK;
}

static void k1_bt_uart_rxattach(FAR const struct btuart_lowerhalf_s *lower,
                                btuart_rxcallback_t callback,
                                FAR void *arg)
{
  FAR struct k1_bt_uart_s *priv =
    (FAR struct k1_bt_uart_s *)lower;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);
  priv->callback = callback;
  priv->callback_arg = arg;
  spin_unlock_irqrestore(&priv->lock, flags);
}

static void k1_bt_uart_rxenable(FAR const struct btuart_lowerhalf_s *lower,
                                bool enable)
{
  FAR struct k1_bt_uart_s *priv =
    (FAR struct k1_bt_uart_s *)lower;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);
  priv->rxenabled = enable;
  k1_bt_uart_putreg(K1_UART_IER_OFFSET,
                    enable ? K1_UART_IER_RDA : 0);
  spin_unlock_irqrestore(&priv->lock, flags);

  if (enable)
    {
      up_enable_irq(K1_IRQ_UART2);
    }
  else
    {
      up_disable_irq(K1_IRQ_UART2);
    }
}

static int k1_bt_uart_setbaud(FAR const struct btuart_lowerhalf_s *lower,
                              uint32_t baud)
{
  irqstate_t flags;
  int ret;

  (void)lower;

  flags = enter_critical_section();
  ret = k1_bt_uart_configure(baud);
  leave_critical_section(flags);
  return ret;
}

static ssize_t k1_bt_uart_read(FAR const struct btuart_lowerhalf_s *lower,
                               FAR void *buffer, size_t buflen)
{
  FAR struct k1_bt_uart_s *priv =
    (FAR struct k1_bt_uart_s *)lower;
  FAR uint8_t *dest = buffer;
  irqstate_t flags;
  size_t count = 0;

  if (buffer == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&priv->lock);
  while (count < buflen && priv->rxtail != priv->rxhead)
    {
      dest[count++] = priv->rxbuffer[priv->rxtail];
      priv->rxtail = k1_bt_uart_next(priv->rxtail);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return count == 0 ? -EAGAIN : (ssize_t)count;
}

static ssize_t k1_bt_uart_write(FAR const struct btuart_lowerhalf_s *lower,
                                FAR const void *buffer, size_t buflen)
{
  FAR const uint8_t *source = buffer;
  uint32_t timeout;
  size_t count;
  int ret;

  (void)lower;

  if (buffer == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_k1_bt_uart.txlock);
  if (ret < 0)
    {
      return ret;
    }

  for (count = 0; count < buflen; count++)
    {
      for (timeout = CONFIG_K1_BT_UART_TX_TIMEOUT_USEC / 10u;
           (k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_THRE) == 0;
           timeout--)
        {
          if (timeout == 0)
            {
              nxmutex_unlock(&g_k1_bt_uart.txlock);
              return count == 0 ? -ETIMEDOUT : (ssize_t)count;
            }

          up_udelay(10);
        }

      k1_bt_uart_putreg(K1_UART_THR_OFFSET, source[count]);
    }

  nxmutex_unlock(&g_k1_bt_uart.txlock);
  return (ssize_t)count;
}

static ssize_t k1_bt_uart_rxdrain(FAR const struct btuart_lowerhalf_s *lower)
{
  FAR struct k1_bt_uart_s *priv =
    (FAR struct k1_bt_uart_s *)lower;
  irqstate_t flags;
  ssize_t count;

  flags = spin_lock_irqsave(&priv->lock);
  count = (priv->rxhead + K1_BT_UART_RXBUFSIZE - priv->rxtail) %
          K1_BT_UART_RXBUFSIZE;
  priv->rxtail = priv->rxhead;
  spin_unlock_irqrestore(&priv->lock, flags);
  return count;
}

static int k1_bt_uart_ioctl(FAR const struct btuart_lowerhalf_s *lower,
                            int cmd, unsigned long arg)
{
  (void)lower;
  (void)cmd;
  (void)arg;
  return -ENOTTY;
}

static int k1_bt_uart_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct k1_bt_uart_s *priv = &g_k1_bt_uart;
  btuart_rxcallback_t callback = NULL;
  FAR void *callback_arg = NULL;
  irqstate_t flags;
  bool received = false;

  (void)irq;
  (void)context;
  (void)arg;

  while ((k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) != 0)
    {
      uint16_t next;
      uint8_t ch = k1_bt_uart_getreg(K1_UART_RBR_OFFSET) & UINT8_MAX;

      flags = spin_lock_irqsave(&priv->lock);
      next = k1_bt_uart_next(priv->rxhead);
      if (next != priv->rxtail)
        {
          priv->rxbuffer[priv->rxhead] = ch;
          priv->rxhead = next;
          received = true;
        }

      spin_unlock_irqrestore(&priv->lock, flags);
    }

  if (received)
    {
      flags = spin_lock_irqsave(&priv->lock);
      if (priv->rxenabled && priv->callback != NULL)
        {
          callback = priv->callback;
          callback_arg = priv->callback_arg;
        }

      spin_unlock_irqrestore(&priv->lock, flags);

      if (callback != NULL)
        {
          callback(&priv->lower, callback_arg);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_bt_uart_initialize(void)
{
  int ret;

  if (g_k1_bt_uart.initialized)
    {
      return OK;
    }

  /* UART2 is independent from the inherited UART0 console.  Select the
   * documented slow UART parent, release its reset, then leave RX interrupts
   * disabled until the Bluetooth upper half opens the H4 endpoint.
   */

  modifyreg32(K1_APBC_UART2_CLK_RST,
              K1_CLK_RESET | K1_APBC_UART_CLK_SEL_MASK,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE |
              K1_APBC_UART_CLK_SEL_SLOW_14M);

  ret = k1_bt_uart_configure(115200);
  if (ret < 0)
    {
      return ret;
    }

  k1_bt_uart_putreg(K1_UART_IER_OFFSET, 0);
  ret = irq_attach(K1_IRQ_UART2, k1_bt_uart_interrupt, NULL);
  if (ret < 0)
    {
      return ret;
    }

  up_disable_irq(K1_IRQ_UART2);
  ret = btuart_register(&g_k1_bt_uart.lower);
  if (ret < 0)
    {
      irq_detach(K1_IRQ_UART2);
      return ret;
    }

  g_k1_bt_uart.initialized = true;
  return OK;
}

#endif /* CONFIG_K1_RTL8852BS2_BT */
