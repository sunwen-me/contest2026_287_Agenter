/****************************************************************************
 * apps/external/k1_watchdog_smoke/k1_wdt_smoke.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/timers/watchdog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_WDT_SMOKE_TIMEOUT_MSEC  10000ul
#define K1_WDT_SMOKE_PING_MSEC      1000ul
#define K1_WDT_SMOKE_PING_COUNT         3

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int k1_wdt_smoke_status(int fd, bool active)
{
  struct watchdog_status_s status;
  int ret;

  ret = ioctl(fd, WDIOC_GETSTATUS, (unsigned long)&status);
  if (ret < 0)
    {
      printf("k1_wdt_smoke: FAIL status: %d\n", errno);
      return -errno;
    }

  if (status.timeout != K1_WDT_SMOKE_TIMEOUT_MSEC ||
      status.timeleft > status.timeout ||
      ((status.flags & WDFLAGS_ACTIVE) != 0) != active ||
      (status.flags & WDFLAGS_RESET) == 0)
    {
      printf("k1_wdt_smoke: FAIL invalid status flags=%08lx timeout=%lu "
             "timeleft=%lu\n", (unsigned long)status.flags,
             (unsigned long)status.timeout, (unsigned long)status.timeleft);
      return -EIO;
    }

  printf("k1_wdt_smoke: status flags=%08lx timeout=%lu timeleft=%lu\n",
         (unsigned long)status.flags, (unsigned long)status.timeout,
         (unsigned long)status.timeleft);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int ping;
  int ret;
  bool started = false;

  (void)argc;
  (void)argv;

  fd = open(CONFIG_WATCHDOG_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      printf("k1_wdt_smoke: FAIL open %s: %d\n", CONFIG_WATCHDOG_DEVPATH,
             errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_SETTIMEOUT, K1_WDT_SMOKE_TIMEOUT_MSEC);
  if (ret < 0)
    {
      printf("k1_wdt_smoke: FAIL settimeout: %d\n", errno);
      goto errout;
    }

  ret = ioctl(fd, WDIOC_START, 0);
  if (ret < 0)
    {
      printf("k1_wdt_smoke: FAIL start: %d\n", errno);
      goto errout;
    }

  started = true;
  for (ping = 1; ping <= K1_WDT_SMOKE_PING_COUNT; ping++)
    {
      usleep(K1_WDT_SMOKE_PING_MSEC * 1000ul);
      ret = ioctl(fd, WDIOC_KEEPALIVE, 0);
      if (ret < 0)
        {
          printf("k1_wdt_smoke: FAIL keepalive %d: %d\n", ping, errno);
          goto errout;
        }

      ret = k1_wdt_smoke_status(fd, true);
      if (ret < 0)
        {
          goto errout;
        }
    }

  ret = ioctl(fd, WDIOC_STOP, 0);
  if (ret < 0)
    {
      printf("k1_wdt_smoke: FAIL stop: %d\n", errno);
      goto errout;
    }

  started = false;
  ret = k1_wdt_smoke_status(fd, false);
  if (ret < 0)
    {
      goto errout;
    }

  close(fd);
  printf("k1_wdt_smoke: PASS watchdog started, pinged, and stopped\n");
  return EXIT_SUCCESS;

errout:
  if (started)
    {
      ioctl(fd, WDIOC_STOP, 0);
    }

  close(fd);
  return EXIT_FAILURE;
}
