/****************************************************************************
 * vendor/spacemit/chips/k1/k1_emac.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_K1_EMAC

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <net/if.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/net/ioctl.h>
#include <nuttx/net/mii.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/wqueue.h>

#include <debug.h>

#include "hardware/k1_emac.h"
#include "k1_cache.h"
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_EMAC_MDIO_POLL_USEC       100u
#define K1_EMAC_MDIO_TIMEOUT_USEC    10000u

#define K1_EMAC_RGMII_FIRST_GPIO     0u
#define K1_EMAC_RGMII_LAST_GPIO      14u
#define K1_EMAC_RGMII_REFCLK_GPIO    45u

#define K1_EMAC_CACHE_LINE_SIZE       64u
#define K1_EMAC_BUFFER_SIZE           1536u
#define K1_EMAC_MIN_FRAME_SIZE          64u
#define K1_EMAC_MAX_FRAME_SIZE         1518u
#define K1_EMAC_FCS_SIZE                  4u
#define K1_EMAC_MAX_PACKET_SIZE \
  (K1_EMAC_MAX_FRAME_SIZE - K1_EMAC_FCS_SIZE)

#define K1_EMAC_PHY_PAGE_SELECT_REG   0x1fu
#define K1_EMAC_PHY_STATUS_PAGE       0xa43u
#define K1_EMAC_PHY_STATUS_REG        0x1au
#define K1_EMAC_PHY_STATUS_DUPLEX     (1u << 3)
#define K1_EMAC_PHY_STATUS_SPEED_MASK 0x30u
#define K1_EMAC_PHY_STATUS_SPEED_100   0x10u
#define K1_EMAC_PHY_STATUS_SPEED_1000  0x20u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_emac_dev_s
{
  struct netdev_lowerhalf_s dev; /* Must be first */
  struct work_s link_work;
  struct work_s dma_work;
  FAR uint32_t *tx_desc;
  FAR uint32_t *rx_desc;
  FAR uint8_t *tx_buffer;
  FAR uint8_t *rx_buffer;
  FAR netpkt_t *tx_pkt;
  bool dma_ready;
  bool tx_pending;
  bool ifup;
  bool link_up;
  uint8_t phy_addr;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int k1_emac_ifup(FAR struct netdev_lowerhalf_s *dev);
static int k1_emac_ifdown(FAR struct netdev_lowerhalf_s *dev);
static int k1_emac_transmit(FAR struct netdev_lowerhalf_s *dev,
                            FAR netpkt_t *pkt);
static FAR netpkt_t *k1_emac_receive(FAR struct netdev_lowerhalf_s *dev);
static void k1_emac_dma_worker(FAR void *arg);
#ifdef CONFIG_NETDEV_IOCTL
static int k1_emac_ioctl(FAR struct netdev_lowerhalf_s *dev, int cmd,
                         unsigned long arg);
