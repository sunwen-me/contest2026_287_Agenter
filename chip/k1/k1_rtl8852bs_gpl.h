/****************************************************************************
 * vendor/spacemit/chips/k1/k1_rtl8852bs_gpl.h
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright(c) 2019 Realtek Corporation. All rights reserved.
 * Copyright (c) 2026 The openvela contributors.
 *
 * Derived from the Realtek RTL8852BS driver in SpacemiT Linux 6.6,
 * k1-bl-v2.2.y.  See k1_rtl8852bs_gpl.c and K1_SOURCE_AND_LICENSES.md.
 ****************************************************************************/

#ifndef __CHIP_K1_K1_RTL8852BS_GPL_H
#define __CHIP_K1_K1_RTL8852BS_GPL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One frame parsed from the SDIO RX FIFO.  RX request data may contain
 * multiple frames; next_offset is rounded to the RTL8852B 8-byte aggregate
 * alignment and is relative to the beginning of the transfer buffer.
 */

struct k1_rtl8852bs_rx_frame_s
{
  size_t payload_offset;
  size_t next_offset;
  uint32_t descriptor0;
  uint32_t descriptor3;
  uint16_t payload_length;
  uint8_t packet_type;
  bool crc_error;
  bool icv_error;
};

/* The bounded subset of an 802.11 management frame needed by the scan
 * diagnostic.  The parser copies these fields before the RX FIFO is read
 * again, so no RX aggregate buffer lifetime escapes to the caller.
 */

struct k1_rtl8852bs_mgmt_frame_s
{
  uint8_t bssid[6];

  /* The three 802.11 address fields, kept separately from bssid.  A Probe
   * Response only proves this host transmitted when its A1 is this host's own
   * address: the receiver runs with sniffer mode on and unicast CAM matching
   * off during a sweep, so a Probe Response an access point sent to a
   * different station is delivered here as well and must never be counted as
   * evidence of our own transmit.  A2 is what identifies a frame this part
   * transmitted itself, which is how a MAC loopback is observed.
   */

  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  bool addr1_valid;
  bool addr2_valid;
  bool addr3_valid;
  uint8_t ssid[32];
  uint8_t ssid_length;
  uint8_t channel;
  uint16_t frame_control;
  uint16_t beacon_interval;
  uint16_t capability;
  bool is_management;
  bool is_beacon;
  bool is_probe_response;
  bool bssid_valid;
  bool ssid_present;
};

/* The maximum number of access points one passive scan sweep reports.  The
 * table is fixed size because the scan runs with no allocation of its own
 * beyond the RX aggregate buffer.
 */

#define K1_RTL8852BS_SCAN_BSS_MAX 24

/* One access point accepted by a passive scan sweep.  Only a Beacon or a
 * Probe Response backs an entry: a BSSID observed solely in a data frame
 * proves nothing about a joinable BSS, so the scan counts those separately
 * and reports no entry for them.
 *
 * There is deliberately no signal-strength field.  RSSI needs the BB/RF
 * data-manager initialization this port does not perform yet, and reporting
 * a fabricated level would be worse than reporting none.
 */

struct k1_rtl8852bs_scan_bss_s
{
  uint8_t bssid[6];
  uint8_t ssid[32];
  uint8_t ssid_length;
  uint8_t channel;
  uint16_t capability;
  uint16_t beacon_interval;
  uint16_t beacon_frames;
  uint16_t probe_response_frames;
  bool ssid_present;
};

/* The outcome of one passive scan sweep.  The counters describe what the
 * sweep observed but did not report, so a caller can tell an empty result
 * apart from a filtered one.
 */

struct k1_rtl8852bs_scan_result_s
{
  struct k1_rtl8852bs_scan_bss_s bss[K1_RTL8852BS_SCAN_BSS_MAX];
  uint16_t entered_channels;
  uint16_t advanced_channels;
  uint8_t bss_count;
  uint8_t data_only_count;
  uint8_t dropped_count;
  bool scan_end;
};

/* A C2H packet begins with the common 8-byte Realtek firmware-command
 * header.  content remains owned by the RX aggregate buffer, so a handler
 * must consume it before the caller issues another RX FIFO read.
 */

struct k1_rtl8852bs_c2h_s
{
  FAR const uint8_t *content;
  uint16_t total_length;
  uint16_t content_length;
  uint8_t category;
  uint8_t class_id;
  uint8_t function;
  uint8_t sequence;
  bool receive_ack;
  bool done_ack;
};

