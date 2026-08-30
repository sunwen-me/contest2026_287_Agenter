#define _POSIX_C_SOURCE 200809L

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read aligned 32-bit K1 MMIO registers through /dev/mem.  Build and run
 * this helper on the board's Linux image only; it never opens a writable
 * mapping and accepts addresses on its command line.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define K1_SDH1_BASE             0xd4280800u
#define K1_SDHC_BLOCK_SIZE        0x004u
#define K1_SDHC_BLOCK_COUNT       0x006u
#define K1_SDHC_ARGUMENT          0x008u
#define K1_SDHC_TRANSFER_MODE     0x00cu
#define K1_SDHC_COMMAND           0x00eu
#define K1_SDHC_PRESENT_STATE     0x024u
#define K1_SDHC_HOST_CONTROL      0x028u
#define K1_SDHC_POWER_CONTROL     0x029u
#define K1_SDHC_CLOCK_CONTROL     0x02cu
#define K1_SDHC_TIMEOUT_CONTROL   0x02eu
#define K1_SDHC_INT_STATUS        0x030u
#define K1_SDHC_INT_ENABLE        0x034u
#define K1_SDHC_SIGNAL_ENABLE     0x038u
#define K1_SDHC_HOST_CONTROL2     0x03eu
#define K1_SDHC_ADMA_ERROR        0x054u
#define K1_SDHC_ADMA_ADDRESS      0x058u
#define K1_SDHC_OP_EXT            0x108u
#define K1_SDHC_MMC_CONTROL        0x114u
#define K1_SDHC_RX_CONTROL         0x118u
#define K1_SDHC_TX_CONTROL         0x11cu
#define K1_SDHC_DLINE_CONTROL      0x130u
#define K1_SDHC_DLINE_CONFIG       0x134u

#define K1_SDHC_PRESENT_DATA_INHIBIT (1u << 1)
#define K1_SDHC_CMD53              53u
#define K1_SDHC_WATCH_TIMEOUT_SEC  30u

static uint16_t read16(volatile const uint8_t *base, uintptr_t offset)
{
  volatile const uint16_t *reg;

  reg = (volatile const uint16_t *)(base + offset);
  return *reg;
}

static uint32_t read32(volatile const uint8_t *base, uintptr_t offset)
{
  volatile const uint32_t *reg;

  reg = (volatile const uint32_t *)(base + offset);
  return *reg;
}

static uint8_t read8(volatile const uint8_t *base, uintptr_t offset)
{
  return *(base + offset);
}

static unsigned long elapsed_seconds(const struct timespec *start,
                                     const struct timespec *now)
{
  time_t seconds;

  seconds = now->tv_sec - start->tv_sec;
  if (now->tv_nsec < start->tv_nsec)
    {
      seconds--;
    }

  return seconds < 0 ? 0ul : (unsigned long)seconds;
}

