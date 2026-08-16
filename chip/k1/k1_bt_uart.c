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
#define K1_BT_H5_HCI_WAIT_MSEC     1500u
#define K1_BT_H5_RX_DRAIN_MAX      2048u
#define K1_BT_H5_RX_PER_TICK       128u
#define K1_BT_H5_FRAME_MAX         32u

#define K1_BT_H5_DELIMITER         0xc0u
#define K1_BT_H5_ESCAPE            0xdbu
#define K1_BT_H5_ESCAPE_DELIMITER  0xdcu
#define K1_BT_H5_ESCAPE_ESCAPE     0xddu
#define K1_BT_H5_ESCAPE_XON        0xdeu
#define K1_BT_H5_ESCAPE_XOFF       0xdfu

#define K1_BT_H5_HEADER_RELIABLE   (1u << 7)
#define K1_BT_H5_HEADER_CRC        (1u << 6)

#define K1_BT_H5_ACK               0x00u
#define K1_BT_H5_COMMAND            0x01u
#define K1_BT_H5_EVENT              0x04u
#define K1_BT_H5_LINK_CONTROL       0x0fu

#define K1_BT_HCI_EVENT_COMPLETE    0x0eu
#define K1_BT_HCI_READ_LOCAL_VER    0x1001u

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum k1_bt_h5_message_e
{
  K1_BT_H5_NONE,
  K1_BT_H5_SYNC_RESPONSE,
  K1_BT_H5_CONFIG_RESPONSE,
  K1_BT_H5_LOCAL_VERSION,
};

struct k1_bt_h5_rx_s
{
  uint8_t data[K1_BT_H5_FRAME_MAX];
  uint8_t length;
  bool started;
  bool escaped;
};

struct k1_bt_h5_link_s
{
  struct k1_bt_h5_rx_s rx;
  uint8_t txseq;
  uint8_t rxseq_txack;
  bool use_crc;
};

struct k1_bt_h5_frame_s
{
  FAR const uint8_t *payload;
  uint16_t payload_length;
  uint8_t type;
  uint8_t sequence;
  bool reliable;
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

static void k1_bt_h5_crc_update(FAR uint16_t *crc, uint8_t byte)
{
  unsigned int bit;

  for (bit = 0; bit < 8; bit++)
    {
      if (((*crc ^ byte) & 1u) != 0)
        {
          *crc = (*crc >> 1) ^ 0x8408u;
        }
      else
        {
          *crc >>= 1;
        }

      byte >>= 1;
    }
}

static uint16_t k1_bt_h5_reverse16(uint16_t value)
{
  uint16_t reversed = 0;
  unsigned int bit;

  for (bit = 0; bit < 16; bit++)
    {
      reversed = (reversed << 1) | (value & 1u);
      value >>= 1;
    }

  return reversed;
}

static int k1_bt_h5_send_frame(FAR struct k1_bt_h5_link_s *link,
                               FAR const uint8_t *payload,
                               uint16_t payload_length, uint8_t type,
                               bool reliable)
{
  uint8_t header[4];
  uint16_t crc = UINT16_MAX;
  uint16_t index;
  int ret;

  if ((payload == NULL && payload_length != 0) ||
      payload_length > 0x0fffu || type > 0x0fu)
    {
      return -EINVAL;
    }

  header[0] = link->rxseq_txack << 3;
  if (reliable)
    {
      header[0] |= K1_BT_H5_HEADER_RELIABLE | link->txseq;
      link->txseq = (link->txseq + 1u) & 0x07u;
    }

  if (link->use_crc)
    {
      header[0] |= K1_BT_H5_HEADER_CRC;
    }

  header[1] = ((payload_length << 4) & UINT8_MAX) | type;
  header[2] = payload_length >> 4;
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

      if (link->use_crc)
        {
          k1_bt_h5_crc_update(&crc, header[index]);
        }
    }

  for (index = 0; index < payload_length; index++)
    {
      ret = k1_bt_h5_write_escaped(payload[index]);
      if (ret < 0)
        {
          return ret;
        }

      if (link->use_crc)
        {
          k1_bt_h5_crc_update(&crc, payload[index]);
        }
    }

  if (link->use_crc)
    {
      crc = k1_bt_h5_reverse16(crc);
      ret = k1_bt_h5_write_escaped(crc >> 8);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_bt_h5_write_escaped(crc & UINT8_MAX);
      if (ret < 0)
        {
          return ret;
        }
    }

  return k1_bt_uart_write_byte(K1_BT_H5_DELIMITER);
}

static int k1_bt_h5_send_ack(FAR struct k1_bt_h5_link_s *link)
{
  return k1_bt_h5_send_frame(link, NULL, 0, K1_BT_H5_ACK, false);
}

static void k1_bt_h5_reset(FAR struct k1_bt_h5_rx_s *rx)
{
  rx->length = 0;
  rx->started = false;
  rx->escaped = false;
}

