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
#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_EMAC_MDIO_POLL_USEC       100u
#define K1_EMAC_MDIO_TIMEOUT_USEC    10000u

#define K1_EMAC_RGMII_FIRST_GPIO     0u
#define K1_EMAC_RGMII_LAST_GPIO      14u
#define K1_EMAC_RGMII_REFCLK_GPIO    45u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_emac_dev_s
{
  struct netdev_lowerhalf_s dev; /* Must be first */
  struct work_s link_work;
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

  priv->link_up = false;
  priv->ifup = true;
  k1_emac_update_link(priv);

  ret = work_queue(LPWORK, &priv->link_work, k1_emac_link_worker, priv,
                   MSEC2TICK(CONFIG_K1_EMAC_LINK_POLL_MSEC));
  if (ret < 0)
    {
      priv->ifup = false;
      return ret;
    }

  return OK;
}

static int k1_emac_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct k1_emac_dev_s *priv = (FAR struct k1_emac_dev_s *)dev;

  if (!priv->ifup)
    {
      return OK;
    }

  priv->ifup = false;
  work_cancel_sync(LPWORK, &priv->link_work);
  putreg32(0, K1_EMAC_DMA_CONTROL);
  putreg32(0, K1_EMAC_MAC_TRANSMIT_CONTROL);
  putreg32(0, K1_EMAC_MAC_RECEIVE_CONTROL);

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
  (void)dev;
  (void)pkt;

  /* The DMA descriptor/data path is intentionally absent from phase one. */

  return -ENOSYS;
}

static FAR netpkt_t *k1_emac_receive(FAR struct netdev_lowerhalf_s *dev)
{
  (void)dev;

  /* The DMA descriptor/data path is intentionally absent from phase one. */

  return NULL;
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
 *   Register the MUSE Pi Pro EMAC0 PHY/MDIO validation lower-half as eth0.
 *   This is deliberately not an Ethernet traffic driver yet.
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
  priv->dev.rxtype = NETDEV_RX_DIRECT;
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