static int watch_cmd53(int fd, long pagesize, unsigned int sample_count)
{
  uintptr_t page;
  void *mapping;
  volatile const uint8_t *base;
  struct timespec start;
  struct timespec now;
  bool active = false;
  unsigned int captured = 0;

  page = K1_SDH1_BASE & ~((uintptr_t)pagesize - 1);
  mapping = mmap(NULL, (size_t)pagesize, PROT_READ, MAP_SHARED, fd,
                 (off_t)page);
  if (mapping == MAP_FAILED)
    {
      perror("mmap(/dev/mem)");
      return EXIT_FAILURE;
    }

  base = (volatile const uint8_t *)mapping + (K1_SDH1_BASE - page);
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    {
      perror("clock_gettime");
      munmap(mapping, (size_t)pagesize);
      return EXIT_FAILURE;
    }

  while (captured < sample_count)
    {
      uint16_t command = read16(base, K1_SDHC_COMMAND);
      uint32_t present = read32(base, K1_SDHC_PRESENT_STATE);
      bool current;

      current = ((command >> 8) & 0x3fu) == K1_SDHC_CMD53 &&
                (present & K1_SDHC_PRESENT_DATA_INHIBIT) != 0;
      if (current && !active)
        {
          printf("CMD53 sample %u\n", captured + 1);
          printf("  BLOCK_SIZE=0x%04x BLOCK_COUNT=0x%04x\n",
                 read16(base, K1_SDHC_BLOCK_SIZE),
                 read16(base, K1_SDHC_BLOCK_COUNT));
          printf("  ARGUMENT=0x%08x TRANSFER_MODE=0x%04x COMMAND=0x%04x\n",
                 read32(base, K1_SDHC_ARGUMENT),
                 read16(base, K1_SDHC_TRANSFER_MODE), command);
          printf("  PRESENT_STATE=0x%08x HOST=0x%02x POWER=0x%02x\n",
                 present, read8(base, K1_SDHC_HOST_CONTROL),
                 read8(base, K1_SDHC_POWER_CONTROL));
          printf("  CLOCK=0x%04x TIMEOUT=0x%02x HOST_CONTROL2=0x%04x\n",
                 read16(base, K1_SDHC_CLOCK_CONTROL),
                 read8(base, K1_SDHC_TIMEOUT_CONTROL),
                 read16(base, K1_SDHC_HOST_CONTROL2));
          printf("  INT_STATUS=0x%08x INT_ENABLE=0x%08x SIGNAL_ENABLE=0x%08x\n",
                 read32(base, K1_SDHC_INT_STATUS),
                 read32(base, K1_SDHC_INT_ENABLE),
                 read32(base, K1_SDHC_SIGNAL_ENABLE));
          printf("  ADMA_ERROR=0x%08x ADMA_ADDRESS=0x%08x\n",
                 read32(base, K1_SDHC_ADMA_ERROR),
                 read32(base, K1_SDHC_ADMA_ADDRESS));
          printf("  OP_EXT=0x%08x MMC_CONTROL=0x%08x RX_CONTROL=0x%08x\n",
                 read32(base, K1_SDHC_OP_EXT),
                 read32(base, K1_SDHC_MMC_CONTROL),
                 read32(base, K1_SDHC_RX_CONTROL));
          printf("  TX_CONTROL=0x%08x DLINE_CONTROL=0x%08x DLINE_CONFIG=0x%08x\n",
                 read32(base, K1_SDHC_TX_CONTROL),
                 read32(base, K1_SDHC_DLINE_CONTROL),
                 read32(base, K1_SDHC_DLINE_CONFIG));
          fflush(stdout);
          captured++;
        }

      active = current;
      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        {
          perror("clock_gettime");
          munmap(mapping, (size_t)pagesize);
          return EXIT_FAILURE;
        }

      if (elapsed_seconds(&start, &now) >= K1_SDHC_WATCH_TIMEOUT_SEC)
        {
          fprintf(stderr, "timed out waiting for CMD53 data phase\n");
          munmap(mapping, (size_t)pagesize);
          return EXIT_FAILURE;
        }
    }

  munmap(mapping, (size_t)pagesize);
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
  long pagesize;
  int fd;
  int index;

  if (argc < 2)
    {
      fprintf(stderr, "Usage: %s <aligned-mmio-address> [...]\n", argv[0]);
      fprintf(stderr, "       %s --watch-cmd53 <sample-count>\n", argv[0]);
      return EXIT_FAILURE;
    }

  pagesize = sysconf(_SC_PAGESIZE);
  if (pagesize <= 0)
    {
      perror("sysconf(_SC_PAGESIZE)");
      return EXIT_FAILURE;
    }

  fd = open("/dev/mem", O_RDONLY | O_SYNC);
  if (fd < 0)
    {
      perror("open(/dev/mem)");
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "--watch-cmd53") == 0)
    {
      char *end;
      unsigned long requested;
      int ret;

      if (argc != 3)
        {
          fprintf(stderr, "--watch-cmd53 requires one sample count\n");
          close(fd);
          return EXIT_FAILURE;
        }

      errno = 0;
      requested = strtoul(argv[2], &end, 0);
      if (errno != 0 || *argv[2] == '\0' || *end != '\0' ||
          requested == 0 || requested > UINT_MAX)
        {
          fprintf(stderr, "invalid sample count: %s\n", argv[2]);
          close(fd);
          return EXIT_FAILURE;
        }

      ret = watch_cmd53(fd, pagesize, (unsigned int)requested);
      close(fd);
      return ret;
    }

  for (index = 1; index < argc; index++)
    {
      char *end;
      uintptr_t address;
      uintptr_t page;
      size_t offset;
      void *mapping;
      volatile const uint32_t *reg;

      errno = 0;
      address = (uintptr_t)strtoull(argv[index], &end, 0);
      if (errno != 0 || *argv[index] == '\0' || *end != '\0' ||
          (address & (sizeof(uint32_t) - 1)) != 0)
        {
          fprintf(stderr, "invalid aligned address: %s\n", argv[index]);
          close(fd);
          return EXIT_FAILURE;
        }

      page = address & ~((uintptr_t)pagesize - 1);
      offset = address - page;
      mapping = mmap(NULL, (size_t)pagesize, PROT_READ, MAP_SHARED, fd,
                     (off_t)page);
      if (mapping == MAP_FAILED)
        {
          perror("mmap(/dev/mem)");
          close(fd);
          return EXIT_FAILURE;
        }

      reg = (volatile const uint32_t *)((const uint8_t *)mapping + offset);
      printf("0x%08" PRIxPTR ": 0x%08" PRIx32 "\n", address, *reg);
      munmap(mapping, (size_t)pagesize);
    }

  close(fd);
  return EXIT_SUCCESS;
}
