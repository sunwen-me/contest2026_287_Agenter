/****************************************************************************
 * vendor/spacemit/chips/k1/k1_console.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/signal.h>

#include "hardware/k1_uart.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_K1_PRESERVE_BOOT_UART
#  error "K1 polling console requires the inherited U-Boot UART setup"
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static ssize_t k1_console_read(FAR struct file *filep, FAR char *buffer,
                               size_t buflen);
static ssize_t k1_console_write(FAR struct file *filep,
                                FAR const char *buffer, size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_k1_console_fops =
{
  .read  = k1_console_read,
  .write = k1_console_write,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t k1_uart_getreg(unsigned int offset)
{
  return getreg32(K1_UART0_BASE + offset);
}

static inline void k1_uart_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, K1_UART0_BASE + offset);
}

static bool k1_uart_rxready(void)
{
  return (k1_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_DR) != 0;
}

static void k1_uart_send(int ch)
{
  while ((k1_uart_getreg(K1_UART_LSR_OFFSET) & K1_UART_LSR_THRE) == 0)
    {
    }

  k1_uart_putreg(K1_UART_THR_OFFSET, (uint8_t)ch);
}

static ssize_t k1_console_read(FAR struct file *filep, FAR char *buffer,
                               size_t buflen)
{
  size_t nread = 0;

  if (buffer == NULL)
    {
      return -EINVAL;
    }

  if (buflen == 0)
    {
      return 0;
    }

  while (!k1_uart_rxready())
    {
      if ((filep->f_oflags & O_NONBLOCK) != 0)
        {
          return -EAGAIN;
        }

      nxsig_usleep(CONFIG_K1_UART_POLL_USEC);
    }

  do
    {
      buffer[nread++] =
        (char)(k1_uart_getreg(K1_UART_RBR_OFFSET) & UINT8_MAX);
    }
  while (nread < buflen && k1_uart_rxready());

  return (ssize_t)nread;
}

static ssize_t k1_console_write(FAR struct file *filep,
                                FAR const char *buffer, size_t buflen)
{
  size_t nwritten;

  UNUSED(filep);

  if (buffer == NULL)
    {
      return -EINVAL;
    }

  for (nwritten = 0; nwritten < buflen; nwritten++)
    {
      k1_uart_send(buffer[nwritten]);
    }

  return (ssize_t)nwritten;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k1_early_puts(FAR const char *str)
{
  while (*str != '\0')
    {
      k1_uart_send(*str++);
    }
}

void k1_early_puthex(uintreg_t value)
{
  int shift;
  int digit;

  k1_uart_send('0');
  k1_uart_send('x');

  for (shift = sizeof(value) * 8 - 4; shift >= 0; shift -= 4)
    {
      digit = (value >> shift) & 0xf;
      k1_uart_send(digit < 10 ? digit + '0' : digit - 10 + 'a');
    }
}

void riscv_earlyserialinit(void)
{
  /* Preserve the complete UART configuration inherited from U-Boot.
   * In particular, this driver never writes K1_UART_IER_OFFSET.
   */
}

void riscv_serialinit(void)
{
  int ret;

  ret = register_driver("/dev/console", &g_k1_console_fops, 0666, NULL);

#ifdef CONFIG_K1_EARLY_BOOT_LOG
  if (ret < 0)
    {
      k1_early_puts("K1: /dev/console registration failed\r\n");
    }
#else
  UNUSED(ret);
#endif
}

void riscv_lowputc(char ch)
{
  k1_uart_send(ch);
}

void up_putc(int ch)
{
  k1_uart_send(ch);
}