#endif
static void k1_emac_link_worker(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct netdev_ops_s g_k1_emac_ops =
{
  .ifup     = k1_emac_ifup,
  .ifdown   = k1_emac_ifdown,
  .transmit = k1_emac_transmit,
  .receive  = k1_emac_receive,
#ifdef CONFIG_NETDEV_IOCTL
  .ioctl    = k1_emac_ioctl,
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int k1_emac_mdio_wait(void)
{
  unsigned int elapsed;

  for (elapsed = 0; elapsed < K1_EMAC_MDIO_TIMEOUT_USEC;
       elapsed += K1_EMAC_MDIO_POLL_USEC)
    {
      if ((getreg32(K1_EMAC_MAC_MDIO_CONTROL) & K1_EMAC_MDIO_START) == 0)
        {
          return OK;
        }

      up_udelay(K1_EMAC_MDIO_POLL_USEC);
    }

  return -EBUSY;
}

static int k1_emac_phyread(FAR struct k1_emac_dev_s *priv, uint8_t phy,
                           uint8_t reg, FAR uint16_t *value)
{
  uint32_t command;
  int ret;

  (void)priv;

  if (phy > K1_EMAC_MDIO_PHYADDR_MASK || reg > K1_EMAC_MDIO_PHYADDR_MASK ||
      value == NULL)
    {
      return -EINVAL;
    }

  command = phy | ((uint32_t)reg << K1_EMAC_MDIO_REGADDR_SHIFT) |
            K1_EMAC_MDIO_READ | K1_EMAC_MDIO_START;

  putreg32(0, K1_EMAC_MAC_MDIO_DATA);
  putreg32(command, K1_EMAC_MAC_MDIO_CONTROL);

  ret = k1_emac_mdio_wait();
  if (ret < 0)
    {
      nerr("ERROR: K1 EMAC MDIO read timed out (phy=%u reg=%u)\n", phy,
           reg);
      return ret;
    }

  *value = (uint16_t)(getreg32(K1_EMAC_MAC_MDIO_DATA) &
                      K1_EMAC_MDIO_DATA_MASK);
  return OK;
}

static int k1_emac_phywrite(FAR struct k1_emac_dev_s *priv, uint8_t phy,
                            uint8_t reg, uint16_t value)
{
  uint32_t command;
  int ret;

  (void)priv;

  if (phy > K1_EMAC_MDIO_PHYADDR_MASK || reg > K1_EMAC_MDIO_PHYADDR_MASK)
    {
      return -EINVAL;
    }

  command = phy | ((uint32_t)reg << K1_EMAC_MDIO_REGADDR_SHIFT) |
            K1_EMAC_MDIO_START;

  putreg32(value, K1_EMAC_MAC_MDIO_DATA);
  putreg32(command, K1_EMAC_MAC_MDIO_CONTROL);

  ret = k1_emac_mdio_wait();
  if (ret < 0)
    {
      nerr("ERROR: K1 EMAC MDIO write timed out (phy=%u reg=%u)\n", phy,
           reg);
    }

  return ret;
}

static int k1_emac_link_status(FAR struct k1_emac_dev_s *priv,
                                FAR bool *link_up)
{
  uint16_t status;
  int ret;

  /* MII link status is latch-low: read once to clear an old low condition,
   * then read again for the current carrier state.
   */

  ret = k1_emac_phyread(priv, priv->phy_addr, MII_MSR, &status);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_emac_phyread(priv, priv->phy_addr, MII_MSR, &status);
  if (ret < 0)
    {
      return ret;
    }

  *link_up = (status & MII_MSR_LINKSTATUS) != 0;
  return OK;
}

static int k1_emac_update_mac_mode(FAR struct k1_emac_dev_s *priv)
{
  uint16_t status;
  uint32_t control;
  int restore_ret;
  int ret;

  ret = k1_emac_phywrite(priv, priv->phy_addr,
                          K1_EMAC_PHY_PAGE_SELECT_REG,
                          K1_EMAC_PHY_STATUS_PAGE);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_emac_phyread(priv, priv->phy_addr, K1_EMAC_PHY_STATUS_REG,
                         &status);

  /* Always restore the standard register page, even if the status read
   * fails.  A later generic MDIO ioctl must not inherit page 0xa43.
   */

  restore_ret = k1_emac_phywrite(priv, priv->phy_addr,
                                  K1_EMAC_PHY_PAGE_SELECT_REG, 0);
  if (ret < 0)
    {
      return ret;
    }

  if (restore_ret < 0)
    {
      return restore_ret;
    }

  control = getreg32(K1_EMAC_MAC_GLOBAL_CONTROL);
  control &= ~(K1_EMAC_MAC_SPEED_MASK | K1_EMAC_MAC_FULL_DUPLEX);

  switch (status & K1_EMAC_PHY_STATUS_SPEED_MASK)
    {
      case K1_EMAC_PHY_STATUS_SPEED_1000:
        control |= K1_EMAC_MAC_SPEED_1000;
        break;

      case K1_EMAC_PHY_STATUS_SPEED_100:
        control |= K1_EMAC_MAC_SPEED_100;
        break;

      default:
        control |= K1_EMAC_MAC_SPEED_10;
        break;
    }

  if ((status & K1_EMAC_PHY_STATUS_DUPLEX) != 0)
    {
      control |= K1_EMAC_MAC_FULL_DUPLEX;
    }

  putreg32(control, K1_EMAC_MAC_GLOBAL_CONTROL);
  ninfo("K1 EMAC: link mode %s %u Mbps\n",
        (status & K1_EMAC_PHY_STATUS_DUPLEX) != 0 ? "full" : "half",
        (status & K1_EMAC_PHY_STATUS_SPEED_MASK) ==
        K1_EMAC_PHY_STATUS_SPEED_1000 ? 1000 :
        (status & K1_EMAC_PHY_STATUS_SPEED_MASK) ==
        K1_EMAC_PHY_STATUS_SPEED_100 ? 100 : 10);
  return OK;
}

static void k1_emac_update_link(FAR struct k1_emac_dev_s *priv)
{
  bool link_up;
  int ret;

  ret = k1_emac_link_status(priv, &link_up);
  if (ret < 0)
    {
      nwarn("WARNING: K1 EMAC PHY link read failed: %d\n", ret);
      return;
    }

  if (link_up == priv->link_up)
    {
      return;
    }

  priv->link_up = link_up;
  if (link_up)
    {
      ret = k1_emac_update_mac_mode(priv);
      if (ret < 0)
        {
          nwarn("WARNING: K1 EMAC PHY mode read failed: %d\n", ret);
        }

      ninfo("K1 EMAC: PHY link up\n");
      netdev_lower_carrier_on(&priv->dev);
    }
  else
    {
      ninfo("K1 EMAC: PHY link down\n");
      netdev_lower_carrier_off(&priv->dev);
    }
}

static void k1_emac_link_worker(FAR void *arg)
{
  FAR struct k1_emac_dev_s *priv = arg;

  if (!priv->ifup)
    {
      return;
    }

  k1_emac_update_link(priv);

  if (priv->ifup)
    {
      work_queue(LPWORK, &priv->link_work, k1_emac_link_worker, priv,
                 MSEC2TICK(CONFIG_K1_EMAC_LINK_POLL_MSEC));
    }
}

static void k1_emac_configure_pin(uint8_t gpio)
{
  uintptr_t mfpr = K1_MFPR_BASE + ((uintptr_t)(gpio + 1u) << 2);

  putreg32(K1_MFPR_MUX_MODE1 | K1_MFPR_EDGE_CLEAR |
           K1_MFPR_DRIVE_1V8_DS2, mfpr);
}

static void k1_emac_pinmux(void)
{
  uint8_t gpio;

  for (gpio = K1_EMAC_RGMII_FIRST_GPIO;
       gpio <= K1_EMAC_RGMII_LAST_GPIO; gpio++)
    {
      k1_emac_configure_pin(gpio);
    }

  k1_emac_configure_pin(K1_EMAC_RGMII_REFCLK_GPIO);
}

static void k1_emac_reset_phy(void)
{
  /* PHY reset is active low. Set the latch before output enable, hold low
   * for the DTS-mandated 10 ms, then let the PHY stabilise for 100 ms.
   */

  putreg32(K1_MFPR_MUX_MODE0 | K1_MFPR_EDGE_CLEAR |
           K1_MFPR_DRIVE_1V8_DS2, K1_EMAC_PHY_RESET_MFPR);
  putreg32(K1_EMAC_PHY_RESET_MASK,
           K1_EMAC_PHY_RESET_BANK + K1_GPIO_GPSR_OFFSET);
  putreg32(K1_EMAC_PHY_RESET_MASK,
           K1_EMAC_PHY_RESET_BANK + K1_GPIO_GSDR_OFFSET);
  up_udelay(2);

  putreg32(K1_EMAC_PHY_RESET_MASK,
           K1_EMAC_PHY_RESET_BANK + K1_GPIO_GPCR_OFFSET);
  up_mdelay(10);

  putreg32(K1_EMAC_PHY_RESET_MASK,
           K1_EMAC_PHY_RESET_BANK + K1_GPIO_GPSR_OFFSET);
  up_mdelay(100);
}

static void k1_emac_hardware_setup(FAR struct k1_emac_dev_s *priv)
{
  uint32_t value;
  uint64_t mac = CONFIG_K1_EMAC_MACADDR;

  (void)priv;

  k1_emac_pinmux();

  /* U-Boot's K1 reset provider records bit 1 as active high reset, with
   * bit 1 set meaning the reset is released. Bit 0 is the EMAC bus gate.
   */

  modifyreg32(K1_APMU_EMAC0_CLK_RST, 0,
              K1_EMAC_CLK_BUS_ENABLE | K1_EMAC_RESET_DEASSERT);

  /* MUSE Pi Pro EMAC0 is RGMII and takes its reference clock from the PHY.
   * AXI single-ID mode is the setting used by the vendor driver.
   */

  modifyreg32(K1_APMU_EMAC0_CLK_RST, K1_EMAC_RGMII_TX_CLK_FROM_SOC,
              K1_EMAC_RGMII | K1_EMAC_AXI_SINGLE_ID);

  value = getreg32(K1_APMU_EMAC0_DLINE);
  value &= ~(K1_EMAC_RX_DLINE_CODE_MASK | K1_EMAC_TX_DLINE_CODE_MASK);
  value |= K1_EMAC_RX_DLINE_ENABLE | K1_EMAC_TX_DLINE_ENABLE |
           ((uint32_t)K1_EMAC_RGMII_RX_PHASE <<
            K1_EMAC_RX_DLINE_CODE_SHIFT) |
           ((uint32_t)K1_EMAC_RGMII_TX_PHASE <<
            K1_EMAC_TX_DLINE_CODE_SHIFT);
  putreg32(value, K1_APMU_EMAC0_DLINE);

  /* Do not enable MAC or DMA traffic in the PHY/MDIO phase. */

  putreg32(0, K1_EMAC_DMA_INTERRUPT_ENABLE);
  putreg32(0, K1_EMAC_MAC_INTERRUPT_ENABLE);
  putreg32(0, K1_EMAC_DMA_CONTROL);
  putreg32(0, K1_EMAC_MAC_TRANSMIT_CONTROL);
  putreg32(0, K1_EMAC_MAC_RECEIVE_CONTROL);
  putreg32(0, K1_EMAC_MAC_GLOBAL_CONTROL);

  putreg32(K1_EMAC_MAC_ADDRESS1_ENABLE, K1_EMAC_MAC_ADDRESS_CONTROL);
  putreg32((((uint32_t)(mac >> 32) & 0xffu) << 8) |
           ((uint32_t)(mac >> 40) & 0xffu),
           K1_EMAC_MAC_ADDRESS1_HIGH);
  putreg32((((uint32_t)(mac >> 16) & 0xffu) << 8) |
           ((uint32_t)(mac >> 24) & 0xffu),
           K1_EMAC_MAC_ADDRESS1_MED);
  putreg32((((uint32_t)mac & 0xffu) << 8) |
           ((uint32_t)(mac >> 8) & 0xffu),
           K1_EMAC_MAC_ADDRESS1_LOW);

  k1_emac_reset_phy();
}

static uint32_t k1_emac_dma_address(FAR const void *address)
{
  uintptr_t value = (uintptr_t)address;

  /* K1 runs this port with satp=0, so the DRAM virtual address is already
   * the DMA bus address.  The EMAC descriptor fields are 32-bit.
   */

  DEBUGASSERT(value <= UINT32_MAX);
  return (uint32_t)value;
}

static void k1_emac_desc_clean(FAR uint32_t *desc)
{
  k1_dcache_clean((uintptr_t)desc, K1_EMAC_CACHE_LINE_SIZE);
}

static void k1_emac_desc_flush(FAR uint32_t *desc)
{
  k1_dcache_invalidate((uintptr_t)desc, K1_EMAC_CACHE_LINE_SIZE);
}

static void k1_emac_prepare_rx(FAR struct k1_emac_dev_s *priv)
{
  FAR uint32_t *desc = priv->rx_desc;

  desc[1] = K1_EMAC_BUFFER_SIZE | K1_EMAC_DESC_END_OF_RING;
  desc[2] = k1_emac_dma_address(priv->rx_buffer);
  desc[3] = 0;

  /* OWN must be the final descriptor store before the cache clean. */

  asm volatile ("fence rw, rw" : : : "memory");
  desc[0] = K1_EMAC_DESC_OWN;
  k1_emac_desc_clean(desc);
}

static int k1_emac_dma_setup(FAR struct k1_emac_dev_s *priv)
{
  uint32_t configuration;

  if (priv->tx_desc == NULL)
    {
      priv->tx_desc = kmm_memalign(K1_EMAC_CACHE_LINE_SIZE,
                                   K1_EMAC_CACHE_LINE_SIZE);
    }

  if (priv->rx_desc == NULL)
    {
      priv->rx_desc = kmm_memalign(K1_EMAC_CACHE_LINE_SIZE,
                                   K1_EMAC_CACHE_LINE_SIZE);
    }

  if (priv->tx_buffer == NULL)
    {
      priv->tx_buffer = kmm_memalign(K1_EMAC_CACHE_LINE_SIZE,
                                     K1_EMAC_BUFFER_SIZE);
    }

  if (priv->rx_buffer == NULL)
    {
      priv->rx_buffer = kmm_memalign(K1_EMAC_CACHE_LINE_SIZE,
                                     K1_EMAC_BUFFER_SIZE);
    }

  if (priv->tx_desc == NULL || priv->rx_desc == NULL ||
      priv->tx_buffer == NULL || priv->rx_buffer == NULL)
    {
      return -ENOMEM;
    }

  /* Each 16-byte descriptor has an entire cache line to itself.  This is
   * required because clean/invalidate operates on 64-byte K1 cache blocks.
   */

  k1_dcache_invalidate((uintptr_t)priv->rx_buffer, K1_EMAC_BUFFER_SIZE);
  priv->tx_desc[0] = 0;
  priv->tx_desc[1] = K1_EMAC_DESC_END_OF_RING;
  priv->tx_desc[2] = k1_emac_dma_address(priv->tx_buffer);
  priv->tx_desc[3] = 0;
  k1_emac_desc_clean(priv->tx_desc);
  k1_emac_prepare_rx(priv);

  putreg32(0, K1_EMAC_DMA_INTERRUPT_ENABLE);
  putreg32(0, K1_EMAC_MAC_INTERRUPT_ENABLE);
  putreg32(0, K1_EMAC_DMA_CONTROL);

  putreg32(K1_EMAC_DMA_SOFTWARE_RESET, K1_EMAC_DMA_CONFIGURATION);
  up_mdelay(10);
  putreg32(0, K1_EMAC_DMA_CONFIGURATION);
  up_mdelay(10);

  configuration = K1_EMAC_DMA_STRICT_BURST |
                  K1_EMAC_DMA_64BIT_MODE |
                  K1_EMAC_DMA_BURST_16WORD;
  putreg32(configuration, K1_EMAC_DMA_CONFIGURATION);

  putreg32(K1_EMAC_TX_FIFO_ALMOST_FULL,
           K1_EMAC_MAC_TRANSMIT_FIFO_ALMOST_FULL);
  putreg32(K1_EMAC_TX_STORE_FORWARD_THRESHOLD,
           K1_EMAC_MAC_TRANSMIT_PACKET_START);
  putreg32(K1_EMAC_RX_STORE_FORWARD_THRESHOLD,
           K1_EMAC_MAC_RECEIVE_PACKET_START);
  putreg32(K1_EMAC_MAX_FRAME_SIZE & K1_EMAC_MAC_MAX_FRAME_SIZE_MASK,
           K1_EMAC_MAC_MAXIMUM_FRAME_SIZE);

  putreg32(k1_emac_dma_address(priv->tx_desc), K1_EMAC_DMA_TRANSMIT_BASE);
  putreg32(k1_emac_dma_address(priv->rx_desc), K1_EMAC_DMA_RECEIVE_BASE);
  putreg32(0, K1_EMAC_DMA_TRANSMIT_AUTO_POLL);

  modifyreg32(K1_EMAC_MAC_TRANSMIT_CONTROL,
              K1_EMAC_MAC_TRANSMIT_IFG_MASK,
              K1_EMAC_MAC_TRANSMIT_ENABLE |
              K1_EMAC_MAC_TRANSMIT_AUTO_RETRY);
  modifyreg32(K1_EMAC_MAC_RECEIVE_CONTROL, 0,
              K1_EMAC_MAC_RECEIVE_ENABLE |
              K1_EMAC_MAC_RECEIVE_STORE_FORWARD);
  modifyreg32(K1_EMAC_DMA_CONTROL, 0,
              K1_EMAC_DMA_START_TRANSMIT | K1_EMAC_DMA_START_RECEIVE);

  priv->dma_ready = true;
  return OK;
}

static void k1_emac_dma_stop(FAR struct k1_emac_dev_s *priv)
{
  putreg32(0, K1_EMAC_DMA_CONTROL);
  putreg32(0, K1_EMAC_MAC_TRANSMIT_CONTROL);
  putreg32(0, K1_EMAC_MAC_RECEIVE_CONTROL);
  putreg32(K1_EMAC_DMA_SOFTWARE_RESET, K1_EMAC_DMA_CONFIGURATION);
  up_mdelay(10);
  putreg32(0, K1_EMAC_DMA_CONFIGURATION);
  priv->dma_ready = false;
}

static void k1_emac_dma_release(FAR struct k1_emac_dev_s *priv)
{
  kmm_free(priv->rx_buffer);
  kmm_free(priv->tx_buffer);
  kmm_free(priv->rx_desc);
  kmm_free(priv->tx_desc);
  priv->rx_buffer = NULL;
  priv->tx_buffer = NULL;
  priv->rx_desc = NULL;
  priv->tx_desc = NULL;
}

static bool k1_emac_tx_complete(FAR struct k1_emac_dev_s *priv)
{
  k1_emac_desc_flush(priv->tx_desc);
  return (priv->tx_desc[0] & K1_EMAC_DESC_OWN) == 0;
}

static void k1_emac_reclaim_tx(FAR struct k1_emac_dev_s *priv)
{
  if (priv->tx_pending && k1_emac_tx_complete(priv))
    {
      netpkt_free(&priv->dev, priv->tx_pkt, NETPKT_TX);
      priv->tx_pkt = NULL;
      priv->tx_pending = false;
      netdev_lower_txdone(&priv->dev);
    }
}

static void k1_emac_dma_worker(FAR void *arg)
{
  FAR struct k1_emac_dev_s *priv = arg;

  if (!priv->ifup || !priv->dma_ready)
    {
      return;
    }

  /* TX completion can race with the upper-half transmit callback.  Both
   * sides use the netdev lock to serialize ownership of the one descriptor.
   */

  netdev_lock(&priv->dev.netdev);
  if (priv->ifup && priv->dma_ready)
    {
      k1_emac_reclaim_tx(priv);

      k1_emac_desc_flush(priv->rx_desc);
      if ((priv->rx_desc[0] & K1_EMAC_DESC_OWN) == 0)
        {
          /* Keep the notification under d_lock.  The upper-half ifdown
           * path cancels its receive work while holding this lock, so it
           * cannot miss a newly queued receive worker before DMA storage is
           * released.
           */

          netdev_lower_rxready(&priv->dev);
        }

      work_queue(LPWORK, &priv->dma_work, k1_emac_dma_worker, priv,
                 MSEC2TICK(CONFIG_K1_EMAC_DMA_POLL_MSEC));
    }

  netdev_unlock(&priv->dev.netdev);
}

static void k1_emac_cancel_workers(FAR struct k1_emac_dev_s *priv)
{
  unsigned int lock_count;

  /* ifdown is called with d_lock held.  The link worker may be waiting for
   * that same lock in netdev_lower_carrier_{on,off}(), so release it while
   * synchronously stopping both workers.  ifup is already false when this
   * helper is called, which prevents either worker from re-queueing itself.
   */

  netdev_breaklock(&priv->dev.netdev, &lock_count);
  work_cancel_sync(LPWORK, &priv->dma_work);
  work_cancel_sync(LPWORK, &priv->link_work);
  netdev_restorelock(&priv->dev.netdev, lock_count);
}

static int k1_emac_check_phy(FAR struct k1_emac_dev_s *priv)
{
  uint16_t id1;
  uint16_t id2;
  int ret;

  ret = k1_emac_phyread(priv, priv->phy_addr, MII_PHYID1, &id1);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_emac_phyread(priv, priv->phy_addr, MII_PHYID2, &id2);
  if (ret < 0)
    {
      return ret;
    }

  if (id1 == 0 || id1 == UINT16_MAX || id2 == 0 || id2 == UINT16_MAX)
    {
      nerr("ERROR: K1 EMAC PHY %u did not answer MDIO (id=%04x:%04x)\n",
           priv->phy_addr, id1, id2);
      return -ENODEV;
    }

  ninfo("K1 EMAC: PHY %u id=%04x:%04x\n", priv->phy_addr, id1, id2);
  return OK;
}

static int k1_emac_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct k1_emac_dev_s *priv = (FAR struct k1_emac_dev_s *)dev;
  int ret;

  if (priv->ifup)
    {
      return OK;
    }

  k1_emac_hardware_setup(priv);

  ret = k1_emac_check_phy(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_emac_dma_setup(priv);
  if (ret < 0)
    {
      nerr("ERROR: K1 EMAC DMA allocation failed: %d\n", ret);
      k1_emac_dma_stop(priv);
      k1_emac_dma_release(priv);
      return ret;
    }

  priv->link_up = false;
  priv->ifup = true;
  k1_emac_update_link(priv);

  ret = work_queue(LPWORK, &priv->link_work, k1_emac_link_worker, priv,
                   MSEC2TICK(CONFIG_K1_EMAC_LINK_POLL_MSEC));
  if (ret < 0)
    {
      goto err_stop_dma;
    }

  ret = work_queue(LPWORK, &priv->dma_work, k1_emac_dma_worker, priv,
                   MSEC2TICK(CONFIG_K1_EMAC_DMA_POLL_MSEC));
  if (ret < 0)
    {
      goto err_stop_dma;
    }

  return OK;

err_stop_dma:
  priv->ifup = false;
  k1_emac_cancel_workers(priv);
  k1_emac_dma_stop(priv);
  k1_emac_dma_release(priv);

  if (priv->link_up)
    {
      priv->link_up = false;
      netdev_lower_carrier_off(dev);
    }

  return ret;
}

static int k1_emac_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct k1_emac_dev_s *priv = (FAR struct k1_emac_dev_s *)dev;

  if (!priv->ifup)
    {
      return OK;
    }

  priv->ifup = false;
  k1_emac_cancel_workers(priv);
  k1_emac_dma_stop(priv);

  if (priv->tx_pending)
    {
      netpkt_free(dev, priv->tx_pkt, NETPKT_TX);
      priv->tx_pkt = NULL;
      priv->tx_pending = false;
    }

  k1_emac_dma_release(priv);

  if (priv->link_up)
    {
      priv->link_up = false;
      netdev_lower_carrier_off(dev);
    }

  return OK;
}

static int k1_emac_transmit(FAR struct netdev_lowerhalf_s *dev,
                            FAR netpkt_t *pkt)
{
  FAR struct k1_emac_dev_s *priv = (FAR struct k1_emac_dev_s *)dev;
  unsigned int length;
  int ret;

  if (!priv->ifup || !priv->dma_ready || !priv->link_up)
    {
      return -ENETDOWN;
    }

  k1_emac_reclaim_tx(priv);
  if (priv->tx_pending)
    {
      return -EBUSY;
    }

  length = netpkt_getdatalen(dev, pkt);
  if (length > K1_EMAC_MAX_PACKET_SIZE)
    {
      return -EMSGSIZE;
    }

  ret = netpkt_copyout(dev, priv->tx_buffer, pkt, length, 0);
  if (ret != (int)length)
    {
      return ret < 0 ? ret : -EIO;
    }

  k1_dcache_clean((uintptr_t)priv->tx_buffer, length);
  priv->tx_desc[1] = (length & K1_EMAC_DESC_BUFFER1_SIZE_MASK) |
                     K1_EMAC_DESC_END_OF_RING |
                     K1_EMAC_DESC_TX_FIRST | K1_EMAC_DESC_TX_LAST |
                     K1_EMAC_DESC_TX_INTERRUPT_ON_DONE;
  priv->tx_desc[2] = k1_emac_dma_address(priv->tx_buffer);
  priv->tx_desc[3] = 0;

  /* OWN must be the final descriptor store before the cache clean. */

  asm volatile ("fence rw, rw" : : : "memory");
  priv->tx_desc[0] = K1_EMAC_DESC_OWN;
  k1_emac_desc_clean(priv->tx_desc);
  priv->tx_pkt = pkt;
  priv->tx_pending = true;
  putreg32(0xff, K1_EMAC_DMA_TRANSMIT_POLL_DEMAND);

  return OK;
}

static FAR netpkt_t *k1_emac_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct k1_emac_dev_s *priv = (FAR struct k1_emac_dev_s *)dev;
  FAR netpkt_t *pkt;
  uint32_t status;
  unsigned int length;
  int ret;

  if (!priv->ifup || !priv->dma_ready)
    {
      return NULL;
    }

  k1_emac_desc_flush(priv->rx_desc);
  status = priv->rx_desc[0];
  if ((status & K1_EMAC_DESC_OWN) != 0)
    {
      return NULL;
    }

  length = status & K1_EMAC_DESC_RX_FRAME_LENGTH_MASK;
  if ((status & (K1_EMAC_DESC_RX_FIRST | K1_EMAC_DESC_RX_LAST)) !=
      (K1_EMAC_DESC_RX_FIRST | K1_EMAC_DESC_RX_LAST) ||
      (status & K1_EMAC_DESC_RX_ERROR_STATUS) != 0 ||
      length < K1_EMAC_MIN_FRAME_SIZE ||
      length > K1_EMAC_MAX_FRAME_SIZE)
    {
      nwarn("WARNING: K1 EMAC dropped RX descriptor status=%08" PRIx32
            "\n", status);
      k1_emac_prepare_rx(priv);
      putreg32(0xff, K1_EMAC_DMA_RECEIVE_POLL_DEMAND);
      return NULL;
    }

  /* Hardware reports the frame including FCS.  NuttX Ethernet packets do
   * not include the four FCS octets.
   */

  length -= K1_EMAC_FCS_SIZE;
  k1_dcache_invalidate((uintptr_t)priv->rx_buffer, length);
  pkt = netpkt_alloc(dev, NETPKT_RX);
  if (pkt == NULL)
    {
      k1_emac_prepare_rx(priv);
      putreg32(0xff, K1_EMAC_DMA_RECEIVE_POLL_DEMAND);
      return NULL;
    }

  ret = netpkt_copyin(dev, pkt, priv->rx_buffer, length, 0);
  if (ret != (int)length)
    {
      netpkt_free(dev, pkt, NETPKT_RX);
      k1_emac_prepare_rx(priv);
      putreg32(0xff, K1_EMAC_DMA_RECEIVE_POLL_DEMAND);
      return NULL;
    }

  k1_emac_prepare_rx(priv);
  putreg32(0xff, K1_EMAC_DMA_RECEIVE_POLL_DEMAND);

  return pkt;
}