/* Metadata used to construct one normal 802.11 data TX descriptor.  The
 * current SDIO/SCC HCI flow-control setup makes channels 0--3 and 8--9
 * available; the caller supplies an already-formed 802.11 frame separately.
 * There is intentionally no packet ownership or MAC/netdev policy here.
 */

struct k1_rtl8852bs_data_tx_info_s
{
  uint16_t packet_length;
  uint16_t sequence;
  uint8_t dma_channel;
  uint8_t tid;
  uint8_t mac_id;
  uint8_t wmm;
};

/* Calculated transport values for one normal data TX packet. */

struct k1_rtl8852bs_data_tx_layout_s
{
  uint32_t fifo_address;
  uint16_t transfer_length;
  uint16_t required_ple_pages;
  uint16_t required_wde_pages;
};

/* One synchronous snapshot of the SDIO data-path flow-control state. */

struct k1_rtl8852bs_data_tx_resources_s
{
  uint16_t wp_available_pages;
  uint16_t channel_used_pages;
  uint16_t channel_max_pages;
};

/* Input fields for the firmware MEDIA_RPT/FWROLE_MAINTAIN command.  Values
 * use the RTL8852B MAC_AX enumeration values from the recorded GPL source.
 * The serializer accepts only the RTL8852B band-0 subset and does not submit
 * the result to the device.
 */

struct k1_rtl8852bs_fwrole_maintain_info_s
{
  uint8_t mac_id;
  uint8_t self_role;
  uint8_t update_mode;
  uint8_t wifi_role;
  uint8_t band;
  uint8_t port;
};

/* Input fields for the firmware MEDIA_RPT/JOININFO command.  The upstream
 * driver derives wmm from the selected band and WMM instance; on the
 * RTL8852B band-0 path it must be 0 or 1.
 */

struct k1_rtl8852bs_join_info_s
{
  uint8_t mac_id;
  uint8_t wmm;
  uint8_t downlink_bandwidth;
  uint8_t trigger_frame_padding;
  uint8_t downlink_target_packet_extension;
  uint8_t port;
  uint8_t network_type;
  uint8_t wifi_role;
  uint8_t self_role;
  bool disconnected;
  bool band;
  bool trigger;
  bool he_station;
};

/* Inputs for an RTL8852B MAC/ADDR_CAM_UPDATE/ADDRCAM_INFO create payload.
 * This bounded builder supports a non-multicast band-0 no-link station,
 * infrastructure station, or AP role without security/WOL state.  A no-link
 * station requires a genuine self MAC and zero target/BSSID values; other
 * roles require genuine non-zero unicast addresses.  It never submits the
 * resulting payload.
 */

struct k1_rtl8852bs_addr_cam_info_s
{
  uint8_t address_cam_index;
  uint8_t bssid_cam_index;
  uint8_t mac_id;
  uint8_t port;
  uint8_t network_type;
  uint8_t self_role;
  uint8_t address_mask;
  uint8_t mask_selection;
  uint8_t bss_color;
  uint8_t beacon_hit_condition;
  uint8_t hit_rule;
  uint8_t tsf_sync;
  uint8_t target_indicator;
  uint8_t frame_target_indicator;
  uint16_t aid;
  uint8_t self_mac[6];
  uint8_t target_mac[6];
  uint8_t bssid[6];
  bool trigger;
  bool lsig_txop;
};