static bool k1_bt_h5_validate(FAR const struct k1_bt_h5_rx_s *rx,
                               FAR struct k1_bt_h5_frame_s *frame)
{
  uint16_t actual_crc;
  uint16_t expected_crc;
  uint16_t payload_length;
  uint16_t total_length;
  uint16_t index;
  bool has_crc;

  if (rx->length < 4 ||
      (uint8_t)~(rx->data[0] + rx->data[1] + rx->data[2]) != rx->data[3])
    {
      return false;
    }

  has_crc = (rx->data[0] & K1_BT_H5_HEADER_CRC) != 0;
  payload_length = (rx->data[1] >> 4) | ((uint16_t)rx->data[2] << 4);
  total_length = 4 + payload_length + (has_crc ? 2 : 0);
  if (total_length != rx->length)
    {
      return false;
    }

  if (has_crc)
    {
      expected_crc = UINT16_MAX;
      for (index = 0; index < 4 + payload_length; index++)
        {
          k1_bt_h5_crc_update(&expected_crc, rx->data[index]);
        }

      expected_crc = k1_bt_h5_reverse16(expected_crc);
      actual_crc = ((uint16_t)rx->data[total_length - 2] << 8) |
                   rx->data[total_length - 1];
      if (actual_crc != expected_crc)
        {
          return false;
        }
    }

  frame->payload = &rx->data[4];
  frame->payload_length = payload_length;
  frame->type = rx->data[1] & 0x0fu;
  frame->sequence = rx->data[0] & 0x07u;
  frame->reliable = (rx->data[0] & K1_BT_H5_HEADER_RELIABLE) != 0;
  return true;
}

static bool k1_bt_h5_consume(FAR struct k1_bt_h5_rx_s *rx, uint8_t byte,
                              FAR struct k1_bt_h5_frame_s *frame)
{
  bool complete;

  if (byte == K1_BT_H5_DELIMITER)
    {
      if (!rx->started)
        {
          rx->started = true;
          rx->length = 0;
          rx->escaped = false;
          return false;
        }

      complete = k1_bt_h5_validate(rx, frame);
      k1_bt_h5_reset(rx);
      return complete;
    }

  if (!rx->started || rx->length == K1_BT_H5_FRAME_MAX)
    {
      return false;
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
            return false;
        }

      rx->escaped = false;
    }
  else if (byte == K1_BT_H5_ESCAPE)
    {
      rx->escaped = true;
      return false;
    }

  rx->data[rx->length++] = byte;
  return false;
}

static int k1_bt_h5_control_message(
  FAR struct k1_bt_h5_link_s *link,
  FAR const struct k1_bt_h5_frame_s *frame,
  FAR enum k1_bt_h5_message_e *message)
{
  FAR const uint8_t *payload = frame->payload;

  *message = K1_BT_H5_NONE;

  if (frame->type != K1_BT_H5_LINK_CONTROL || frame->payload_length < 2)
    {
      return OK;
    }

  if (payload[0] == 0x01u && payload[1] == 0x7eu)
    {
      static const uint8_t g_sync_response[] =
      {
        0x02u, 0x7du
      };

      return k1_bt_h5_send_frame(link, g_sync_response,
                                  sizeof(g_sync_response),
                                  K1_BT_H5_LINK_CONTROL, false);
    }

  if (payload[0] == 0x02u && payload[1] == 0x7du)
    {
      *message = K1_BT_H5_SYNC_RESPONSE;
      return OK;
    }

  if (payload[0] == 0x03u && payload[1] == 0xfcu)
    {
      static const uint8_t g_config_response[] =
      {
        0x04u, 0x7bu
      };

      return k1_bt_h5_send_frame(link, g_config_response,
                                  sizeof(g_config_response),
                                  K1_BT_H5_LINK_CONTROL, false);
    }

  if (payload[0] == 0x04u && payload[1] == 0x7bu)
    {
      if (frame->payload_length > 2)
        {
          link->use_crc = (payload[2] & (1u << 4)) != 0;
        }

      *message = K1_BT_H5_CONFIG_RESPONSE;
      return OK;
    }

  return OK;
}

static int k1_bt_h5_read_local_version(
  FAR const struct k1_bt_h5_frame_s *frame,
  FAR struct k1_bt_h5_info_s *info,
  FAR enum k1_bt_h5_message_e *message)
{
  FAR const uint8_t *payload = frame->payload;
  uint16_t opcode;

  if (frame->type != K1_BT_H5_EVENT || frame->payload_length < 5 ||
      payload[0] != K1_BT_HCI_EVENT_COMPLETE ||
      payload[1] < 4)
    {
      return OK;
    }

  opcode = payload[3] | ((uint16_t)payload[4] << 8);
  if (opcode != K1_BT_HCI_READ_LOCAL_VER)
    {
      return OK;
    }

  if (frame->payload_length < 14 || payload[1] < 12)
    {
      return -EPROTO;
    }

  if (payload[5] != 0)
    {
      return -EIO;
    }

  info->hci_version = payload[6];
  info->hci_revision = payload[7] | ((uint16_t)payload[8] << 8);
  info->lmp_version = payload[9];
  info->manufacturer = payload[10] | ((uint16_t)payload[11] << 8);
  info->lmp_subversion = payload[12] | ((uint16_t)payload[13] << 8);
  *message = K1_BT_H5_LOCAL_VERSION;
  return OK;
}