#ifdef CONFIG_NETDEV_IOCTL
static int k1_emac_ioctl(FAR struct netdev_lowerhalf_s *dev, int cmd,
                         unsigned long arg)
{
#ifdef CONFIG_NETDEV_PHY_IOCTL
  FAR struct k1_emac_dev_s *priv = (FAR struct k1_emac_dev_s *)dev;
  FAR struct mii_ioctl_data_s *request =
    (FAR struct mii_ioctl_data_s *)(uintptr_t)arg;

  if (request == NULL)
    {
      return -EINVAL;
    }

  switch (cmd)
    {
      case SIOCGMIIPHY:
        request->phy_id = priv->phy_addr;
        return OK;

      case SIOCGMIIREG:
        return k1_emac_phyread(priv, request->phy_id, request->reg_num,
                                &request->val_out);

      case SIOCSMIIREG:
        return k1_emac_phywrite(priv, request->phy_id, request->reg_num,
                                 request->val_in);

      default:
        return -ENOTTY;
    }
#else
  (void)dev;
  (void)cmd;
  (void)arg;
  return -ENOTTY;
#endif
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: k1_emac_initialize
 *
 * Description:
 *   Register the MUSE Pi Pro EMAC0 polling Ethernet lower-half as eth0.
 *
 ****************************************************************************/

int k1_emac_initialize(void)
{
  FAR struct k1_emac_dev_s *priv;
  uint64_t mac = CONFIG_K1_EMAC_MACADDR;
  int ret;

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->dev.ops = &g_k1_emac_ops;
  priv->dev.quota[NETPKT_TX] = 1;
  priv->dev.quota[NETPKT_RX] = 1;
  priv->dev.rxtype = NETDEV_RX_WORK;
  priv->dev.priority = LPWORK;
  priv->phy_addr = CONFIG_K1_EMAC_PHY_ADDR;

  priv->dev.netdev.d_mac.ether.ether_addr_octet[0] = (uint8_t)(mac >> 40);
  priv->dev.netdev.d_mac.ether.ether_addr_octet[1] = (uint8_t)(mac >> 32);
  priv->dev.netdev.d_mac.ether.ether_addr_octet[2] = (uint8_t)(mac >> 24);
  priv->dev.netdev.d_mac.ether.ether_addr_octet[3] = (uint8_t)(mac >> 16);
  priv->dev.netdev.d_mac.ether.ether_addr_octet[4] = (uint8_t)(mac >> 8);
  priv->dev.netdev.d_mac.ether.ether_addr_octet[5] = (uint8_t)mac;

  ret = netdev_lower_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      kmm_free(priv);
    }

  return ret;
}

#if !defined(CONFIG_NETDEV_LATEINIT)
/****************************************************************************
 * Name: riscv_netinitialize
 *
 * Description:
 *   Register the K1 network lower-half during the RISC-V standard network
 *   initialization stage.
 *
 ****************************************************************************/

void riscv_netinitialize(void)
{
  int ret;

  ret = k1_emac_initialize();
  if (ret < 0)
    {
      nerr("ERROR: K1 EMAC initialize failed: %d\n", ret);
    }
}
#endif

#endif /* CONFIG_K1_EMAC */