typedef int (*k1_rtl8852bs_c2h_handler_t)(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int k1_rtl8852bs_bootstrap(void);
int k1_rtl8852bs_hci_dmac_pre_init(void);
int k1_rtl8852bs_sdio_pre_init(void);
int k1_rtl8852bs_dle_scc_init(void);
int k1_rtl8852bs_hci_fc_init(void);
int k1_rtl8852bs_fwdl_image_layout_diagnostic(void);
int k1_rtl8852bs_fwdl_mss_efuse_diagnostic(void);
int k1_rtl8852bs_fwdl_mss_legacy_signature_diagnostic(void);
int k1_rtl8852bs_fwdl_full_download(void);
int k1_rtl8852bs_efuse_read_mac(FAR uint8_t *mac);
int k1_rtl8852bs_fwdl_rf_context_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_status_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_transport_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_h2c_loopback_diagnostic(void);
int k1_rtl8852bs_runtime_h2c_loopback_submit(void);
int k1_rtl8852bs_runtime_rx_read(FAR uint8_t *buffer, size_t buffer_size,
                                 FAR size_t *transfer_length);
int k1_rtl8852bs_runtime_rx_parse(FAR const uint8_t *buffer,
                                  size_t transfer_length, size_t offset,
                                  FAR struct k1_rtl8852bs_rx_frame_s *frame);
int k1_rtl8852bs_runtime_mgmt_parse(
  FAR const uint8_t *payload, size_t payload_length,
  FAR struct k1_rtl8852bs_mgmt_frame_s *frame);
int k1_rtl8852bs_runtime_c2h_parse(FAR const uint8_t *buffer,
                                   size_t buffer_length,
                                   FAR struct k1_rtl8852bs_c2h_s *c2h);
int k1_rtl8852bs_runtime_c2h_dispatch(FAR const uint8_t *buffer,
                                      size_t buffer_length,
                                      k1_rtl8852bs_c2h_handler_t handler,
                                      FAR void *arg);
int k1_rtl8852bs_runtime_data_tx_build(
  FAR const struct k1_rtl8852bs_data_tx_info_s *info,
  FAR uint8_t *descriptor, size_t descriptor_length,
  FAR struct k1_rtl8852bs_data_tx_layout_s *layout);
int k1_rtl8852bs_runtime_data_tx_preflight(
  FAR const struct k1_rtl8852bs_data_tx_info_s *info,
  FAR uint8_t *descriptor, size_t descriptor_length,
  FAR struct k1_rtl8852bs_data_tx_layout_s *layout,
  FAR struct k1_rtl8852bs_data_tx_resources_s *resources);
int k1_rtl8852bs_fwdl_runtime_data_tx_diagnostic(void);
int k1_rtl8852bs_runtime_mac_core_init(void);
int k1_rtl8852bs_fwdl_runtime_mac_core_diagnostic(void);
int k1_rtl8852bs_runtime_bb_rf_enable(void);
int k1_rtl8852bs_fwdl_runtime_bb_rf_diagnostic(void);
int k1_rtl8852bs_runtime_phy_cr_init(void);
int k1_rtl8852bs_fwdl_runtime_phy_cr_diagnostic(void);
int k1_rtl8852bs_runtime_bb_reset(void);
int k1_rtl8852bs_fwdl_runtime_bb_reset_diagnostic(void);
int k1_rtl8852bs_runtime_rf_cr_init(void);
int k1_rtl8852bs_fwdl_runtime_rf_cr_diagnostic(void);
int k1_rtl8852bs_runtime_fwrole_maintain_build(
  FAR const struct k1_rtl8852bs_fwrole_maintain_info_s *info,
  FAR uint8_t *content, size_t content_length);
int k1_rtl8852bs_runtime_join_info_build(
  FAR const struct k1_rtl8852bs_join_info_s *info, FAR uint8_t *content,
  size_t content_length);
int k1_rtl8852bs_fwdl_runtime_control_plane_diagnostic(void);
int k1_rtl8852bs_runtime_addr_cam_build(
  FAR const struct k1_rtl8852bs_addr_cam_info_s *info,
  FAR uint8_t *content, size_t content_length);
int k1_rtl8852bs_fwdl_runtime_addr_cam_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_role_cam_done_ack_diagnostic(
  FAR const uint8_t *self_mac);
int k1_rtl8852bs_runtime_scanofld_passive_ch1_build(
  FAR uint8_t *content, size_t content_length);
int k1_rtl8852bs_fwdl_runtime_scanofld_ch_done_ack_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_scanofld_passive_rx_diagnostic(void);
int k1_rtl8852bs_fwdl_runtime_scanofld_active_diagnostic(
  FAR const uint8_t *self_mac);
int k1_rtl8852bs_runtime_scanofld_passive_scan(
  FAR struct k1_rtl8852bs_scan_result_s *result);
int k1_rtl8852bs_fwdl_runtime_auth_diagnostic(FAR const uint8_t *self_mac);
int k1_rtl8852bs_fwdl_runtime_join_diagnostic(FAR const uint8_t *self_mac);
int k1_rtl8852bs_fwdl_preboot_diagnostic(void);
int k1_rtl8852bs_fwdl_h2c_tx_diagnostic(void);
int k1_rtl8852bs_fwdl_fw_header_packet_diagnostic(void);
int k1_rtl8852bs_fwdl_fw_section0_packet_diagnostic(void);
int k1_rtl8852bs_fwdl_fw_section0_tail_packet_diagnostic(void);
int k1_rtl8852bs_fwdl_fw_section0_second_packet_diagnostic(void);
int k1_rtl8852bs_fwdl_fw_section0_third_packet_diagnostic(void);
int k1_rtl8852bs_fwdl_fw_section0_fourth_packet_diagnostic(void);
int k1_rtl8852bs_cmd53_write_diagnostic(void);
int k1_rtl8852bs_first_cmd53_diagnostic(void);

#endif /* __CHIP_K1_K1_RTL8852BS_GPL_H */