static int k1_bt_h5_handle_frame(FAR struct k1_bt_h5_link_s *link,
                                  FAR const struct k1_bt_h5_frame_s *frame,
                                  FAR struct k1_bt_h5_info_s *info,
                                  FAR enum k1_bt_h5_message_e *message)
{
  int frame_ret = OK;
  int ret;

  *message = K1_BT_H5_NONE;
  if (frame->reliable)
    {
      if (frame->sequence != link->rxseq_txack)
        {
          return k1_bt_h5_send_ack(link);
        }

      link->rxseq_txack = (frame->sequence + 1u) & 0x07u;
    }

  if (frame->type == K1_BT_H5_LINK_CONTROL)
    {
      frame_ret = k1_bt_h5_control_message(link, frame, message);
    }
  else if (frame->type == K1_BT_H5_EVENT)
    {
      frame_ret = k1_bt_h5_read_local_version(frame, info, message);
    }

  if (frame->reliable)
    {
      ret = k1_bt_h5_send_ack(link);
      if (ret < 0)
        {
          return ret;
        }
    }

  return frame_ret;
}

static int k1_bt_h5_wait(FAR struct k1_bt_h5_link_s *link,
                         FAR struct k1_bt_h5_info_s *info,
                         enum k1_bt_h5_message_e expected,
                         unsigned int timeout_msec)
{
  unsigned int elapsed;
  unsigned int count;

  for (elapsed = 0; elapsed < timeout_msec; elapsed++)
    {
      for (count = 0; count < K1_BT_H5_RX_PER_TICK; count++)
        {
          struct k1_bt_h5_frame_s frame;
          enum k1_bt_h5_message_e message;
          uint8_t byte;
          int ret;

          if ((k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) == 0)
            {
              break;
            }

          byte = k1_bt_uart_getreg(K1_UART_RBR_OFFSET) & UINT8_MAX;
          if (!k1_bt_h5_consume(&link->rx, byte, &frame))
            {
              continue;
            }

          ret = k1_bt_h5_handle_frame(link, &frame, info, &message);
          if (ret < 0)
            {
              return ret;
            }

          if (message == expected)
            {
              return OK;
            }
        }

      up_mdelay(1);
    }

  return -ETIMEDOUT;
}

static int k1_bt_h5_negotiate(FAR struct k1_bt_h5_link_s *link,
                               FAR struct k1_bt_h5_info_s *info)
{
  static const uint8_t g_sync_request[] =
  {
    0x01u, 0x7eu
  };

  static const uint8_t g_config_request[] =
  {
    0x03u, 0xfcu, 0x14u
  };

  unsigned int retry;
  int ret;

  for (retry = 0; retry < K1_BT_H5_RETRIES; retry++)
    {
      ret = k1_bt_h5_send_frame(link, g_sync_request,
                                 sizeof(g_sync_request),
                                 K1_BT_H5_LINK_CONTROL, false);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_bt_h5_wait(link, info, K1_BT_H5_SYNC_RESPONSE,
                           K1_BT_H5_WAIT_MSEC);
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
      ret = k1_bt_h5_send_frame(link, g_config_request,
                                 sizeof(g_config_request),
                                 K1_BT_H5_LINK_CONTROL, false);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_bt_h5_wait(link, info, K1_BT_H5_CONFIG_RESPONSE,
                           K1_BT_H5_WAIT_MSEC);
      if (ret == OK)
        {
          return k1_bt_h5_send_ack(link);
        }
      else if (ret != -ETIMEDOUT)
        {
          return ret;
        }
    }

  return -ETIMEDOUT;
}

static int k1_bt_h5_query_local_version(FAR struct k1_bt_h5_link_s *link,
                                         FAR struct k1_bt_h5_info_s *info)
{
  static const uint8_t g_read_local_version[] =
  {
    0x01u, 0x10u, 0x00u
  };

  int ret;

  ret = k1_bt_h5_send_frame(link, g_read_local_version,
                             sizeof(g_read_local_version),
                             K1_BT_H5_COMMAND, true);
  if (ret < 0)
    {
      return ret;
    }

  return k1_bt_h5_wait(link, info, K1_BT_H5_LOCAL_VERSION,
                       K1_BT_H5_HCI_WAIT_MSEC);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_bt_uart_initialize(FAR struct k1_bt_h5_info_s *info)
{
  struct k1_bt_h5_link_s link;
  int ret;

  if (info == NULL)
    {
      return -EINVAL;
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
  k1_bt_h5_reset(&link.rx);
  link.txseq = 0;
  link.rxseq_txack = 0;
  link.use_crc = false;
  ret = k1_bt_h5_negotiate(&link, info);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_bt_h5_query_local_version(&link, info);
  if (ret < 0)
    {
      return ret;
    }

  info->crc_enabled = link.use_crc;
  return OK;
}

#endif /* CONFIG_K1_RTL8852BS2_BT */
