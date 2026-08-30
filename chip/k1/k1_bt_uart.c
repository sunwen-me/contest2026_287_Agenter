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
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>
#include <nuttx/wqueue.h>

#include "hardware/k1_gpio.h"
#include "hardware/k1_uart.h"
#include "k1_bt_uart.h"
#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_BT_H5_RETRIES           10u
#define K1_BT_H5_WAIT_MSEC         500u
#define K1_BT_H5_HCI_WAIT_MSEC     1500u
#define K1_BT_H5_RX_DRAIN_MAX      2048u
#define K1_BT_H5_RX_PER_TICK       128u
#define K1_BT_H5_FRAME_MAX         512u
#define K1_BT_H5_RX_POLL_MSEC      2u
#define K1_BT_H5_PATCH_DATA_MAX    252u
#define K1_BT_H5_BAUD_SETTLE_MSEC  50u

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
#define K1_BT_H5_ACL                0x02u
#define K1_BT_H5_EVENT              0x04u
#define K1_BT_H5_ISO                0x05u
#define K1_BT_H5_LINK_CONTROL       0x0fu

#define K1_BT_HCI_EVENT_COMPLETE    0x0eu
#define K1_BT_HCI_READ_LOCAL_VER    0x1001u
#define K1_BT_HCI_RESET             0x0c03u
#define K1_BT_HCI_VENDOR_CHANGE_BAUD 0xfc17u
#define K1_BT_HCI_VENDOR_PATCH       0xfc20u
#define K1_BT_HCI_VENDOR_READ        0xfc61u
#define K1_BT_HCI_VENDOR_ROM_VER     0xfc6du

