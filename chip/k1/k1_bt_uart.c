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
#include <stddef.h>
#include <stdint.h>

#include <nuttx/arch.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_uart.h"
#include "k1_bt_uart.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_BT_H5_RETRIES           10u
#define K1_BT_H5_WAIT_MSEC         500u
#define K1_BT_H5_RX_DRAIN_MAX      2048u
#define K1_BT_H5_RX_PER_TICK       128u
#define K1_BT_H5_FRAME_MAX         32u

#define K1_BT_H5_DELIMITER         0xc0u
#define K1_BT_H5_ESCAPE            0xdbu
#define K1_BT_H5_ESCAPE_DELIMITER  0xdcu
#define K1_BT_H5_ESCAPE_ESCAPE     0xddu
#define K1_BT_H5_ESCAPE_XON        0xdeu
#define K1_BT_H5_ESCAPE_XOFF       0xdfu

#define K1_BT_H5_LINK_CONTROL      0x0fu

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum k1_bt_h5_message_e
{
  K1_BT_H5_NONE,
  K1_BT_H5_SYNC_REQUEST,
  K1_BT_H5_SYNC_RESPONSE,
  K1_BT_H5_CONFIG_REQUEST,
  K1_BT_H5_CONFIG_RESPONSE,
};

struct k1_bt_h5_rx_s
{
  uint8_t data[K1_BT_H5_FRAME_MAX];
  uint8_t length;
  bool started;
  bool escaped;
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

static int k1_bt_uart_configure(void)
{
  uint32_t divisor;

  divisor = (K1_UART2_CLOCK_HZ + 115200u * 8u) / (115200u * 16u);
  if (divisor == 0 || divisor > UINT16_MAX)
    {
      return -ERANGE;
    }

  k1_bt_uart_putreg(K1_UART_LCR_OFFSET,
                    K1_UART_LCR_WLS_8 | K1_UART_LCR_DLAB);
  k1_bt_uart_putreg(K1_UART_DLL_OFFSET, divisor & UINT8_MAX);
  k1_bt_uart_putreg(K1_UART_DLM_OFFSET, divisor >> 8);

  /* The vendor's rtk_hciattach configures the RTL8852BS H5 link as
   * 115200 8E1 with hardware flow control disabled.
   */

  k1_bt_uart_putreg(K1_UART_LCR_OFFSET,
                    K1_UART_LCR_WLS_8 | K1_UART_LCR_PEN |
                    K1_UART_LCR_EPS);
  k1_bt_uart_putreg(K1_UART_FCR_OFFSET,
                    K1_UART_FCR_FIFO_EN | K1_UART_FCR_RXRST |
                    K1_UART_FCR_TXRST | K1_UART_FCR_TRIG_14);
  k1_bt_uart_putreg(K1_UART_MCR_OFFSET, 0);
  k1_bt_uart_putreg(K1_UART_IER_OFFSET, 0);
  return OK;
}

static void k1_bt_uart_drain(void)
{
  unsigned int count;

  for (count = 0; count < K1_BT_H5_RX_DRAIN_MAX; count++)
    {
      if ((k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) == 0)
        {
          break;
        }

      (void)k1_bt_uart_getreg(K1_UART_RBR_OFFSET);
    }
}

static int k1_bt_uart_write_byte(uint8_t byte)
{
  uint32_t timeout;

  for (timeout = CONFIG_K1_BT_UART_TX_TIMEOUT_USEC / 10u;
       (k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_THRE) == 0;
       timeout--)
    {
      if (timeout == 0)
        {
          return -ETIMEDOUT;
        }

      up_udelay(10);
    }

  k1_bt_uart_putreg(K1_UART_THR_OFFSET, byte);
  return OK;
}

static int k1_bt_h5_write_escaped(uint8_t byte)
{
  int ret;

  if (byte == K1_BT_H5_DELIMITER)
    {
      ret = k1_bt_uart_write_byte(K1_BT_H5_ESCAPE);
      if (ret < 0)
        {
          return ret;
        }

      return k1_bt_uart_write_byte(K1_BT_H5_ESCAPE_DELIMITER);
    }

  if (byte == K1_BT_H5_ESCAPE)
    {
      ret = k1_bt_uart_write_byte(K1_BT_H5_ESCAPE);
      if (ret < 0)
        {
          return ret;
        }

      return k1_bt_uart_write_byte(K1_BT_H5_ESCAPE_ESCAPE);
    }

  if (byte == 0x11u)
    {
      ret = k1_bt_uart_write_byte(K1_BT_H5_ESCAPE);
      if (ret < 0)
        {
          return ret;
        }

      return k1_bt_uart_write_byte(K1_BT_H5_ESCAPE_XON);
    }

  if (byte == 0x13u)
    {
      ret = k1_bt_uart_write_byte(K1_BT_H5_ESCAPE);
      if (ret < 0)
        {
          return ret;
        }

      return k1_bt_uart_write_byte(K1_BT_H5_ESCAPE_XOFF);
    }

  return k1_bt_uart_write_byte(byte);
}

static int k1_bt_h5_send_link(FAR const uint8_t *payload, uint8_t length)
{
  uint8_t header[4];
  uint8_t index;
  int ret;

  if (payload == NULL || length == 0)
    {
      return -EINVAL;
    }

  header[0] = 0;
  header[1] = (length << 4) | K1_BT_H5_LINK_CONTROL;
  header[2] = length >> 4;
  header[3] = (uint8_t)~(header[0] + header[1] + header[2]);

  ret = k1_bt_uart_write_byte(K1_BT_H5_DELIMITER);
  if (ret < 0)
    {
      return ret;
    }

  for (index = 0; index < sizeof(header); index++)
    {
      ret = k1_bt_h5_write_escaped(header[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  for (index = 0; index < length; index++)
    {
      ret = k1_bt_h5_write_escaped(payload[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  return k1_bt_uart_write_byte(K1_BT_H5_DELIMITER);
}

static void k1_bt_h5_reset(FAR struct k1_bt_h5_rx_s *rx)
{
  rx->length = 0;
  rx->started = false;
  rx->escaped = false;
}

static enum k1_bt_h5_message_e
k1_bt_h5_validate(FAR const struct k1_bt_h5_rx_s *rx)
{
  FAR const uint8_t *payload;
  uint16_t payload_length;

  if (rx->length < 6 ||
      (uint8_t)~(rx->data[0] + rx->data[1] + rx->data[2]) != rx->data[3])
    {
      return K1_BT_H5_NONE;
    }

  if ((rx->data[0] & (1u << 6)) != 0 ||
      (rx->data[1] & 0x0fu) != K1_BT_H5_LINK_CONTROL)
    {
      return K1_BT_H5_NONE;
    }

  payload_length = (rx->data[1] >> 4) | ((uint16_t)rx->data[2] << 4);
  if (payload_length != rx->length - 4)
    {
      return K1_BT_H5_NONE;
    }

  payload = &rx->data[4];
  if (payload[0] == 0x01u && payload[1] == 0x7eu)
    {
      return K1_BT_H5_SYNC_REQUEST;
    }

  if (payload[0] == 0x02u && payload[1] == 0x7du)
    {
      return K1_BT_H5_SYNC_RESPONSE;
    }

  if (payload[0] == 0x03u && payload[1] == 0xfcu)
    {
      return K1_BT_H5_CONFIG_REQUEST;
    }

  if (payload[0] == 0x04u && payload[1] == 0x7bu)
    {
      return K1_BT_H5_CONFIG_RESPONSE;
    }

  return K1_BT_H5_NONE;
}

static enum k1_bt_h5_message_e
k1_bt_h5_consume(FAR struct k1_bt_h5_rx_s *rx, uint8_t byte)
{
  enum k1_bt_h5_message_e message;

  if (byte == K1_BT_H5_DELIMITER)
    {
      if (!rx->started)
        {
          rx->started = true;
          rx->length = 0;
          rx->escaped = false;
          return K1_BT_H5_NONE;
        }

      message = k1_bt_h5_validate(rx);
      rx->length = 0;
      rx->escaped = false;
      return message;
    }

  if (!rx->started || rx->length == K1_BT_H5_FRAME_MAX)
    {
      return K1_BT_H5_NONE;
    }

  if (rx->escaped)
    {
      switch (byte)
        {
          case K1_BT_H5_ESCAPE_DELIMITER:
            byte = K1_BT_H5_DELIMITER;
            break;

          case K1_BT_H5_ESCAPE_ESCAPE:
            byte = K1_BT_H5_ESCAPE;
            break;

          case K1_BT_H5_ESCAPE_XON:
            byte = 0x11u;
            break;

          case K1_BT_H5_ESCAPE_XOFF:
            byte = 0x13u;
            break;

          default:
            k1_bt_h5_reset(rx);
            return K1_BT_H5_NONE;
        }

      rx->escaped = false;
    }
  else if (byte == K1_BT_H5_ESCAPE)
    {
      rx->escaped = true;
      return K1_BT_H5_NONE;
    }

  rx->data[rx->length++] = byte;
  return K1_BT_H5_NONE;
}

static int k1_bt_h5_handle_message(enum k1_bt_h5_message_e message)
{
  static const uint8_t g_sync_response[] =
  {
    0x02u, 0x7du
  };

  static const uint8_t g_config_response[] =
  {
    0x04u, 0x7bu
  };

  if (message == K1_BT_H5_SYNC_REQUEST)
    {
      return k1_bt_h5_send_link(g_sync_response,
                                 sizeof(g_sync_response));
    }

  if (message == K1_BT_H5_CONFIG_REQUEST)
    {
      return k1_bt_h5_send_link(g_config_response,
                                 sizeof(g_config_response));
    }

  return OK;
}

static int k1_bt_h5_wait(FAR struct k1_bt_h5_rx_s *rx,
                         enum k1_bt_h5_message_e expected)
{
  unsigned int elapsed;
  unsigned int count;

  for (elapsed = 0; elapsed < K1_BT_H5_WAIT_MSEC; elapsed++)
    {
      for (count = 0; count < K1_BT_H5_RX_PER_TICK; count++)
        {
          enum k1_bt_h5_message_e message;
          uint8_t byte;
          int ret;

          if ((k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) == 0)
            {
              break;
            }

          byte = k1_bt_uart_getreg(K1_UART_RBR_OFFSET) & UINT8_MAX;
          message = k1_bt_h5_consume(rx, byte);
          if (message == expected)
            {
              return OK;
            }

          ret = k1_bt_h5_handle_message(message);
          if (ret < 0)
            {
              return ret;
            }
        }

      up_mdelay(1);
    }

  return -ETIMEDOUT;
}

static int k1_bt_h5_sync(void)
{
  static const uint8_t g_sync_request[] =
  {
    0x01u, 0x7eu
  };

  static const uint8_t g_config_request[] =
  {
    0x03u, 0xfcu, 0x14u
  };

  struct k1_bt_h5_rx_s rx;
  unsigned int retry;
  int ret;

  k1_bt_h5_reset(&rx);
  for (retry = 0; retry < K1_BT_H5_RETRIES; retry++)
    {
      ret = k1_bt_h5_send_link(g_sync_request, sizeof(g_sync_request));
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_bt_h5_wait(&rx, K1_BT_H5_SYNC_RESPONSE);
      if (ret == OK)
        {
          break;
        }
      else if (ret != -ETIMEDOUT)
        {
          return ret;
        }
    }

  if (retry == K1_BT_H5_RETRIES)
    {
      return -ETIMEDOUT;
    }

  for (retry = 0; retry < K1_BT_H5_RETRIES; retry++)
    {
      ret = k1_bt_h5_send_link(g_config_request, sizeof(g_config_request));
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_bt_h5_wait(&rx, K1_BT_H5_CONFIG_RESPONSE);
      if (ret == OK)
        {
          return OK;
        }
      else if (ret != -ETIMEDOUT)
        {
          return ret;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_bt_uart_initialize(void)
{
  static bool initialized;
  int ret;

  if (initialized)
    {
      return OK;
    }

  modifyreg32(K1_APBC_UART2_CLK_RST,
              K1_CLK_RESET | K1_APBC_UART_CLK_SEL_MASK,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE |
              K1_APBC_UART_CLK_SEL_SLOW_14M);

  ret = k1_bt_uart_configure();
  if (ret < 0)
    {
      return ret;
    }

  k1_bt_uart_drain();
  ret = k1_bt_h5_sync();
  if (ret < 0)
    {
      return ret;
    }

  initialized = true;
  return OK;
}

#endif /* CONFIG_K1_RTL8852BS2_BT */
