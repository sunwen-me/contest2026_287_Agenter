/****************************************************************************
 * vendor/spacemit/chips/k1/k1_rtl8852bs_netdev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The openvela contributors.
 *
 * A wireless network device for the RTL8852BS2 passive scan.  It follows the
 * NuttX in-tree IEEE 802.11 driver pattern - a classic net_driver_s
 * registered as NET_LL_IEEE80211 - and drives the GPL-2.0-only scan-offload
 * component through its public interface.  No vendor material is used here.
 *
 * What this device does not do is as important as what it does, so it is
 * stated instead of hidden:
 *
 *   - There is no transmit or receive data path.  The scan runs entirely in
 *     the device firmware and reports management frames, so the network
 *     stack has nothing to send through and no address to configure.
 *   - There is no association, authentication or key handling.
 *   - There is no signal strength.  RSSI needs the baseband and RF data
 *     manager initialization this port does not perform yet, so the scan
 *     report carries no quality event at all rather than a made-up level.
 *
 * Every wireless request other than the two scan requests therefore returns
 * -ENOTTY, which is what the caller needs to hear: the request is not
 * implemented, as opposed to implemented and answered with a default.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>

#include <nuttx/mutex.h>
#include <nuttx/net/netdev.h>
#include <nuttx/wireless/wireless.h>

#include "k1_rtl8852bs_gpl.h"
#include "k1_rtl8852bs_netdev.h"

#ifdef CONFIG_K1_RTL8852BS2_WLAN_NETDEV

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The events one reported access point occupies, before the padded SSID:
 * the BSSID that opens the cell, the SSID, the channel and the privacy bit.
 * The order matters to the reader: wapi allocates a new cell on the BSSID
 * event and fills the rest into it.
 */

#define K1_WLAN_SCAN_CELL_FIXED_LENGTH \
  (IW_EV_LEN(ap_addr) + IW_EV_LEN(essid) + IW_EV_LEN(freq) + IW_EV_LEN(data))

/* A wireless-extensions payload is padded so that the next event header
 * stays aligned for a pointer load.  struct iw_point carries a void *, and
 * the reader reads that field straight out of the stream instead of copying
 * the event first, so on RV64 a four-byte pad would leave every event after
 * an odd-length SSID misaligned.  The reader takes the SSID length from
 * u.essid.length, so the extra pad bytes are simply skipped.
 */

#define K1_WLAN_SCAN_ALIGN ((size_t)sizeof(FAR void *))
#define K1_WLAN_SCAN_PAD(len) \
  (((len) + K1_WLAN_SCAN_ALIGN - 1) & ~(K1_WLAN_SCAN_ALIGN - 1))

/* The Privacy bit of the 802.11 capability information field. */