#define K1_BT_RTL8852BS_ROM_SUBVER   0x8852u
#define K1_BT_RTL8852BS_CHIP_TYPE    0u

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum k1_bt_h5_message_e
{
  K1_BT_H5_NONE,
  K1_BT_H5_SYNC_RESPONSE,
  K1_BT_H5_CONFIG_RESPONSE,
  K1_BT_H5_LOCAL_VERSION,
  K1_BT_H5_RESET_COMPLETE,
  K1_BT_H5_BAUD_CHANGE_COMPLETE,
  K1_BT_H5_ROM_VERSION,
  K1_BT_H5_CHIP_TYPE,
  K1_BT_H5_PATCH_COMPLETE,
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
  uint32_t rx_bytes;
  uint16_t tx_frames;
  uint16_t rx_frames;
  uint8_t txseq;
  uint8_t rxseq_txack;
  uint8_t patch_index;
  bool patch_final_pending;
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

/* The NuttX H5 layer hands complete SLIP frames to this lower transport.
 * UART2 has no interrupt path in the current K1 port, so LPWORK drains its
 * FIFO and preserves one complete escaped frame at a time.
 */

struct k1_bt_h5_transport_s
{
  struct bt_driver_s lower;
  mutex_t state_lock;
  mutex_t tx_lock;
  struct work_s rx_work;
#ifdef CONFIG_K1_BT_H5_HOST
  struct k1_bt_h5_link_s link;
#else
  FAR uint8_t *rx_buffer;
  size_t rx_length;
  bool rx_escaped;
  bool rx_started;
#endif
  bool opened;
};

#ifdef CONFIG_K1_BT_H5_HOST
/* UART2 is the board's only Bluetooth transport.  Board bring-up completes
 * the vendor patch exchange before the Host thread is created, so retain
 * that UART state instead of dropping the controller back to 115200 baud.
 */

static struct k1_bt_h5_link_s g_k1_bt_h5_handoff_link;
static bool g_k1_bt_h5_handoff_ready;
#endif

#ifdef CONFIG_K1_BT_H5_VENDOR_FIRMWARE
/* This locally generated include contains only the board-matched epatch
 * payload followed by its Realtek config. It is deliberately ignored by
 * Git because the package installed on the board carries no redistribution
 * license for the firmware binary.
 */

static const uint8_t g_k1_bt_rtl8852bs_patch[] =
{
#include "k1_rtl8852bs_bt_patch.inc"
};
#endif

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

static void k1_bt_uart_report(FAR const struct k1_bt_h5_link_s *link)
{
  k1_early_puts("K1 Bluetooth: UART2 APBC=");
  k1_early_puthex(getreg32(K1_APBC_UART2_CLK_RST));
  k1_early_puts(" LCR=");
  k1_early_puthex(k1_bt_uart_getreg(K1_UART_LCR_OFFSET));
  k1_early_puts(" IER=");
  k1_early_puthex(k1_bt_uart_getreg(K1_UART_IER_OFFSET));
  k1_early_puts(" MCR=");
  k1_early_puthex(k1_bt_uart_getreg(K1_UART_MCR_OFFSET));
  k1_early_puts(" LSR=");
  k1_early_puthex(k1_bt_uart_getreg(K1_UART_LSR_OFFSET));
  k1_early_puts(" H5 TX frames=");
  k1_early_puthex(link->tx_frames);
  k1_early_puts(" RX bytes=");
  k1_early_puthex(link->rx_bytes);
  k1_early_puts(" RX frames=");
  k1_early_puthex(link->rx_frames);
  k1_early_puts("\r\n");
}

static int k1_bt_uart_configure(uint32_t clock_hz, uint32_t baud)
{
  uint32_t divisor;

  if (baud == 0)
    {
      return -EINVAL;
    }

  divisor = (clock_hz + baud * 8u) / (baud * 16u);
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

  /* UART2 is an XScale-compatible UART.  Its Unit Enable bit is located in
   * IER[6], while all interrupt-enable bits stay clear for polling.
   */

  k1_bt_uart_putreg(K1_UART_IER_OFFSET, K1_UART_IER_UUE);
  return OK;
}

static int k1_bt_uart_set_baud(uint32_t baud)
{
  uint32_t clock_hz;
  uint32_t clock_select;

  if (baud == 115200u)
    {
      clock_hz = K1_UART2_SLOW_14M_CLOCK_HZ;
      clock_select = K1_APBC_UART_CLK_SEL_SLOW_14M;
    }
  else if (baud == 1500000u)
    {
      clock_hz = K1_UART2_SLOW_48M_CLOCK_HZ;
      clock_select = K1_APBC_UART_CLK_SEL_SLOW_48M;
    }
  else
    {
      return -EINVAL;
    }

  modifyreg32(K1_APBC_UART2_CLK_RST, K1_APBC_UART_CLK_SEL_MASK,
              clock_select);
  return k1_bt_uart_configure(clock_hz, baud);
}

static int k1_bt_uart_wait_tx_empty(void)
{
  uint32_t timeout;

  for (timeout = CONFIG_K1_BT_UART_TX_TIMEOUT_USEC / 10u;
       (k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_TEMT) == 0;
       timeout--)
    {
      if (timeout == 0)
        {
          return -ETIMEDOUT;
        }

      up_udelay(10);
    }

  return OK;
}

static void k1_bt_uart_set_flow_control(bool enabled)
{
  uint32_t mcr = 0;

  if (enabled)
    {
      /* The stock rtk_hciattach leaves UART2 MCR at 0x2b after applying
       * the RTL8852BS configuration: DTR, RTS, OUT2 and auto RTS/CTS.
       * AFCE alone leaves the host RTS line deasserted on this UART.
       */

      mcr = K1_UART_MCR_DTR | K1_UART_MCR_RTS | K1_UART_MCR_OUT2 |
            K1_UART_MCR_AFCE;
    }

  k1_bt_uart_putreg(K1_UART_MCR_OFFSET, mcr);
}

static int k1_bt_uart_prepare(void)
{
  modifyreg32(K1_APBC_UART2_CLK_RST,
              K1_CLK_RESET | K1_APBC_UART_CLK_SEL_MASK,
              K1_CLK_BUS_ENABLE | K1_CLK_FUNCTION_ENABLE |
              K1_APBC_UART_CLK_SEL_SLOW_14M);
  return k1_bt_uart_configure(K1_UART2_SLOW_14M_CLOCK_HZ, 115200u);
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

  ret = k1_bt_uart_write_byte(K1_BT_H5_DELIMITER);
  if (ret >= 0)
    {
      link->tx_frames++;
    }

  return ret;
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

static int k1_bt_h5_read_reset_complete(
  FAR const struct k1_bt_h5_frame_s *frame,
  FAR enum k1_bt_h5_message_e *message)
{
  FAR const uint8_t *payload = frame->payload;
  uint16_t opcode;

  if (frame->type != K1_BT_H5_EVENT || frame->payload_length < 5 ||
      payload[0] != K1_BT_HCI_EVENT_COMPLETE || payload[1] < 4)
    {
      return OK;
    }

  opcode = payload[3] | ((uint16_t)payload[4] << 8);
  if (opcode != K1_BT_HCI_RESET)
    {
      return OK;
    }

  if (frame->payload_length < 6)
    {
      return -EPROTO;
    }

  if (payload[5] != 0)
    {
      return -EIO;
    }

  *message = K1_BT_H5_RESET_COMPLETE;
  return OK;
}

static int k1_bt_h5_read_vendor_complete(
  FAR struct k1_bt_h5_link_s *link,
  FAR const struct k1_bt_h5_frame_s *frame,
  FAR struct k1_bt_h5_info_s *info,
  FAR enum k1_bt_h5_message_e *message)
{
  FAR const uint8_t *payload = frame->payload;
  uint16_t opcode;

  if (frame->type != K1_BT_H5_EVENT || frame->payload_length < 6 ||
      payload[0] != K1_BT_HCI_EVENT_COMPLETE || payload[1] < 4)
    {
      return OK;
    }

  opcode = payload[3] | ((uint16_t)payload[4] << 8);
  if (opcode != K1_BT_HCI_VENDOR_ROM_VER &&
      opcode != K1_BT_HCI_VENDOR_READ &&
      opcode != K1_BT_HCI_VENDOR_CHANGE_BAUD &&
      opcode != K1_BT_HCI_VENDOR_PATCH)
    {
      return OK;
    }

  if (payload[5] != 0)
    {
      return -EIO;
    }

  switch (opcode)
    {
      case K1_BT_HCI_VENDOR_CHANGE_BAUD:
        *message = K1_BT_H5_BAUD_CHANGE_COMPLETE;
        break;

      case K1_BT_HCI_VENDOR_ROM_VER:
        if (frame->payload_length < 7 || payload[1] < 5)
          {
            return -EPROTO;
          }

        info->rom_version = payload[6];
        *message = K1_BT_H5_ROM_VERSION;
        break;

      case K1_BT_HCI_VENDOR_READ:
        if (frame->payload_length < 8 || payload[1] < 6)
          {
            return -EPROTO;
          }

        info->chip_type = payload[6] & 0x0fu;
        info->chip_version = payload[7] & 0x0fu;
        *message = K1_BT_H5_CHIP_TYPE;
        break;

      case K1_BT_HCI_VENDOR_PATCH:
        if (frame->payload_length < 7 || payload[1] < 5)
          {
            return -EPROTO;
          }

        link->patch_index = payload[6];
        *message = K1_BT_H5_PATCH_COMPLETE;
        break;

      default:
        break;
    }

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
          /* The RTL8852BS applies the final patch/config packet before it
           * reports Command Complete.  Its H5 RX sequence can restart at
           * that point; the vendor hciattach accepts precisely this event.
           */

          if (!link->patch_final_pending ||
              frame->type != K1_BT_H5_EVENT ||
              frame->payload_length < 5 ||
              frame->payload[0] != K1_BT_HCI_EVENT_COMPLETE ||
              frame->payload[3] != 0x20u ||
              frame->payload[4] != 0xfcu)
            {
              return k1_bt_h5_send_ack(link);
            }

          link->rxseq_txack = frame->sequence;
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
      if (frame_ret == OK && *message == K1_BT_H5_NONE)
        {
          frame_ret = k1_bt_h5_read_reset_complete(frame, message);
        }

      if (frame_ret == OK && *message == K1_BT_H5_NONE)
        {
          frame_ret = k1_bt_h5_read_vendor_complete(link, frame, info,
                                                     message);
        }
    }

  if (frame->reliable)
    {
      ret = k1_bt_h5_send_ack(link);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (*message == K1_BT_H5_PATCH_COMPLETE &&
      (link->patch_index & 0x80u) != 0)
    {
      link->patch_final_pending = false;
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
          link->rx_bytes++;
          if (!k1_bt_h5_consume(&link->rx, byte, &frame))
            {
              continue;
            }

          link->rx_frames++;

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

#if defined(CONFIG_K1_BT_H5_VENDOR_FIRMWARE) || \
    defined(CONFIG_K1_BT_H5_RESET_PROBE)
static int k1_bt_h5_reset_controller(FAR struct k1_bt_h5_link_s *link,
                                      FAR struct k1_bt_h5_info_s *info);
#endif

#ifdef CONFIG_K1_BT_H5_VENDOR_FIRMWARE
static int k1_bt_h5_query_rom_version(FAR struct k1_bt_h5_link_s *link,
                                      FAR struct k1_bt_h5_info_s *info)
{
  static const uint8_t g_read_rom_version[] =
  {
    0x6du, 0xfcu, 0x00u
  };

  int ret;

  ret = k1_bt_h5_send_frame(link, g_read_rom_version,
                             sizeof(g_read_rom_version),
                             K1_BT_H5_COMMAND, true);
  if (ret < 0)
    {
      return ret;
    }

  return k1_bt_h5_wait(link, info, K1_BT_H5_ROM_VERSION,
                       K1_BT_H5_HCI_WAIT_MSEC);
}

static int k1_bt_h5_query_chip_type(FAR struct k1_bt_h5_link_s *link,
                                    FAR struct k1_bt_h5_info_s *info)
{
  static const uint8_t g_read_chip_type[] =
  {
    0x61u, 0xfcu, 0x05u, 0x10u, 0xa6u, 0xadu, 0x00u, 0xb0u
  };

  int ret;

  ret = k1_bt_h5_send_frame(link, g_read_chip_type,
                             sizeof(g_read_chip_type),
                             K1_BT_H5_COMMAND, true);
  if (ret < 0)
    {
      return ret;
    }

  return k1_bt_h5_wait(link, info, K1_BT_H5_CHIP_TYPE,
                       K1_BT_H5_HCI_WAIT_MSEC);
}

static int k1_bt_h5_change_baud(FAR struct k1_bt_h5_link_s *link,
                                 FAR struct k1_bt_h5_info_s *info)
{
  static const uint8_t g_change_baud[] =
  {
    0x17u, 0xfcu, 0x04u,
    K1_BT_RTL8852BS_CONFIG_VENDOR_BAUD & UINT8_MAX,
    (K1_BT_RTL8852BS_CONFIG_VENDOR_BAUD >> 8) & UINT8_MAX,
    (K1_BT_RTL8852BS_CONFIG_VENDOR_BAUD >> 16) & UINT8_MAX,
    (K1_BT_RTL8852BS_CONFIG_VENDOR_BAUD >> 24) & UINT8_MAX
  };

  int ret;

  ret = k1_bt_h5_send_frame(link, g_change_baud, sizeof(g_change_baud),
                             K1_BT_H5_COMMAND, true);
  if (ret < 0)
    {
      return ret;
    }

  return k1_bt_h5_wait(link, info, K1_BT_H5_BAUD_CHANGE_COMPLETE,
                       K1_BT_H5_HCI_WAIT_MSEC);
}

static int k1_bt_h5_send_patch(FAR struct k1_bt_h5_link_s *link,
                                FAR const uint8_t *data, size_t length,
                                uint8_t index,
                                FAR struct k1_bt_h5_info_s *info)
{
  uint8_t command[K1_BT_H5_PATCH_DATA_MAX + 4u];
  int ret;

  if (length > K1_BT_H5_PATCH_DATA_MAX ||
      (length != 0 && data == NULL))
    {
      return -EINVAL;
    }

  command[0] = 0x20u;
  command[1] = 0xfcu;
  command[2] = length + 1u;
  command[3] = index;
  if (length != 0)
    {
      memcpy(&command[4], data, length);
    }

  if ((index & 0x80u) != 0)
    {
      link->patch_final_pending = true;
    }

  ret = k1_bt_h5_send_frame(link, command, length + 4u,
                             K1_BT_H5_COMMAND, true);
  if (ret < 0)
    {
      return ret;
    }

  if ((index & 0x80u) != 0 &&
      K1_BT_RTL8852BS_CONFIG_HARDWARE_FLOW_CONTROL != 0u)
    {
      ret = k1_bt_uart_wait_tx_empty();
      if (ret < 0)
        {
          return ret;
        }

      /* The Realtek config switches the controller to RTS/CTS when it
       * accepts its final patch packet.  Match rtk_hciattach before waiting
       * for that packet's Command Complete event.
       */

      k1_bt_uart_set_flow_control(true);
      k1_early_puts("K1 Bluetooth: H5 hardware flow control enabled\r\n");
    }

  ret = k1_bt_h5_wait(link, info, K1_BT_H5_PATCH_COMPLETE,
                       K1_BT_H5_HCI_WAIT_MSEC);
  if (ret < 0)
    {
      return ret;
    }

  return (link->patch_index & 0x7fu) == (index & 0x7fu) ? OK : -EPROTO;
}

static int k1_bt_h5_align_tx_sequence(FAR struct k1_bt_h5_link_s *link,
                                       FAR struct k1_bt_h5_info_s *info)
{
  int ret;

  /* bt_slip starts its next H5 connection attempt at sequence zero.  Match
   * the vendor attacher's padding behavior after the patch/reset exchange.
   */

  while (link->txseq != 0)
    {
      ret = k1_bt_h5_query_rom_version(link, info);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static int k1_bt_h5_load_vendor_firmware(
  FAR struct k1_bt_h5_link_s *link,
  FAR struct k1_bt_h5_info_s *info)
{
  size_t data_packets;
  size_t packet_count;
  size_t offset = 0;
  uint8_t padding_packets;
  uint8_t packet_index = 0;
  int ret;

  if (info->lmp_subversion != K1_BT_RTL8852BS_ROM_SUBVER)
    {
      return -ENODEV;
    }

  ret = k1_bt_h5_query_rom_version(link, info);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_bt_h5_query_chip_type(link, info);
  if (ret < 0)
    {
      return ret;
    }

  if (info->rom_version != 1u ||
      info->chip_type != K1_BT_RTL8852BS_CHIP_TYPE)
    {
      return -ENODEV;
    }

  k1_early_puts("K1 Bluetooth: RTL8852BS H5 ROM=");
  k1_early_puthex(info->rom_version);
  k1_early_puts(" chip=");
  k1_early_puthex(info->chip_type);
  k1_early_puts(" baud command=");
  k1_early_puthex(K1_BT_RTL8852BS_CONFIG_VENDOR_BAUD);
  k1_early_puts("\r\n");

  ret = k1_bt_h5_change_baud(link, info);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_bt_uart_wait_tx_empty();
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(K1_BT_H5_BAUD_SETTLE_MSEC);
  ret = k1_bt_uart_set_baud(K1_BT_RTL8852BS_CONFIG_UART_BAUD);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Bluetooth: H5 UART baud=");
  k1_early_puthex(K1_BT_RTL8852BS_CONFIG_UART_BAUD);
  k1_early_puts("\r\n");

  data_packets = (sizeof(g_k1_bt_rtl8852bs_patch) +
                  K1_BT_H5_PATCH_DATA_MAX - 1u) /
                 K1_BT_H5_PATCH_DATA_MAX;
  padding_packets = (8u - ((link->txseq + data_packets) & 0x07u)) & 0x07u;
  if (padding_packets != 0)
    {
      padding_packets--;
    }
  else
    {
      padding_packets = 7u;
    }

  packet_count = data_packets + padding_packets;
  k1_early_puts("K1 Bluetooth: H5 patch data=");
  k1_early_puthex(data_packets);
  k1_early_puts(" total=");
  k1_early_puthex(packet_count);
  k1_early_puts("\r\n");

  while (info->patch_packets < packet_count)
    {
      FAR const uint8_t *data = NULL;
      size_t length = 0;
      uint8_t command_index = packet_index;

      if (offset < sizeof(g_k1_bt_rtl8852bs_patch))
        {
          data = &g_k1_bt_rtl8852bs_patch[offset];
          length = sizeof(g_k1_bt_rtl8852bs_patch) - offset;
          if (length > K1_BT_H5_PATCH_DATA_MAX)
            {
              length = K1_BT_H5_PATCH_DATA_MAX;
            }
        }

      if (info->patch_packets + 1u == packet_count)
        {
          command_index |= 0x80u;
        }

      ret = k1_bt_h5_send_patch(link, data, length, command_index, info);
      if (ret < 0)
        {
          return ret;
        }

      offset += length;
      info->patch_packets++;
      packet_index++;
      if (packet_index == 0x80u)
        {
          packet_index = 1u;
        }
    }

  k1_early_puts("K1 Bluetooth: H5 patch complete\r\n");

  ret = k1_bt_h5_reset_controller(link, info);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Bluetooth: H5 post-patch reset complete\r\n");

  ret = k1_bt_h5_query_local_version(link, info);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Bluetooth: H5 post-patch subversion=");
  k1_early_puthex(info->lmp_subversion);
  k1_early_puts("\r\n");

  if (info->lmp_subversion == K1_BT_RTL8852BS_ROM_SUBVER)
    {
      return -EIO;
    }

  ret = k1_bt_h5_align_tx_sequence(link, info);
  if (ret < 0)
    {
      return ret;
    }

  info->vendor_firmware_loaded = true;
  return OK;
}
#endif

#if defined(CONFIG_K1_BT_H5_VENDOR_FIRMWARE) || \
    defined(CONFIG_K1_BT_H5_RESET_PROBE)
static int k1_bt_h5_reset_controller(FAR struct k1_bt_h5_link_s *link,
                                      FAR struct k1_bt_h5_info_s *info)
{
  static const uint8_t g_hci_reset[] =
  {
    0x03u, 0x0cu, 0x00u
  };

  int ret;

  ret = k1_bt_h5_send_frame(link, g_hci_reset, sizeof(g_hci_reset),
                             K1_BT_H5_COMMAND, true);
  if (ret < 0)
    {
      return ret;
    }

  return k1_bt_h5_wait(link, info, K1_BT_H5_RESET_COMPLETE,
                       K1_BT_H5_HCI_WAIT_MSEC);
}
#endif

#ifndef CONFIG_K1_BT_H5_HOST
static void k1_bt_h5_transport_reset(FAR struct k1_bt_h5_transport_s *priv)
{
  priv->rx_length = 0;
  priv->rx_escaped = false;
  priv->rx_started = false;
}

static bool k1_bt_h5_transport_append(
  FAR struct k1_bt_h5_transport_s *priv, uint8_t byte)
{
  if (priv->rx_length >= CONFIG_K1_BT_H5_RX_FRAME_MAX)
    {
      k1_bt_h5_transport_reset(priv);
      return false;
    }

  priv->rx_buffer[priv->rx_length++] = byte;
  return true;
}

static bool k1_bt_h5_transport_consume(
  FAR struct k1_bt_h5_transport_s *priv, uint8_t byte)
{
  if (byte == K1_BT_H5_DELIMITER)
    {
      if (!priv->rx_started)
        {
          priv->rx_started = true;
          priv->rx_escaped = false;
          priv->rx_length = 0;
          (void)k1_bt_h5_transport_append(priv, byte);
          return false;
        }

      if (priv->rx_length == 1)
        {
          priv->rx_escaped = false;
          return false;
        }

      return k1_bt_h5_transport_append(priv, byte);
    }

  if (!priv->rx_started)
    {
      return false;
    }

  if (priv->rx_escaped)
    {
      priv->rx_escaped = false;

      /* bt_slip.c predates the optional H5 XON/XOFF escape codes.  Decode
       * only those two codes here and retain its C0/DB escaping unchanged.
       */

      if (byte == K1_BT_H5_ESCAPE_XON)
        {
          (void)k1_bt_h5_transport_append(priv, 0x11u);
          return false;
        }

      if (byte == K1_BT_H5_ESCAPE_XOFF)
        {
          (void)k1_bt_h5_transport_append(priv, 0x13u);
          return false;
        }

      if (!k1_bt_h5_transport_append(priv, K1_BT_H5_ESCAPE))
        {
          return false;
        }

      (void)k1_bt_h5_transport_append(priv, byte);
      return false;
    }

  if (byte == K1_BT_H5_ESCAPE)
    {
      priv->rx_escaped = true;
      return false;
    }

  (void)k1_bt_h5_transport_append(priv, byte);
  return false;
}
#endif

#ifdef CONFIG_K1_BT_H5_HOST
static int k1_bt_h5_transport_input_type(uint8_t h5_type,
                                          FAR enum bt_buf_type_e *bt_type)
{
  switch (h5_type)
    {
      case K1_BT_H5_EVENT:
        *bt_type = BT_EVT;
        return OK;

      case K1_BT_H5_ACL:
        *bt_type = BT_ACL_IN;
        return OK;

      case K1_BT_H5_ISO:
        *bt_type = BT_ISO_IN;
        return OK;

      default:
        return -ENOMSG;
    }
}

static int k1_bt_h5_transport_output_type(enum bt_buf_type_e bt_type,
                                           FAR uint8_t *h5_type)
{
  switch (bt_type)
    {
      case BT_CMD:
        *h5_type = K1_BT_H5_COMMAND;
        return OK;

      case BT_ACL_OUT:
        *h5_type = K1_BT_H5_ACL;
        return OK;

      case BT_ISO_OUT:
        *h5_type = K1_BT_H5_ISO;
        return OK;

      default:
        return -EINVAL;
    }
}

static void k1_bt_h5_transport_worker(FAR void *arg)
{
  FAR struct k1_bt_h5_transport_s *priv = arg;
  bool reschedule;
  unsigned int count;
  int ret;

  for (count = 0; count < K1_BT_H5_RX_PER_TICK; count++)
    {
      struct k1_bt_h5_frame_s frame;
      struct k1_bt_h5_info_s ignored_info;
      enum k1_bt_h5_message_e ignored_message;
      enum bt_buf_type_e bt_type;
      bool received = false;

      ret = nxmutex_lock(&priv->state_lock);
      if (ret < 0)
        {
          return;
        }

      if (!priv->opened ||
          (k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) == 0)
        {
          nxmutex_unlock(&priv->state_lock);
          break;
        }

      priv->link.rx_bytes++;
      if (k1_bt_h5_consume(&priv->link.rx,
                           k1_bt_uart_getreg(K1_UART_RBR_OFFSET) & UINT8_MAX,
                           &frame))
        {
          priv->link.rx_frames++;
          ret = nxmutex_lock(&priv->tx_lock);
          if (ret >= 0)
            {
              ret = k1_bt_h5_handle_frame(&priv->link, &frame, &ignored_info,
                                           &ignored_message);
              nxmutex_unlock(&priv->tx_lock);
            }

          if (ret >= 0 &&
              k1_bt_h5_transport_input_type(frame.type, &bt_type) == OK)
            {
              received = true;
            }
        }

      nxmutex_unlock(&priv->state_lock);
      if (ret < 0)
        {
          k1_early_puts("K1 Bluetooth: H5 frame error=\r\n");
          k1_early_puthex((uintreg_t)-ret);
          continue;
        }

      if (received && priv->lower.receive != NULL)
        {
          ret = priv->lower.receive(&priv->lower, bt_type,
                                    (FAR void *)frame.payload,
                                    frame.payload_length);
          if (ret < 0)
            {
              k1_early_puts("K1 Bluetooth: HCI receive error=\r\n");
              k1_early_puthex((uintreg_t)-ret);
            }
        }
    }

  ret = nxmutex_lock(&priv->state_lock);
  if (ret < 0)
    {
      return;
    }

  reschedule = priv->opened;
  nxmutex_unlock(&priv->state_lock);
  if (reschedule)
    {
      ret = work_queue(LPWORK, &priv->rx_work, k1_bt_h5_transport_worker,
                       priv, MSEC2TICK(K1_BT_H5_RX_POLL_MSEC));
      if (ret < 0)
        {
          k1_early_puts("K1 Bluetooth: H5 polling error=\r\n");
          k1_early_puthex((uintreg_t)-ret);
        }
    }
}
#else
static void k1_bt_h5_transport_worker(FAR void *arg)
{
  FAR struct k1_bt_h5_transport_s *priv = arg;
  bool complete;
  bool reschedule;
  unsigned int count;
  int ret;

  for (count = 0; count < K1_BT_H5_RX_PER_TICK; count++)
    {
      ret = nxmutex_lock(&priv->state_lock);
      if (ret < 0)
        {
          return;
        }

      if (!priv->opened ||
          (k1_bt_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) == 0)
        {
          nxmutex_unlock(&priv->state_lock);
          break;
        }

      complete = k1_bt_h5_transport_consume(
        priv, k1_bt_uart_getreg(K1_UART_RBR_OFFSET) & UINT8_MAX);
      nxmutex_unlock(&priv->state_lock);

      if (complete)
        {
          if (priv->lower.receive != NULL)
            {
              ret = priv->lower.receive(&priv->lower, BT_EVT,
                                        priv->rx_buffer, priv->rx_length);
              if (ret < 0)
                {
                  k1_early_puts("K1 Bluetooth: H5 frame error=\r\n");
                  k1_early_puthex((uintreg_t)-ret);
                }
            }

          nxmutex_lock(&priv->state_lock);
          k1_bt_h5_transport_reset(priv);
          nxmutex_unlock(&priv->state_lock);
        }
    }

  ret = nxmutex_lock(&priv->state_lock);
  if (ret < 0)
    {
      return;
    }

  reschedule = priv->opened;
  nxmutex_unlock(&priv->state_lock);

  if (reschedule)
    {
      ret = work_queue(LPWORK, &priv->rx_work, k1_bt_h5_transport_worker,
                       priv, MSEC2TICK(K1_BT_H5_RX_POLL_MSEC));
      if (ret < 0)
        {
          k1_early_puts("K1 Bluetooth: H5 polling error=\r\n");
          k1_early_puthex((uintreg_t)-ret);
        }
    }
}
#endif

static int k1_bt_h5_transport_open(FAR struct bt_driver_s *lower)
{
  FAR struct k1_bt_h5_transport_s *priv =
    (FAR struct k1_bt_h5_transport_s *)lower;
  int ret;

  ret = nxmutex_lock(&priv->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->opened)
    {
      nxmutex_unlock(&priv->state_lock);
      return OK;
    }

  /* The initial H5 exchange leaves the controller active at its configured
   * baud rate.  Host mode must retain that exact link state and hand its HCI
   * packets to the controller directly.
   */

#ifdef CONFIG_K1_BT_H5_HOST
  ret = g_k1_bt_h5_handoff_ready ? OK : -ENOTCONN;
#else
  ret = k1_bt_uart_prepare();
#endif
  if (ret >= 0)
    {
#ifdef CONFIG_K1_BT_H5_HOST
      priv->link = g_k1_bt_h5_handoff_link;
#else
      k1_bt_uart_drain();
      k1_bt_h5_transport_reset(priv);
#endif
      priv->opened = true;
    }

  nxmutex_unlock(&priv->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = work_queue(LPWORK, &priv->rx_work, k1_bt_h5_transport_worker,
                   priv, 0);
  if (ret < 0)
    {
      nxmutex_lock(&priv->state_lock);
      priv->opened = false;
      nxmutex_unlock(&priv->state_lock);
      return ret;
    }

  k1_early_puts("K1 Bluetooth: H5 transport opened\r\n");
  return OK;
}

static int k1_bt_h5_transport_send(FAR struct bt_driver_s *lower,
                                    enum bt_buf_type_e type,
                                    FAR void *data, size_t length)
{
  FAR struct k1_bt_h5_transport_s *priv =
    (FAR struct k1_bt_h5_transport_s *)lower;
#ifdef CONFIG_K1_BT_H5_HOST
  uint8_t h5_type;
#else
  FAR const uint8_t *buffer = data;
  bool escaped = false;
  size_t index;
#endif
  bool opened;
  int ret;

  if (data == NULL || length == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  opened = priv->opened;
  if (!opened)
    {
      nxmutex_unlock(&priv->state_lock);
      return -ENOTCONN;
    }

#ifndef CONFIG_K1_BT_H5_HOST
  nxmutex_unlock(&priv->state_lock);
#endif

  ret = nxmutex_lock(&priv->tx_lock);
  if (ret < 0)
    {
#ifdef CONFIG_K1_BT_H5_HOST
      nxmutex_unlock(&priv->state_lock);
#endif
      return ret;
    }

#ifdef CONFIG_K1_BT_H5_HOST
  ret = k1_bt_h5_transport_output_type(type, &h5_type);
  if (ret >= 0)
    {
      ret = k1_bt_h5_send_frame(&priv->link, data, length, h5_type, true);
    }

  nxmutex_unlock(&priv->tx_lock);
  nxmutex_unlock(&priv->state_lock);
  return ret < 0 ? ret : (int)length;
#else
  for (index = 0; index < length; index++)
    {
      if (!escaped && buffer[index] == K1_BT_H5_ESCAPE)
        {
          escaped = true;
        }
      else if (!escaped &&
               (buffer[index] == 0x11u || buffer[index] == 0x13u))
        {
          ret = k1_bt_uart_write_byte(K1_BT_H5_ESCAPE);
          if (ret >= 0)
            {
              ret = k1_bt_uart_write_byte(buffer[index] == 0x11u ?
                                           K1_BT_H5_ESCAPE_XON :
                                           K1_BT_H5_ESCAPE_XOFF);
            }

          if (ret < 0)
            {
              break;
            }

          continue;
        }
      else
        {
          escaped = false;
        }

      ret = k1_bt_uart_write_byte(buffer[index]);
      if (ret < 0)
        {
          break;
        }
    }

  nxmutex_unlock(&priv->tx_lock);
  return ret < 0 ? ret : (int)length;
#endif
}

static void k1_bt_h5_transport_close(FAR struct bt_driver_s *lower)
{
  FAR struct k1_bt_h5_transport_s *priv =
    (FAR struct k1_bt_h5_transport_s *)lower;

  nxmutex_lock(&priv->state_lock);
  priv->opened = false;
#ifdef CONFIG_K1_BT_H5_HOST
  k1_bt_h5_reset(&priv->link.rx);
#else
  k1_bt_h5_transport_reset(priv);
#endif
  nxmutex_unlock(&priv->state_lock);
  work_cancel_sync(LPWORK, &priv->rx_work);
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

  memset(info, 0, sizeof(*info));

#ifdef CONFIG_K1_BT_H5_HOST
  g_k1_bt_h5_handoff_ready = false;
#endif

  ret = k1_bt_uart_prepare();
  if (ret < 0)
    {
      return ret;
    }

  k1_bt_uart_drain();
  k1_bt_h5_reset(&link.rx);
  link.rx_bytes = 0;
  link.tx_frames = 0;
  link.rx_frames = 0;
  link.txseq = 0;
  link.rxseq_txack = 0;
  link.patch_index = 0;
  link.patch_final_pending = false;
  link.use_crc = false;
  ret = k1_bt_h5_negotiate(&link, info);
  if (ret < 0)
    {
      k1_bt_uart_report(&link);
      return ret;
    }

  ret = k1_bt_h5_query_local_version(&link, info);
  if (ret < 0)
    {
      k1_bt_uart_report(&link);
      return ret;
    }

#ifdef CONFIG_K1_BT_H5_VENDOR_FIRMWARE
  ret = k1_bt_h5_load_vendor_firmware(&link, info);
  if (ret < 0)
    {
      k1_bt_uart_report(&link);
      return ret;
    }

  k1_early_puts("K1 Bluetooth: RTL8852BS H5 patch packets=");
  k1_early_puthex(info->patch_packets);
  k1_early_puts(" ROM=");
  k1_early_puthex(info->rom_version);
  k1_early_puts(" chip=");
  k1_early_puthex(info->chip_type);
  k1_early_puts(" post-subversion=");
  k1_early_puthex(info->lmp_subversion);
  k1_early_puts("\r\n");
#elif defined(CONFIG_K1_BT_H5_RESET_PROBE)
  ret = k1_bt_h5_reset_controller(&link, info);
  if (ret < 0)
    {
      k1_bt_uart_report(&link);
      return ret;
    }

  k1_early_puts("K1 Bluetooth: H5 HCI reset complete\r\n");
#endif

  info->crc_enabled = link.use_crc;

#ifdef CONFIG_K1_BT_H5_HOST
  g_k1_bt_h5_handoff_link = link;
  g_k1_bt_h5_handoff_ready = true;
#endif

  return OK;
}

int k1_bt_uart_register(void)
{
  FAR struct k1_bt_h5_transport_s *priv;
  int ret;

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

#ifndef CONFIG_K1_BT_H5_HOST
  priv->rx_buffer = kmm_zalloc(CONFIG_K1_BT_H5_RX_FRAME_MAX);
  if (priv->rx_buffer == NULL)
    {
      kmm_free(priv);
      return -ENOMEM;
    }
#endif

  priv->lower.open = k1_bt_h5_transport_open;
  priv->lower.send = k1_bt_h5_transport_send;
  priv->lower.close = k1_bt_h5_transport_close;
  nxmutex_init(&priv->state_lock);
  nxmutex_init(&priv->tx_lock);

  ret = bt_driver_register(&priv->lower);
  if (ret < 0)
    {
      nxmutex_destroy(&priv->tx_lock);
      nxmutex_destroy(&priv->state_lock);
#ifndef CONFIG_K1_BT_H5_HOST
      kmm_free(priv->rx_buffer);
#endif
      kmm_free(priv);
      return ret;
    }

#ifdef CONFIG_K1_BT_H5_HOST
  k1_early_puts("K1 Bluetooth: H5 host stack registered\r\n");
#else
  k1_early_puts("K1 Bluetooth: H5 raw HCI registered /dev/ttyHCI0\r\n");
#endif
  return OK;
}

#endif /* CONFIG_K1_RTL8852BS2_BT */