#define K1_WLAN_CAPABILITY_PRIVACY 0x0010

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_wlan_dev_s
{
  struct net_driver_s dev;                    /* Interface understood by the
                                               * network subsystem */
  mutex_t lock;                               /* Serializes the sweep and the
                                               * report of one sweep */
  struct k1_rtl8852bs_scan_result_s result;   /* The last sweep report */
  bool registered;                            /* wlan0 exists */
  bool result_valid;                          /* A sweep has completed */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct k1_wlan_dev_s g_k1_wlan_dev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: k1_wlan_scan_report_length
 *
 * Description:
 *   The exact number of bytes the current report needs in the caller
 *   buffer.  A caller that asks with a short buffer is told this length and
 *   -E2BIG, which is also how wapi polls for readiness.
 ****************************************************************************/

static size_t k1_wlan_scan_report_length(
  FAR const struct k1_rtl8852bs_scan_result_s *result)
{
  size_t length = 0;
  unsigned int index;

  for (index = 0; index < result->bss_count; index++)
    {
      length += K1_WLAN_SCAN_CELL_FIXED_LENGTH;
      length += K1_WLAN_SCAN_PAD(result->bss[index].ssid_length);
    }

  return length;
}

/****************************************************************************
 * Name: k1_wlan_scan_format
 *
 * Description:
 *   Serialize the report as a wireless-extensions event stream.  A payload
 *   carried by an event is stored right behind the event union and is
 *   referenced by its offset from that union, which is the layout the
 *   reader recomputes into a pointer of its own.
 ****************************************************************************/

static void k1_wlan_scan_format(
  FAR const struct k1_rtl8852bs_scan_result_s *result,
  FAR struct iwreq *iwr)
{
  FAR const struct k1_rtl8852bs_scan_bss_s *bss;
  FAR struct iw_event *iwe;
  FAR char *pointer;
  unsigned int index;

  pointer = iwr->u.data.pointer;

  for (index = 0; index < result->bss_count; index++)
    {
      bss = &result->bss[index];

      /* The BSSID opens the cell. */

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWAP;
      iwe->u.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(iwe->u.ap_addr.sa_data, bss->bssid, IFHWADDRLEN);
      iwe->len = IW_EV_LEN(ap_addr);
      pointer += iwe->len;

      /* The SSID.  A Beacon with no SSID element, or with a zero-length one,
       * is a hidden network: the flag stays clear and the payload is empty
       * instead of inventing a name.
       */

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWESSID;
      iwe->u.essid.flags = bss->ssid_present && bss->ssid_length > 0 ? 1 : 0;
      iwe->u.essid.length = bss->ssid_length;
      iwe->u.essid.pointer = (FAR void *)sizeof(iwe->u.essid);
      memset(&iwe->u.essid + 1, 0,
             K1_WLAN_SCAN_PAD(bss->ssid_length));
      memcpy(&iwe->u.essid + 1, bss->ssid, bss->ssid_length);
      iwe->len = IW_EV_LEN(essid) + K1_WLAN_SCAN_PAD(bss->ssid_length);
      pointer += iwe->len;

      /* The channel, as a channel number rather than a frequency: it comes
       * from the DS Parameter Set of the received frame.
       */

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWFREQ;
      iwe->u.freq.e = 0;
      iwe->u.freq.m = bss->channel;
      iwe->len = IW_EV_LEN(freq);
      pointer += iwe->len;

      /* Privacy, straight out of the capability field.  This says whether
       * the BSS advertises encryption, not which suite it uses.
       */

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWENCODE;
      iwe->u.data.flags = (bss->capability & K1_WLAN_CAPABILITY_PRIVACY) != 0 ?
                          IW_ENCODE_ENABLED | IW_ENCODE_NOKEY :
                          IW_ENCODE_DISABLED;
      iwe->u.data.length = 0;
      iwe->u.data.pointer = NULL;
      iwe->len = IW_EV_LEN(data);
      pointer += iwe->len;
    }

  iwr->u.data.length = pointer - (FAR char *)iwr->u.data.pointer;
}

/****************************************************************************
 * Name: k1_wlan_start_scan
 *
 * Description:
 *   Run one passive 2.4 GHz 1-13 sweep and keep its report for the matching
 *   get request.  The sweep runs to completion here, in the caller task: a
 *   wireless ioctl holds no network lock, so the seconds it takes stall
 *   nothing but the caller, and the report is ready as soon as this returns.
 *
 *   The request may ask for an active scan, a single SSID or a channel
 *   subset.  None of those are honoured, and none are refused either: the
 *   device performs its one validated sweep, which transmits no probe
 *   request, and the report says what was actually received.
 ****************************************************************************/

static int k1_wlan_start_scan(FAR struct k1_wlan_dev_s *priv,
                              FAR struct iwreq *iwr)
{
  FAR const struct iw_scan_req *req;
  int ret;

  if (iwr->u.data.pointer != NULL &&
      iwr->u.data.length >= sizeof(struct iw_scan_req))
    {
      req = (FAR const struct iw_scan_req *)iwr->u.data.pointer;
      if (req->scan_type == IW_SCAN_TYPE_ACTIVE)
        {
          nwarn("WARNING: wlan0 has no probe request path; "
                "running a passive sweep instead\n");
        }

      if (req->essid_len > 0 || req->num_channels > 0)
        {
          nwarn("WARNING: wlan0 ignores the SSID and channel filters; "
                "sweeping channels 1-13\n");
        }
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  priv->result_valid = false;

  ret = k1_rtl8852bs_runtime_scanofld_passive_scan(&priv->result);

  /* The report is kept even when the sweep failed part way: the counters in
   * it are the evidence for what went wrong.  It is only offered to a reader
   * once the sweep reached its scan-end event.
   */

  if (ret >= 0 && priv->result.scan_end)
    {
      priv->result_valid = true;
    }

  ninfo("wlan0 sweep ret=%d end=%d bss=%u data-only=%u dropped=%u\n",
        ret, priv->result.scan_end, priv->result.bss_count,
        priv->result.data_only_count, priv->result.dropped_count);

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: k1_wlan_get_scan_results
 *
 * Description:
 *   Report the last sweep.  -EAGAIN means no sweep has completed yet, so a
 *   caller that polls for readiness keeps polling; -E2BIG together with the
 *   required length means the buffer is too small, which is also how wapi
 *   asks whether a report is ready at all.
 *
 *   The report stays available until the next set request, so reading the
 *   results is a separate step that can be repeated.
 ****************************************************************************/

static int k1_wlan_get_scan_results(FAR struct k1_wlan_dev_s *priv,
                                    FAR struct iwreq *iwr)
{
  size_t length;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      iwr->u.data.length = 0;
      return ret;
    }

  if (!priv->result_valid)
    {
      iwr->u.data.length = 0;
      ret = -EAGAIN;
      goto out;
    }

  length = k1_wlan_scan_report_length(&priv->result);
  if (length == 0)
    {
      /* The sweep completed and received no Beacon or Probe Response.  That
       * is an empty report, not an error and not a pending scan.
       */

      iwr->u.data.length = 0;
      ret = OK;
      goto out;
    }

  if (iwr->u.data.pointer == NULL || iwr->u.data.length < length)
    {
      iwr->u.data.length = length;
      ret = -E2BIG;
      goto out;
    }

  k1_wlan_scan_format(&priv->result, iwr);
  ret = OK;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: k1_wlan_ifup
 *
 * Description:
 *   Bring the interface up.  There is nothing to start: the radio and the
 *   firmware are brought up by the board initialization, and this device has
 *   no data path to enable.  The scan requests are answered whatever the
 *   interface state, so this exists to keep ifconfig and the network
 *   subsystem consistent rather than to gate the hardware.
 ****************************************************************************/

static int k1_wlan_ifup(FAR struct net_driver_s *dev)
{
  netdev_carrier_on(dev);
  return OK;
}

/****************************************************************************
 * Name: k1_wlan_ifdown
 ****************************************************************************/

static int k1_wlan_ifdown(FAR struct net_driver_s *dev)
{
  netdev_carrier_off(dev);
  return OK;
}

/****************************************************************************
 * Name: k1_wlan_txavail
 *
 * Description:
 *   There is no transmit path.  Say so instead of accepting the poll and
 *   dropping the packet silently.
 ****************************************************************************/

static int k1_wlan_txavail(FAR struct net_driver_s *dev)
{
  return -ENOSYS;
}

/****************************************************************************
 * Name: k1_wlan_ioctl
 *
 * Description:
 *   Handle the wireless requests this device implements.  Anything else
 *   returns -ENOTTY: an unimplemented request has to fail visibly, because a
 *   default answer here would be indistinguishable from a working radio.
 ****************************************************************************/

static int k1_wlan_ioctl(FAR struct net_driver_s *dev, int cmd,
                         unsigned long arg)
{
  FAR struct k1_wlan_dev_s *priv = dev->d_private;
  FAR struct iwreq *iwr = (FAR struct iwreq *)((uintptr_t)arg);
  int ret;

  if (iwr == NULL)
    {
      return -EINVAL;
    }

  switch (cmd)
    {
      case SIOCSIWSCAN:
        ret = k1_wlan_start_scan(priv, iwr);
        break;

      case SIOCGIWSCAN:
        ret = k1_wlan_get_scan_results(priv, iwr);
        break;

      default:
        ninfo("wlan0 unimplemented wireless request %04x\n", cmd);
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_rtl8852bs_netdev_register(FAR const uint8_t *mac)
{
  FAR struct k1_wlan_dev_s *priv = &g_k1_wlan_dev;

  if (mac == NULL)
    {
      return -EINVAL;
    }

  if (priv->registered)
    {
      return -EALREADY;
    }

  memset(priv, 0, sizeof(*priv));
  nxmutex_init(&priv->lock);

  priv->dev.d_ifup    = k1_wlan_ifup;
  priv->dev.d_ifdown  = k1_wlan_ifdown;
  priv->dev.d_txavail = k1_wlan_txavail;
  priv->dev.d_ioctl   = k1_wlan_ioctl;
  priv->dev.d_private = priv;

  /* No packet ever passes through this device, so it owns no packet buffer
   * and never hands one to the network stack.
   */

  priv->dev.d_buf = NULL;

  memcpy(priv->dev.d_mac.ether.ether_addr_octet, mac, IFHWADDRLEN);

  netdev_register(&priv->dev, NET_LL_IEEE80211);
  priv->registered = true;
  return OK;
}

#endif /* CONFIG_K1_RTL8852BS2_WLAN_NETDEV */
