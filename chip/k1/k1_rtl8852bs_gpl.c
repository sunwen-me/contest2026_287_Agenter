/****************************************************************************
 * chip/k1/k1_rtl8852bs_gpl.c
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright(c) 2019 Realtek Corporation. All rights reserved.
 * Copyright (c) 2026 The openvela contributors.
 *
 * Derived from:
 *   https://github.com/spacemit-com/linux-6.6
 *   branch k1-bl-v2.2.y, revision 31c449aeaad8c7759bc983ca0e26946e5b6746dc
 *   drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/mac/mac_ax/
 *     mac_8852b/pwr_seq_8852b.c
 *     mac_8852b/_sdio_8852b.c
 *     mac_8852b/init_8852b.c
 *     mac_8852b/dle_8852b.c
 *     mac_8852b/hci_fc_8852b.c
 *     mac_8852b/efuse_8852b.c
 *     fwofld.c
 *     fwofld.h
 *     dle.c
 *     efuse.c
 *     hci_fc.c
 *     _sdio.c
 *     fwdl.c
 *     fwcmd.c
 *     init.c
 *     pwr.c
 *     trxcfg.c
 *     role.c
 *     addr_cam.c
 *     rx_filter.c
 *     hw.c
 *     hw.h
 *   drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/mac/fw_ax/inc_hdr/
 *     fwcmd_intf.h
 *   drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/mac/fw_ax/rtl8852b/
 *     hal8852b_fw.c
 *   drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/mac/
 *     hci_reg_ax.h
 *     mac_reg_ax.h
 *     txdesc.h
 *   drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/
 *     hal_api_mac.c
 *     hal_rx.c
 *   drivers/net/wireless/realtek/rtl8852bs/phl/
 *     phl_scanofld.c
 *
 * Adaptation: retain the SDIO power-on table and SDIO pre-init register
 * updates, omit PCIe-only item 0x71, and replace Linux/MMC access with K1
 * Function 1 CMD52 helpers for the power table and post-power registers.
 * The optional DLE/SCC and HCI flow-control setup use bounded Function 1
 * indirect CMD53 access.
 * The optional firmware preboot probe enters and then exits the vendor's
 * download-ready CPU state.  The optional H2C TX diagnostic reads the
 * resource table and builds its descriptor entirely in RAM.  The optional
 * firmware-header diagnostic sends only the static 80-byte header from the
 * GPL U2 NICCE image, then immediately returns the WLAN CPU to its reset
 * state.  The optional section diagnostics repeat that header and send the
 * first section packet plus only the explicitly selected bounded follow-up
 * packets before cleanup.  The optional full-download profile uses the same
 * source image, applies the selected legacy MSS signature only in its
 * bounded packet buffer, and leaves the WCPU running only after
 * firmware-ready.  The optional MSS eFuse diagnostic only powers the eFuse
 * read path long enough to read its two selector bytes.  The runtime RX
 * helpers expose only bounded FIFO transfer and descriptor parsing for a
 * future network worker; they do not register an 802.11 MAC or create a
 * network device.  The optional runtime MAC-core diagnostic applies the
 * directly usable static fields from trxcfg.c after firmware-ready and
 * verifies every field, while omitting the scheduler, address CAM, role,
 * station, security, scan, and association parts of MAC initialization.  The
 * runtime control-plane serializer follows role.c for the FWROLE_MAINTAIN
 * and JOININFO payload layouts.  The optional role/CAM diagnostic uses the
 * eFuse self MAC to create exactly one no-link STA role and its CAM record,
 * and checks both volatile firmware done acknowledgements.  It does not
 * scan,
 * associate, register a network device, or retain state across a reset.  The
 * address CAM serializer follows addr_cam.c for the non-multicast create
 * layout and otherwise remains a bounded RAM-only helper.
 * The optional PHY CR diagnostic replays the complete static RTL8852B
 * array_mp_8852b_phy_reg image after the BB/RF release and verifies spread
 * read-back sentinels.  It deliberately excludes the RF image, calibration,
 * any 802.11 protocol stack, and persistent storage.
 * C2H parser follows c2h_field_parsing() and
 * mac_process_c2h() in fwcmd.c plus the common header bit layout in
 * fwcmd_intf.h; it additionally rejects length fields outside the received
 * SDIO frame before exposing any content.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_K1_RTL8852BS2_GPL_BOOTSTRAP

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>

#include "k1_rtl8852bs_gpl.h"
#include "k1_sdio.h"
#include "riscv_internal.h"

extern void k1_early_puts(FAR const char *str);
extern void k1_early_puthex(uintreg_t value);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_RTL8852BS_LOCAL_REG_START       0x1000u
#define K1_RTL8852BS_LOCAL_REG_END         0x1f00u

#define K1_RTL8852BS_INDIRECT_ADDR         0x1040u
#define K1_RTL8852BS_INDIRECT_DATA         0x1044u
#define K1_RTL8852BS_INDIRECT_CTRL         0x1048u
#define K1_RTL8852BS_INDIRECT_READY        (1u << 7)
#define K1_RTL8852BS_INDIRECT_REG_READ     0x08u
#define K1_RTL8852BS_INDIRECT_REG_WRITE8   0x04u
#define K1_RTL8852BS_INDIRECT_REG_WRITE32  0x06u
#define K1_RTL8852BS_INDIRECT_POLL_COUNT   50u
#define K1_RTL8852BS_INDIRECT_POLL_USEC    20u

/* k1_sdio_wifi_cmd53() resets the K1 command and data state machines on a
 * request error.  PHY CR writes configure ordinary registers, so replaying
 * an identical value after a recovered data-CRC error is safe.  Do not use
 * this policy for FIFO, H2C, firmware, or write-one-to-clear transactions.
 */

#define K1_RTL8852BS_PHY_CR_CRC_RETRIES     1u

/* These match PWR_POLL_CNT and PWR_POLL_DLY_US in the derived driver. */

#define K1_RTL8852BS_POWER_POLL_COUNT      2000u
#define K1_RTL8852BS_POWER_POLL_USEC       1000u

/* SDIO pre-init register definitions from the derived Realtek driver. */

#define K1_RTL8852BS_HCI_OPT_CTRL          0x0074u
#define K1_RTL8852BS_HCI_FUNC_EN           0x8380u
#define K1_RTL8852BS_DMAC_FUNC_EN          0x8400u
#define K1_RTL8852BS_DMAC_CLK_EN           0x8404u
#define K1_RTL8852BS_SDIO_TX_CTRL          0x1000u
#define K1_RTL8852BS_SDIO_BUS_CTRL         0x1084u
#define K1_RTL8852BS_HCI_DMA_EN            0x00000003u
#define K1_RTL8852BS_DMAC_PRE_FUNC_EN      0x60440000u
#define K1_RTL8852BS_DMAC_PRE_CLK_EN       0x00040000u
#define K1_RTL8852BS_DMAC_FUNC_EN_FULL     0xfb7d0000u

/* The DMAC function-enable word is exactly the original's list for this chip:
 * protect, MAC, DMAC, MPDU processor, WD release, transmit packet control,
 * station scheduler, packet buffer, DMAC table, packet in, DLE CPU I/O,
 * dispatcher and security engine.  The clock-enable word next to it was not:
 * it was missing the DLE CPU I/O and packet-in clocks, which the original
 * enables in the very same function.  Those two blocks are the ones that hand
 * a newly queued descriptor onward - the firmware's own frames arrive through
 * CPU I/O and are charged to the WLAN-CPU quota, a host-submitted frame
 * arrives through packet-in and is charged to the host-interface quota - so
 * with their clocks off both kinds of frame can be accepted and accounted for
 * while nothing ever advances them and no error is raised.  The wide witness
 * recorded precisely that for both.  The two DLE clocks are added later by
 * the DLE SCC init, and the MPDU clock this port already sets is kept.
 */

#define K1_RTL8852BS_DMAC_CLK_EN_CPUIO     (1u << 19)
#define K1_RTL8852BS_DMAC_CLK_EN_PKT_IN    (1u << 20)
#define K1_RTL8852BS_DMAC_CLK_EN_FULL      (0x1f070000u | \
                                            K1_RTL8852BS_DMAC_CLK_EN_CPUIO | \
                                            K1_RTL8852BS_DMAC_CLK_EN_PKT_IN)
#define K1_RTL8852BS_CMAC_FUNC_EN          0xc000u
#define K1_RTL8852BS_CMAC_FUNC_EN_VALUE    0xf000003fu
#define K1_RTL8852BS_CMAC_CLK_EN           0xc004u
#define K1_RTL8852BS_CMAC_CLK_EN_VALUE     0x4000003fu
#define K1_RTL8852BS_SDIO_DATA_PAD_SMT     (1u << 19)
#define K1_RTL8852BS_CMD53_TX_FORMAT       (1u << 13)
#define K1_RTL8852BS_RXINT_READ_MASK_DIS   (1u << 3)
#define K1_RTL8852BS_EN_RPT_TXCRC          (1u << 9)

/* RTL8852B BB/RF release sequence from set_enable_bb_rf().  This is the
 * bounded precondition before the much larger BB/RF parameter images and
 * calibration flows run in the original driver.
 */

#define K1_RTL8852BS_SYS_FUNC_EN            0x0002u
#define K1_RTL8852BS_SYS_FUNC_BB_ENABLE     0x03u
#define K1_RTL8852BS_SPS_DIG_ON_CTRL0       0x0200u
#define K1_RTL8852BS_SPS_DIG_ZCDC_MASK      0x00060000u
#define K1_RTL8852BS_SPS_DIG_ZCDC_VALUE     0x00020000u
#define K1_RTL8852BS_WLRF_CTRL              0x02f0u
#define K1_RTL8852BS_WLRF_AFEDIG            (1u << 17)
#define K1_RTL8852BS_WLAN_XTAL_SI_CTRL      0x0270u
#define K1_RTL8852BS_XTAL_SI_CMD_POLL       (1u << 31)
#define K1_RTL8852BS_XTAL_SI_MODE_READ      (1u << 24)
#define K1_RTL8852BS_XTAL_SI_FULL_MASK      (0xffu << 16)
#define K1_RTL8852BS_XTAL_SI_DATA_SHIFT     8u
#define K1_RTL8852BS_XTAL_SI_WL_RFC_S0      0x80u
#define K1_RTL8852BS_XTAL_SI_WL_RFC_S1      0x81u
#define K1_RTL8852BS_XTAL_SI_WL_RFC_VALUE   0xc7u
#define K1_RTL8852BS_XTAL_SI_POLL_COUNT     1000u
#define K1_RTL8852BS_XTAL_SI_POLL_USEC      50u
#define K1_RTL8852BS_PHYREG_SET             0x8040u
#define K1_RTL8852BS_PHYREG_XYN_CYCLE       0x0eu

/* RTL8852B Wi-Fi eFuse read sequence from read_hw_efuse() and the 8852B
 * eFuse power-cut helpers.  This implementation only uses the read mode:
 * it never writes the eFuse data field, unlock code, or burn control.
 */

#define K1_RTL8852BS_SYS_ISO_CTRL           0x0000u
#define K1_RTL8852BS_EFUSE_CTRL             0x0030u
#define K1_RTL8852BS_PMC_DBG_CTRL2          0x00ccu
#define K1_RTL8852BS_EFUSE_READY            (1u << 29)
#define K1_RTL8852BS_EFUSE_ADDRESS_SHIFT    16u
#define K1_RTL8852BS_EFUSE_ADDRESS_MASK     0x07ffu
#define K1_RTL8852BS_EFUSE_DATA_MASK        0xffffu
#define K1_RTL8852BS_EFUSE_ISO_ENABLE       (1u << 14)
#define K1_RTL8852BS_EFUSE_ISO_STABLE       (1u << 15)
#define K1_RTL8852BS_EFUSE_ISO_BLOCK        (1u << 8)
#define K1_RTL8852BS_EFUSE_PMCR_WRITE       (1u << 2)
#define K1_RTL8852BS_EFUSE_READ_POLL_COUNT  30000u
#define K1_RTL8852BS_EFUSE_READ_POLL_USEC   1u
#define K1_RTL8852BS_EFUSE_POWER_DELAY_USEC 1000u
#define K1_RTL8852BS_EFUSE_EXTERNAL_PN      0x05ecu
#define K1_RTL8852BS_EFUSE_CUSTOMER         0x05edu
#define K1_RTL8852BS_EFUSE_SEC_CTRL_SIZE    4u
#define K1_RTL8852BS_EFUSE_PHYSICAL_SIZE    1536u
#define K1_RTL8852BS_EFUSE_LOGICAL_SIZE     2048u
#define K1_RTL8852BS_EFUSE_MAC_OFFSET       0x041au
#define K1_RTL8852BS_EFUSE_MAC_SIZE         6u

/* Logical eFuse offsets of the RF context, from the 8852B halrf eFuse map
 * (halrf_efuse_8852b.h).  Only reads happen here.  A byte reading 0xff is an
 * unprogrammed eFuse cell; the vendor defaults that halrf substitutes in that
 * case are listed beside each offset so the report can say which value the
 * vendor driver would end up using.
 */

#define K1_RTL8852BS_EFUSE_RF_BOARD_OPTION  0x02c1u  /* default 0x01 */
#define K1_RTL8852BS_EFUSE_RF_CHAN_PLAN     0x02b8u  /* default 0x7f */
#define K1_RTL8852BS_EFUSE_RF_XTAL          0x02b9u  /* default 0x3f */
#define K1_RTL8852BS_EFUSE_RF_RFE_TYPE      0x02cau  /* default 0x01 */
#define K1_RTL8852BS_EFUSE_RF_THERMAL_A     0x02d0u  /* default 0x22 */
#define K1_RTL8852BS_EFUSE_RF_THERMAL_B     0x02d1u  /* default 0x22 */
#define K1_RTL8852BS_EFUSE_RF_TSSI_DE_FIRST 0x0210u  /* default 0x00 */
#define K1_RTL8852BS_EFUSE_RF_TSSI_DE_LAST  0x0259u
#define K1_RTL8852BS_EFUSE_RF_GAIN_K_FIRST  0x02d4u  /* default 0x0f */
#define K1_RTL8852BS_EFUSE_RF_GAIN_K_LAST   0x02ddu
#define K1_RTL8852BS_EFUSE_RF_DEFAULT_RFE   0x01u
#define K1_RTL8852BS_EFUSE_RF_UNPROGRAMMED  0xffu

/* Chip cut version.  R_AX_SYS_CFG1 bits 15:12, i.e. the high nibble of the
 * byte at 0x00f1.  enum rtw_cv numbers CAV 0, CBV 1, CCV 2.
 */

#define K1_RTL8852BS_SYS_CFG1_CV_BYTE       0x00f1u
#define K1_RTL8852BS_SYS_CFG1_CV_SHIFT      4u

/* RTL8852B SDIO/SCC DLE setup from dle_mem_sdio_8852b. */

#define K1_RTL8852BS_WDE_PKTBUF_CFG        0x8c08u
#define K1_RTL8852BS_WDE_QTA0_CFG          0x8c40u
#define K1_RTL8852BS_WDE_QTA1_CFG          0x8c44u
#define K1_RTL8852BS_WDE_QTA3_CFG          0x8c4cu
#define K1_RTL8852BS_WDE_QTA4_CFG          0x8c50u
#define K1_RTL8852BS_WDE_INI_STATUS        0x8d00u
#define K1_RTL8852BS_PLE_PKTBUF_CFG        0x9008u
#define K1_RTL8852BS_PLE_QTA0_CFG          0x9040u
#define K1_RTL8852BS_PLE_QTA1_CFG          0x9044u
#define K1_RTL8852BS_PLE_QTA2_CFG          0x9048u
#define K1_RTL8852BS_PLE_QTA3_CFG          0x904cu
#define K1_RTL8852BS_PLE_QTA4_CFG          0x9050u
#define K1_RTL8852BS_PLE_QTA5_CFG          0x9054u
#define K1_RTL8852BS_PLE_QTA6_CFG          0x9058u
#define K1_RTL8852BS_PLE_QTA7_CFG          0x905cu
#define K1_RTL8852BS_PLE_QTA8_CFG          0x9060u
#define K1_RTL8852BS_PLE_QTA9_CFG          0x9064u
#define K1_RTL8852BS_PLE_QTA10_CFG         0x9068u
#define K1_RTL8852BS_PLE_INI_STATUS        0x9100u
#define K1_RTL8852BS_DLE_ENABLE_MASK       ((1u << 26) | (1u << 23))
#define K1_RTL8852BS_DLE_PKTBUF_FIELDS     0x1fff3f03u
#define K1_RTL8852BS_DLE_READY_MASK        0x00000003u
#define K1_RTL8852BS_DLE_READY_POLL_COUNT  2000u
#define K1_RTL8852BS_DLE_READY_POLL_USEC   1u

/* RTL8852B SDIO/SCC HCI flow-control setup from hfc_init(adapter, 1, 1, 1).
 * The page counts come from hfc_chcfg_sdio_8852b and hfc_pubcfg_sdio_8852b.
 */

#define K1_RTL8852BS_HCI_FC_CTRL           0x8a00u
#define K1_RTL8852BS_CH_PAGE_CTRL          0x8a04u
#define K1_RTL8852BS_ACH0_PAGE_CTRL        0x8a10u
#define K1_RTL8852BS_ACH1_PAGE_CTRL        0x8a14u
#define K1_RTL8852BS_ACH2_PAGE_CTRL        0x8a18u
#define K1_RTL8852BS_ACH3_PAGE_CTRL        0x8a1cu
#define K1_RTL8852BS_CH8_PAGE_CTRL         0x8a30u
#define K1_RTL8852BS_CH9_PAGE_CTRL         0x8a34u
#define K1_RTL8852BS_PUB_PAGE_CTRL1        0x8a90u
#define K1_RTL8852BS_PUB_PAGE_CTRL2        0x8a94u
#define K1_RTL8852BS_WP_PAGE_CTRL1         0x8aa0u
#define K1_RTL8852BS_WP_PAGE_CTRL2         0x8aa4u
#define K1_RTL8852BS_HCI_FC_ENABLE         (1u << 0)
#define K1_RTL8852BS_HCI_FC_MODE_SDIO      (2u << 1)
#define K1_RTL8852BS_HCI_FC_H2C_ENABLE     (1u << 3)
#define K1_RTL8852BS_HCI_FC_CONFIG_MASK    0x00000ffeu
#define K1_RTL8852BS_HCI_FC_DISABLE_MASK   \
  (K1_RTL8852BS_HCI_FC_ENABLE | K1_RTL8852BS_HCI_FC_H2C_ENABLE)
#define K1_RTL8852BS_HCI_FC_ENABLE_VALUE   \
  (K1_RTL8852BS_HCI_FC_ENABLE | K1_RTL8852BS_HCI_FC_MODE_SDIO | \
   K1_RTL8852BS_HCI_FC_H2C_ENABLE)
#define K1_RTL8852BS_HCI_FC_CHANNEL_PAGES  0x00660002u
#define K1_RTL8852BS_HCI_FC_PUBLIC_PAGES   112u
#define K1_RTL8852BS_HCI_FC_PRECOSTS        0x00280001u

/* RTL8852B firmware-download CPU preboot from mac_enable_cpu(..., 1),
 * fwdl_phase0(), and mac_disable_cpu().  The probe deliberately stops after
 * H2C path readiness.
 */

#define K1_RTL8852BS_SYS_CLK_CTRL           0x0008u
#define K1_RTL8852BS_PLATFORM_ENABLE        0x0088u
#define K1_RTL8852BS_HALT_H2C_CTRL           0x0160u
#define K1_RTL8852BS_HALT_C2H_CTRL           0x0164u
#define K1_RTL8852BS_HALT_H2C                0x0168u
#define K1_RTL8852BS_HALT_C2H                0x016cu
#define K1_RTL8852BS_HISR0                   0x01a4u
#define K1_RTL8852BS_WCPU_FW_CTRL           0x01e0u
#define K1_RTL8852BS_BOOT_REASON            0x01e6u
#define K1_RTL8852BS_LDM                     0x01e8u
#define K1_RTL8852BS_CPU_CLK_ENABLE         (1u << 14)
#define K1_RTL8852BS_WCPU_ENABLE            (1u << 1)
#define K1_RTL8852BS_WCPU_FWDL_ENABLE       (1u << 0)
#define K1_RTL8852BS_H2C_PATH_READY         (1u << 1)
#define K1_RTL8852BS_FWDL_PATH_READY        (1u << 2)
#define K1_RTL8852BS_WCPU_FWDL_STATUS_MASK  (7u << 5)
#define K1_RTL8852BS_WCPU_FWDL_STATUS_SHIFT 5u
#define K1_RTL8852BS_FWDL_CHECKSUM_FAIL     2u
#define K1_RTL8852BS_FWDL_SECURITY_FAIL     3u
#define K1_RTL8852BS_FWDL_CUT_NOT_MATCH     4u
#define K1_RTL8852BS_FWDL_READY             7u
#define K1_RTL8852BS_BOOT_REASON_MASK       0x00000007u
#define K1_RTL8852BS_FW_PREBOOT_POLL_COUNT  400000u
#define K1_RTL8852BS_FW_PREBOOT_POLL_USEC   1u

/* RTL8852B SDIO H2C TX accounting from ud_fs_8852b(), tx_allow_fwcmd_ch(),
 * tx_cmd_addr_sdio(), and txdes_proc_h2c_fwdl_8852b().  This diagnostic
 * deliberately stops before the resulting FIFO address is submitted to
 * CMD53.
 */

#define K1_RTL8852BS_SDIO_TXPG_WP           0x1110u
#define K1_RTL8852BS_SDIO_TXPG_WP_SIZE      28u
#define K1_RTL8852BS_SDIO_HIMR              0x1100u
#define K1_RTL8852BS_SDIO_HISR              0x1104u
#define K1_RTL8852BS_SDIO_RX_REQ_LEN        0x1108u
#define K1_RTL8852BS_SDIO_RX_REQ_LEN_MASK   0x0003ffffu
#define K1_RTL8852BS_H2C_AVAL_PAGE_SHIFT    16u
#define K1_RTL8852BS_H2C_AVAL_PAGE_MASK     0x1fffu
#define K1_RTL8852BS_H2C_CHANNEL            12u
#define K1_RTL8852BS_H2C_PLE_PAGE_SIZE      128u
#define K1_RTL8852BS_H2C_DESCRIPTOR_SIZE    24u
#define K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE  8u
#define K1_RTL8852BS_H2C_TX_FIFO_BASE       0x00010000u
#define K1_RTL8852BS_H2C_TX_FIFO_SHIFT      12u
#define K1_RTL8852BS_H2C_TX_UNIT_SIZE       8u
#define K1_RTL8852BS_H2C_TX_UNIT_MASK       0x0fffu
#define K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT  16u
#define K1_RTL8852BS_H2C_TXD_LENGTH_MASK    0x3fffu
#define K1_RTL8852BS_H2C_FWCMD_IDENTIFIER   0x0000000du
#define K1_RTL8852BS_H2C_RESOURCE_POLL_COUNT 5u
#define K1_RTL8852BS_H2C_RESOURCE_POLL_MSEC 5u
#define K1_RTL8852BS_DATA_TX_DESCRIPTOR_SIZE 24u
#define K1_RTL8852BS_DATA_TX_CHANNEL_MAX     12u
#define K1_RTL8852BS_DATA_TX_CHANNEL_PAGES   102u
#define K1_RTL8852BS_DATA_TX_WDE_PAGES       1u
#define K1_RTL8852BS_DATA_TX_PLE_RESERVED    32u
#define K1_RTL8852BS_DATA_TX_PAYLOAD_DESC    24u
#define K1_RTL8852BS_DATA_TX_PLE_RESERVE     52u
#define K1_RTL8852BS_DATA_TXD_STF_MODE       (1u << 10)
#define K1_RTL8852BS_DATA_TXD_CH_DMA_SHIFT   16u
#define K1_RTL8852BS_DATA_TXD_QSEL_SHIFT     17u
#define K1_RTL8852BS_DATA_TXD_TID_IND        (1u << 23)
#define K1_RTL8852BS_DATA_TXD_MACID_SHIFT    24u
#define K1_RTL8852BS_DATA_TXD_SEQUENCE_MASK  0x0fffu
#define K1_RTL8852BS_DATA_TXD_MACID_MASK     0x007fu

/* Host side management transmit descriptor.  txdes_proc_mgnt_8852b() builds a
 * WD BODY followed by a WD INFO, both twenty four bytes, and the SDIO variant
 * of that routine sets store and forward mode in the body without the WD page
 * bit the PCIe variant uses.  The field positions are the 8852B block of the
 * original txdesc.h.
 */

#define K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE    24u
#define K1_RTL8852BS_MGMT_TX_WD_INFO_SIZE    24u
#define K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE 48u
#define K1_RTL8852BS_MGMT_TXD_WDINFO_EN      (1u << 22)
#define K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG    8u
#define K1_RTL8852BS_MGMT_TXD_QSEL_B0MG      0x12u
#define K1_RTL8852BS_MGMT_TXI_USERATE_SEL    (1u << 30)
#define K1_RTL8852BS_MGMT_TXI_DATARATE_SHIFT 16u
#define K1_RTL8852BS_MGMT_TXI_DATARATE_CCK1  0x000u
#define K1_RTL8852BS_MGMT_TXI_DISDATAFB      (1u << 10)
#define K1_RTL8852BS_MGMT_TXI_BMC            (1u << 11)
#define K1_RTL8852BS_MGMT_TX_MACID           0u
#define K1_RTL8852BS_MGMT_TX_DRAIN_POLL      20u
#define K1_RTL8852BS_MGMT_TX_DRAIN_USEC      1000u

/* R_AX_SS_CTRL and the four steps sta_sch_init() performs on it.  The station
 * scheduler is the DMAC block that walks the WDE queues and tells a CMAC
 * scheduler which of them hold a frame; nothing in this port has ever written
 * this register, so if its reset value leaves the block disabled then a frame
 * the firmware enqueues stays in WDE with no error bit raised anywhere, which
 * is exactly the state the wide witness recorded.  The original sequence for
 * this chip in hardware transmit mode is: set the enable, wait for the
 * initialisation-done flag, set the warm-initialisation flag, and clear the
 * non-empty report path that only the software transmit mode uses.  The
 * chip-specific SS2FINFO destination patch is disabled for 8852B, so it has
 * no equivalent here.
 */

#define K1_RTL8852BS_SS_CTRL                 0x9e10u
#define K1_RTL8852BS_SS_CTRL_EN              (1u << 0)
#define K1_RTL8852BS_SS_CTRL_NONEMPTY_SS2F   (1u << 28)
#define K1_RTL8852BS_SS_CTRL_WARM_INIT_FLG   (1u << 29)
#define K1_RTL8852BS_SS_CTRL_INIT_DONE_1     (1u << 31)
#define K1_RTL8852BS_SS_CTRL_POLL_COUNT      2000u
#define K1_RTL8852BS_SS_CTRL_POLL_USEC       1u

/* Static post-firmware fields from mac_trx_init() in trxcfg.c.  The full
 * vendor routine also requires scheduler, address-CAM, security, role, and
 * firmware policy state that is not present in this staged NuttX port.
 */

#define K1_RTL8852BS_MPDU_PROC               0x9c00u
#define K1_RTL8852BS_ACTION_FWD0             0x9c04u
#define K1_RTL8852BS_TF_FWD                  0x9c14u
#define K1_RTL8852BS_CUT_AMSDU_CTRL          0x9c40u
#define K1_RTL8852BS_TX_SUB_CARRIER_VALUE    0xc088u
#define K1_RTL8852BS_PTCL_RRSR1              0xc090u
#define K1_RTL8852BS_PTCL_COMMON_SETTING0    0xc600u
#define K1_RTL8852BS_TB_PPDU_CTRL            0xc60cu
#define K1_RTL8852BS_PTCLRPT_FULL_HDL        0xc660u
#define K1_RTL8852BS_RXDMA_CTRL0             0xc804u
#define K1_RTL8852BS_TCR0                    0xca00u
#define K1_RTL8852BS_TXD_FIFO_CTRL           0xca1cu
#define K1_RTL8852BS_TRXPTCL_RESP0           0xcc04u
#define K1_RTL8852BS_MAC_LOOPBACK            0xcc20u
#define K1_RTL8852BS_MAC_LOOPBACK_COUNT      0xcc28u
#define K1_RTL8852BS_RXTRIG_TEST_USER2       0xccb0u
#define K1_RTL8852BS_RCR                     0xce00u
#define K1_RTL8852BS_PLCP_HDR_FLTR           0xce04u
#define K1_RTL8852BS_MGNT_FLTR               0xce28u
#define K1_RTL8852BS_RX_FLTR_OPT             0xce20u
#define K1_RTL8852BS_ADDR_CAM_CTRL            0xce34u
#define K1_RTL8852BS_RESPBA_CAM_CTRL         0xce3cu
#define K1_RTL8852BS_PPDU_STAT               0xce40u
#define K1_RTL8852BS_MACID_MATCH             0xce48u

/* Coexistence arbitration.  This part combines a Wi-Fi and a Bluetooth core
 * behind one 2.4 GHz front end, and an arbiter decides which of them may use
 * it.  The vendor programs this block from its own coex_init operation and
 * mainline from rtw89_mac_coex_init(), and every vendor call site sits inside
 * CONFIG_BTCOEX, so a build without Bluetooth coexistence never runs any of
 * it.  What such a build does run is scheduler_init_ax(), which clears the one
 * bit here that lets the Bluetooth core's channel assessment hold the medium
 * against the Wi-Fi core.  That bit is set at reset, and this port had never
 * cleared it.  The addresses and bits are identical in the vendor register
 * header and in mainline.
 *
 * The arbiter enables, the arbitration mode, the pin direction and the
 * Bluetooth statistics counters are deliberately not named here: a Wi-Fi only
 * image has no reason to start an arbiter for a core that is not running, and
 * the first board run that did start one changed nothing.  The registers are
 * still read and reported, which is what the addresses below are for.
 */

#define K1_RTL8852BS_GPIO_MUXCFG              0x0040u
#define K1_RTL8852BS_SYS_SDIO_CTRL            0x0070u
#define K1_RTL8852BS_CCA_CFG_0                0xc340u
#define K1_RTL8852BS_BTCCA_EN                 0x00000020u
#define K1_RTL8852BS_BTCCA_BRK_TXOP_EN        0x00000200u
#define K1_RTL8852BS_RSP_CHK_BTCCA            0x02000000u
#define K1_RTL8852BS_BTC_FUNC_EN              0xda20u
#define K1_RTL8852BS_PTA_WL_TX_EN             0x00000002u
#define K1_RTL8852BS_BT_COEX_CFG_2            0xda34u
#define K1_RTL8852BS_CSR_MODE                 0xda40u
#define K1_RTL8852BS_TDMA_MODE                0xda4cu
#define K1_RTL8852BS_BT_COEX_CFG_5            0xda6cu

/* The two grant words are not memory mapped.  They are reached through a
 * command register that takes a byte enable of all four bytes together with
 * the word offset, the same window rtw89_mac_read_lte() uses.  The grant value
 * below is one band of the original cfg_gnt operation: the Wi-Fi grant
 * asserted and the Bluetooth grant withdrawn, both under software control,
 * which is what a Wi-Fi only image wants from a shared front end.
 */

#define K1_RTL8852BS_LTE_CTRL                 0xdaf0u
#define K1_RTL8852BS_LTE_WDATA                0xdaf4u
#define K1_RTL8852BS_LTE_RDATA                0xdaf8u
#define K1_RTL8852BS_LTE_READY                0x20000000u
#define K1_RTL8852BS_LTE_READ_COMMAND         0x800f0000u
#define K1_RTL8852BS_LTE_WRITE_COMMAND        0xc00f0000u
#define K1_RTL8852BS_LTE_SW_CFG_1             0x0038u
#define K1_RTL8852BS_LTE_SW_CFG_2             0x003cu
#define K1_RTL8852BS_LTE_WL_RX_CTRL           0x00000100u
#define K1_RTL8852BS_LTE_POLL_COUNT           100u
#define K1_RTL8852BS_LTE_POLL_USEC            50u
#define K1_RTL8852BS_GNT_WL_SW_BAND0          0x00007700u

/* The grant words only reach the arbiter when the multiplexer in front of it
 * selects the software path; rtw89_mac_cfg_ctrl_path() sets this bit for
 * Wi-Fi and clears it for Bluetooth.  Left clear, the values written through
 * the indirect window above are stored and ignored.
 */

#define K1_RTL8852BS_LTE_MUX_CTRL_PATH        0x04000000u

/* The other two words that can hold the medium busy: the contention, SIFS and
 * trigger-based check enables, and the cap on how long a received frame may
 * reserve the medium.  Both are sampled with the coex state and both are now
 * programmed to their vendor value; the masks and values are below, next to
 * the scheduler registers they belong with.
 */

#define K1_RTL8852BS_CCA_CONTROL              0xc390u
#define K1_RTL8852BS_WMAC_NAV_CTL             0xcc80u

/* Transmit power, in baseband register numbers.  halbb_set_txpwr_dbm_8852b()
 * forces a constant power with the enable bit in the first word and a nine bit
 * signed value in the second; halbb_set_cck_txpwr_idx_8852b() carries the CCK
 * index per path; halbb_get_txinfo_txpwr_dbm_8852b() reads back the power the
 * baseband recorded for the most recent transmit, which is the one number here
 * that says something about a frame rather than about a setting.
 */

#define K1_RTL8852BS_BB_TXPWR_FORCE_CTRL      0x09a4u
#define K1_RTL8852BS_BB_TXPWR_FORCE_ENABLE    0x00010000u
#define K1_RTL8852BS_BB_TXPWR_FORCE_VALUE     0x4594u
#define K1_RTL8852BS_BB_TXPWR_VALUE_MASK      0x7fc00000u
#define K1_RTL8852BS_BB_TXPWR_VALUE_SHIFT     22
#define K1_RTL8852BS_BB_TXPWR_FORCE_DBM       0x40u
#define K1_RTL8852BS_BB_CCK_TXPWR_INDEX_A     0x5808u
#define K1_RTL8852BS_BB_CCK_TXPWR_INDEX_B     0x7808u
#define K1_RTL8852BS_BB_CCK_TXPWR_INDEX_MASK  0x0003fe00u
#define K1_RTL8852BS_BB_CCK_TXPWR_INDEX_SHIFT 9
#define K1_RTL8852BS_BB_TXINFO_TXPWR          0x1804u
#define K1_RTL8852BS_BB_TXINFO_TXPWR_MASK     0x07fc0000u
#define K1_RTL8852BS_BB_TXINFO_TXPWR_SHIFT    18

/* Transmit power in MAC space.  This is the block halrf_set_power() fills and
 * halrf_wlan_tx_power_control_8852b() overrides: the first word forces one
 * constant power for every rate, the second lets the Bluetooth side drive the
 * transmit gain instead, and the three tables hold the per rate power the
 * hardware uses when nothing forces it.  The tables are read to find out
 * whether they are still zero, which is what a port that never ran
 * halrf_set_power() would leave behind.
 */

#define K1_RTL8852BS_PWR_RATE_CTRL            0xd200u
#define K1_RTL8852BS_PWR_FORCE_BY_RATE_ALL    0x000003ffu
#define K1_RTL8852BS_PWR_FORCE_BY_RATE_EN     0x00000200u
#define K1_RTL8852BS_PWR_COEXT_CTRL           0xd220u
#define K1_RTL8852BS_PWR_COEXT_TXAGC_BT       0x00000ffau
#define K1_RTL8852BS_PWR_BY_RATE_TABLE0       0xd2c0u
#define K1_RTL8852BS_PWR_BY_RATE_1SS_MAX      0xd2d8u
#define K1_RTL8852BS_PWR_BY_RATE_MAX          0xd2e8u
#define K1_RTL8852BS_MAC_TXPWR_FORCE_VALUE    0x00000040u
/* RMAC per-PPDU-type receive counter window, the register pair behind the
 * original mac_rx_cnt().  Byte 0 selects one of 48 counters and the upper
 * half word returns its 16 bit value, so a read costs one selection write
 * and one read back.  The vendor writes byte 0 alone; this component only
 * has 32 bit indirect access, so the selection keeps byte 1, clears the
 * reset trigger in bit 8 so a read never restarts the counters, and leaves
 * the read-only count in the upper half word to be overwritten with zero.
 */

#define K1_RTL8852BS_RX_DBG_CNT_SEL          0xcee0u
#define K1_RTL8852BS_RX_DBG_CNT_INDEX_MASK   0x0000003fu
#define K1_RTL8852BS_RX_DBG_CNT_KEEP_MASK    0x0000ff00u
#define K1_RTL8852BS_RX_DBG_CNT_RESET        0x00000100u
#define K1_RTL8852BS_RX_DBG_CNT_VALUE_SHIFT  16
#define K1_RTL8852BS_RX_DBG_CNT_VALUE_MASK   0x0000ffffu
#define K1_RTL8852BS_RX_PPDU_TYPES           8

/* The RMAC counter indices that do not belong to a PPDU type, named after the
 * original rx_cnt_type[] table.  They separate the possible receive faults:
 * RECCA counts every clear channel assessment the baseband reports, so it is
 * non-zero on any live band even when nothing demodulates; INVD, FULLDRP,
 * FULLDRP_PKT and RXDMA count frames lost inside RMAC or on the way to the
 * bus; PKTFLTR_DRP counts frames the receive filter rejected.
 */

#define K1_RTL8852BS_RX_CNT_INVALID          30u
#define K1_RTL8852BS_RX_CNT_RECCA            31u
#define K1_RTL8852BS_RX_CNT_FULL_DROP        32u
#define K1_RTL8852BS_RX_CNT_FULL_DROP_PKT    33u
#define K1_RTL8852BS_RX_CNT_RXDMA            34u
#define K1_RTL8852BS_RX_CNT_FILTER_DROP      35u

/* The transmit side of the same counter mechanism, R_AX_TX_PPDU_CNT of the
 * original tx_cnt_dump().  The selection index is four bits wide here, the
 * read index sits in byte 1 and the reset trigger in bit 12, so the same
 * keep-and-clear rule applies.  Index 0 to 3 are the LCCK, SCCK, OFDM and HT
 * entries of the vendor tx_cnt_type_g6[] table, which is every modulation a
 * 2.4 GHz Probe Request can be transmitted with.
 *
 * A transmitted PPDU is counted here by the transmit MAC itself, so these
 * counters separate the two reasons a Probe Response can be missing: a frame
 * that was never transmitted leaves them unchanged, and a frame that was
 * transmitted but not answered increments them.
 */

#define K1_RTL8852BS_TX_PPDU_CNT_SEL         0xcae0u
#define K1_RTL8852BS_TX_PPDU_CNT_INDEX_MASK  0x0000000fu
#define K1_RTL8852BS_TX_PPDU_CNT_RESET       0x00001000u
#define K1_RTL8852BS_TX_PPDU_CNT_VALUE_SHIFT 16
#define K1_RTL8852BS_TX_PPDU_CNT_VALUE_MASK  0x0000ffffu
#define K1_RTL8852BS_TX_PPDU_TYPES           11u
#define K1_RTL8852BS_TX_PPDU_CNT_INDEX_OFDM  2u

/* Two details of the original mac_get_tx_cnt() that this component used to
 * leave out, and that decide whether a table of zeroes means anything.
 *
 * The original waits a millisecond between writing the selection index and
 * reading the counter, so without the wait the value read belongs to whichever
 * index was selected before rather than to the one just written.  The original
 * also writes the index with a sixteen bit access, so the counter half of the
 * word is never written at all; this component has only a thirty two bit
 * accessor, so the counter half is carried back unchanged instead of being
 * cleared the way a keep mask over the low half would clear it.
 */

#define K1_RTL8852BS_TX_PPDU_CNT_SETTLE_US   1000u

/* R_AX_MACTX_DBG_SEL_CNT, the transmit MAC's own MPDU and DMA counters, read
 * by the original mac_tx_status_dump().  On this chip they need no index
 * selection: the 8852B dump path prints the word exactly as it reads it, so
 * one read yields both counters and nothing is written.  That makes this an
 * independent transmit side witness, which R_AX_TX_PPDU_CNT with its indexed
 * read is not.
 */

#define K1_RTL8852BS_MACTX_DBG_SEL_CNT       0xca20u
#define K1_RTL8852BS_MACTX_MPDU_CNT_SHIFT    24u
#define K1_RTL8852BS_MACTX_DMA_CNT_SHIFT     16u
#define K1_RTL8852BS_MACTX_CNT_MASK          0xffu

/* The registers mac_tx_status_dump() reads when a queued frame does not reach
 * the air, in its own order: the scheduler per queue contention transmit
 * enable, the protocol common setting that holds the transmit mode, the
 * per-MACID sleep, pause and drop words of the dispatcher and of CMAC, the
 * MAC loopback switch, and the response transmit clear channel abort counter.
 * All of them are read here and none is written.
 */

#define K1_RTL8852BS_CTN_TXEN                0xc348u
#define K1_RTL8852BS_CTN_TXEN_MGQ            0x00000100u
#define K1_RTL8852BS_CTN_TXEN_CPUMGQ         0x00000400u
#define K1_RTL8852BS_PTCL_COMMON_SETTING_0   0xc600u
#define K1_RTL8852BS_MACID_SLEEP_0           0xc2c0u
#define K1_RTL8852BS_SS_MACID_PAUSE_0        0x9eb0u
#define K1_RTL8852BS_CMAC_MACID_DROP_0       0xc2e0u
#define K1_RTL8852BS_DMAC_MACID_DROP_0       0x8840u
#define K1_RTL8852BS_MAC_LOOPBACK_STATE      0xcc20u
#define K1_RTL8852BS_RESP_TX_CCA_ABORT_CNT   0xcc18u

/* The medium access gating the vendor programs and this component so far did
 * not.  cca_ctrl_init() of trxcfg.c decides, per transmit stage, which busy
 * indications may hold a frame back; nav_ctrl_init() decides how long a
 * received frame may reserve the medium.  Both were still at their hardware
 * reset value here, and the reset value of R_AX_CCA_CONTROL is exactly the
 * union of the bits that function sets with the bits it clears, which is the
 * proof that nothing had written it.
 *
 * The one bit on the contention path that the vendor clears and reset leaves
 * set is B_AX_CTN_CHK_TXNAV: with it set, a transmit NAV that the hardware
 * recorded for itself keeps holding back every contention winner, and with
 * B_AX_WMAC_NAV_UPPER_EN clear in the NAV control word no NAV of any kind is
 * bounded, so such a hold never expires.  That is the shape a scan offload
 * with thirteen firmware transmits, no firmware failure and no answer takes.
 *
 * The remaining three are the scheduler leftovers of scheduler_init(): the
 * SIFS-to-MACTXEN time, the TSF advance for a port reset, and the beacon path
 * EDCA parameters, written as the upper half of the beacon queue parameter
 * word because set_hw_edca_param() writes that path sixteen bits wide at
 * R_AX_EDCA_BCNQ_PARAM + 2.  R_AX_PREBKF_CFG_0 is deliberately absent: the
 * vendor writes it only for PCIe in NIC mode, so on SDIO its reset value is
 * correct and it is read for evidence only.
 */

#define K1_RTL8852BS_CCA_CONTROL_MASK        0xff3f01ffu
#define K1_RTL8852BS_CCA_CONTROL_VALUE       0x712100ffu
#define K1_RTL8852BS_CCA_CONTROL_2           0xc394u
#define K1_RTL8852BS_WMAC_NAV_CTL_MASK       0x0403ff00u
#define K1_RTL8852BS_WMAC_NAV_CTL_VALUE      0x0403c400u
#define K1_RTL8852BS_PREBKF_CFG_0            0xc338u
#define K1_RTL8852BS_PREBKF_CFG_1            0xc33cu
#define K1_RTL8852BS_SIFS_MACTXEN_T1_MASK    0x0000007fu
#define K1_RTL8852BS_SIFS_MACTXEN_T1_VALUE   0x00000047u
#define K1_RTL8852BS_SCH_EXT_CTRL            0xc3fcu
#define K1_RTL8852BS_PORT_RST_TSF_ADV        0x00000002u
#define K1_RTL8852BS_EDCA_MGQ_PARAM          0xc320u
#define K1_RTL8852BS_EDCA_BCNQ_PARAM         0xc324u
#define K1_RTL8852BS_EDCA_BCNQ_UPPER_MASK    0xffff0000u
#define K1_RTL8852BS_EDCA_BCNQ_UPPER_VALUE   0x32190000u
#define K1_RTL8852BS_RSP_CHK_SIG             0xcc00u

/* The observation side of the same increment.  Everything below is read only.
 *
 * The dispatcher queue state is not directly visible in a register: it is read
 * through the debug function interface of dle_dfi_ctrl_8852b(), a control word
 * that takes a target and an address and self clears bit 31 when the data word
 * holds the answer.  Target 7 is the queue empty bitmap, and in the bitmap of
 * WDE group 4 a set bit means empty, so B_CMAC0_CPUMGQ clear is a frame that
 * the firmware handed to the CPU management queue and that never left it.
 * Target 1 is the per quota page use count, and target 0 address 1 is the
 * public free page count, which is the positive control for the port itself:
 * a plausible non-zero page count proves the interface answers at all.
 *
 * The protocol and scheduler debug ports are the vendor's own transmit stuck
 * instruments from tx_flow_ptcl_dbg_port_8852b() and
 * tx_flow_sch_dbg_port_8852b(): an eight bit selector written into the low
 * byte of the select register, a short delay, then a thirty two bit read of
 * the data register.  Scheduler selector 7 is the transmit NAV abort state,
 * which is the direct witness for the B_AX_CTN_CHK_TXNAV hypothesis, and
 * protocol selectors 0, 1 and 0x10 are the two transmit state machines and the
 * physical layer handshake.
 */

#define K1_RTL8852BS_DLE_EMPTY0              0x8430u
#define K1_RTL8852BS_DLE_EMPTY1              0x8434u
#define K1_RTL8852BS_WDE_DFI_CTRL            0x8d10u
#define K1_RTL8852BS_WDE_DFI_DATA            0x8d14u
#define K1_RTL8852BS_PLE_DFI_CTRL            0x9110u
#define K1_RTL8852BS_PLE_DFI_DATA            0x9114u
#define K1_RTL8852BS_DFI_ACTIVE              0x80000000u
#define K1_RTL8852BS_DFI_TARGET_SHIFT        16
#define K1_RTL8852BS_DFI_TARGET_MASK         0x000f0000u
#define K1_RTL8852BS_DFI_ADDRESS_MASK        0x0000ffffu
#define K1_RTL8852BS_DFI_TYPE_FREEPG         0u
#define K1_RTL8852BS_DFI_TYPE_QUOTA          1u
#define K1_RTL8852BS_DFI_TYPE_QEMPTY         7u
#define K1_RTL8852BS_DFI_FREEPG_PUBNUM       1u
#define K1_RTL8852BS_DFI_PUB_PGNUM_MASK      0x00001fffu
#define K1_RTL8852BS_DFI_USE_PGNUM_SHIFT     16
#define K1_RTL8852BS_DFI_USE_PGNUM_MASK      0x0fff0000u
#define K1_RTL8852BS_DFI_QEMPTY_MGQ_GROUP    4u
#define K1_RTL8852BS_DFI_QEMPTY_CMAC0_MGQ    0x00000004u
#define K1_RTL8852BS_DFI_QEMPTY_CMAC0_NOPS   0x00000008u
#define K1_RTL8852BS_DFI_QEMPTY_CMAC0_CPUMGQ 0x00000010u
#define K1_RTL8852BS_WDE_QTAID_WLAN_CPU      1u
#define K1_RTL8852BS_PLE_QTAID_B0_TXPL       0u
#define K1_RTL8852BS_PLE_QTAID_WLAN_CPU      4u
#define K1_RTL8852BS_DFI_POLL_COUNT          1000u
#define K1_RTL8852BS_PTCL_DBG_SEL            0xc6f4u
#define K1_RTL8852BS_PTCL_DBG_INFO           0xc6f0u
#define K1_RTL8852BS_PTCL_DBG_ENABLE         0x00000100u
#define K1_RTL8852BS_PTCL_DBG_SEL_FSM_0      0x00u
#define K1_RTL8852BS_PTCL_DBG_SEL_FSM_1      0x01u
#define K1_RTL8852BS_PTCL_DBG_SEL_PHY_DBG    0x10u
#define K1_RTL8852BS_SCH_DBG_SEL             0xc3f4u
#define K1_RTL8852BS_SCH_DBG_INFO            0xc3f8u
#define K1_RTL8852BS_SCH_DBG_ENABLE          0x00010000u
#define K1_RTL8852BS_SCH_DBG_SEL_PREBKF_1    0x03u
#define K1_RTL8852BS_SCH_DBG_SEL_TX_NAV_ABT  0x07u
#define K1_RTL8852BS_DBG_PORT_DELAY_US       10
#define K1_RTL8852BS_PTCL_TX_CTN_SEL         0xc6ecu
#define K1_RTL8852BS_PTCL_TX_ON_STAT         0x00000080u
#define K1_RTL8852BS_PTCL_DROP               0x00000020u
#define K1_RTL8852BS_PTCL_QUEUE_IDX_MASK     0x0000001fu
#define K1_RTL8852BS_RESP_TX_NAV_ABORT_CNT   0xcc14u
#define K1_RTL8852BS_TRXPTCL_RESP_TX_ABT_CNT 0xcc1cu

/* Every error status register the original reads and this port never has.
 * mac_dump_err_status() walks the dispatcher, queue engine, packet in,
 * release, protocol and scheduler interrupt status words after a fault, and
 * dmac_err_dump()/cmac_err_dump() read them one block at a time; the interrupt
 * mask registers this port already programmes during initialisation are what
 * make the status bits latch, so a fault that happened during a sweep is still
 * named here afterwards.  Unlike a counter, a set bit is the part naming its
 * own failure: R_AX_WDE_ERR_ISR alone distinguishes a bad destination queue
 * identifier, a bad source queue identifier, an unknown command type, a packet
 * count overflow and an unavailable buffer request from one another.  Nothing
 * here writes them: these are write one to clear registers, and clearing them
 * would destroy exactly the history a later phase has to explain.
 */

#define K1_RTL8852BS_SER_DBG_INFO            0x8424u
#define K1_RTL8852BS_DMAC_ERR_ISR            0x8524u
#define K1_RTL8852BS_DISP_OTHER_ERR_ISR      0x8804u
#define K1_RTL8852BS_DISP_HOST_ERR_ISR       0x8808u
#define K1_RTL8852BS_DISP_CPU_ERR_ISR        0x880cu
#define K1_RTL8852BS_WDE_ERR_ISR             0x8c3cu
#define K1_RTL8852BS_PLE_ERR_FLAG_ISR        0x903cu
#define K1_RTL8852BS_WDRLS_ERR_ISR           0x9434u
#define K1_RTL8852BS_CPUIO_ERR_ISR           0x9844u
#define K1_RTL8852BS_PKTIN_ERR_ISR           0x9a24u
#define K1_RTL8852BS_MPDU_TX_ERR_ISR         0x9bf0u
#define K1_RTL8852BS_MPDU_RX_ERR_ISR         0x9cf0u
#define K1_RTL8852BS_STA_SCH_ERR_ISR         0x9ef4u
#define K1_RTL8852BS_TXPKTCTL_ERR_ISR        0x9f1cu
#define K1_RTL8852BS_CMAC_ERR_ISR            0xc164u

/* The error status words the command and management block and the baseband
 * report blocks own, the other half of the walk mac_dump_err_status() makes.
 * cmac_err_dump() reads the scheduler, the transmit and the receive protocol
 * blocks and the physical layer information block separately from
 * R_AX_CMAC_ERR_ISR, and the scheduler word is the one that names a sorting
 * engine which is not idle and a state machine timeout.  Those are the faults
 * a frame that entered a queue and never left a scheduler would raise, and no
 * register this port reads today would name them.  The two top level mask
 * registers are read once beside them, because a status word that reads zero
 * only rules a fault out while the mask that lets it latch is enabled.
 */

#define K1_RTL8852BS_DMAC_ERR_IMR            0x8520u
#define K1_RTL8852BS_BBRPT_COM_ERR_ISR       0x960cu
#define K1_RTL8852BS_BBRPT_CHINFO_ERR_ISR    0x962cu
#define K1_RTL8852BS_BBRPT_DFS_ERR_ISR       0x963cu
#define K1_RTL8852BS_CMAC_ERR_IMR            0xc160u
#define K1_RTL8852BS_SCHEDULE_ERR_ISR        0xc3ecu
#define K1_RTL8852BS_TMAC_ERR_ISR            0xccecu
#define K1_RTL8852BS_PHYINFO_ERR_ISR         0xccfcu
#define K1_RTL8852BS_RMAC_ERR_ISR            0xcef4u

/* The remaining quota identifiers the dispatcher debug interface answers for.
 * The witness already reads the wireless CPU quota of both engines and the
 * band zero payload quota; the packet in and host to chip identifiers are the
 * ones R_AX_DLE_EMPTY0 names in the two bits that separate an active sweep
 * from a passive one, so a page count for them turns a single empty bit into a
 * number of pages that are held.
 */

#define K1_RTL8852BS_WDE_QTAID_HOST_IF       0u
#define K1_RTL8852BS_WDE_QTAID_PKTIN         3u
#define K1_RTL8852BS_PLE_QTAID_H2C           3u
#define K1_RTL8852BS_WDE_QTAID_DATA_CPU      2u
#define K1_RTL8852BS_WDE_QTAID_CPUIO         4u
#define K1_RTL8852BS_WDE_QTAID_COUNT         5u
#define K1_RTL8852BS_PLE_QTAID_COUNT         12u

/* The queue link table of both dispatcher engines, DLE_DFI_TYPE_QLNKTBL.  Its
 * address is the queue index shifted left by one with the information selector
 * in the low bit, the encoding dle.h gives: selector one answers the packet
 * count in bits eleven to zero, the tail packet identifier in bits twenty
 * three to twelve and the low eight bits of the head packet identifier above
 * them, and selector zero answers the four head bits that do not fit.  This is
 * what an empty word cannot say.  R_AX_DLE_EMPTY0 and the group empty words
 * name a queue group that holds something; the link table turns that into a
 * queue index and a number of frames, which is the difference between knowing
 * a frame is stuck somewhere in the queue engine and knowing which queue holds
 * it.
 */

#define K1_RTL8852BS_DFI_TYPE_QLNKTBL        6u
#define K1_RTL8852BS_QLNKTBL_PKT_CNT_MASK    0x00000fffu
#define K1_RTL8852BS_QLNKTBL_TAIL_MASK       0x00fff000u
#define K1_RTL8852BS_QLNKTBL_TAIL_SHIFT      12
#define K1_RTL8852BS_QLNKTBL_HEAD_MASK       0xff000000u
#define K1_RTL8852BS_QLNKTBL_HEAD_SHIFT      24
#define K1_RTL8852BS_QLNKTBL_HEAD_HIGH_MASK  0x0000000fu
#define K1_RTL8852BS_QEMPTY_GROUP_BITS       32u
#define K1_RTL8852BS_WDE_QEMPTY_GROUPS       5u
#define K1_RTL8852BS_PLE_QEMPTY_GROUPS       4u
#define K1_RTL8852BS_QLNKTBL_MAX_PROBE       40u
#define K1_RTL8852BS_DFI_FREEPG_INDEX        0u

/* The debug ports print_dbg_port() walks and the selector field of each one.
 * A port is a select register and a data register: the protocol, scheduler,
 * transmit protocol and transmit information ports take a small selector field
 * inside their select register, the two direct memory access ports place
 * theirs high inside a control register whose other bits have to survive the
 * write, and the two dispatcher engines, the release block and the transmit
 * packet control block take one selector per output half, which is why their
 * selector values repeat the same byte or the same halfword twice.  Every one
 * of these is a state machine on the stretch between the queue engine and the
 * command block, which is the stretch this port has had no way to see: the
 * witness samples three of the sixty four protocol selectors and five of the
 * forty eight scheduler ones and nothing else.
 */

#define K1_RTL8852BS_DBG_PORT_SEL            0x00c0u
#define K1_RTL8852BS_WDE_DBG_CTL             0x8d18u
#define K1_RTL8852BS_WDE_DBG_OUT             0x8d1cu
#define K1_RTL8852BS_PLE_DBG_CTL             0x9118u
#define K1_RTL8852BS_PLE_DBG_OUT             0x911cu
#define K1_RTL8852BS_WDRLS_DBG_CTL           0x9438u
#define K1_RTL8852BS_WDRLS_DBG_OUT           0x943cu
#define K1_RTL8852BS_TXPKT_DBG_CTL           0x9f38u
#define K1_RTL8852BS_TXPKT_DBG_OUT           0x9f3cu
#define K1_RTL8852BS_RXDMA_DBG_SEL_SHIFT     25
#define K1_RTL8852BS_RXDMA_DBG_SEL_MASK      0x0000003fu
#define K1_RTL8852BS_TXDMA_DBG_SEL           0xc840u
#define K1_RTL8852BS_TXDMA_DBG_SEL_SHIFT     27
#define K1_RTL8852BS_TXDMA_DBG_SEL_MASK      0x0000001fu
#define K1_RTL8852BS_MACTX_DBG_SEL_MASK      0x0000003fu
#define K1_RTL8852BS_WMAC_TX_CTRL_DBG        0xcae4u
#define K1_RTL8852BS_WMAC_TX_CTRL_SEL_MASK   0x0000000fu
#define K1_RTL8852BS_WMAC_TX_INFO0_DBG       0xcae8u
#define K1_RTL8852BS_WMAC_TX_INFO1_DBG       0xcaecu
#define K1_RTL8852BS_WMAC_TX_TF_INFO_0       0xccd0u
#define K1_RTL8852BS_WMAC_TX_TF_SEL_MASK     0x00000007u
#define K1_RTL8852BS_WMAC_TX_TF_INFO_1       0xccd4u
#define K1_RTL8852BS_WMAC_TX_TF_INFO_2       0xccd8u
#define K1_RTL8852BS_TRXPTCL_DBG_SEL         0xccf4u
#define K1_RTL8852BS_DBG_SEL_BYTE_MASK       0x000000ffu
#define K1_RTL8852BS_DBG_SEL_WORD_MASK       0x0000ffffu
#define K1_RTL8852BS_DBG_SEL_LONG_MASK       0xffffffffu
#define K1_RTL8852BS_DBG_PORT_PER_LINE       8u

/* Host side A-die RF register read window, the register pair behind the
 * original halbb_read_rf_reg_8852b_a().  These are baseband registers: on this
 * AX series chip the baseband control registers live at their own number plus
 * bb0_cr_offset, which halbb_init.c sets to 0x10000, inside the same indirect
 * address space as the MAC registers, so the existing 32 bit accessors reach
 * them once that offset is added.  0x378 takes the path in bits 10:8 and
 * the RF offset in bits 7:0, and the serial interface reports its state and
 * returns the 20 bit RF value in 0x174c.  Nothing here writes an RF register:
 * the firmware owns every RF write in this component through CMD_OFLD.
 */

#define K1_RTL8852BS_BB_CR_OFFSET            0x00010000u
#define K1_RTL8852BS_RF_READ_ADDRESS         0x0378u
#define K1_RTL8852BS_RF_READ_SELECT_MASK     0x000007ffu
#define K1_RTL8852BS_RF_READ_STATUS          0x174cu
#define K1_RTL8852BS_RF_READ_WRITE_BUSY      (1u << 24)
#define K1_RTL8852BS_RF_READ_READ_BUSY       (1u << 25)
#define K1_RTL8852BS_RF_READ_DONE            (1u << 26)
#define K1_RTL8852BS_RF_READ_VALUE_MASK      0x000fffffu
#define K1_RTL8852BS_RF_READ_POLL_COUNT      500
#define K1_RTL8852BS_RF_READ_POLL_USEC       5

/* Host side A-die RF register write window, the register pair behind the
 * original halbb_write_rf_reg_8852b_a().  0x370 takes one command word,
 * a mask enable in bit 31, the path in bits 30:28, the RF offset in bits
 * 27:20 and the 20 bit value in bits 19:0, and the original loops on reading
 * it back until it reads what was written.  0x374 carries the bit mask and is
 * only used for a partial write; a full 20 bit write leaves the mask enable
 * clear and touches 0x374 not at all.  Both are baseband registers, so they
 * need the same bb0_cr_offset the read window needs.
 *
 * The only writes this component issues through this window are the two the
 * access diagnostic below makes, each writing the value the radio image of
 * that path already wrote to that same register, so nothing new is ever
 * programmed into the radio from here.
 */

#define K1_RTL8852BS_RF_WRITE_ADDRESS        0x0370u
#define K1_RTL8852BS_RF_WRITE_MASK_ADDRESS   0x0374u
#define K1_RTL8852BS_RF_WRITE_MASK_ENABLE    (1u << 31)
#define K1_RTL8852BS_RF_WRITE_PATH_SHIFT     28u
#define K1_RTL8852BS_RF_WRITE_OFFSET_SHIFT   20u
#define K1_RTL8852BS_RF_WRITE_POLL_COUNT     500
#define K1_RTL8852BS_RF_WRITE_POLL_USEC      5

/* The RF registers this component reads back: the mode word, the register the
 * calibrations save and restore, the channel word whose low byte is the tuned
 * channel number, and the RC calibration trigger and its done bit.
 */

#define K1_RTL8852BS_RF_REG_MODE             0x00u
#define K1_RTL8852BS_RF_REG_MODE_SAVE        0x05u
#define K1_RTL8852BS_RF_REG_CHANNEL          0x18u
#define K1_RTL8852BS_RF_REG_RCK_TRIGGER      0x1bu
#define K1_RTL8852BS_RF_REG_RCK_STATUS       0x1cu
#define K1_RTL8852BS_RF_CHANNEL_MASK         0x000000ffu
#define K1_RTL8852BS_RF_PATHS                2

/* The radio register the read back diagnostic probes with a known value.  The
 * radio image of each path writes RF 0x5a exactly once and nothing else in
 * this component touches it, and the value differs per path, so a firmware
 * side compare against it says whether the radio holds what the images wrote
 * and whether the path routing is right.  The impossible value is the control
 * of the compare itself: a compare that agrees with a value no register can
 * hold would mean the compare proves nothing.
 */

#define K1_RTL8852BS_RF_REG_STATIC           0x5au
#define K1_RTL8852BS_RF_STATIC_VALUE_A       0x7ffffu
#define K1_RTL8852BS_RF_STATIC_VALUE_B       0x7f000u
#define K1_RTL8852BS_RF_IMPOSSIBLE_VALUE     0xfffffu
#define K1_RTL8852BS_RF_PROBES_PER_PATH      4u

/* halrf_si_reset_8852b(), the serial interface reset in halrf_dm_init().  The
 * vendor comment on the 1000 us delay that follows it in halrf_si_reset() is
 * "wa for S0 RCK val = 0 issue": without this reset the radio reads back as
 * zero, which is what this board reports for every RF register on both paths.
 * 0x1200 and 0x3200 carry the hardware serial interface trigger of paths A
 * and B in bits 30:28, 0x12ac and 0x32ac the D die interface enable in bit 0,
 * and bits 0xc0 of crystal interface registers 0x80 and 0x81 the A die
 * interface.  halrf reaches baseband registers through RF_OFST 0x10000, the
 * same offset halbb uses, so these go through the baseband window.
 */

#define K1_RTL8852BS_RF_HWSI_CTRL_A          0x1200u
#define K1_RTL8852BS_RF_HWSI_CTRL_B          0x3200u
#define K1_RTL8852BS_RF_HWSI_TRIGGER_MASK    0x70000000u
#define K1_RTL8852BS_RF_HWSI_TRIGGER_OFF     0x70000000u
#define K1_RTL8852BS_RF_DDIE_SI_CTRL_A       0x12acu
#define K1_RTL8852BS_RF_DDIE_SI_CTRL_B       0x32acu
#define K1_RTL8852BS_RF_DDIE_SI_ENABLE       (1u << 0)
#define K1_RTL8852BS_RF_ADIE_SI_MASK         0xc0u
#define K1_RTL8852BS_RF_SI_SETTLE_USEC       1
#define K1_RTL8852BS_RF_SI_RESET_USEC        1000

/* Sequence window for the firmware side read back.  A single
 * RTW_MAC_COMPARE_OFLD entry with source RF asks the firmware itself to read
 * one radio register and compare it, and c2h_cmd_ofld_rsp_hdl() answers a
 * mismatch with the offset, the expected value and the value the firmware
 * read, so a compare against the value this host measured is a second,
 * independent reading that does not depend on the host reaching the serial
 * interface at all.  It writes nothing.  0x30 is outside the BB reset, PHY CR
 * and radio A/B sequence windows.
 */

#define K1_RTL8852BS_RF_PROBE_SEQUENCE_BASE  0x30u

/* Sequence window for the firmware side access probe below.  The compare
 * probes take 0x30..0x37, eight sequences, so the three firmware
 * transactions each path of the access probe needs fit in 0x38..0x3d and
 * still leave 0x3e and 0x3f unused.
 */

#define K1_RTL8852BS_RF_ACCESS_SEQUENCE_BASE 0x38u
#define K1_RTL8852BS_RF_ACCESS_PROBES_PER_PATH 3u

/* Number of entries in the PHY CR sentinel table below.  The read back
 * diagnostic keeps one sample per sentinel.
 */

#define K1_RTL8852BS_PHY_CR_SENTINELS        5u
#define K1_RTL8852BS_MPDU_PROC_STATIC_MASK   0x00000003u
#define K1_RTL8852BS_MPDU_PROC_STATIC_VALUE  0x00000003u
#define K1_RTL8852BS_ACTION_FWD0_VALUE       0x02a95a95u
#define K1_RTL8852BS_TF_FWD_VALUE            0x0000aa55u
#define K1_RTL8852BS_CUT_AMSDU_CTRL_VALUE    0x010e05f0u
#define K1_RTL8852BS_TXSC_MASK               0x00000fffu
#define K1_RTL8852BS_RRSR_RATE_MASK          0x00000f00u
#define K1_RTL8852BS_RRSR_RATE_VALUE         0x00000300u
/* R_AX_PTCL_COMMON_SETTING_0 bits 0-4, from ptcl_init()'s band-0 branch:
 * CMAC_TX_MODE_0 BIT(0) and CMAC_TX_MODE_1 BIT(1) are set, and
 * PTCL_TRIGGER_SS_EN_0 BIT(2), _1 BIT(3) and _UL BIT(4) are cleared.  The
 * two mode bits are what puts the protocol engine in the transmit mode that
 * serves an ordinary transmit request; with them clear and the three trigger
 * bits set, the engine waits for a trigger frame that never comes, so a
 * frame is accepted, never transmitted, and never counted as a failure
 * either, because the lifetime bits of this same register are also off.
 * This value used to be 0x1c, which is the clear mask rather than the set
 * mask, and that is what board run 3 measured: the firmware reported a
 * successful transmit on all 13 channels while every TMAC transmit PPDU
 * counter stayed at zero.
 */

#define K1_RTL8852BS_PTCL_MODE_MASK          0x0000001fu
#define K1_RTL8852BS_PTCL_MODE_VALUE         0x00000003u
#define K1_RTL8852BS_SW_PREFER_AC_MASK       0x00000003u
#define K1_RTL8852BS_SPE_RPT_PATH_MASK       0x00000030u
#define K1_RTL8852BS_SPE_RPT_PATH_WLCPU      0x00000010u
#define K1_RTL8852BS_RXDMA_FULL_MODE_MASK    0x0000003fu
#define K1_RTL8852BS_TCR_UDF_THRESHOLD_MASK  0x007f0000u
#define K1_RTL8852BS_TCR_UDF_THRESHOLD_VALUE 0x00060000u
#define K1_RTL8852BS_TXDFIFO_MCS_MASK        0x0000ff00u
#define K1_RTL8852BS_TXDFIFO_MCS_VALUE       0x00007700u
#define K1_RTL8852BS_SIFS_MASK               0x0000ffffu
#define K1_RTL8852BS_SIFS_VALUE              0x0000110au
#define K1_RTL8852BS_RXTRIG_FCSCHK           0x00100000u
#define K1_RTL8852BS_RCR_CHANNEL_MASK        0x0000000fu
#define K1_RTL8852BS_RCR_CHANNEL_VALUE       0x0000000fu
#define K1_RTL8852BS_DLK_PROTECT_MASK        0xfff20000u
#define K1_RTL8852BS_DLK_PROTECT_VALUE       0x20f20000u

/* Spatial reuse shares the 32-bit word at 0xce48 with the MACID match
 * control, so the two fields the vendor writes as byte accesses are applied
 * as one masked update: clear B_AX_SR_EN and B_AX_SR_CTRL_PLCP_EN in
 * R_AX_RX_SR_CTRL (0xce4a) and set B_AX_PLCP_SRC_EN in
 * R_AX_BSSID_SRC_CTRL (0xce4b).  B_AX_SRG_CHK_EN, B_AX_SR_OP_MODE and the
 * BSSID/BSS-colour/partial-AID match bits are left as the vendor leaves
 * them, and the MACID match bytes below are untouched.
 */

#define K1_RTL8852BS_SPATIAL_REUSE_MASK      0x01030000u
#define K1_RTL8852BS_SPATIAL_REUSE_VALUE     0x01000000u
#define K1_RTL8852BS_RX_MPDU_MAX_MASK        0x003f0000u
#define K1_RTL8852BS_RX_MPDU_MAX_VALUE       0x00170000u
#define K1_RTL8852BS_VHT_SIGB_CRC_CHECK      0x00000010u
#define K1_RTL8852BS_RESPBA_SSN_SELECT       0x00000004u
/* R_AX_MAC_LOOPBACK and R_AX_MAC_LOOPBACK_COUNT, from the original
 * mac_reg_ax.h.  B_AX_MACLBK_EN is a single bit the original's tmac_init sets
 * whenever its transmit mode is MAC_AX_TRX_LOOPBACK and clears otherwise; it
 * has no queue, DLE or baseband prerequisite, and the rest of the register
 * keeps the ready-period, PLCP-delay and ready-number defaults the part comes
 * up with.  The count register exposes a read-only sixteen-bit count of looped
 * PPDUs at bit 16 and a write-one clear at bit 0, which is what turns the
 * loopback into a transmit positive control that needs no radio.
 */

#define K1_RTL8852BS_MAC_LOOPBACK_EN         0x00000001u
#define K1_RTL8852BS_MAC_LOOPBACK_COUNT_CLR  0x00000001u
#define K1_RTL8852BS_MAC_LOOPBACK_COUNT_SHIFT 16u
#define K1_RTL8852BS_MAC_LOOPBACK_COUNT_MASK 0xffffu

#define K1_RTL8852BS_SCAN_RX_FLTR_OPT_MASK   0x000000beu
#define K1_RTL8852BS_SCAN_RX_FLTR_OPT_VALUE  0x0000000eu
#define K1_RTL8852BS_SCAN_MGNT_FLTR_TO_HOST  0x55555555u
#define K1_RTL8852BS_PPDU_STAT_RPT_EN        0x00000001u
#define K1_RTL8852BS_ADDR_CAM_RANGE           (0x7fu << 16)
#define K1_RTL8852BS_ADDR_CAM_CLEAR           (1u << 8)
#define K1_RTL8852BS_ADDR_CAM_ENABLE          (1u << 0)
#define K1_RTL8852BS_ADDR_CAM_INIT_POLL_COUNT 100u
#define K1_RTL8852BS_ADDR_CAM_INIT_POLL_USEC  10u
#define K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE 24u
#define K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE \
  (K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + \
   K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE)
#define K1_RTL8852BS_H2C_LOOPBACK_TRANSFER_SIZE \
  (((K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + \
     K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_H2C_LOOPBACK_RESPONSE_POLL_COUNT 100u
#define K1_RTL8852BS_H2C_LOOPBACK_RESPONSE_POLL_MSEC 1u
#define K1_RTL8852BS_H2C_LOOPBACK_RXFIFO       0x1f00u
#define K1_RTL8852BS_H2C_LOOPBACK_RX_MAX       512u
#define K1_RTL8852BS_H2C_LOOPBACK_RXDESC_SIZE  16u
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_LEN_MASK 0x00003fffu
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_SHIFT    14u
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_SHIFT_MASK 0x3u
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_TYPE_SHIFT 24u
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_TYPE_MASK 0x0fu
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE 10u
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_DRVINFO_SHIFT 28u
#define K1_RTL8852BS_H2C_LOOPBACK_RXD_DRVINFO_MASK 0x7u
#define K1_RTL8852BS_RXDESC_SHORT_SIZE          16u
#define K1_RTL8852BS_RXDESC_LONG_SIZE           32u
#define K1_RTL8852BS_RXDESC_LENGTH_MASK         0x00003fffu
#define K1_RTL8852BS_RXDESC_SHIFT               14u
#define K1_RTL8852BS_RXDESC_SHIFT_MASK          0x3u
#define K1_RTL8852BS_RXDESC_PACKET_TYPE_SHIFT   24u
#define K1_RTL8852BS_RXDESC_PACKET_TYPE_MASK    0x0fu
#define K1_RTL8852BS_RXDESC_PACKET_TYPE_WIFI    0u
#define K1_RTL8852BS_RXDESC_DRIVER_INFO_SHIFT   28u
#define K1_RTL8852BS_RXDESC_DRIVER_INFO_MASK    0x7u
#define K1_RTL8852BS_RXDESC_LONG                (1u << 31)
#define K1_RTL8852BS_RXDESC_CRC_ERROR           (1u << 9)
#define K1_RTL8852BS_RXDESC_ICV_ERROR           (1u << 10)
#define K1_RTL8852BS_IEEE80211_TYPE_MASK        0x3u
#define K1_RTL8852BS_IEEE80211_SUBTYPE_SHIFT    4u
#define K1_RTL8852BS_IEEE80211_SUBTYPE_MASK     0xfu
#define K1_RTL8852BS_IEEE80211_TYPE_MANAGEMENT  0u
#define K1_RTL8852BS_IEEE80211_TYPE_DATA        2u
#define K1_RTL8852BS_IEEE80211_SUBTYPE_PROBE_REQUEST 4u
#define K1_RTL8852BS_IEEE80211_SUBTYPE_PROBE_RESPONSE 5u
#define K1_RTL8852BS_IEEE80211_SUBTYPE_BEACON   8u
#define K1_RTL8852BS_IEEE80211_SUBTYPE_AUTHENTICATION 11u
#define K1_RTL8852BS_IEEE80211_HEADER_SIZE      24u
#define K1_RTL8852BS_IEEE80211_BEACON_FIXED_SIZE 12u
#define K1_RTL8852BS_IEEE80211_SSID_IE          0u
#define K1_RTL8852BS_IEEE80211_CHANNEL_IE       3u
#define K1_RTL8852BS_SCAN_OFLD_C2H_HEADER_SIZE  16u
#define K1_RTL8852BS_SCAN_OFLD_REPORT_SIZE      4u
#define K1_RTL8852BS_RX_AGG_ALIGNMENT           8u
#define K1_RTL8852BS_C2H_HEADER_SIZE             8u
#define K1_RTL8852BS_C2H_CATEGORY_MASK           0x3u
#define K1_RTL8852BS_C2H_CLASS_SHIFT             2u
#define K1_RTL8852BS_C2H_CLASS_MASK              0x3fu
#define K1_RTL8852BS_C2H_FUNCTION_SHIFT          8u
#define K1_RTL8852BS_C2H_FUNCTION_MASK           0xffu
#define K1_RTL8852BS_C2H_DELIVERY_TYPE_SHIFT     16u
#define K1_RTL8852BS_C2H_DELIVERY_TYPE_MASK      0x0fu
#define K1_RTL8852BS_C2H_DELIVERY_TYPE_C2H       1u
#define K1_RTL8852BS_C2H_SEQUENCE_SHIFT          24u
#define K1_RTL8852BS_C2H_SEQUENCE_MASK           0xffu
#define K1_RTL8852BS_C2H_TOTAL_LENGTH_MASK       0x00003fffu
#define K1_RTL8852BS_C2H_RECEIVE_ACK             (1u << 14)
#define K1_RTL8852BS_C2H_DONE_ACK                (1u << 15)
#define K1_RTL8852BS_H2C_LOOPBACK_H2C_CATEGORY 0u
#define K1_RTL8852BS_H2C_LOOPBACK_H2C_CLASS    0u
#define K1_RTL8852BS_H2C_LOOPBACK_H2C_FUNCTION 0u
#define K1_RTL8852BS_H2C_LOOPBACK_H2C_SEQUENCE 0u
#define K1_RTL8852BS_H2C_LOOPBACK_C2H_CATEGORY 0u
#define K1_RTL8852BS_H2C_LOOPBACK_C2H_CLASS    0u
#define K1_RTL8852BS_H2C_LOOPBACK_C2H_FUNCTION 0u
#define K1_RTL8852BS_H2C_HEADER_CATEGORY_MASK  0x3u
#define K1_RTL8852BS_H2C_HEADER_CLASS_SHIFT    2u
#define K1_RTL8852BS_H2C_HEADER_CLASS_MASK     0x3fu
#define K1_RTL8852BS_H2C_HEADER_FUNCTION_SHIFT 8u
#define K1_RTL8852BS_H2C_HEADER_FUNCTION_MASK  0xffu
#define K1_RTL8852BS_H2C_HEADER_SEQUENCE_SHIFT 24u
#define K1_RTL8852BS_H2C_HEADER_TOTAL_MASK     0x00003fffu
#define K1_RTL8852BS_H2C_HEADER_DONE_ACK        (1u << 15)
#define K1_RTL8852BS_FWROLE_MAINTAIN_SIZE      4u
#define K1_RTL8852BS_JOININFO_SIZE             12u
#define K1_RTL8852BS_ADDR_CAM_SIZE             60u
#define K1_RTL8852BS_MACID_PAUSE_SLEEP_SIZE    256u

/* The 2.4 GHz passive scan channel list (4 + 13 * 28 bytes) is the largest
 * runtime H2C content body, so this shared ceiling follows it instead of the
 * MACID pause/sleep bitmap.  A build check next to the scan-offload sizes
 * keeps the two in step.
 */

#define K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX   384u
#define K1_RTL8852BS_RUNTIME_H2C_PACKET_MAX    \
  (K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + \
   K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX)
#define K1_RTL8852BS_RUNTIME_H2C_TRANSFER_MAX  \
  (((K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + \
     K1_RTL8852BS_RUNTIME_H2C_PACKET_MAX + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_RUNTIME_DONE_ACK_CATEGORY 1u
#define K1_RTL8852BS_RUNTIME_DONE_ACK_CLASS    0u
#define K1_RTL8852BS_RUNTIME_DONE_ACK_FUNCTION 1u
#define K1_RTL8852BS_RUNTIME_DONE_ACK_SIZE     4u
#define K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_COUNT 1000u
#define K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC 1u
#define K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX   512u
#define K1_RTL8852BS_CMD_OFLD_CATEGORY          1u
#define K1_RTL8852BS_CMD_OFLD_H2C_CLASS         9u
#define K1_RTL8852BS_CMD_OFLD_C2H_CLASS         1u
#define K1_RTL8852BS_CMD_OFLD_H2C_FUNCTION      0x13u
#define K1_RTL8852BS_CMD_OFLD_C2H_FUNCTION      0x08u
#define K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE        16u
#define K1_RTL8852BS_CMD_OFLD_BATCH_MAX         256u
#define K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH \
  (K1_RTL8852BS_CMD_OFLD_BATCH_MAX / \
   K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE)
#define K1_RTL8852BS_CMD_OFLD_SEQUENCE_BASE     0x40u
#define K1_RTL8852BS_CMD_OFLD_LAST_COMMAND      (1u << 4)
#define K1_RTL8852BS_CMD_OFLD_COMMAND_SHIFT     8u
#define K1_RTL8852BS_CMD_OFLD_OFFSET_SHIFT      16u
#define K1_RTL8852BS_CMD_OFLD_RESPONSE_SIZE     16u
#define K1_RTL8852BS_CMD_OFLD_RESPONSE_ERROR    (1u << 0)
#define K1_RTL8852BS_CMD_OFLD_RESPONSE_COMMAND_SHIFT 8u
#define K1_RTL8852BS_CMD_OFLD_RESPONSE_COMMAND_MASK  0xffu

/* Firmware I/O-offload source/type/path fields of fwcmd_cmd_ofld dword0.
 * The BB PHY CR path above leaves source, type, and path at zero because a
 * BB write offload is source 0, type 0, path 0.  An RF write needs the
 * explicit RF source and the radio path, so the fields are named here.
 */

#define K1_RTL8852BS_CMD_OFLD_SOURCE_SHIFT      0u
#define K1_RTL8852BS_CMD_OFLD_SOURCE_BB         0u
#define K1_RTL8852BS_CMD_OFLD_SOURCE_RF         1u
#define K1_RTL8852BS_CMD_OFLD_SOURCE_RF_DDIE    3u
#define K1_RTL8852BS_CMD_OFLD_TYPE_SHIFT        2u
#define K1_RTL8852BS_CMD_OFLD_TYPE_WRITE        0u
#define K1_RTL8852BS_CMD_OFLD_TYPE_COMPARE      1u
#define K1_RTL8852BS_CMD_OFLD_PATH_SHIFT        5u
#define K1_RTL8852BS_CMD_OFLD_OFFSET_MASK       0xffffu

/* RTL8852B radio A/B parameter image.  halrf_cfg_rf_radio_{a,b}_8852b()
 * applies every entry through halrf_wrf(..., MASKRF, ...), and halrf_wrf()
 * splits the image in two: an entry carrying bit 16 is an RF D-die register
 * that becomes a BB write at offset_write_rf[path] + ((address & 0xff) << 2),
 * everything else is an RF serial-interface write on the given path.  The
 * 0xf9..0xfe pseudo addresses are vendor delay records; the generated images
 * contain none, and this component refuses an image that does.
 */

#define K1_RTL8852BS_RF_PATH_A                  0u
#define K1_RTL8852BS_RF_PATH_B                  1u
#define K1_RTL8852BS_RF_MASK                    0x000fffffu
#define K1_RTL8852BS_RF_DDIE_FLAG               (1u << 16)
#define K1_RTL8852BS_RF_DDIE_ADDRESS_MASK       0xffu
#define K1_RTL8852BS_RF_DDIE_BB_BASE_A          0xe000u
#define K1_RTL8852BS_RF_DDIE_BB_BASE_B          0xf000u
#define K1_RTL8852BS_RF_SI_ADDRESS_MASK         0xffffu
#define K1_RTL8852BS_RF_DELAY_FIRST             0xf9u
#define K1_RTL8852BS_RF_DELAY_LAST              0xfeu

/* Sequence windows.  The BB PHY CR image occupies 0x40..0x7f, so the two
 * radio images take the next two disjoint windows.
 */

#define K1_RTL8852BS_RF_CR_SEQUENCE_BASE_A      0x80u
#define K1_RTL8852BS_RF_CR_SEQUENCE_BASE_B      0xc0u
#define K1_RTL8852BS_RF_CR_SEQUENCE_MAX         0xffu

/* halbb_reset_bb(), the reset half of rtw_hal_init_bb_reg().  Every write
 * goes through the same firmware I/O offload the PHY CR image uses, because
 * that is what the vendor itself does once dev_cap.io_ofld is set:
 * halbb_reset_bb_phy() then selects halbb_fwofld_bb_reset_8852b() over the
 * host halbb_bb_reset_8852b(), and the offload variant drops the host-only
 * packet-detection toggle (0x2344 BIT31, 0xc3c BIT9) and the phy-sts stop and
 * restart around the reset pulse (0xce40 bit 0) together with its 2 us delay.
 * Both packet-detection bits are already enabled by the generated phy_reg
 * image (0x2344 = 0x0006318a, 0xc3c = 0x2840e1bf), so the shorter sequence
 * leaves nothing to restore.  The whole sequence is 11 entries, well inside
 * one batch, and sequence 0x20 is disjoint from every other window in this
 * component.
 */

#define K1_RTL8852BS_BB_RESET_SEQUENCE_BASE     0x20u

/* The RF init config H2C that closes halrf_dm_init().  It reaches the chip
 * through rtw_hal_mac_send_h2c() -> mac_outsrc_h2c_common(), so the FWCMD
 * header carries category FWCMD_H2C_CAT_OUTSRC, class 0xa and function
 * FWCMD_H2C_RF_INIT_CFG.  Sequence 8 is outside every window above.
 */

#define K1_RTL8852BS_RF_INIT_CFG_CATEGORY       2u
#define K1_RTL8852BS_RF_INIT_CFG_CLASS          0xau
#define K1_RTL8852BS_RF_INIT_CFG_FUNCTION       0xeu
#define K1_RTL8852BS_RF_INIT_CFG_H2C_SEQUENCE   8u
#define K1_RTL8852BS_RF_INIT_CFG_SIZE           4u
#define K1_RTL8852BS_RF_INIT_CFG_POLL_COUNT     50u
#define K1_RTL8852BS_RF_INIT_CFG_POLL_MSEC      2u

/* Headline guard.  Every row of the generated table is one vendor headline
 * in table order; images records which compiled image that headline selects.
 */

#define K1_RTL8852BS_RF_HEADLINE_IMAGE_A        0x1u
#define K1_RTL8852BS_RF_HEADLINE_IMAGE_B        0x2u
#define K1_RTL8852BS_RF_HEADLINE_IMAGE_BOTH     0x3u
#define K1_RTL8852BS_RF_HEADLINE_DONT_CARE      0xffu
#define K1_RTL8852BS_RF_IMAGE_RFE_TYPE          0x01u
#define K1_RTL8852BS_RF_IMAGE_CHIP_CV           0x01u
#define K1_RTL8852BS_ADDR_CAM_LONG_LENGTH      0x40u
#define K1_RTL8852BS_BSSID_CAM_LENGTH          0x08u
#define K1_RTL8852BS_FWROLE_MAINTAIN_CATEGORY  1u
#define K1_RTL8852BS_FWROLE_MAINTAIN_CLASS     8u
#define K1_RTL8852BS_FWROLE_MAINTAIN_FUNCTION  4u
#define K1_RTL8852BS_JOININFO_CATEGORY         1u
#define K1_RTL8852BS_JOININFO_CLASS            8u
#define K1_RTL8852BS_JOININFO_FUNCTION         0u
#define K1_RTL8852BS_ADDR_CAM_CATEGORY          1u
#define K1_RTL8852BS_ADDR_CAM_CLASS             6u
#define K1_RTL8852BS_ADDR_CAM_FUNCTION          0u
#define K1_RTL8852BS_MACID_PAUSE_SLEEP_CATEGORY 1u
#define K1_RTL8852BS_MACID_PAUSE_SLEEP_CLASS    9u
#define K1_RTL8852BS_MACID_PAUSE_SLEEP_FUNCTION 0x28u
#define K1_RTL8852BS_H2C_AGG_CATEGORY            1u
#define K1_RTL8852BS_H2C_AGG_CLASS               9u
#define K1_RTL8852BS_H2C_AGG_FUNCTION            0x15u
#define K1_RTL8852BS_H2C_AGG_SUB_HEADER_SIZE     4u
#define K1_RTL8852BS_H2C_AGG_ALIGNMENT           4u
#define K1_RTL8852BS_H2C_AGG_LOOPBACK_COUNT       2u
#define K1_RTL8852BS_H2C_AGG_LOOPBACK_SEQUENCE    2u
#define K1_RTL8852BS_ROLE_CAM_H2C_SEQUENCE        3u
#define K1_RTL8852BS_SCAN_OFLD_H2C_SEQUENCE       5u
#define K1_RTL8852BS_SCAN_OFLD_CATEGORY           1u
#define K1_RTL8852BS_SCAN_OFLD_CLASS              9u
#define K1_RTL8852BS_SCAN_OFLD_FUNCTION           0x16u
#define K1_RTL8852BS_SCAN_OFLD_START_H2C_SEQUENCE 6u
#define K1_RTL8852BS_SCAN_OFLD_START_FUNCTION     0x17u
#define K1_RTL8852BS_SCAN_OFLD_NEXT_H2C_SEQUENCE  7u
#define K1_RTL8852BS_SCAN_OFLD_NEXT_FUNCTION      0x22u
#define K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE  28u
#define K1_RTL8852BS_SCAN_OFLD_CONTENT_HEADER_SIZE 4u
#define K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT 13u
#define K1_RTL8852BS_SCAN_OFLD_CH1_CONTENT_SIZE   \
  (K1_RTL8852BS_SCAN_OFLD_CONTENT_HEADER_SIZE + \
   K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE)
#define K1_RTL8852BS_SCAN_OFLD_CONTENT_SIZE       \
  (K1_RTL8852BS_SCAN_OFLD_CONTENT_HEADER_SIZE + \
   K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT * \
   K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE)
#if K1_RTL8852BS_SCAN_OFLD_CONTENT_SIZE > K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX
#  error "passive scan channel list exceeds the runtime H2C content ceiling"
#endif
#define K1_RTL8852BS_SCAN_OFLD_START_CONTENT_SIZE  28u
#define K1_RTL8852BS_SCAN_OFLD_NEXT_CONTENT_SIZE   4u
#define K1_RTL8852BS_SCAN_OFLD_PASSIVE_PERIOD_MSEC 250u

/* A dwell only yields frames while the host keeps draining the RX FIFO, so
 * the scan path needs room for a multi-frame aggregate rather than the
 * single-C2H ceiling used by the done-ack helpers.  The bounded BSS table
 * deduplicates by BSSID; the drain limits bound how long the loop keeps
 * reading after the firmware reports scan end.
 */

#define K1_RTL8852BS_SCAN_OFLD_RX_MAX              8192u
#define K1_RTL8852BS_SCAN_OFLD_BSS_MAX             24u
#define K1_RTL8852BS_SCAN_OFLD_DRAIN_POLL_COUNT    256u
#define K1_RTL8852BS_SCAN_OFLD_DRAIN_IDLE_POLLS    32u
#define K1_RTL8852BS_SCAN_OFLD_PASSIVE_POLL_COUNT  16000u
#define K1_RTL8852BS_SCAN_OFLD_FRAME_LOG_MAX       1u
#define K1_RTL8852BS_SCAN_OFLD_PARSE_ERROR_LOG_MAX 8u
#define K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL     1u
#define K1_RTL8852BS_SCAN_OFLD_NOTIFY_ENTER_CHANNEL (1u << 6)
#define K1_RTL8852BS_SCAN_OFLD_PAUSE_TX_DATA        (1u << 5)
#define K1_RTL8852BS_SCAN_OFLD_TX_PKT               (1u << 4)

/* The remaining chinfo notification requests, all in the same byte as the
 * enter-channel one: dwell in bit 3, pre-transmit in bit 4, post-transmit in
 * bit 5 and leave-channel in bit 7.  The pre- and post-transmit ones are the
 * firmware's own account of the probe request it is asked to send, so an
 * active entry asks for them: a pre-transmit notification means the firmware
 * reached the point of transmitting, and a post-transmit notification carries
 * the result in the same status field every other notification uses.
 */

#define K1_RTL8852BS_SCAN_OFLD_NOTIFY_PRE_TX        (1u << 4)
#define K1_RTL8852BS_SCAN_OFLD_NOTIFY_POST_TX       (1u << 5)
#define K1_RTL8852BS_SCAN_OFLD_NOTIFY_LEAVE_CHANNEL (1u << 7)

/* MAC/FW_OFLD/PACKET_OFLD, the original mac_add_pkt_ofld().  The content is
 * one 4-byte struct mac_ax_pkt_ofld_hdr - identifier, three-bit operation and
 * a 16-bit length - followed by the bare 802.11 frame.  The firmware builds
 * the transmit descriptor for an offloaded packet itself, so this path needs
 * no host transmit ring and no WD/TXD page of its own.
 *
 * The identifier is chosen by this host out of the 0-255 offload table.  Only
 * one is ever allocated here and it is reused on every sweep, so nothing has
 * to be freed between sweeps; 0xff is the original PKT_OFLD_NOT_EXISTS_ID and
 * is used here to mean "this channel list carries no probe request".
 */

#define K1_RTL8852BS_PKT_OFLD_CATEGORY              1u
#define K1_RTL8852BS_PKT_OFLD_CLASS                 9u
#define K1_RTL8852BS_PKT_OFLD_FUNCTION              0x1u
#define K1_RTL8852BS_PKT_OFLD_H2C_SEQUENCE          9u
#define K1_RTL8852BS_PKT_OFLD_HEADER_SIZE           4u
#define K1_RTL8852BS_PKT_OFLD_OP_ADD                0u
#define K1_RTL8852BS_PKT_OFLD_OP_SHIFT              8u
#define K1_RTL8852BS_PKT_OFLD_LENGTH_SHIFT          16u
#define K1_RTL8852BS_PKT_OFLD_PROBE_REQUEST_ID      0u
#define K1_RTL8852BS_PKT_OFLD_ID_NONE               0xffu

/* The wildcard Probe Request that is offloaded: broadcast destination and
 * BSSID, the eFuse self MAC as the transmitter address, an empty SSID element
 * and the 802.11b/g/a rate sets.  It deliberately carries no DS Parameter Set
 * element, because the one offloaded frame is transmitted on all thirteen
 * channels and a fixed channel element would contradict twelve of them.
 */

#define K1_RTL8852BS_PROBE_REQUEST_FRAME_CONTROL    0x0040u
#define K1_RTL8852BS_PROBE_REQUEST_SUPPORTED_RATES_IE 1u
#define K1_RTL8852BS_PROBE_REQUEST_EXTENDED_RATES_IE  50u
#define K1_RTL8852BS_PROBE_REQUEST_MAX_SIZE         64u

/* One open-system Authentication Request, IEEE 802.11 clause 9.3.3.11.  The
 * frame control is a management frame of subtype 11 with every flag clear: an
 * Authentication frame is neither to nor from the distribution system, and it
 * is sent unprotected because open-system authentication has nothing to
 * protect it with.  Address 1 is the access point, which makes the frame
 * unicast and therefore acknowledged, so the transmit descriptor must not
 * carry the broadcast/multicast bit the Probe Request needs.
 *
 * The body is the three little-endian 16-bit fields of clause 9.4.1:
 * algorithm number 0 (open system), transaction sequence number 1 for the
 * request, and a status code that is reserved in the request and carries the
 * access point's verdict in the response.  No challenge text element exists
 * in open system, so the frame is exactly thirty bytes.
 */

#define K1_RTL8852BS_AUTH_FRAME_CONTROL             0x00b0u
#define K1_RTL8852BS_AUTH_ALGORITHM_OPEN            0u
#define K1_RTL8852BS_AUTH_SEQUENCE_REQUEST          1u
#define K1_RTL8852BS_AUTH_SEQUENCE_RESPONSE         2u
#define K1_RTL8852BS_AUTH_STATUS_SUCCESS            0u
#define K1_RTL8852BS_AUTH_BODY_SIZE                 6u
#define K1_RTL8852BS_AUTH_FRAME_SIZE                \
  (K1_RTL8852BS_IEEE80211_HEADER_SIZE + K1_RTL8852BS_AUTH_BODY_SIZE)
#define K1_RTL8852BS_SCAN_OFLD_START_OPERATION      1u
#define K1_RTL8852BS_SCAN_OFLD_START_OPERATION_SHIFT 20u
#define K1_RTL8852BS_SCAN_OFLD_START_NOTIFY_END      (1u << 0)
#define K1_RTL8852BS_SCAN_OFLD_C2H_CATEGORY          1u
#define K1_RTL8852BS_SCAN_OFLD_C2H_CLASS             1u
#define K1_RTL8852BS_SCAN_OFLD_C2H_FUNCTION          9u
#define K1_RTL8852BS_SCAN_OFLD_C2H_CONTENT_SIZE      16u
#define K1_RTL8852BS_SCAN_OFLD_C2H_PRIMARY_MASK      0xffu
#define K1_RTL8852BS_SCAN_OFLD_C2H_REASON_SHIFT      16u
#define K1_RTL8852BS_SCAN_OFLD_C2H_REASON_MASK       0x0fu
#define K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_SHIFT      20u
#define K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_MASK       0x0fu
#define K1_RTL8852BS_SCAN_OFLD_C2H_CHANNEL_BAND_SHIFT 24u
#define K1_RTL8852BS_SCAN_OFLD_C2H_CHANNEL_BAND_MASK  0x03u
#define K1_RTL8852BS_SCAN_OFLD_C2H_HARDWARE_BAND      (1u << 26)
#define K1_RTL8852BS_SCAN_OFLD_C2H_PRE_TX             1u
#define K1_RTL8852BS_SCAN_OFLD_C2H_POST_TX            2u
#define K1_RTL8852BS_SCAN_OFLD_C2H_ENTER_CHANNEL      3u
#define K1_RTL8852BS_SCAN_OFLD_C2H_LEAVE_CHANNEL      4u

/* The rest of the notification's second reported dword: the firmware's own
 * count of failed transmissions in its low nibble, and the active-channel flag
 * it sets for a channel whose entry asked for a transmission.  Both are the
 * firmware's view of the request this host serialized, so they say whether the
 * transmit bit and the probe identifier arrived as intended.
 */

#define K1_RTL8852BS_SCAN_OFLD_C2H_TX_FAIL_MASK       0x0fu
#define K1_RTL8852BS_SCAN_OFLD_C2H_ACTIVE_CHANNEL     (1u << 29)
#define K1_RTL8852BS_SCAN_OFLD_C2H_SCAN_END           5u
#define K1_RTL8852BS_SCAN_OFLD_C2H_GET_REPORT         6u
#define K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_FAILURE     2u
#define K1_RTL8852BS_SCAN_OFLD_FRAME_DUMP_BYTES       64u
#define K1_RTL8852BS_MACID_PAUSE_MASK_DWORD     4u
#define K1_RTL8852BS_MACID_SLEEP_MASK_DWORD     12u
#define K1_RTL8852BS_SELF_ROLE_MAX             2u
#define K1_RTL8852BS_WIFI_ROLE_MAX             12u
#define K1_RTL8852BS_UPDATE_MODE_MAX           6u
#define K1_RTL8852BS_PORT_MAX                  4u
#define K1_RTL8852BS_NETWORK_TYPE_MAX          3u
#define K1_RTL8852BS_ADDR_CAM_MASK_SELECTION_MAX 3u
#define K1_RTL8852BS_ADDR_CAM_BSSID_INDEX_MAX  63u
#define K1_RTL8852BS_FW_HEADER_STATIC_SIZE   80u
#define K1_RTL8852BS_FW_HEADER_DYNAMIC_SIZE  80u
#define K1_RTL8852BS_FW_HEADER_SIZE          \
  (K1_RTL8852BS_FW_HEADER_STATIC_SIZE + \
   K1_RTL8852BS_FW_HEADER_DYNAMIC_SIZE)
#define K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET 32u
#define K1_RTL8852BS_FW_SECTION_HEADER_SIZE  16u
#define K1_RTL8852BS_FW_SECTION_MAX_COUNT    6u
#define K1_RTL8852BS_FW_SECTION_SIZE_MASK    0x00ffffffu
#define K1_RTL8852BS_FW_SECTION_TYPE_SHIFT   24u
#define K1_RTL8852BS_FW_SECTION_TYPE_MASK    0x0fu
#define K1_RTL8852BS_FW_SECTION_CHECKSUM     (1u << 28)
#define K1_RTL8852BS_FW_SECTION_CHECKSUM_SIZE 8u
/* array_8852b_u2_nicce is the U2 image with FW_CONFIG_SCAN_OFFLOAD. */

#define K1_RTL8852BS_U2_NICCE_IMAGE_SIZE       341216u
#define K1_RTL8852BS_FW_SECURITY_SECTION_INDEX 2u
#define K1_RTL8852BS_FW_SECURITY_SECTION_TYPE  9u
#define K1_RTL8852BS_FW_SECURITY_SECTION_SIZE  2048u
#define K1_RTL8852BS_FW_SECURITY_ALT_SECTION_SIZE 960u
#define K1_RTL8852BS_FW_SECURITY_SIG_OFFSET    448u
#define K1_RTL8852BS_FW_SECURITY_SIG_SIZE      512u
#define K1_RTL8852BS_FW_LEGACY_MSS_SIG_COUNT   2u
#define K1_RTL8852BS_FW_LEGACY_MSS_TRAILER_SIZE \
  (K1_RTL8852BS_FW_LEGACY_MSS_SIG_COUNT * \
   K1_RTL8852BS_FW_SECURITY_SIG_SIZE)
#define K1_RTL8852BS_OTP_KEY_INFO_COUNT        2u
#define K1_RTL8852BS_FW_HEADER_PACKET_SIZE   \
  (K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + K1_RTL8852BS_FW_HEADER_STATIC_SIZE)
#define K1_RTL8852BS_FW_HEADER_TRANSFER_SIZE \
  (K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + K1_RTL8852BS_FW_HEADER_PACKET_SIZE)
#define K1_RTL8852BS_FW_HEADER_FIFO_ADDRESS 0x0001c00eu
#define K1_RTL8852BS_FW_HEADER_REQUIRED_PAGES 2u
#define K1_RTL8852BS_FW_SECTION0_OFFSET      0x00a0u
#define K1_RTL8852BS_FW_SECTION0_SIZE        2048u
#define K1_RTL8852BS_FW_SECTION0_PACKET_SIZE 2020u
#define K1_RTL8852BS_FW_SECTION0_PACKET_BYTES \
  (K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + K1_RTL8852BS_FW_SECTION0_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_TRANSFER_SIZE \
  (((K1_RTL8852BS_FW_SECTION0_PACKET_BYTES + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_FW_SECTION0_FIFO_ADDRESS 0x0001c100u
#define K1_RTL8852BS_FW_SECTION0_REQUIRED_PAGES 32u
#define K1_RTL8852BS_FW_SECTION0_TAIL_OFFSET \
  (K1_RTL8852BS_FW_SECTION0_OFFSET + K1_RTL8852BS_FW_SECTION0_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_SIZE \
  (K1_RTL8852BS_FW_SECTION0_SIZE - K1_RTL8852BS_FW_SECTION0_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_BYTES \
  (K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + \
   K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_TAIL_TRANSFER_SIZE \
  (((K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_BYTES + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_FW_SECTION0_TAIL_FIFO_ADDRESS 0x0001c007u
#define K1_RTL8852BS_FW_SECTION0_TAIL_REQUIRED_PAGES 2u
#define K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_OFFSET \
  (K1_RTL8852BS_FW_SECTION0_OFFSET + K1_RTL8852BS_FW_SECTION0_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE 2020u
#define K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_BYTES \
  (K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + \
   K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_SECOND_TRANSFER_SIZE \
  (((K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_BYTES + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_FW_SECTION0_SECOND_FIFO_ADDRESS 0x0001c100u
#define K1_RTL8852BS_FW_SECTION0_SECOND_REQUIRED_PAGES 32u
#define K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_OFFSET \
  (K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_OFFSET + \
   K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE 2020u
#define K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_BYTES \
  (K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + \
   K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_THIRD_TRANSFER_SIZE \
  (((K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_BYTES + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_FW_SECTION0_THIRD_FIFO_ADDRESS 0x0001c100u
#define K1_RTL8852BS_FW_SECTION0_THIRD_REQUIRED_PAGES 32u
#define K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_OFFSET \
  (K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_OFFSET + \
   K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_SIZE 2020u
#define K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_BYTES \
  (K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + \
   K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_SIZE)
#define K1_RTL8852BS_FW_SECTION0_FOURTH_TRANSFER_SIZE \
  (((K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_BYTES + \
     K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) / \
    K1_RTL8852BS_H2C_TX_UNIT_SIZE) * K1_RTL8852BS_H2C_TX_UNIT_SIZE)
#define K1_RTL8852BS_FW_SECTION0_FOURTH_FIFO_ADDRESS 0x0001c100u
#define K1_RTL8852BS_FW_SECTION0_FOURTH_REQUIRED_PAGES 32u
#define K1_RTL8852BS_H2C_TXD_FWDL_ENABLE     (1u << 20)

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum k1_rtl8852bs_power_op_e
{
  K1_RTL8852BS_POWER_WRITE,
  K1_RTL8852BS_POWER_POLL
};

enum k1_rtl8852bs_section0_follow_e
{
  K1_RTL8852BS_SECTION0_FOLLOW_NONE,
  K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE,
  K1_RTL8852BS_SECTION0_FOLLOW_SECOND_PACKET,
  K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET,
  K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET
};

struct k1_rtl8852bs_power_step_s
{
  uint16_t address;
  uint8_t mask;
  uint8_t value;
  enum k1_rtl8852bs_power_op_e operation;
};

struct k1_rtl8852bs_fwdl_section_s
{
  uint32_t offset;
  uint32_t length;
  uint32_t mssc;
  uint8_t type;
};

struct k1_rtl8852bs_register_value_s
{
  uint16_t address;
  uint32_t value;
};

/* One entry of a generated RTL8852B radio A/B parameter image.  The address
 * needs 32 bits because an RF D-die entry carries bit 16.
 */

struct k1_rtl8852bs_rf_register_value_s
{
  uint32_t address;
  uint32_t value;
};

/* One vendor headline of array_mp_8852b_radio{a,b}[], in table order.  images
 * is a bitmap of the compiled images that headline selects: bit 0 radio A,
 * bit 1 radio B.
 */

struct k1_rtl8852bs_rf_headline_s
{
  uint8_t rfe_type;
  uint8_t chip_cv;
  uint8_t images;
};

/* Board RF context.  Captured before the firmware starts, because the eFuse
 * read sequence drives the power-cut and isolation registers and the host
 * indirect register window stops returning live values once the WCPU owns
 * the chip.  The radio image guard consumes the cached copy later.
 */

struct k1_rtl8852bs_rf_context_s
{
  bool valid;
  uint8_t cv_byte;
  uint8_t chip_cv;
  uint8_t rfe_efuse;
  uint8_t rfe_type;
  bool rfe_default;
  uint8_t board_option;
  uint8_t chan_plan;
  uint8_t xtal;
  uint8_t thermal_a;
  uint8_t thermal_b;
  uint16_t tssi_de_programmed;
  uint8_t gain_k_programmed;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* SDIO entries from mac_pwron_8852b.  The original 0x0071 entry is PCIe-only
 * and is intentionally absent.  The source table has no delay entries.
 */

static const struct k1_rtl8852bs_power_step_s g_k1_rtl8852bs_power_on[] =
{
  {0x1086, 0x01, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x1086, 0x02, 0x02, K1_RTL8852BS_POWER_POLL},
  {0x0005, 0x18, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x0005, 0x80, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x0005, 0x04, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x0006, 0x02, 0x02, K1_RTL8852BS_POWER_POLL},
  {0x0006, 0x01, 0x01, K1_RTL8852BS_POWER_WRITE},
  {0x0005, 0x01, 0x01, K1_RTL8852BS_POWER_WRITE},
  {0x0005, 0x01, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0088, 0x01, 0x01, K1_RTL8852BS_POWER_WRITE},
  {0x0018, 0x40, 0x40, K1_RTL8852BS_POWER_WRITE},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x40, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x40, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},

  /* SYS_ADIE_PAD_PWR_CTRL BIT(5) is SYM_PADPDN_WL_RFC_1P3, the A-die WL RF
   * clock pad.  mac_pwron_8852b sets it between the first and the second
   * XTAL_SI transaction; without it the A-die serial interface answers every
   * transaction with zero.
   */

  {0x0018, 0x20, 0x20, K1_RTL8852BS_POWER_WRITE},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x20, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x20, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x04, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x04, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x08, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x08, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x10, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x01, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x01, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x02, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x02, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x0272, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0xff, 0x80, K1_RTL8852BS_POWER_WRITE},
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},
  {0x0001, 0x01, 0x01, K1_RTL8852BS_POWER_WRITE},
  {0x0001, 0x80, 0x00, K1_RTL8852BS_POWER_WRITE},
  {0x0001, 0x40, 0x00, K1_RTL8852BS_POWER_WRITE}
};

/* This complete GPL U2 NICCE image is mechanically extracted by
 * tools/extract_rtl8852bs_u2_nic_fw.sh from the recorded source revision.
 * It has the same header and legacy MSS layout as the previously tested NIC
 * image, but its firmware capability table enables scan offload.  Full FWDL
 * reuses it directly and patches only the current packet when the selected
 * legacy MSS signature overlaps the secure section.
 */

static const uint8_t g_k1_rtl8852bs_u2_nicce_image[
  K1_RTL8852BS_U2_NICCE_IMAGE_SIZE] =
{
#include "k1_rtl8852bs_u2_nicce_fw.inc"
};

/* This is the unmodified RTL8852B default PHY CR image.  It is kept in a
 * separate generated include so its GPL source provenance and mechanical
 * ordering remain auditable.  The original table contains no control
 * directives, conditionals, or delay entries.
 */

static const struct k1_rtl8852bs_register_value_s
  g_k1_rtl8852bs_phy_cr_registers[] =
{
#include "k1_rtl8852bs_phy_reg_8852b.inc"
};

/* Check a first, middle, and final portion of the image.  The table has
 * repeated addresses, so every sentinel records the final expected value.
 */

/* The sentinels are also the only values in this component that are known
 * to belong to a baseband register, so the read back diagnostic borrows them
 * to prove its own baseband read window before it believes anything the
 * radio serial interface returns.
 */

#if !defined(CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC) || \
    defined(CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC)
static const struct k1_rtl8852bs_register_value_s
  g_k1_rtl8852bs_phy_cr_sentinels[] =
{
  {0x0704u, 0x601c05ffu},
  {0x49c0u, 0x800cd62du},
  {0x0c14u, 0x85010000u},
  {0xc1f8u, 0x00000001u},
  {0x1210u, 0xc0000c06u}
};
#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC

/* The two generated RTL8852B radio parameter images and the headline table
 * that proves they belong to this board.
 *
 * The vendor array_mp_8852b_radio{a,b}[] tables are conditional packages: a
 * headline block keyed by {RFE type, chip CV} followed by a body of IF /
 * ELSE IF / CHK / ELSE / END directives interleaved with register pairs.  The
 * firmware only ever receives the single branch the running board selects, so
 * the whole 3 MB conditional table is deliberately not part of this
 * component.  tools/k1_rtl8852bs_rf_table_gen.py reproduces
 * halrf_sel_headline_8852b() and halrf_config_8852b_radio_{a,b}_reg()
 * offline and emits the selected branch plus the headline table below.
 *
 * The headline table lets the driver replay the vendor headline selection at
 * run time against the board's real RFE type and chip CV.  A row whose images
 * field is not 0x3 means the vendor driver would have taken a branch that is
 * not the compiled image, and the offload is refused instead of writing the
 * wrong radio parameters.
 */

static const struct k1_rtl8852bs_rf_register_value_s
  g_k1_rtl8852bs_rf_radio_a_registers[] =
{
#include "k1_rtl8852bs_rf_radio_a_8852b.inc"
};

static const struct k1_rtl8852bs_rf_register_value_s
  g_k1_rtl8852bs_rf_radio_b_registers[] =
{
#include "k1_rtl8852bs_rf_radio_b_8852b.inc"
};

static const struct k1_rtl8852bs_rf_headline_s
  g_k1_rtl8852bs_rf_headlines[] =
{
#include "k1_rtl8852bs_rf_headline_8852b.inc"
};

#endif

#ifdef CONFIG_K1_RTL8852BS2_RF_CONTEXT_DIAGNOSTIC
static struct k1_rtl8852bs_rf_context_s g_k1_rtl8852bs_rf_context;
#endif

static const struct k1_rtl8852bs_register_value_s
  g_k1_rtl8852bs_dle_scc_quotas[] =
{
  {K1_RTL8852BS_WDE_QTA0_CFG,  0x00700070u},
  {K1_RTL8852BS_WDE_QTA1_CFG,  0x00080008u},
  {K1_RTL8852BS_WDE_QTA3_CFG,  0x00000000u},
  {K1_RTL8852BS_WDE_QTA4_CFG,  0x00060006u},
  {K1_RTL8852BS_PLE_QTA0_CFG,  0x01400140u},
  {K1_RTL8852BS_PLE_QTA1_CFG,  0x00000000u},
  {K1_RTL8852BS_PLE_QTA2_CFG,  0x007b0010u},
  {K1_RTL8852BS_PLE_QTA3_CFG,  0x00100010u},
  {K1_RTL8852BS_PLE_QTA4_CFG,  0x0085001au},
  {K1_RTL8852BS_PLE_QTA5_CFG,  0x00000000u},
  {K1_RTL8852BS_PLE_QTA6_CFG,  0x011d00b2u},
  {K1_RTL8852BS_PLE_QTA7_CFG,  0x00000000u},
  {K1_RTL8852BS_PLE_QTA8_CFG,  0x007b0010u},
  {K1_RTL8852BS_PLE_QTA9_CFG,  0x00010001u},
  {K1_RTL8852BS_PLE_QTA10_CFG, 0x00180008u}
};

static const struct k1_rtl8852bs_register_value_s
  g_k1_rtl8852bs_hci_fc_registers[] =
{
  {K1_RTL8852BS_ACH0_PAGE_CTRL, K1_RTL8852BS_HCI_FC_CHANNEL_PAGES},
  {K1_RTL8852BS_ACH1_PAGE_CTRL, K1_RTL8852BS_HCI_FC_CHANNEL_PAGES},
  {K1_RTL8852BS_ACH2_PAGE_CTRL, K1_RTL8852BS_HCI_FC_CHANNEL_PAGES},
  {K1_RTL8852BS_ACH3_PAGE_CTRL, K1_RTL8852BS_HCI_FC_CHANNEL_PAGES},
  {K1_RTL8852BS_CH8_PAGE_CTRL,  K1_RTL8852BS_HCI_FC_CHANNEL_PAGES},
  {K1_RTL8852BS_CH9_PAGE_CTRL,  K1_RTL8852BS_HCI_FC_CHANNEL_PAGES},
  {K1_RTL8852BS_PUB_PAGE_CTRL1, K1_RTL8852BS_HCI_FC_PUBLIC_PAGES},
  {K1_RTL8852BS_WP_PAGE_CTRL2,  0u},
  {K1_RTL8852BS_CH_PAGE_CTRL,   K1_RTL8852BS_HCI_FC_PRECOSTS},
  {K1_RTL8852BS_PUB_PAGE_CTRL2, K1_RTL8852BS_HCI_FC_PUBLIC_PAGES},
  {K1_RTL8852BS_WP_PAGE_CTRL1,  0u}
};

/* This is the static subset of dmac_init()/cmac_init() selected from
 * trxcfg.c for MAC_AX_TRX_SW_MODE on RTL8852B band 0.  Every entry is an
 * update-and-readback field, rather than a raw register image, so unrelated
 * hardware-owned bits retain their current values.  Address-CAM, scheduler,
 * security, role, station and RF/BB configuration intentionally remain in a
 * later MAC driver layer.
 */

struct k1_rtl8852bs_register_field_s
{
  uint32_t address;
  uint32_t mask;
  uint32_t value;
};

static const struct k1_rtl8852bs_register_field_s
  g_k1_rtl8852bs_runtime_mac_core_fields[] =
{
  {K1_RTL8852BS_MPDU_PROC, K1_RTL8852BS_MPDU_PROC_STATIC_MASK,
    K1_RTL8852BS_MPDU_PROC_STATIC_VALUE},
  {K1_RTL8852BS_ACTION_FWD0, UINT32_MAX,
    K1_RTL8852BS_ACTION_FWD0_VALUE},
  {K1_RTL8852BS_TF_FWD, UINT32_MAX, K1_RTL8852BS_TF_FWD_VALUE},
  {K1_RTL8852BS_CUT_AMSDU_CTRL, UINT32_MAX,
    K1_RTL8852BS_CUT_AMSDU_CTRL_VALUE},
  {K1_RTL8852BS_MACID_MATCH, K1_RTL8852BS_SPATIAL_REUSE_MASK,
    K1_RTL8852BS_SPATIAL_REUSE_VALUE},
  {K1_RTL8852BS_MAC_LOOPBACK, K1_RTL8852BS_MAC_LOOPBACK_EN, 0u},
  {K1_RTL8852BS_TCR0, K1_RTL8852BS_TCR_UDF_THRESHOLD_MASK,
    K1_RTL8852BS_TCR_UDF_THRESHOLD_VALUE},
  {K1_RTL8852BS_TXD_FIFO_CTRL, K1_RTL8852BS_TXDFIFO_MCS_MASK,
    K1_RTL8852BS_TXDFIFO_MCS_VALUE},
  {K1_RTL8852BS_TB_PPDU_CTRL, K1_RTL8852BS_SW_PREFER_AC_MASK, 0u},
  {K1_RTL8852BS_TRXPTCL_RESP0, K1_RTL8852BS_SIFS_MASK,
    K1_RTL8852BS_SIFS_VALUE},
  {K1_RTL8852BS_RXTRIG_TEST_USER2, K1_RTL8852BS_RXTRIG_FCSCHK,
    K1_RTL8852BS_RXTRIG_FCSCHK},
  {K1_RTL8852BS_RESPBA_CAM_CTRL, K1_RTL8852BS_RESPBA_SSN_SELECT,
    K1_RTL8852BS_RESPBA_SSN_SELECT},
  {K1_RTL8852BS_RCR, K1_RTL8852BS_RCR_CHANNEL_MASK,
    K1_RTL8852BS_RCR_CHANNEL_VALUE},
  {K1_RTL8852BS_RCR, K1_RTL8852BS_DLK_PROTECT_MASK,
    K1_RTL8852BS_DLK_PROTECT_VALUE},
  {K1_RTL8852BS_RX_FLTR_OPT, K1_RTL8852BS_RX_MPDU_MAX_MASK,
    K1_RTL8852BS_RX_MPDU_MAX_VALUE},
  {K1_RTL8852BS_PLCP_HDR_FLTR, K1_RTL8852BS_VHT_SIGB_CRC_CHECK, 0u},
  {K1_RTL8852BS_TX_SUB_CARRIER_VALUE, K1_RTL8852BS_TXSC_MASK, 0u},
  {K1_RTL8852BS_PTCL_RRSR1, K1_RTL8852BS_RRSR_RATE_MASK,
    K1_RTL8852BS_RRSR_RATE_VALUE},
  {K1_RTL8852BS_PTCL_COMMON_SETTING0, K1_RTL8852BS_PTCL_MODE_MASK,
    K1_RTL8852BS_PTCL_MODE_VALUE},
  {K1_RTL8852BS_PTCLRPT_FULL_HDL, K1_RTL8852BS_SPE_RPT_PATH_MASK,
    K1_RTL8852BS_SPE_RPT_PATH_WLCPU},
  {K1_RTL8852BS_RXDMA_CTRL0, K1_RTL8852BS_RXDMA_FULL_MODE_MASK, 0u}
};

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_BB_RESET_DIAGNOSTIC

/* halbb_fwofld_bb_reset_8852b() in vendor order: TSSI protect on, the BB
 * reset pulse on 0x704 BIT(1), then TSSI protect off.
 *
 * Unlike every other table in this component the value column holds the
 * unshifted field value, because that is what the firmware receives.
 * halbb_set_reg() and halbb_set_reg_cmn() divert to halbb_fw_set_reg()
 * before their host-side shift, and that helper stores cmd.value = val next
 * to cmd.mask = mask, leaving the shift to the firmware.  0x704 comes from
 * halbb_set_reg_cmn(..., phy_idx) whose phy offset is zero for HW_PHY_0, and
 * RTL8852B has no DBCC, so no offset is added here.
 *
 * halbb_fwofld_bitmap_en() closes the vendor offload window with one more
 * write, 0x1a24 mask 0xff value 0, purely to carry the last-command flag when
 * the API left it clear.  The batch below already sets that flag on its final
 * entry, so the flush marker has no work to do and is not reproduced.
 */

static const struct k1_rtl8852bs_register_field_s
  g_k1_rtl8852bs_bb_reset_fields[] =
{
  {0x58dcu, 0xc0000000u, 0x1u},
  {0x5818u, 0x40000000u, 0x1u},
  {0x78dcu, 0xc0000000u, 0x1u},
  {0x7818u, 0x40000000u, 0x1u},
  {0x0704u, 0x00000002u, 0x1u},
  {0x0704u, 0x00000002u, 0x0u},
  {0x0704u, 0x00000002u, 0x1u},
  {0x58dcu, 0xc0000000u, 0x3u},
  {0x5818u, 0x40000000u, 0x0u},
  {0x78dcu, 0xc0000000u, 0x3u},
  {0x7818u, 0x40000000u, 0x0u}
};

#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_PHY_COUNTER_DIAGNOSTIC

/* The RMAC receive counter indices of mac_rx_cnt(), copied from the original
 * MAC_AX_RXCRC_OK_IDX, MAC_AX_RXCRC_FAIL_IDX and MAC_AX_RXFA_IDX in the
 * vendor order CCK, OFDM, HT, VHT-SU, VHT-MU, HE-SU, HE-MU, HE-TB.  Only the
 * first three matter on a 2.4 GHz passive scan: a beacon is CCK or OFDM and
 * an HT beacon still carries an HT PPDU, but reading all eight costs nothing
 * and keeps the totals comparable with the vendor helper, which sums the
 * whole array.
 */

static const uint8_t
  g_k1_rtl8852bs_rx_crc_ok_index[K1_RTL8852BS_RX_PPDU_TYPES] =
{
  3u, 0u, 6u, 10u, 14u, 18u, 22u, 26u
};

static const uint8_t
  g_k1_rtl8852bs_rx_crc_fail_index[K1_RTL8852BS_RX_PPDU_TYPES] =
{
  4u, 1u, 7u, 11u, 15u, 19u, 23u, 27u
};

static const uint8_t
  g_k1_rtl8852bs_rx_false_alarm_index[K1_RTL8852BS_RX_PPDU_TYPES] =
{
  5u, 2u, 9u, 13u, 17u, 21u, 25u, 29u
};

#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool k1_rtl8852bs_is_local(uint32_t address)
{
  return address >= K1_RTL8852BS_LOCAL_REG_START &&
         address <= K1_RTL8852BS_LOCAL_REG_END;
}

static int k1_rtl8852bs_indirect_wait(void)
{
  uint8_t status;
  unsigned int attempt;
  int ret;

  for (attempt = 0; attempt < K1_RTL8852BS_INDIRECT_POLL_COUNT; attempt++)
    {
      ret = k1_sdio_wifi_f1_read_byte(K1_RTL8852BS_INDIRECT_ADDR + 3,
                                       &status);
      if (ret < 0)
        {
          return ret;
        }

      if ((status & K1_RTL8852BS_INDIRECT_READY) != 0)
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_INDIRECT_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_rtl8852bs_indirect_select(uint32_t address)
{
  int ret;

  ret = k1_sdio_wifi_f1_write_byte(K1_RTL8852BS_INDIRECT_ADDR,
                                    (uint8_t)address);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_f1_write_byte(K1_RTL8852BS_INDIRECT_ADDR + 1,
                                    (uint8_t)(address >> 8));
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_f1_write_byte(K1_RTL8852BS_INDIRECT_ADDR + 2,
                                    (uint8_t)(address >> 16));
  if (ret < 0)
    {
      return ret;
    }

  return k1_sdio_wifi_f1_write_byte(
    K1_RTL8852BS_INDIRECT_ADDR + 3,
    (uint8_t)(address >> 24) | K1_RTL8852BS_INDIRECT_READY);
}

static int k1_rtl8852bs_indirect_read(uint32_t address, FAR uint8_t *value)
{
  int ret;

  ret = k1_rtl8852bs_indirect_select(address);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_f1_write_byte(K1_RTL8852BS_INDIRECT_CTRL,
                                    K1_RTL8852BS_INDIRECT_REG_READ);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_indirect_wait();
  if (ret < 0)
    {
      return ret;
    }

  return k1_sdio_wifi_f1_read_byte(K1_RTL8852BS_INDIRECT_DATA, value);
}

static int k1_rtl8852bs_indirect_write(uint32_t address, uint8_t value)
{
  int ret;

  ret = k1_rtl8852bs_indirect_select(address);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_f1_write_byte(K1_RTL8852BS_INDIRECT_DATA, value);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_f1_write_byte(K1_RTL8852BS_INDIRECT_CTRL,
                                    K1_RTL8852BS_INDIRECT_REG_WRITE8);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_indirect_wait();
}

static int k1_rtl8852bs_read(uint32_t address, FAR uint8_t *value)
{
  if (k1_rtl8852bs_is_local(address))
    {
      return k1_sdio_wifi_f1_read_byte(address, value);
    }

  return k1_rtl8852bs_indirect_read(address, value);
}

static int k1_rtl8852bs_write(uint32_t address, uint8_t value)
{
  if (k1_rtl8852bs_is_local(address))
    {
      return k1_sdio_wifi_f1_write_byte(address, value);
    }

  return k1_rtl8852bs_indirect_write(address, value);
}

static int k1_rtl8852bs_post_power_read32(uint32_t address,
                                          FAR uint32_t *value)
{
  uint32_t result = 0;
  uint8_t byte;
  unsigned int offset;
  int ret;

  if (value == NULL || (address & (sizeof(uint32_t) - 1)) != 0 ||
      address > 0x1fffcul)
    {
      return -EINVAL;
    }

  /* The SDIO local window is directly CMD52-addressable.  Other MAC
   * registers, including HCI_OPT_CTRL at 0x74, must first be selected
   * through the chip's indirect CMD52 window.  A raw Function 1 CMD52 to
   * HCI_OPT_CTRL produces a command CRC/index error on the K1 host.
   */

  for (offset = 0; offset < sizeof(result); offset++)
    {
      ret = k1_rtl8852bs_read(address + offset, &byte);
      if (ret < 0)
        {
          return ret;
        }

      result |= (uint32_t)byte << (offset * 8);
    }

  *value = result;
  return OK;
}

static int k1_rtl8852bs_post_power_write32(uint32_t address, uint32_t value)
{
  unsigned int offset;
  int ret;

  if ((address & (sizeof(uint32_t) - 1)) != 0 || address > 0x1fffcul)
    {
      return -EINVAL;
    }

  for (offset = 0; offset < sizeof(value); offset++)
    {
      ret = k1_rtl8852bs_write(address + offset,
                                (uint8_t)(value >> (offset * 8)));
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static int k1_rtl8852bs_post_power_update_bits(uint32_t address,
                                                uint32_t clear, uint32_t set)
{
  uint32_t value;
  int ret;

  ret = k1_rtl8852bs_post_power_read32(address, &value);
  if (ret < 0)
    {
      return ret;
    }

  value &= ~clear;
  value |= set;
  return k1_rtl8852bs_post_power_write32(address, value);
}

static int
k1_rtl8852bs_run_step(FAR const struct k1_rtl8852bs_power_step_s *step)
{
  uint8_t value;
  unsigned int attempt;
  int ret;

  if (step->operation == K1_RTL8852BS_POWER_WRITE)
    {
      ret = k1_rtl8852bs_read(step->address, &value);
      if (ret < 0)
        {
          return ret;
        }

      value &= ~step->mask;
      value |= step->value & step->mask;
      return k1_rtl8852bs_write(step->address, value);
    }

  for (attempt = 0; attempt < K1_RTL8852BS_POWER_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_read(step->address, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & step->mask) == (step->value & step->mask))
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_POWER_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static void k1_rtl8852bs_log_error(unsigned int step, uint16_t address,
                                    int error)
{
  k1_early_puts("K1 Wi-Fi GPL: bootstrap step=");
  k1_early_puthex(step);
  k1_early_puts(" address=");
  k1_early_puthex(address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)-error);
  k1_early_puts("\r\n");
}

static void k1_rtl8852bs_log_post_power_error(uint16_t address, int error)
{
  k1_early_puts("K1 Wi-Fi GPL: post-power F1 CMD52 address=");
  k1_early_puthex(address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)-error);
  k1_early_puts("\r\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_rtl8852bs_bootstrap(void)
{
  unsigned int step;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 bootstrap begin\r\n");

  for (step = 0;
       step < sizeof(g_k1_rtl8852bs_power_on) /
              sizeof(g_k1_rtl8852bs_power_on[0]);
       step++)
    {
      ret = k1_rtl8852bs_run_step(&g_k1_rtl8852bs_power_on[step]);
      if (ret < 0)
        {
          k1_rtl8852bs_log_error(step,
                                  g_k1_rtl8852bs_power_on[step].address,
                                  ret);
          return ret;
        }
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 bootstrap complete\r\n");
  return OK;
}

int k1_rtl8852bs_hci_dmac_pre_init(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 HCI/DMAC pre-init begin\r\n");

  ret = k1_rtl8852bs_post_power_update_bits(K1_RTL8852BS_HCI_FUNC_EN, 0,
                                            K1_RTL8852BS_HCI_DMA_EN);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_HCI_FUNC_EN, ret);
      return ret;
    }

  ret = k1_rtl8852bs_post_power_write32(K1_RTL8852BS_DMAC_FUNC_EN,
                                        K1_RTL8852BS_DMAC_PRE_FUNC_EN);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_DMAC_FUNC_EN, ret);
      return ret;
    }

  ret = k1_rtl8852bs_post_power_write32(K1_RTL8852BS_DMAC_CLK_EN,
                                        K1_RTL8852BS_DMAC_PRE_CLK_EN);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_DMAC_CLK_EN, ret);
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 HCI/DMAC pre-init complete\r\n");
  return OK;
}

int k1_rtl8852bs_sdio_pre_init(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 SDIO pre-init begin\r\n");

  ret = k1_rtl8852bs_post_power_update_bits(K1_RTL8852BS_HCI_OPT_CTRL, 0,
                                            K1_RTL8852BS_SDIO_DATA_PAD_SMT);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_HCI_OPT_CTRL, ret);
      return ret;
    }

  ret = k1_rtl8852bs_post_power_update_bits(
    K1_RTL8852BS_SDIO_TX_CTRL, K1_RTL8852BS_CMD53_TX_FORMAT,
    K1_RTL8852BS_RXINT_READ_MASK_DIS);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_SDIO_TX_CTRL, ret);
      return ret;
    }

  ret = k1_rtl8852bs_post_power_update_bits(K1_RTL8852BS_SDIO_BUS_CTRL, 0,
                                            K1_RTL8852BS_EN_RPT_TXCRC);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_SDIO_BUS_CTRL, ret);
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 SDIO pre-init complete\r\n");
  return OK;
}

int k1_rtl8852bs_cmd53_write_diagnostic(void)
{
  uint8_t value[12] =
  {
    0
  };

  uint32_t register_value;
  int ret;

  /* This is the same 12-byte write shape used by the vendor's first
   * successful CMD53 request: F1, incrementing, address 0x1040, byte mode.
   * Read the target first and write it back unchanged so the probe exercises
   * the transport without intentionally changing a MAC register.
   */

  ret = k1_rtl8852bs_post_power_read32(K1_RTL8852BS_HCI_OPT_CTRL,
                                        &register_value);
  if (ret < 0)
    {
      k1_rtl8852bs_log_post_power_error(K1_RTL8852BS_HCI_OPT_CTRL, ret);
      return ret;
    }

  value[0] = (uint8_t)K1_RTL8852BS_HCI_OPT_CTRL;
  value[1] = (uint8_t)(K1_RTL8852BS_HCI_OPT_CTRL >> 8);
  value[2] = (uint8_t)(K1_RTL8852BS_HCI_OPT_CTRL >> 16);
  value[3] = (uint8_t)(K1_RTL8852BS_HCI_OPT_CTRL >> 24) |
             K1_RTL8852BS_INDIRECT_READY;
  value[4] = (uint8_t)register_value;
  value[5] = (uint8_t)(register_value >> 8);
  value[6] = (uint8_t)(register_value >> 16);
  value[7] = (uint8_t)(register_value >> 24);
  value[8] = K1_RTL8852BS_INDIRECT_REG_WRITE32;

  ret = k1_sdio_wifi_write(1, K1_RTL8852BS_INDIRECT_ADDR, true,
                           value, sizeof(value));
  k1_early_puts("K1 Wi-Fi GPL: CMD53 write diagnostic error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

static int k1_rtl8852bs_cmd53_indirect_read32(uint32_t address,
                                               FAR uint32_t *value)
{
  uint8_t request[12] =
  {
    0
  };

  uint8_t reply[8];
  unsigned int attempt;
  int ret;

  if (value == NULL)
    {
      return -EINVAL;
    }

  /* This is _r_indir_cmd53_sdio_8852b() from the derived driver.  The
   * indirect request is complete only after CMD53 reads the ready byte and
   * its four-byte result back from offset 0x1043.
   */

  request[0] = (uint8_t)address;
  request[1] = (uint8_t)(address >> 8);
  request[2] = (uint8_t)(address >> 16);
  request[3] = (uint8_t)(address >> 24) | K1_RTL8852BS_INDIRECT_READY;
  request[8] = K1_RTL8852BS_INDIRECT_REG_READ;

  ret = k1_sdio_wifi_write(1, K1_RTL8852BS_INDIRECT_ADDR, true,
                           request, sizeof(request));
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_INDIRECT_POLL_COUNT; attempt++)
    {
      ret = k1_sdio_wifi_read(1, K1_RTL8852BS_INDIRECT_ADDR + 3, true,
                              reply, sizeof(reply));
      if (ret < 0)
        {
          return ret;
        }

      if ((reply[0] & K1_RTL8852BS_INDIRECT_READY) != 0)
        {
          *value = (uint32_t)reply[1] |
                   (uint32_t)reply[2] << 8 |
                   (uint32_t)reply[3] << 16 |
                   (uint32_t)reply[4] << 24;
          return OK;
        }

      up_udelay(K1_RTL8852BS_INDIRECT_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_rtl8852bs_cmd53_indirect_write32(uint32_t address,
                                                uint32_t value)
{
  uint8_t request[12] =
  {
    0
  };

  int ret;

  /* This is w_indir_cmd53_sdio_8852b() with SDIO_IO_DWORD.  Unlike the read
   * path, the derived driver polls the completed write through CMD52.
   */

  request[0] = (uint8_t)address;
  request[1] = (uint8_t)(address >> 8);
  request[2] = (uint8_t)(address >> 16);
  request[3] = (uint8_t)(address >> 24) | K1_RTL8852BS_INDIRECT_READY;
  request[4] = (uint8_t)value;
  request[5] = (uint8_t)(value >> 8);
  request[6] = (uint8_t)(value >> 16);
  request[7] = (uint8_t)(value >> 24);
  request[8] = K1_RTL8852BS_INDIRECT_REG_WRITE32;

  ret = k1_sdio_wifi_write(1, K1_RTL8852BS_INDIRECT_ADDR, true,
                           request, sizeof(request));
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_indirect_wait();
}

static int k1_rtl8852bs_mac_read32(uint32_t address, FAR uint32_t *value)
{
  if (k1_rtl8852bs_is_local(address))
    {
      return k1_rtl8852bs_post_power_read32(address, value);
    }

  return k1_rtl8852bs_cmd53_indirect_read32(address, value);
}

static int k1_rtl8852bs_mac_write32(uint32_t address, uint32_t value)
{
  if (k1_rtl8852bs_is_local(address))
    {
      return k1_rtl8852bs_post_power_write32(address, value);
    }

  return k1_rtl8852bs_cmd53_indirect_write32(address, value);
}

#if defined(CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC) || \
    !defined(CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC)

/* A baseband register number is not a bus address.  Every host baseband
 * access in the original driver goes through halbb_get_cr() and
 * halbb_set_cr(), which add bb->bb0_cr_offset, and halbb_init.c sets that
 * offset to 0x10000 for every AX series IC, so baseband 0x0704 is bus
 * 0x10704.  Firmware side offload writes are unaffected: the firmware adds
 * the offset itself, which is why the offload path below passes the plain
 * table address.
 */

static int k1_rtl8852bs_bb_read32(uint32_t address, FAR uint32_t *value)
{
  return k1_rtl8852bs_mac_read32(address + K1_RTL8852BS_BB_CR_OFFSET,
                                 value);
}

static int k1_rtl8852bs_bb_write32(uint32_t address, uint32_t value)
{
  return k1_rtl8852bs_mac_write32(address + K1_RTL8852BS_BB_CR_OFFSET,
                                  value);
}

#endif

static int k1_rtl8852bs_mac_update_bits(uint32_t address, uint32_t clear,
                                         uint32_t set)
{
  uint32_t value;
  int ret;

  ret = k1_rtl8852bs_mac_read32(address, &value);
  if (ret < 0)
    {
      return ret;
    }

  value &= ~clear;
  value |= set;
  return k1_rtl8852bs_mac_write32(address, value);
}

static int k1_rtl8852bs_mac_update_field_checked(uint32_t address,
                                                  uint32_t mask,
                                                  uint32_t value)
{
  uint32_t actual;
  uint32_t updated;
  int ret;

  if ((value & ~mask) != 0)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_mac_read32(address, &updated);
  if (ret < 0)
    {
      return ret;
    }

  updated &= ~mask;
  updated |= value;
  ret = k1_rtl8852bs_mac_write32(address, updated);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(address, &actual);
  if (ret < 0)
    {
      return ret;
    }

  return (actual & mask) == value ? OK : -EIO;
}

static int k1_rtl8852bs_xtal_si_wait(FAR uint32_t *result)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  if (result == NULL)
    {
      return -EINVAL;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_XTAL_SI_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WLAN_XTAL_SI_CTRL, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & K1_RTL8852BS_XTAL_SI_CMD_POLL) == 0)
        {
          *result = value;
          return OK;
        }

      up_udelay(K1_RTL8852BS_XTAL_SI_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_rtl8852bs_xtal_si_write(uint8_t offset, uint8_t value)
{
  uint32_t result;
  uint32_t command;
  int ret;

  command = K1_RTL8852BS_XTAL_SI_CMD_POLL |
            K1_RTL8852BS_XTAL_SI_FULL_MASK |
            ((uint32_t)value << K1_RTL8852BS_XTAL_SI_DATA_SHIFT) | offset;
  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_WLAN_XTAL_SI_CTRL, command);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_xtal_si_wait(&result);
}

static int k1_rtl8852bs_xtal_si_read(uint8_t offset, FAR uint8_t *value)
{
  uint32_t result;
  uint32_t command;
  int ret;

  if (value == NULL)
    {
      return -EINVAL;
    }

  command = K1_RTL8852BS_XTAL_SI_CMD_POLL |
            K1_RTL8852BS_XTAL_SI_MODE_READ | offset;
  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_WLAN_XTAL_SI_CTRL, command);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_xtal_si_wait(&result);
  if (ret < 0)
    {
      return ret;
    }

  *value = (uint8_t)(result >> K1_RTL8852BS_XTAL_SI_DATA_SHIFT);
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_bb_rf_enable
 *
 * Description:
 *   Apply the RTL8852B set_enable_bb_rf(..., 1) release sequence after MAC
 *   runtime initialization.  It deliberately stops before PHY parameter
 *   tables, RF parameter tables, and calibration.  Those operations need
 *   board-specific RF/efuse context and cannot safely be inferred here.
 ****************************************************************************/

int k1_rtl8852bs_runtime_bb_rf_enable(void)
{
  uint32_t value;
  uint8_t sys_func;
  uint8_t phyreg;
  uint8_t xtal_s0;
  uint8_t xtal_s1;
  int ret;

  ret = k1_rtl8852bs_read(K1_RTL8852BS_SYS_FUNC_EN, &sys_func);
  if (ret < 0)
    {
      return ret;
    }

  sys_func |= K1_RTL8852BS_SYS_FUNC_BB_ENABLE;
  ret = k1_rtl8852bs_write(K1_RTL8852BS_SYS_FUNC_EN, sys_func);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_read(K1_RTL8852BS_SYS_FUNC_EN, &sys_func);
  if (ret < 0 ||
      (sys_func & K1_RTL8852BS_SYS_FUNC_BB_ENABLE) !=
      K1_RTL8852BS_SYS_FUNC_BB_ENABLE)
    {
      return ret < 0 ? ret : -EIO;
    }

  ret = k1_rtl8852bs_mac_update_field_checked(
    K1_RTL8852BS_SPS_DIG_ON_CTRL0, K1_RTL8852BS_SPS_DIG_ZCDC_MASK,
    K1_RTL8852BS_SPS_DIG_ZCDC_VALUE);
  if (ret < 0)
    {
      return ret;
    }

  /* The original driver requires this exact 1 -> 0 -> 1 AFE-digital edge. */

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_WLRF_CTRL, 0,
                                     K1_RTL8852BS_WLRF_AFEDIG);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_WLRF_CTRL,
                                         K1_RTL8852BS_WLRF_AFEDIG, 0);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_WLRF_CTRL, 0,
                                         K1_RTL8852BS_WLRF_AFEDIG);
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WLRF_CTRL, &value);
  if (ret < 0 || (value & K1_RTL8852BS_WLRF_AFEDIG) == 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  ret = k1_rtl8852bs_xtal_si_write(K1_RTL8852BS_XTAL_SI_WL_RFC_S0,
                                    K1_RTL8852BS_XTAL_SI_WL_RFC_VALUE);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_write(K1_RTL8852BS_XTAL_SI_WL_RFC_S1,
                                        K1_RTL8852BS_XTAL_SI_WL_RFC_VALUE);
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_xtal_si_read(K1_RTL8852BS_XTAL_SI_WL_RFC_S0,
                                   &xtal_s0);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_read(K1_RTL8852BS_XTAL_SI_WL_RFC_S1,
                                       &xtal_s1);
    }

  if (ret < 0 || xtal_s0 != K1_RTL8852BS_XTAL_SI_WL_RFC_VALUE ||
      xtal_s1 != K1_RTL8852BS_XTAL_SI_WL_RFC_VALUE)
    {
      return ret < 0 ? ret : -EIO;
    }

  ret = k1_rtl8852bs_write(K1_RTL8852BS_PHYREG_SET,
                            K1_RTL8852BS_PHYREG_XYN_CYCLE);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_read(K1_RTL8852BS_PHYREG_SET, &phyreg);
  if (ret < 0 || phyreg != K1_RTL8852BS_PHYREG_XYN_CYCLE)
    {
      return ret < 0 ? ret : -EIO;
    }

  k1_early_puts("K1 Wi-Fi GPL: BB/RF release SYS_FUNC=");
  k1_early_puthex(sys_func);
  k1_early_puts(" WLRF=");
  k1_early_puthex(value);
  k1_early_puts(" XTAL=");
  k1_early_puthex(xtal_s0);
  k1_early_puts(",");
  k1_early_puthex(xtal_s1);
  k1_early_puts(" PHYREG=");
  k1_early_puthex(phyreg);
  k1_early_puts("\r\n");
  return OK;
}

int k1_rtl8852bs_fwdl_runtime_bb_rf_diagnostic(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 BB/RF release begin\r\n");
  ret = k1_rtl8852bs_runtime_bb_rf_enable();
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 BB/RF release error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 BB/RF release complete\r\n");
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_phy_cr_init
 *
 * Description:
 *   Apply the original RTL8852B default BB PHY CR image in its exact source
 *   order.  The source driver invokes this from halbb_init_reg() after the
 *   BB/RF release.  Its RF images and calibrations are intentionally outside
 *   this staged diagnostic.
 ****************************************************************************/

#ifndef CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC
static int k1_rtl8852bs_runtime_phy_cr_direct_init(void)
{
  uint32_t actual = 0;
  unsigned int attempt;
  unsigned int index;
  int ret = OK;

  /* Applying this table issues more than a thousand successful indirect
   * CMD53 transactions.  Keep SDHCI error reports and the explicit result
   * below, but do not let register traces overrun the debug UART.  The
   * table holds baseband register numbers, so every access here goes
   * through the baseband window rather than the plain MAC one.
   */

  k1_sdio_wifi_suppress_command_trace(true);

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_phy_cr_registers) /
               sizeof(g_k1_rtl8852bs_phy_cr_registers[0]);
       index++)
    {
      for (attempt = 0; attempt <= K1_RTL8852BS_PHY_CR_CRC_RETRIES;
           attempt++)
        {
          ret = k1_rtl8852bs_bb_write32(
            g_k1_rtl8852bs_phy_cr_registers[index].address,
            g_k1_rtl8852bs_phy_cr_registers[index].value);
          if (ret != -EILSEQ ||
              attempt == K1_RTL8852BS_PHY_CR_CRC_RETRIES)
            {
              break;
            }

          k1_early_puts("K1 Wi-Fi GPL: PHY CR write CRC retry index=");
          k1_early_puthex(index);
          k1_early_puts(" attempt=");
          k1_early_puthex(attempt + 1);
          k1_early_puts("\r\n");
          ret = k1_sdio_wifi_recover_after_crc();
          if (ret < 0)
            {
              k1_early_puts("K1 Wi-Fi GPL: PHY CR write recovery error=");
              k1_early_puthex((uintreg_t)-ret);
              k1_early_puts("\r\n");
              break;
            }
        }

      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: PHY CR write index=");
          k1_early_puthex(index);
          k1_early_puts(" address=");
          k1_early_puthex(g_k1_rtl8852bs_phy_cr_registers[index].address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts("\r\n");
          goto out;
        }
    }

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_phy_cr_sentinels) /
               sizeof(g_k1_rtl8852bs_phy_cr_sentinels[0]);
       index++)
    {
      for (attempt = 0; attempt <= K1_RTL8852BS_PHY_CR_CRC_RETRIES;
           attempt++)
        {
          ret = k1_rtl8852bs_bb_read32(
            g_k1_rtl8852bs_phy_cr_sentinels[index].address, &actual);
          if (ret != -EILSEQ ||
              attempt == K1_RTL8852BS_PHY_CR_CRC_RETRIES)
            {
              break;
            }

          k1_early_puts("K1 Wi-Fi GPL: PHY CR read CRC retry index=");
          k1_early_puthex(index);
          k1_early_puts(" attempt=");
          k1_early_puthex(attempt + 1);
          k1_early_puts("\r\n");
          ret = k1_sdio_wifi_recover_after_crc();
          if (ret < 0)
            {
              k1_early_puts("K1 Wi-Fi GPL: PHY CR read recovery error=");
              k1_early_puthex((uintreg_t)-ret);
              k1_early_puts("\r\n");
              break;
            }
        }

      if (ret < 0 ||
          actual != g_k1_rtl8852bs_phy_cr_sentinels[index].value)
        {
          k1_early_puts("K1 Wi-Fi GPL: PHY CR readback index=");
          k1_early_puthex(index);
          k1_early_puts(" address=");
          k1_early_puthex(g_k1_rtl8852bs_phy_cr_sentinels[index].address);
          k1_early_puts(" actual=");
          k1_early_puthex(actual);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)(ret < 0 ? -ret : EIO));
          k1_early_puts("\r\n");
          if (ret >= 0)
            {
              ret = -EIO;
            }

          goto out;
        }
    }

  k1_early_puts("K1 Wi-Fi GPL: PHY CR image entries=");
  k1_early_puthex(sizeof(g_k1_rtl8852bs_phy_cr_registers) /
                  sizeof(g_k1_rtl8852bs_phy_cr_registers[0]));
  k1_early_puts(" sentinels=");
  k1_early_puthex(sizeof(g_k1_rtl8852bs_phy_cr_sentinels) /
                  sizeof(g_k1_rtl8852bs_phy_cr_sentinels[0]));
  k1_early_puts("\r\n");

out:
  k1_sdio_wifi_suppress_command_trace(false);
  return ret;
}

#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC
static int k1_rtl8852bs_runtime_phy_cr_offload_init(void);
static void k1_rtl8852bs_scanofld_log_bytes(FAR const uint8_t *data,
                                            size_t length);
#endif

int k1_rtl8852bs_runtime_phy_cr_init(void)
{
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC
  return k1_rtl8852bs_runtime_phy_cr_offload_init();
#else
  return k1_rtl8852bs_runtime_phy_cr_direct_init();
#endif
}

int k1_rtl8852bs_fwdl_runtime_phy_cr_diagnostic(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 PHY CR image begin\r\n");
  ret = k1_rtl8852bs_runtime_phy_cr_init();
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 PHY CR image error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 PHY CR image complete\r\n");
  return OK;
}

static int k1_rtl8852bs_dle_wait_ready(uint32_t address)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  for (attempt = 0; attempt < K1_RTL8852BS_DLE_READY_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(address, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & K1_RTL8852BS_DLE_READY_MASK) ==
          K1_RTL8852BS_DLE_READY_MASK)
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_DLE_READY_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_rtl8852bs_hci_fc_write_checked(uint32_t address,
                                              uint32_t expected)
{
  uint32_t value;
  int ret;

  ret = k1_rtl8852bs_mac_write32(address, expected);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(address, &value);
  if (ret < 0)
    {
      return ret;
    }

  if (value != expected)
    {
      k1_early_puts("K1 Wi-Fi GPL: HCI FC readback address=");
      k1_early_puthex(address);
      k1_early_puts(" expected=");
      k1_early_puthex(expected);
      k1_early_puts(" actual=");
      k1_early_puthex(value);
      k1_early_puts("\r\n");
      return -EIO;
    }

  return OK;
}

static uint16_t k1_rtl8852bs_read_le16(FAR const uint8_t *buffer)
{
  return (uint16_t)buffer[0] | (uint16_t)buffer[1] << 8;
}

static uint32_t k1_rtl8852bs_read_le32(FAR const uint8_t *buffer)
{
  return (uint32_t)buffer[0] |
         (uint32_t)buffer[1] << 8 |
         (uint32_t)buffer[2] << 16 |
         (uint32_t)buffer[3] << 24;
}

static void k1_rtl8852bs_write_le16(FAR uint8_t *buffer, uint16_t value)
{
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
}

static void k1_rtl8852bs_write_le32(FAR uint8_t *buffer, uint32_t value)
{
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
}

static int k1_rtl8852bs_post_power_read16(uint32_t address,
                                           FAR uint16_t *value)
{
  uint16_t result = 0;
  uint8_t byte;
  unsigned int offset;
  int ret;

  if (value == NULL || (address & (sizeof(uint16_t) - 1)) != 0 ||
      address > 0x1fffeu)
    {
      return -EINVAL;
    }

  for (offset = 0; offset < sizeof(result); offset++)
    {
      ret = k1_rtl8852bs_read(address + offset, &byte);
      if (ret < 0)
        {
          return ret;
        }

      result |= (uint16_t)byte << (offset * 8);
    }

  *value = result;
  return OK;
}

static int k1_rtl8852bs_post_power_write16(uint32_t address, uint16_t value)
{
  unsigned int offset;
  int ret;

  if ((address & (sizeof(uint16_t) - 1)) != 0 || address > 0x1fffeu)
    {
      return -EINVAL;
    }

  for (offset = 0; offset < sizeof(value); offset++)
    {
      ret = k1_rtl8852bs_write(address + offset,
                                (uint8_t)(value >> (offset * 8)));
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static int k1_rtl8852bs_post_power_update_bits16(uint32_t address,
                                                  uint16_t clear,
                                                  uint16_t set)
{
  uint16_t value;
  int ret;

  ret = k1_rtl8852bs_post_power_read16(address, &value);
  if (ret < 0)
    {
      return ret;
    }

  value &= ~clear;
  value |= set;
  return k1_rtl8852bs_post_power_write16(address, value);
}

static int k1_rtl8852bs_efuse_power(bool enable)
{
  uint16_t iso_value;
  uint8_t pmc_value;
  int ret;

  /* This is enable_efuse_sw_pwr_cut_8852b(..., false) and its matching
   * disable helper.  The write-only unlock byte is intentionally absent.
   */

  ret = k1_rtl8852bs_read(K1_RTL8852BS_PMC_DBG_CTRL2, &pmc_value);
  if (ret < 0)
    {
      return ret;
    }

  if (enable)
    {
      ret = k1_rtl8852bs_write(K1_RTL8852BS_PMC_DBG_CTRL2,
                                pmc_value | K1_RTL8852BS_EFUSE_PMCR_WRITE);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_post_power_read16(K1_RTL8852BS_SYS_ISO_CTRL,
                                            &iso_value);
      if (ret < 0)
        {
          goto err_disable;
        }

      ret = k1_rtl8852bs_post_power_write16(
        K1_RTL8852BS_SYS_ISO_CTRL,
        iso_value | K1_RTL8852BS_EFUSE_ISO_ENABLE);
      if (ret < 0)
        {
          goto err_disable;
        }

      up_udelay(K1_RTL8852BS_EFUSE_POWER_DELAY_USEC);
      ret = k1_rtl8852bs_post_power_update_bits16(
        K1_RTL8852BS_SYS_ISO_CTRL, K1_RTL8852BS_EFUSE_ISO_BLOCK,
        K1_RTL8852BS_EFUSE_ISO_STABLE);
      if (ret >= 0)
        {
          return OK;
        }

err_disable:

      /* Preserve the first transfer error, but undo any gate already set. */

      (void)k1_rtl8852bs_efuse_power(false);
      return ret;
    }

  ret = k1_rtl8852bs_post_power_read16(K1_RTL8852BS_SYS_ISO_CTRL,
                                        &iso_value);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_post_power_write16(
    K1_RTL8852BS_SYS_ISO_CTRL, iso_value | K1_RTL8852BS_EFUSE_ISO_BLOCK);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_post_power_update_bits16(
    K1_RTL8852BS_SYS_ISO_CTRL, K1_RTL8852BS_EFUSE_ISO_STABLE, 0);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_EFUSE_POWER_DELAY_USEC);
  ret = k1_rtl8852bs_post_power_update_bits16(
    K1_RTL8852BS_SYS_ISO_CTRL, K1_RTL8852BS_EFUSE_ISO_ENABLE, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_read(K1_RTL8852BS_PMC_DBG_CTRL2, &pmc_value);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_write(K1_RTL8852BS_PMC_DBG_CTRL2,
                             pmc_value & ~K1_RTL8852BS_EFUSE_PMCR_WRITE);
}

static int k1_rtl8852bs_efuse_read_byte(uint16_t address,
                                         FAR uint8_t *result)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  if (result == NULL || address > K1_RTL8852BS_EFUSE_ADDRESS_MASK)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_mac_write32(
    K1_RTL8852BS_EFUSE_CTRL,
    ((uint32_t)address & K1_RTL8852BS_EFUSE_ADDRESS_MASK) <<
    K1_RTL8852BS_EFUSE_ADDRESS_SHIFT);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_EFUSE_READ_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_EFUSE_CTRL, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & K1_RTL8852BS_EFUSE_READY) != 0)
        {
          *result = (uint8_t)(value & K1_RTL8852BS_EFUSE_DATA_MASK);
          return OK;
        }

      up_udelay(K1_RTL8852BS_EFUSE_READ_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_rtl8852bs_read_mss_efuse(FAR uint8_t *external_pn,
                                        FAR uint8_t *customer)
{
  int ret;
  int power_ret;

  if (external_pn == NULL || customer == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_efuse_power(true);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_efuse_read_byte(K1_RTL8852BS_EFUSE_EXTERNAL_PN,
                                      external_pn);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_efuse_read_byte(K1_RTL8852BS_EFUSE_CUSTOMER,
                                          customer);
    }

  /* Closing the read gate is mandatory even if either selector read fails. */

  power_ret = k1_rtl8852bs_efuse_power(false);
  if (ret < 0)
    {
      return ret;
    }

  return power_ret;
}

static int k1_rtl8852bs_read_wlan_efuse(FAR uint8_t *physical)
{
  unsigned int address;
  int power_ret;
  int ret;

  if (physical == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_efuse_power(true);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: WLAN eFuse physical read begin bytes=");
  k1_early_puthex(K1_RTL8852BS_EFUSE_PHYSICAL_SIZE);
  k1_early_puts("\r\n");

  /* This scan has thousands of successful CMD52 operations.  Do not let
   * their register dumps make the UART the limiting part of the diagnostic.
   * SDHCI errors and the final MAC/runtime markers remain logged.
   */

  k1_sdio_wifi_suppress_command_trace(true);
  for (address = 0; address < K1_RTL8852BS_EFUSE_PHYSICAL_SIZE; address++)
    {
      ret = k1_rtl8852bs_efuse_read_byte(address, &physical[address]);
      if (ret < 0)
        {
          break;
        }
    }

  k1_sdio_wifi_suppress_command_trace(false);

  /* The eFuse gate is read-only, but it must still be restored on errors. */

  power_ret = k1_rtl8852bs_efuse_power(false);
  if (ret < 0)
    {
      return ret;
    }

  if (power_ret == OK)
    {
      k1_early_puts("K1 Wi-Fi GPL: WLAN eFuse physical read complete\r\n");
    }

  return power_ret;
}

static int k1_rtl8852bs_decode_wlan_efuse(FAR const uint8_t *physical,
                                           FAR uint8_t *logical)
{
  uint32_t logical_index;
  uint8_t header;
  uint8_t header2;
  uint8_t word_enable;
  unsigned int block;
  unsigned int physical_index;
  unsigned int word;

  if (physical == NULL || logical == NULL)
    {
      return -EINVAL;
    }

  memset(logical, 0xff, K1_RTL8852BS_EFUSE_LOGICAL_SIZE);
  physical_index = K1_RTL8852BS_EFUSE_SEC_CTRL_SIZE;

  while (physical_index < K1_RTL8852BS_EFUSE_PHYSICAL_SIZE)
    {
      header = physical[physical_index++];
      if (header == 0xffu)
        {
          return OK;
        }

      if (physical_index >= K1_RTL8852BS_EFUSE_PHYSICAL_SIZE)
        {
          return -EPROTO;
        }

      header2 = physical[physical_index++];
      block = ((unsigned int)(header & 0x0fu) << 4) |
              ((unsigned int)(header2 & 0xf0u) >> 4);
      word_enable = header2 & 0x0fu;

      for (word = 0; word < 4; word++)
        {
          if ((word_enable & (1u << word)) != 0)
            {
              continue;
            }

          logical_index = ((uint32_t)block << 3) + ((uint32_t)word << 1);
          if (physical_index + 1 >= K1_RTL8852BS_EFUSE_PHYSICAL_SIZE ||
              logical_index + 1 >= K1_RTL8852BS_EFUSE_LOGICAL_SIZE)
            {
              return -EPROTO;
            }

          logical[logical_index] = physical[physical_index++];
          logical[logical_index + 1] = physical[physical_index++];
        }
    }

  return OK;
}

int k1_rtl8852bs_efuse_read_mac(FAR uint8_t *mac)
{
  FAR uint8_t *logical = NULL;
  FAR uint8_t *physical = NULL;
  bool all_ff = true;
  bool all_zero = true;
  unsigned int index;
  int ret;

  if (mac == NULL)
    {
      return -EINVAL;
    }

  physical = kmm_zalloc(K1_RTL8852BS_EFUSE_PHYSICAL_SIZE);
  logical = kmm_zalloc(K1_RTL8852BS_EFUSE_LOGICAL_SIZE);
  if (physical == NULL || logical == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = k1_rtl8852bs_read_wlan_efuse(physical);
  if (ret < 0)
    {
      goto out;
    }

  ret = k1_rtl8852bs_decode_wlan_efuse(physical, logical);
  if (ret < 0)
    {
      goto out;
    }

  memcpy(mac, logical + K1_RTL8852BS_EFUSE_MAC_OFFSET,
         K1_RTL8852BS_EFUSE_MAC_SIZE);
  for (index = 0; index < K1_RTL8852BS_EFUSE_MAC_SIZE; index++)
    {
      if (mac[index] != 0xffu)
        {
          all_ff = false;
        }

      if (mac[index] != 0)
        {
          all_zero = false;
        }
    }

  if (all_ff || all_zero || (mac[0] & 1u) != 0)
    {
      ret = -ENODATA;
      goto out;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 logical MAC=");
  for (index = 0; index < K1_RTL8852BS_EFUSE_MAC_SIZE; index++)
    {
      k1_early_puthex(mac[index]);
      if (index + 1 < K1_RTL8852BS_EFUSE_MAC_SIZE)
        {
          k1_early_puts(":");
        }
    }

  k1_early_puts("\r\n");

out:
  kmm_free(logical);
  kmm_free(physical);
  return ret;
}

#ifdef CONFIG_K1_RTL8852BS2_RF_CONTEXT_DIAGNOSTIC

/* Print one logical eFuse byte and, when the cell is unprogrammed, the
 * default halrf substitutes for it.
 */

static void k1_rtl8852bs_rf_context_byte(FAR const char *name,
                                         uint8_t value,
                                         uint8_t fallback)
{
  k1_early_puts(name);
  k1_early_puthex(value);
  if (value == K1_RTL8852BS_EFUSE_RF_UNPROGRAMMED)
    {
      k1_early_puts("(blank,default=");
      k1_early_puthex(fallback);
      k1_early_puts(")");
    }
}

/* Count the programmed bytes of one logical eFuse range. */

static unsigned int k1_rtl8852bs_rf_context_count(FAR const uint8_t *logical,
                                                  uint32_t first,
                                                  uint32_t last)
{
  unsigned int programmed = 0;
  uint32_t offset;

  for (offset = first; offset <= last; offset++)
    {
      if (logical[offset] != K1_RTL8852BS_EFUSE_RF_UNPROGRAMMED)
        {
          programmed++;
        }
    }

  return programmed;
}

/* Report the board RF context and cache it for the radio image guard.
 *
 * This reads only.  It must run before the firmware is downloaded: the eFuse
 * read sequence drives the power-cut and isolation registers, and the host
 * indirect register window stops returning live values once the WCPU owns the
 * chip.  Nothing here is written to the eFuse or to persistent storage.
 */

int k1_rtl8852bs_fwdl_rf_context_diagnostic(void)
{
  FAR struct k1_rtl8852bs_rf_context_s *context;
  FAR uint8_t *logical = NULL;
  FAR uint8_t *physical = NULL;
  uint8_t cv_byte = 0;
  int ret;

  context = &g_k1_rtl8852bs_rf_context;
  memset(context, 0, sizeof(struct k1_rtl8852bs_rf_context_s));

  k1_early_puts("K1 Wi-Fi GPL: RF context read begin\r\n");

  ret = k1_rtl8852bs_read(K1_RTL8852BS_SYS_CFG1_CV_BYTE, &cv_byte);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF context SYS_CFG1 read failed ret=");
      k1_early_puthex((uintreg_t)(-ret));
      k1_early_puts("\r\n");
      return ret;
    }

  physical = kmm_zalloc(K1_RTL8852BS_EFUSE_PHYSICAL_SIZE);
  logical = kmm_zalloc(K1_RTL8852BS_EFUSE_LOGICAL_SIZE);
  if (physical == NULL || logical == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = k1_rtl8852bs_read_wlan_efuse(physical);
  if (ret < 0)
    {
      goto out;
    }

  ret = k1_rtl8852bs_decode_wlan_efuse(physical, logical);
  if (ret < 0)
    {
      goto out;
    }

  context->cv_byte = cv_byte;
  context->chip_cv = cv_byte >> K1_RTL8852BS_SYS_CFG1_CV_SHIFT;
  context->rfe_efuse = logical[K1_RTL8852BS_EFUSE_RF_RFE_TYPE];
  context->board_option = logical[K1_RTL8852BS_EFUSE_RF_BOARD_OPTION];
  context->chan_plan = logical[K1_RTL8852BS_EFUSE_RF_CHAN_PLAN];
  context->xtal = logical[K1_RTL8852BS_EFUSE_RF_XTAL];
  context->thermal_a = logical[K1_RTL8852BS_EFUSE_RF_THERMAL_A];
  context->thermal_b = logical[K1_RTL8852BS_EFUSE_RF_THERMAL_B];
  context->tssi_de_programmed = (uint16_t)k1_rtl8852bs_rf_context_count(
    logical, K1_RTL8852BS_EFUSE_RF_TSSI_DE_FIRST,
    K1_RTL8852BS_EFUSE_RF_TSSI_DE_LAST);
  context->gain_k_programmed = (uint8_t)k1_rtl8852bs_rf_context_count(
    logical, K1_RTL8852BS_EFUSE_RF_GAIN_K_FIRST,
    K1_RTL8852BS_EFUSE_RF_GAIN_K_LAST);

  /* hal_rfe_type_chk() rejects an unprogrammed RFE cell unless the vendor
   * bypass_rfe_chk flag is set, in which case halrf falls back to its own
   * default RFE type.  Record which of the two the guard will use.
   */

  if (context->rfe_efuse == K1_RTL8852BS_EFUSE_RF_UNPROGRAMMED)
    {
      context->rfe_type = K1_RTL8852BS_EFUSE_RF_DEFAULT_RFE;
      context->rfe_default = true;
    }
  else
    {
      context->rfe_type = context->rfe_efuse;
      context->rfe_default = false;
    }

  context->valid = true;

  k1_early_puts("K1 Wi-Fi GPL: RF context SYS_CFG1[0x00f1]=");
  k1_early_puthex(context->cv_byte);
  k1_early_puts(" chip CV=");
  k1_early_puthex(context->chip_cv);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: RF context ");
  k1_rtl8852bs_rf_context_byte("rfe[0x2ca]=", context->rfe_efuse,
                               K1_RTL8852BS_EFUSE_RF_DEFAULT_RFE);
  k1_rtl8852bs_rf_context_byte(" board_option[0x2c1]=",
                               context->board_option, 0x01u);
  k1_rtl8852bs_rf_context_byte(" chan_plan[0x2b8]=",
                               context->chan_plan, 0x7fu);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: RF context ");
  k1_rtl8852bs_rf_context_byte("xtal[0x2b9]=", context->xtal, 0x3fu);
  k1_rtl8852bs_rf_context_byte(" thermal_a[0x2d0]=",
                               context->thermal_a, 0x22u);
  k1_rtl8852bs_rf_context_byte(" thermal_b[0x2d1]=",
                               context->thermal_b, 0x22u);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: RF context tssi_de[0x210..0x259] "
                "programmed=");
  k1_early_puthex(context->tssi_de_programmed);
  k1_early_puts("/0x4a rx_gain_k[0x2d4..0x2dd] programmed=");
  k1_early_puthex(context->gain_k_programmed);
  k1_early_puts("/0x0a\r\n");

  /* A blank RFE cell together with a blank TSSI de-emphasis and RX gain
   * range means this board ships without RF calibration data in the eFuse.
   * The radio parameter image still applies, but TSSI and RX gain stay at
   * their table defaults, so absolute TX power and RX sensitivity are
   * uncalibrated.  Say so instead of letting a later stage assume otherwise.
   */

  k1_early_puts("K1 Wi-Fi GPL: RF context calibration=");
  if (context->tssi_de_programmed == 0 && context->gain_k_programmed == 0)
    {
      k1_early_puts("absent(table defaults, uncalibrated TSSI and RX gain)");
    }
  else
    {
      k1_early_puts("present");
    }

  k1_early_puts(" rfe_source=");
  k1_early_puts(context->rfe_default ? "halrf-default" : "efuse");
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RF context read complete\r\n");

out:
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF context read failed ret=");
      k1_early_puthex((uintreg_t)(-ret));
      k1_early_puts("\r\n");
    }

  kmm_free(logical);
  kmm_free(physical);
  return ret;
}

#endif

int k1_rtl8852bs_fwdl_mss_efuse_diagnostic(void)
{
  uint8_t external_pn;
  uint8_t customer;
  uint8_t raw_device_type;
  uint8_t device_type;
  uint8_t customer_index;
  uint8_t key_number;
  int ret;

  /* This reports the selector format used by newer MSSKPOOL images.  The U2
   * NIC image embedded by this component uses the separate legacy MSSC=2
   * branch, which the signature diagnostic handles below.
   */

  ret = k1_rtl8852bs_read_mss_efuse(&external_pn, &customer);
  if (ret < 0)
    {
      return ret;
    }

  raw_device_type = external_pn & 0x0fu;
  device_type = raw_device_type;
  customer_index = 0x1fu -
    (((external_pn >> 4) & 0x0fu) | ((customer >> 6) & 0x01u) << 4);
  key_number = 0x0fu - (customer & 0x0fu);

  /* RTL8852B's original compatibility handling has this exact selector;
   * then its MSS key-pool code converts valid device types to pool indices.
   */

  if (external_pn == 0xffu && customer == 0x6eu)
    {
      device_type = 0x0au;
      customer_index = 0;
      key_number = 0;
    }

  switch (device_type)
    {
      case 0x0cu:
        device_type = 0;
        break;

      case 0x0au:
        device_type = 1;
        break;

      case 0x09u:
        device_type = 2;
        break;

      case 0x06u:
        device_type = 3;
        break;

      case 0x0fu:
        break;

      default:
        return -EPROTO;
    }

  k1_early_puts("K1 Wi-Fi GPL: MSS eFuse external-pn=");
  k1_early_puthex(external_pn);
  k1_early_puts(" customer=");
  k1_early_puthex(customer);
  k1_early_puts(" raw-device-type=");
  k1_early_puthex(raw_device_type);
  k1_early_puts(" pool-device-type=");
  k1_early_puthex(device_type);
  k1_early_puts(" customer-index=");
  k1_early_puthex(customer_index);
  k1_early_puts(" key-number=");
  k1_early_puthex(key_number);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 MSS eFuse "
                "diagnostic complete\r\n");
  return OK;
}

static int k1_rtl8852bs_legacy_mss_index(uint8_t external_pn,
                                          uint8_t customer,
                                          FAR uint8_t *signature_index,
                                          FAR uint8_t *decoded_external_pn,
                                          FAR uint8_t *decoded_customer,
                                          FAR uint8_t *decoded_serial,
                                          FAR bool *mapped)
{
  static const uint8_t g_external_pn[K1_RTL8852BS_OTP_KEY_INFO_COUNT] =
  {
    0x00u, 0x00u
  };

  static const uint8_t g_customer[K1_RTL8852BS_OTP_KEY_INFO_COUNT] =
  {
    0x00u, 0x01u
  };

  static const uint8_t g_serial[K1_RTL8852BS_OTP_KEY_INFO_COUNT] =
  {
    0x00u, 0x01u
  };

  uint8_t index;

  if (signature_index == NULL || decoded_external_pn == NULL ||
      decoded_customer == NULL || decoded_serial == NULL || mapped == NULL)
    {
      return -EINVAL;
    }

  /* This is the original __mss_index() decoding and its deliberate index-0
   * fallback when no OTP tuple matches either legacy table entry.
   */

  *decoded_external_pn = 0xffu - external_pn;
  *decoded_customer = 0x0fu - (customer & 0x0fu);
  *decoded_serial = 0x07u - ((customer >> 4) & 0x07u);
  *signature_index = 0;
  *mapped = false;

  for (index = 0; index < K1_RTL8852BS_OTP_KEY_INFO_COUNT; index++)
    {
      if (*decoded_external_pn == g_external_pn[index] &&
          *decoded_customer == g_customer[index] &&
          *decoded_serial == g_serial[index])
        {
          *signature_index = index;
          *mapped = true;
          break;
        }
    }

  return OK;
}

int k1_rtl8852bs_fwdl_mss_legacy_signature_diagnostic(void)
{
  FAR const uint8_t *signature;
  uint32_t header_length;
  uint32_t image_words;
  uint32_t section_words;
  uint32_t section_length;
  uint32_t section_offset;
  uint32_t secure_offset;
  uint32_t secure_length;
  uint32_t secure_mssc;
  uint32_t signature_source;
  uint32_t signature_target;
  uint32_t signature_first_word;
  uint32_t signature_last_word;
  uint32_t section_count;
  uint32_t section_index;
  uint8_t section_type;
  uint8_t external_pn;
  uint8_t customer;
  uint8_t decoded_external_pn;
  uint8_t decoded_customer;
  uint8_t decoded_serial;
  uint8_t signature_index;
  bool mapped;
  int ret;

  /* fwhdr_parser() takes this branch when the 8852B secure section has
   * MSSC=2.  It selects a 512-byte trailer signature, copies it into the
   * secure section at offset 448, then removes the 1024-byte trailer from
   * the transmit length.  This diagnostic proves those calculations but
   * does not make the copy or perform any firmware transfer.
   */

  header_length = (k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + 12) >> 16) & 0xffu;
  image_words = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + 24);
  section_count = (image_words >> 8) & 0xffu;
  if (header_length != K1_RTL8852BS_FW_HEADER_SIZE ||
      section_count <= K1_RTL8852BS_FW_SECURITY_SECTION_INDEX ||
      section_count > K1_RTL8852BS_FW_SECTION_MAX_COUNT)
    {
      return -EPROTO;
    }

  section_offset = header_length;
  secure_offset = 0;
  secure_length = 0;
  secure_mssc = 0;
  for (section_index = 0; section_index < section_count; section_index++)
    {
      section_words = k1_rtl8852bs_read_le32(
        g_k1_rtl8852bs_u2_nicce_image +
        K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
        section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE + 4);
      section_length = section_words & K1_RTL8852BS_FW_SECTION_SIZE_MASK;
      section_type =
        (section_words >> K1_RTL8852BS_FW_SECTION_TYPE_SHIFT) &
        K1_RTL8852BS_FW_SECTION_TYPE_MASK;
      if ((section_words & K1_RTL8852BS_FW_SECTION_CHECKSUM) != 0)
        {
          section_length += K1_RTL8852BS_FW_SECTION_CHECKSUM_SIZE;
        }

      if (section_length == 0 ||
          section_offset > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
          section_length > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - section_offset)
        {
          return -EPROTO;
        }

      if (section_index == K1_RTL8852BS_FW_SECURITY_SECTION_INDEX)
        {
          if (section_type != K1_RTL8852BS_FW_SECURITY_SECTION_TYPE)
            {
              return -EPROTO;
            }

          secure_offset = section_offset;
          secure_length = section_length;
          secure_mssc = k1_rtl8852bs_read_le32(
            g_k1_rtl8852bs_u2_nicce_image +
            K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
            section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE + 8);
        }

      section_offset += section_length;
    }

  if (secure_length != K1_RTL8852BS_FW_SECURITY_SECTION_SIZE ||
      secure_mssc != K1_RTL8852BS_FW_LEGACY_MSS_SIG_COUNT ||
      section_offset > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
      K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - section_offset !=
      K1_RTL8852BS_FW_LEGACY_MSS_TRAILER_SIZE)
    {
      return -EPROTO;
    }

  signature_target = secure_offset + K1_RTL8852BS_FW_SECURITY_SIG_OFFSET;
  if (signature_target > secure_offset + secure_length ||
      K1_RTL8852BS_FW_SECURITY_SIG_SIZE >
      secure_offset + secure_length - signature_target)
    {
      return -EPROTO;
    }

  ret = k1_rtl8852bs_read_mss_efuse(&external_pn, &customer);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_legacy_mss_index(external_pn, customer,
                                       &signature_index,
                                       &decoded_external_pn,
                                       &decoded_customer, &decoded_serial,
                                       &mapped);
  if (ret < 0)
    {
      return ret;
    }

  if (signature_index >= K1_RTL8852BS_FW_LEGACY_MSS_SIG_COUNT)
    {
      return -EPROTO;
    }

  signature_source = section_offset +
    (uint32_t)signature_index * K1_RTL8852BS_FW_SECURITY_SIG_SIZE;
  if (signature_source > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
      K1_RTL8852BS_FW_SECURITY_SIG_SIZE >
      K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - signature_source)
    {
      return -EPROTO;
    }

  signature = g_k1_rtl8852bs_u2_nicce_image + signature_source;
  signature_first_word = k1_rtl8852bs_read_le32(signature);
  signature_last_word = k1_rtl8852bs_read_le32(
    signature + K1_RTL8852BS_FW_SECURITY_SIG_SIZE - sizeof(uint32_t));

  k1_early_puts("K1 Wi-Fi GPL: MSS legacy external-pn=");
  k1_early_puthex(external_pn);
  k1_early_puts(" customer=");
  k1_early_puthex(customer);
  k1_early_puts(" decoded-external-pn=");
  k1_early_puthex(decoded_external_pn);
  k1_early_puts(" decoded-customer=");
  k1_early_puthex(decoded_customer);
  k1_early_puts(" serial=");
  k1_early_puthex(decoded_serial);
  k1_early_puts(" index=");
  k1_early_puthex(signature_index);
  k1_early_puts(" mapped=");
  k1_early_puthex(mapped ? 1 : 0);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: MSS legacy signature source=");
  k1_early_puthex(signature_source);
  k1_early_puts(" target=");
  k1_early_puthex(signature_target);
  k1_early_puts(" bytes=");
  k1_early_puthex(K1_RTL8852BS_FW_SECURITY_SIG_SIZE);
  k1_early_puts(" first=");
  k1_early_puthex(signature_first_word);
  k1_early_puts(" last=");
  k1_early_puthex(signature_last_word);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 MSS legacy signature "
                "diagnostic complete\r\n");
  return OK;
}

int k1_rtl8852bs_fwdl_image_layout_diagnostic(void)
{
  uint32_t header_words;
  uint32_t header_length;
  uint32_t image_words;
  uint32_t firmware_words;
  uint32_t section_length;
  uint32_t section_words;
  uint32_t dynamic_length;
  uint32_t section_offset;
  uint32_t dynamic_offset;
  uint32_t dynamic_consumed;
  uint32_t section_count;
  uint32_t dynamic_count;
  uint32_t section_index;
  uint32_t dynamic_index;
  uint16_t dynamic_entry_length;
  uint8_t section_type;
  bool section_checksum;

  /* This mirrors fwhdr_hdr_parser(), fwhdr_section_parser(), and
   * mac_get_dynamic_hdr_ax().  It performs no SDIO transaction: the purpose
   * is to make the exact upstream image boundaries visible before firmware
   * transmission expands beyond the already verified first four packets.
   */

  header_words = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + 12);
  header_length = (header_words >> 16) & 0xffu;
  image_words = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + 24);
  firmware_words = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + 28);
  section_count = (image_words >> 8) & 0xffu;

  if (header_length != K1_RTL8852BS_FW_HEADER_SIZE ||
      section_count == 0 ||
      section_count > K1_RTL8852BS_FW_SECTION_MAX_COUNT ||
      (firmware_words & (1u << 16)) == 0)
    {
      return -EPROTO;
    }

  if (K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
      section_count * K1_RTL8852BS_FW_SECTION_HEADER_SIZE +
      K1_RTL8852BS_FW_HEADER_DYNAMIC_SIZE != header_length)
    {
      return -EPROTO;
    }

  dynamic_length = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + K1_RTL8852BS_FW_HEADER_STATIC_SIZE);
  dynamic_count = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + K1_RTL8852BS_FW_HEADER_STATIC_SIZE + 4);
  if (dynamic_length != K1_RTL8852BS_FW_HEADER_DYNAMIC_SIZE ||
      dynamic_count == 0)
    {
      return -EPROTO;
    }

  dynamic_offset = 8;
  dynamic_consumed = 8;
  for (dynamic_index = 0; dynamic_index < dynamic_count; dynamic_index++)
    {
      if (dynamic_offset + 4 > dynamic_length)
        {
          return -EPROTO;
        }

      dynamic_entry_length =
        (uint16_t)g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_HEADER_STATIC_SIZE + dynamic_offset] |
        (uint16_t)g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_HEADER_STATIC_SIZE + dynamic_offset + 1] << 8;
      if (dynamic_entry_length < 4 ||
          dynamic_offset + dynamic_entry_length > dynamic_length)
        {
          return -EPROTO;
        }

      dynamic_offset += dynamic_entry_length;
      dynamic_consumed += dynamic_entry_length;
    }

  dynamic_consumed = (dynamic_consumed + 15u) & ~15u;
  if (dynamic_consumed != dynamic_length)
    {
      return -EPROTO;
    }

  k1_early_puts("K1 Wi-Fi GPL: U2 NICCE layout header=");
  k1_early_puthex(header_length);
  k1_early_puts(" static=");
  k1_early_puthex(K1_RTL8852BS_FW_HEADER_STATIC_SIZE);
  k1_early_puts(" dynamic=");
  k1_early_puthex(dynamic_length);
  k1_early_puts(" sections=");
  k1_early_puthex(section_count);
  k1_early_puts(" image=");
  k1_early_puthex(K1_RTL8852BS_U2_NICCE_IMAGE_SIZE);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: U2 NICCE dynamic entries=");
  k1_early_puthex(dynamic_count);
  k1_early_puts(" consumed=");
  k1_early_puthex(dynamic_consumed);
  k1_early_puts("\r\n");

  section_offset = header_length;
  for (section_index = 0; section_index < section_count; section_index++)
    {
      header_words = k1_rtl8852bs_read_le32(
        g_k1_rtl8852bs_u2_nicce_image +
        K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
        section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE);
      section_words = k1_rtl8852bs_read_le32(
        g_k1_rtl8852bs_u2_nicce_image +
        K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
        section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE + 4);
      section_length = section_words & K1_RTL8852BS_FW_SECTION_SIZE_MASK;
      section_checksum =
        (section_words & K1_RTL8852BS_FW_SECTION_CHECKSUM) != 0;
      section_type = (section_words >> K1_RTL8852BS_FW_SECTION_TYPE_SHIFT) &
                     K1_RTL8852BS_FW_SECTION_TYPE_MASK;
      if (section_checksum)
        {
          section_length += K1_RTL8852BS_FW_SECTION_CHECKSUM_SIZE;
        }

      if (section_length == 0 ||
          section_offset > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
          section_length > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - section_offset)
        {
          return -EPROTO;
        }

      k1_early_puts("K1 Wi-Fi GPL: U2 NICCE section=");
      k1_early_puthex(section_index);
      k1_early_puts(" offset=");
      k1_early_puthex(section_offset);
      k1_early_puts(" length=");
      k1_early_puthex(section_length);
      k1_early_puts(" address=");
      k1_early_puthex(header_words);
      k1_early_puts(" type=");
      k1_early_puthex(section_type);
      k1_early_puts(" checksum=");
      k1_early_puthex(section_checksum ? 1 : 0);
      k1_early_puts(" mssc=");
      k1_early_puthex(k1_rtl8852bs_read_le32(
        g_k1_rtl8852bs_u2_nicce_image +
        K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
        section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE + 8));
      k1_early_puts("\r\n");

      section_offset += section_length;
    }

  k1_early_puts("K1 Wi-Fi GPL: U2 NICCE section end=");
  k1_early_puthex(section_offset);
  k1_early_puts(" trailer=");
  k1_early_puthex(K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - section_offset);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: U2 NICCE layout complete\r\n");
  return OK;
}

static int k1_rtl8852bs_h2c_resource_read(FAR uint16_t *available_pages)
{
  uint8_t page_status[K1_RTL8852BS_SDIO_TXPG_WP_SIZE];
  uint32_t status;
  int ret;

  /* ud_fs_8852b() reads the whole SDIO TX-page window with CMD53, then uses
   * fs0[28:16] for channel 12.  Keep the same transaction shape here.
   */

  ret = k1_sdio_wifi_read(1, K1_RTL8852BS_SDIO_TXPG_WP, true,
                           page_status, sizeof(page_status));
  if (ret < 0)
    {
      return ret;
    }

  status = k1_rtl8852bs_read_le32(page_status);
  *available_pages = (status >> K1_RTL8852BS_H2C_AVAL_PAGE_SHIFT) &
                     K1_RTL8852BS_H2C_AVAL_PAGE_MASK;
  return OK;
}

static int k1_rtl8852bs_sdio_local_read32(uint32_t address,
                                           FAR uint32_t *value)
{
  uint8_t data[sizeof(*value)];
  int ret;

  ret = k1_sdio_wifi_read(1, address, true, data, sizeof(data));
  if (ret < 0)
    {
      return ret;
    }

  *value = k1_rtl8852bs_read_le32(data);
  return OK;
}

static bool k1_rtl8852bs_data_tx_channel_supported(uint8_t channel)
{
  /* hfc_chcfg_sdio_8852b() assigns pages only to ACH0--3 and B0MG/B0HI.
   * The K1 HCI flow-control setup writes that same six-channel profile.
   */

  return channel < K1_RTL8852BS_DATA_TX_CHANNEL_MAX &&
         (channel <= 3u || channel == 8u || channel == 9u);
}

static int k1_rtl8852bs_data_tx_resources_read(
  uint8_t channel, FAR struct k1_rtl8852bs_data_tx_resources_s *resources)
{
  uint8_t page_status[K1_RTL8852BS_SDIO_TXPG_WP_SIZE];
  uint32_t status;
  unsigned int status_word;
  unsigned int status_shift;
  int ret;

  if (resources == NULL ||
      !k1_rtl8852bs_data_tx_channel_supported(channel))
    {
      return -EINVAL;
    }

  /* ud_fs_8852b() obtains the data-channel used-page counters from this
   * one 28-byte window.  No local accounting is retained: every preflight
   * starts from the device snapshot so a later queue cannot double-reserve.
   */

  ret = k1_sdio_wifi_read(1, K1_RTL8852BS_SDIO_TXPG_WP, true,
                           page_status, sizeof(page_status));
  if (ret < 0)
    {
      return ret;
    }

  status = k1_rtl8852bs_read_le32(page_status);
  resources->wp_available_pages =
    status & K1_RTL8852BS_H2C_AVAL_PAGE_MASK;
  status_word = 1u + channel / 2u;
  status_shift = (channel & 1u) == 0 ? 0u : 16u;
  status = k1_rtl8852bs_read_le32(page_status + status_word * 4u);
  resources->channel_used_pages =
    (status >> status_shift) & K1_RTL8852BS_H2C_AVAL_PAGE_MASK;
  resources->channel_max_pages = K1_RTL8852BS_DATA_TX_CHANNEL_PAGES;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_data_tx_build
 *
 * Description:
 *   Build the 24-byte RTL8852B SDIO normal-data TX descriptor and calculate
 *   its fixed-address FIFO encoding.  This is deliberately a pure memory
 *   operation: management, station, encryption, and ownership decisions
 *   remain outside the chip layer.
 ****************************************************************************/

int k1_rtl8852bs_runtime_data_tx_build(
  FAR const struct k1_rtl8852bs_data_tx_info_s *info,
  FAR uint8_t *descriptor, size_t descriptor_length,
  FAR struct k1_rtl8852bs_data_tx_layout_s *layout)
{
  static const uint8_t qsel_by_tid[] =
  {
    0u, 1u, 1u, 0u, 2u, 2u, 3u, 3u
  };

  static const uint8_t tid_ind_by_tid[] =
  {
    0u, 0u, 1u, 1u, 0u, 1u, 0u, 1u
  };

  uint32_t descriptor0;
  uint32_t descriptor2;
  uint32_t descriptor3;
  uint32_t length_units;
  uint32_t total_length;
  uint32_t ple_length;

  if (info == NULL || descriptor == NULL || layout == NULL)
    {
      return -EINVAL;
    }

  if (descriptor_length < K1_RTL8852BS_DATA_TX_DESCRIPTOR_SIZE ||
      info->packet_length == 0 ||
      info->packet_length > K1_RTL8852BS_H2C_TXD_LENGTH_MASK ||
      !k1_rtl8852bs_data_tx_channel_supported(info->dma_channel) ||
      info->tid >= sizeof(qsel_by_tid) ||
      info->mac_id > K1_RTL8852BS_DATA_TXD_MACID_MASK || info->wmm != 0 ||
      info->sequence > K1_RTL8852BS_DATA_TXD_SEQUENCE_MASK)
    {
      return -EINVAL;
    }

  /* txdes_proc_data_8852b() uses STF mode on SDIO.  Until the MAC control
   * path exists, leave WDINFO, crypto, aggregation, and HW sequence control
   * clear; the supplied 802.11 header owns its sequence number.
   */

  descriptor0 = K1_RTL8852BS_DATA_TXD_STF_MODE |
                ((uint32_t)info->dma_channel <<
                 K1_RTL8852BS_DATA_TXD_CH_DMA_SHIFT);
  descriptor2 = info->packet_length |
                ((uint32_t)qsel_by_tid[info->tid] <<
                 K1_RTL8852BS_DATA_TXD_QSEL_SHIFT) |
                ((uint32_t)info->mac_id <<
                 K1_RTL8852BS_DATA_TXD_MACID_SHIFT);
  if (tid_ind_by_tid[info->tid] != 0)
    {
      descriptor2 |= K1_RTL8852BS_DATA_TXD_TID_IND;
    }

  descriptor3 = info->sequence;
  memset(descriptor, 0, K1_RTL8852BS_DATA_TX_DESCRIPTOR_SIZE);
  k1_rtl8852bs_write_le32(descriptor, descriptor0);
  k1_rtl8852bs_write_le32(descriptor + 8, descriptor2);
  k1_rtl8852bs_write_le32(descriptor + 12, descriptor3);

  total_length = K1_RTL8852BS_DATA_TX_DESCRIPTOR_SIZE +
                 info->packet_length;
  length_units = (total_length + K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1u) /
                 K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  if (length_units == 0 || length_units > K1_RTL8852BS_H2C_TX_UNIT_MASK)
    {
      return -E2BIG;
    }

  ple_length = info->packet_length + K1_RTL8852BS_DATA_TX_PLE_RESERVED +
               K1_RTL8852BS_DATA_TX_PAYLOAD_DESC;
  layout->fifo_address = K1_RTL8852BS_H2C_TX_FIFO_BASE |
                         ((uint32_t)info->dma_channel <<
                          K1_RTL8852BS_H2C_TX_FIFO_SHIFT) |
                         length_units;
  layout->transfer_length =
    length_units * K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  layout->required_ple_pages =
    ((ple_length + K1_RTL8852BS_H2C_PLE_PAGE_SIZE - 1u) /
     K1_RTL8852BS_H2C_PLE_PAGE_SIZE) << 1u;
  layout->required_wde_pages = K1_RTL8852BS_DATA_TX_WDE_PAGES;

  if (k1_rtl8852bs_read_le32(descriptor) != descriptor0 ||
      k1_rtl8852bs_read_le32(descriptor + 8) != descriptor2 ||
      k1_rtl8852bs_read_le32(descriptor + 12) != descriptor3)
    {
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_data_tx_preflight
 *
 * Description:
 *   Combine the pure descriptor calculation with a fresh SDIO data-channel
 *   flow-control snapshot.  It never reserves pages or writes TXFF; a future
 *   LPWORK TX queue must call this immediately before its FIFO transaction.
 ****************************************************************************/

int k1_rtl8852bs_runtime_data_tx_preflight(
  FAR const struct k1_rtl8852bs_data_tx_info_s *info,
  FAR uint8_t *descriptor, size_t descriptor_length,
  FAR struct k1_rtl8852bs_data_tx_layout_s *layout,
  FAR struct k1_rtl8852bs_data_tx_resources_s *resources)
{
  int ret;

  if (info == NULL || resources == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_runtime_data_tx_build(info, descriptor,
                                            descriptor_length, layout);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_data_tx_resources_read(info->dma_channel, resources);
  if (ret < 0)
    {
      return ret;
    }

  if (resources->channel_used_pages > resources->channel_max_pages)
    {
      return -EIO;
    }

  if (layout->required_wde_pages >
      resources->channel_max_pages - resources->channel_used_pages ||
      resources->wp_available_pages <
      layout->required_ple_pages + K1_RTL8852BS_DATA_TX_PLE_RESERVE)
    {
      return -ENOSPC;
    }

  return OK;
}

int k1_rtl8852bs_fwdl_runtime_data_tx_diagnostic(void)
{
  uint8_t descriptor[K1_RTL8852BS_DATA_TX_DESCRIPTOR_SIZE];
  struct k1_rtl8852bs_data_tx_info_s info =
  {
    .packet_length = 24u,
    .sequence = 0u,
    .dma_channel = 0u,
    .tid = 0u,
    .mac_id = 0u,
    .wmm = 0u
  };

  struct k1_rtl8852bs_data_tx_layout_s layout;
  struct k1_rtl8852bs_data_tx_resources_s resources;
  int ret;

  /* The minimum 24-byte input models only a well-formed 802.11 header.  The
   * diagnostic does not submit it because MACID 0 has not been configured
   * with a peer or security context at this stage of the migration.
   */

  ret = k1_rtl8852bs_runtime_data_tx_preflight(
    &info, descriptor, sizeof(descriptor), &layout, &resources);
  if (ret < 0)
    {
      goto error;
    }

  if (k1_rtl8852bs_read_le32(descriptor) != 0x00000400u ||
      k1_rtl8852bs_read_le32(descriptor + 8) != 24u ||
      k1_rtl8852bs_read_le32(descriptor + 12) != 0u ||
      layout.fifo_address != 0x00010006u || layout.transfer_length != 48u ||
      layout.required_ple_pages != 2u || layout.required_wde_pages != 1u)
    {
      ret = -EIO;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime data TX preflight WP=");
  k1_early_puthex(resources.wp_available_pages);
  k1_early_puts(" ACH0-used=");
  k1_early_puthex(resources.channel_used_pages);
  k1_early_puts(" WDE=");
  k1_early_puthex(layout.required_wde_pages);
  k1_early_puts(" PLE=");
  k1_early_puthex(layout.required_ple_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(layout.fifo_address);
  k1_early_puts(" TXD0=");
  k1_early_puthex(k1_rtl8852bs_read_le32(descriptor));
  k1_early_puts(" TXD2=");
  k1_early_puthex(k1_rtl8852bs_read_le32(descriptor + 8));
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime data TX descriptor "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: runtime data TX descriptor error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_mac_core_init
 *
 * Description:
 *   Apply and verify the independently usable static MAC control fields from
 *   the RTL8852B MAC_AX_TRX_SW_MODE initialization.  This intentionally
 *   stops before address CAM, MACID, role, station, security, scan, or
 *   association configuration, and it never submits an 802.11 frame.
 ****************************************************************************/

int k1_rtl8852bs_runtime_mac_core_init(void)
{
  uint32_t firmware_control;
  uint32_t platform_enable;
  uint32_t sys_clock_control;
  unsigned int index;
  int ret;

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL,
                                &firmware_control);
  if (ret < 0)
    {
      return ret;
    }

  if (((firmware_control & K1_RTL8852BS_WCPU_FWDL_STATUS_MASK) >>
       K1_RTL8852BS_WCPU_FWDL_STATUS_SHIFT) !=
      K1_RTL8852BS_FWDL_READY)
    {
      return -EPIPE;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PLATFORM_ENABLE,
                                &platform_enable);
  if (ret < 0)
    {
      return ret;
    }

  if ((platform_enable & K1_RTL8852BS_WCPU_ENABLE) == 0)
    {
      return -EPIPE;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SYS_CLK_CTRL,
                                &sys_clock_control);
  if (ret < 0)
    {
      return ret;
    }

  if ((sys_clock_control & K1_RTL8852BS_CPU_CLK_ENABLE) == 0)
    {
      return -EPIPE;
    }

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_runtime_mac_core_fields) /
               sizeof(g_k1_rtl8852bs_runtime_mac_core_fields[0]);
       index++)
    {
      ret = k1_rtl8852bs_mac_update_field_checked(
        g_k1_rtl8852bs_runtime_mac_core_fields[index].address,
        g_k1_rtl8852bs_runtime_mac_core_fields[index].mask,
        g_k1_rtl8852bs_runtime_mac_core_fields[index].value);
      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: runtime MAC core field=");
          k1_early_puthex(index);
          k1_early_puts(" address=");
          k1_early_puthex(
            g_k1_rtl8852bs_runtime_mac_core_fields[index].address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts("\r\n");
          return ret;
        }
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime MAC core fields=");
  k1_early_puthex(index);
  k1_early_puts("\r\n");
  return OK;
}

int k1_rtl8852bs_fwdl_runtime_mac_core_diagnostic(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime MAC core init begin\r\n");
  ret = k1_rtl8852bs_runtime_mac_core_init();
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime MAC core init "
                "complete\r\n");
  return OK;
}

static int k1_rtl8852bs_runtime_mac_function_enable(void)
{
  uint32_t value;
  int ret;

  /* mac_sys_init() writes these values after firmware is ready.  The earlier
   * HCI/DMAC pre-init intentionally leaves scheduler, TMAC, RMAC, and
   * protocol-top blocks disabled for firmware download.
   */

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_DMAC_FUNC_EN,
                                 K1_RTL8852BS_DMAC_FUNC_EN_FULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_DMAC_CLK_EN,
                                 K1_RTL8852BS_DMAC_CLK_EN_FULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_CMAC_CLK_EN,
                                 K1_RTL8852BS_CMAC_CLK_EN_VALUE);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_CMAC_FUNC_EN,
                                 K1_RTL8852BS_CMAC_FUNC_EN_VALUE);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_DMAC_FUNC_EN, &value);
  if (ret < 0 || (value & K1_RTL8852BS_DMAC_FUNC_EN_FULL) !=
                 K1_RTL8852BS_DMAC_FUNC_EN_FULL)
    {
      return ret < 0 ? ret : -EIO;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_CMAC_FUNC_EN, &value);
  if (ret < 0 || (value & K1_RTL8852BS_CMAC_FUNC_EN_VALUE) !=
                 K1_RTL8852BS_CMAC_FUNC_EN_VALUE)
    {
      return ret < 0 ? ret : -EIO;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 DMAC/CMAC function enable "
                "complete\r\n");
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_fwrole_maintain_build
 *
 * Description:
 *   Serialize the four-byte MEDIA_RPT/FWROLE_MAINTAIN payload used by
 *   mac_fw_role_maintain() in the recorded Realtek driver.  This helper
 *   performs no H2C submission; role lifetime and completion handling belong
 *   to the future runtime control-plane state machine.
 ****************************************************************************/

int k1_rtl8852bs_runtime_fwrole_maintain_build(
  FAR const struct k1_rtl8852bs_fwrole_maintain_info_s *info,
  FAR uint8_t *content, size_t content_length)
{
  uint32_t word;

  if (info == NULL || content == NULL ||
      content_length != K1_RTL8852BS_FWROLE_MAINTAIN_SIZE ||
      info->self_role > K1_RTL8852BS_SELF_ROLE_MAX ||
      info->update_mode > K1_RTL8852BS_UPDATE_MODE_MAX ||
      info->wifi_role > K1_RTL8852BS_WIFI_ROLE_MAX ||
      info->band != 0 || info->port > K1_RTL8852BS_PORT_MAX)
    {
      return -EINVAL;
    }

  word = (uint32_t)info->mac_id |
         ((uint32_t)info->self_role << 8) |
         ((uint32_t)info->update_mode << 10) |
         ((uint32_t)info->wifi_role << 13) |
         ((uint32_t)info->band << 17) |
         ((uint32_t)info->port << 19);
  k1_rtl8852bs_write_le32(content, word);
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_join_info_build
 *
 * Description:
 *   Serialize the twelve-byte MEDIA_RPT/JOININFO payload used by
 *   mac_h2c_join_info() in the recorded Realtek driver.  The trailing two
 *   reserved dwords are deterministically cleared instead of inheriting
 *   allocator contents.  This helper performs no H2C submission.
 ****************************************************************************/

int k1_rtl8852bs_runtime_join_info_build(
  FAR const struct k1_rtl8852bs_join_info_s *info, FAR uint8_t *content,
  size_t content_length)
{
  uint32_t word;

  if (info == NULL || content == NULL ||
      content_length != K1_RTL8852BS_JOININFO_SIZE ||
      info->band || info->wmm > 1 ||
      info->downlink_bandwidth > 3 ||
      info->trigger_frame_padding > 3 ||
      info->downlink_target_packet_extension > 7 ||
      info->port > K1_RTL8852BS_PORT_MAX ||
      info->network_type > K1_RTL8852BS_NETWORK_TYPE_MAX ||
      info->wifi_role > K1_RTL8852BS_WIFI_ROLE_MAX ||
      info->self_role > K1_RTL8852BS_SELF_ROLE_MAX)
    {
      return -EINVAL;
    }

  word = (uint32_t)info->mac_id |
         ((uint32_t)info->disconnected << 8) |
         ((uint32_t)info->band << 9) |
         ((uint32_t)info->wmm << 10) |
         ((uint32_t)info->trigger << 12) |
         ((uint32_t)info->he_station << 13) |
         ((uint32_t)info->downlink_bandwidth << 14) |
         ((uint32_t)info->trigger_frame_padding << 16) |
         ((uint32_t)info->downlink_target_packet_extension << 18) |
         ((uint32_t)info->port << 21) |
         ((uint32_t)info->network_type << 24) |
         ((uint32_t)info->wifi_role << 26) |
         ((uint32_t)info->self_role << 30);
  memset(content, 0, K1_RTL8852BS_JOININFO_SIZE);
  k1_rtl8852bs_write_le32(content, word);
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_control_plane_diagnostic
 *
 * Description:
 *   Validate the two upstream control-plane payload layouts entirely in RAM.
 *   The samples exercise every non-reserved bit field, including the
 *   RTL8852B band-0 and WMM restrictions.  No role or station is created,
 *   no CAM is configured, and no command reaches the SDIO TX FIFO.
 ****************************************************************************/

int k1_rtl8852bs_fwdl_runtime_control_plane_diagnostic(void)
{
  struct k1_rtl8852bs_fwrole_maintain_info_s role =
  {
    .mac_id = 0x23u,
    .self_role = 2u,
    .update_mode = 6u,
    .wifi_role = 1u,
    .band = 0u,
    .port = 4u
  };

  struct k1_rtl8852bs_join_info_s join =
  {
    .mac_id = 0x23u,
    .wmm = 1u,
    .downlink_bandwidth = 3u,
    .trigger_frame_padding = 2u,
    .downlink_target_packet_extension = 5u,
    .port = 4u,
    .network_type = 2u,
    .wifi_role = 1u,
    .self_role = 0u,
    .disconnected = true,
    .band = false,
    .trigger = true,
    .he_station = false
  };

  uint8_t role_content[K1_RTL8852BS_FWROLE_MAINTAIN_SIZE];
  uint8_t join_content[K1_RTL8852BS_JOININFO_SIZE];
  int ret;

  ret = k1_rtl8852bs_runtime_fwrole_maintain_build(
    &role, role_content, sizeof(role_content));
  if (ret < 0 || k1_rtl8852bs_read_le32(role_content) != 0x00203a23u)
    {
      return ret < 0 ? ret : -EIO;
    }

  ret = k1_rtl8852bs_runtime_join_info_build(
    &join, join_content, sizeof(join_content));
  if (ret < 0 || k1_rtl8852bs_read_le32(join_content) != 0x0696d523u ||
      k1_rtl8852bs_read_le32(join_content + 4) != 0 ||
      k1_rtl8852bs_read_le32(join_content + 8) != 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  join.band = true;
  if (k1_rtl8852bs_runtime_join_info_build(&join, join_content,
                                           sizeof(join_content)) != -EINVAL)
    {
      return -EIO;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime control-plane role=");
  k1_early_puthex(k1_rtl8852bs_read_le32(role_content));
  k1_early_puts(" join=");
  k1_early_puthex(k1_rtl8852bs_read_le32(join_content));
  k1_early_puts(" H2C=1/8/4,1/8/0\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime control-plane "
                "serialization complete\r\n");
  return OK;
}

static bool k1_rtl8852bs_addr_cam_mask_valid(uint8_t address_mask)
{
  return address_mask == 0x3fu || address_mask == 0x1fu ||
         address_mask == 0x0fu || address_mask == 0x07u ||
         address_mask == 0x03u || address_mask == 0x01u ||
         address_mask == 0;
}

static bool k1_rtl8852bs_addr_cam_mac_valid(FAR const uint8_t *mac)
{
  unsigned int index;

  if ((mac[0] & 1u) != 0)
    {
      return false;
    }

  for (index = 0; index < 6; index++)
    {
      if (mac[index] != 0)
        {
          return true;
        }
    }

  return false;
}

static bool k1_rtl8852bs_addr_cam_mac_is_zero(FAR const uint8_t *mac)
{
  unsigned int index;

  for (index = 0; index < 6; index++)
    {
      if (mac[index] != 0)
        {
          return false;
        }
    }

  return true;
}

static uint8_t k1_rtl8852bs_addr_cam_hash(FAR const uint8_t *mac,
                                          uint8_t count)
{
  uint8_t hash = 0;
  unsigned int index;

  for (index = 0; index < count; index++)
    {
      hash ^= mac[index];
    }

  return hash;
}

static uint8_t k1_rtl8852bs_addr_cam_mask_count(uint8_t address_mask)
{
  uint8_t count = 0;

  while (address_mask != 0)
    {
      count++;
      address_mask &= address_mask - 1u;
    }

  return count;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_addr_cam_build
 *
 * Description:
 *   Serialize the 60-byte MAC/ADDR_CAM_UPDATE/ADDRCAM_INFO payload from
 *   fill_addr_cam_info() and fill_bssid_cam_info() in the recorded Realtek
 *   driver.  It supports a non-multicast band-0 no-link station,
 *   infrastructure station, or AP create record with no security,
 *   wake-on-wireless, or ranging state.  A no-link station has only a real
 *   self MAC; target MAC and BSSID must be zero as in the original
 *   role-create path.  Allocation, H2C transmission and C2H done
 *   acknowledgement are intentionally left to the future control-plane state
 *   machine.
 ****************************************************************************/

int k1_rtl8852bs_runtime_addr_cam_build(
  FAR const struct k1_rtl8852bs_addr_cam_info_s *info,
  FAR uint8_t *content, size_t content_length)
{
  uint8_t compare_length;
  uint8_t bssid_mask;
  uint8_t self_hash;
  uint8_t target_hash;
  uint32_t word;

  if (info == NULL || content == NULL ||
      content_length != K1_RTL8852BS_ADDR_CAM_SIZE ||
      info->bssid_cam_index > K1_RTL8852BS_ADDR_CAM_BSSID_INDEX_MAX ||
      info->port > K1_RTL8852BS_PORT_MAX ||
      (info->network_type != 0u && info->network_type != 2u &&
       info->network_type != 3u) ||
      (info->network_type == 0u && info->self_role != 0u) ||
      (info->network_type == 2u && info->self_role != 0u) ||
      (info->network_type == 3u && info->self_role != 1u) ||
      !k1_rtl8852bs_addr_cam_mask_valid(info->address_mask) ||
      info->mask_selection > K1_RTL8852BS_ADDR_CAM_MASK_SELECTION_MAX ||
      info->bss_color > 0x3fu || info->beacon_hit_condition > 3u ||
      info->hit_rule > 3u || info->tsf_sync > 7u ||
      info->target_indicator > 7u || info->frame_target_indicator > 7u ||
      info->aid > 0x0fffu ||
      !k1_rtl8852bs_addr_cam_mac_valid(info->self_mac))
    {
      return -EINVAL;
    }

  if (info->network_type == 0u &&
      (!k1_rtl8852bs_addr_cam_mac_is_zero(info->target_mac) ||
       !k1_rtl8852bs_addr_cam_mac_is_zero(info->bssid)))
    {
      return -EINVAL;
    }

  if (info->network_type != 0u &&
      (!k1_rtl8852bs_addr_cam_mac_valid(info->target_mac) ||
       !k1_rtl8852bs_addr_cam_mac_valid(info->bssid)))
    {
      return -EINVAL;
    }

  if (info->network_type == 3u &&
      (memcmp(info->self_mac, info->bssid, 6) != 0 ||
       memcmp(info->target_mac, info->bssid, 6) != 0))
    {
      return -EINVAL;
    }

  compare_length = k1_rtl8852bs_addr_cam_mask_count(info->address_mask);
  if (info->mask_selection == 1u)
    {
      self_hash = k1_rtl8852bs_addr_cam_hash(info->self_mac, compare_length);
      target_hash = k1_rtl8852bs_addr_cam_hash(info->target_mac, 6);
    }
  else if (info->mask_selection == 2u)
    {
      self_hash = k1_rtl8852bs_addr_cam_hash(info->self_mac, 6);
      target_hash =
        k1_rtl8852bs_addr_cam_hash(info->target_mac, compare_length);
    }
  else
    {
      self_hash = k1_rtl8852bs_addr_cam_hash(info->self_mac, 6);
      target_hash = k1_rtl8852bs_addr_cam_hash(info->target_mac, 6);
    }

  bssid_mask = info->mask_selection == 3u ? info->address_mask : 0x3fu;
  memset(content, 0, K1_RTL8852BS_ADDR_CAM_SIZE);

  word = (uint32_t)info->address_cam_index |
         ((uint32_t)K1_RTL8852BS_ADDR_CAM_LONG_LENGTH << 16);
  k1_rtl8852bs_write_le32(content + 4, word);

  word = 1u |
         ((uint32_t)info->network_type << 1) |
         ((uint32_t)info->beacon_hit_condition << 3) |
         ((uint32_t)info->hit_rule << 5) |
         ((uint32_t)info->address_mask << 8) |
         ((uint32_t)info->mask_selection << 14) |
         ((uint32_t)self_hash << 16) |
         ((uint32_t)target_hash << 24);
  k1_rtl8852bs_write_le32(content + 8, word);

  k1_rtl8852bs_write_le32(content + 12, info->bssid_cam_index);
  k1_rtl8852bs_write_le32(content + 16,
                           k1_rtl8852bs_read_le32(info->self_mac));
  word = (uint32_t)info->self_mac[4] |
         ((uint32_t)info->self_mac[5] << 8) |
         ((uint32_t)info->target_mac[0] << 16) |
         ((uint32_t)info->target_mac[1] << 24);
  k1_rtl8852bs_write_le32(content + 20, word);
  k1_rtl8852bs_write_le32(content + 24,
                           k1_rtl8852bs_read_le32(info->target_mac + 2));

  word = (uint32_t)info->mac_id |
         ((uint32_t)info->port << 8) |
         ((uint32_t)info->tsf_sync << 11) |
         ((uint32_t)info->trigger << 14) |
         ((uint32_t)info->lsig_txop << 15) |
         ((uint32_t)info->target_indicator << 24) |
         ((uint32_t)info->frame_target_indicator << 27);
  k1_rtl8852bs_write_le32(content + 32, word);
  k1_rtl8852bs_write_le32(content + 36, info->aid);

  word = (uint32_t)info->bssid_cam_index |
         ((uint32_t)K1_RTL8852BS_BSSID_CAM_LENGTH << 16);
  k1_rtl8852bs_write_le32(content + 48, word);
  word = 1u |
         ((uint32_t)bssid_mask << 2) |
         ((uint32_t)info->bss_color << 8) |
         ((uint32_t)info->bssid[0] << 16) |
         ((uint32_t)info->bssid[1] << 24);
  k1_rtl8852bs_write_le32(content + 52, word);
  k1_rtl8852bs_write_le32(content + 56,
                           k1_rtl8852bs_read_le32(info->bssid + 2));
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_addr_cam_diagnostic
 *
 * Description:
 *   Verify the upstream address/BSSID CAM serialization with a non-live RAM
 *   test vector.  The bytes never reach the RTL8852BS2, so they cannot
 *   create an address-CAM entry or change RF behavior on the board.
 ****************************************************************************/

int k1_rtl8852bs_fwdl_runtime_addr_cam_diagnostic(void)
{
  struct k1_rtl8852bs_addr_cam_info_s info =
  {
    .address_cam_index = 0x12u,
    .bssid_cam_index = 0x0eu,
    .mac_id = 0x23u,
    .port = 4u,
    .network_type = 2u,
    .self_role = 0u,
    .address_mask = 0x3fu,
    .mask_selection = 0u,
    .bss_color = 0x2au,
    .beacon_hit_condition = 3u,
    .hit_rule = 2u,
    .tsf_sync = 5u,
    .target_indicator = 3u,
    .frame_target_indicator = 5u,
    .aid = 0x05aau,
    .self_mac = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u},
    .target_mac = {0x66u, 0x77u, 0x88u, 0x99u, 0xaau, 0xbbu},
    .bssid = {0x0au, 0x1bu, 0x2cu, 0x3du, 0x4eu, 0x5fu},
    .trigger = true,
    .lsig_txop = true
  };

  struct k1_rtl8852bs_addr_cam_info_s no_link =
  {
    .network_type = 0u,
    .self_role = 0u,
    .self_mac = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u}
  };

  uint8_t content[K1_RTL8852BS_ADDR_CAM_SIZE];
  uint8_t no_link_content[K1_RTL8852BS_ADDR_CAM_SIZE];
  int ret;

  ret = k1_rtl8852bs_runtime_addr_cam_build(&info, content, sizeof(content));
  if (ret < 0 || k1_rtl8852bs_read_le32(content) != 0 ||
      k1_rtl8852bs_read_le32(content + 4) != 0x00400012u ||
      k1_rtl8852bs_read_le32(content + 8) != 0x11133f5du ||
      k1_rtl8852bs_read_le32(content + 12) != 0x0000000eu ||
      k1_rtl8852bs_read_le32(content + 16) != 0x33221102u ||
      k1_rtl8852bs_read_le32(content + 20) != 0x77665544u ||
      k1_rtl8852bs_read_le32(content + 24) != 0xbbaa9988u ||
      k1_rtl8852bs_read_le32(content + 28) != 0 ||
      k1_rtl8852bs_read_le32(content + 32) != 0x2b00ec23u ||
      k1_rtl8852bs_read_le32(content + 36) != 0x000005aau ||
      k1_rtl8852bs_read_le32(content + 40) != 0 ||
      k1_rtl8852bs_read_le32(content + 44) != 0 ||
      k1_rtl8852bs_read_le32(content + 48) != 0x0008000eu ||
      k1_rtl8852bs_read_le32(content + 52) != 0x1b0a2afdu ||
      k1_rtl8852bs_read_le32(content + 56) != 0x5f4e3d2cu)
    {
      return ret < 0 ? ret : -EIO;
    }

  ret = k1_rtl8852bs_runtime_addr_cam_build(
    &no_link, no_link_content, sizeof(no_link_content));
  if (ret < 0 ||
      k1_rtl8852bs_read_le32(no_link_content + 4) != 0x00400000u ||
      k1_rtl8852bs_read_le32(no_link_content + 8) != 0x00130001u ||
      k1_rtl8852bs_read_le32(no_link_content + 20) != 0x00005544u ||
      k1_rtl8852bs_read_le32(no_link_content + 24) != 0 ||
      k1_rtl8852bs_read_le32(no_link_content + 52) != 0x000000fdu ||
      k1_rtl8852bs_read_le32(no_link_content + 56) != 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  no_link.target_mac[0] = 0x02u;
  if (k1_rtl8852bs_runtime_addr_cam_build(
        &no_link, no_link_content, sizeof(no_link_content)) != -EINVAL)
    {
      return -EIO;
    }

  no_link.target_mac[0] = 0;
  no_link.bssid[0] = 0x02u;
  if (k1_rtl8852bs_runtime_addr_cam_build(
        &no_link, no_link_content, sizeof(no_link_content)) != -EINVAL)
    {
      return -EIO;
    }

  info.self_mac[0] |= 1u;
  if (k1_rtl8852bs_runtime_addr_cam_build(&info, content,
                                          sizeof(content)) != -EINVAL)
    {
      return -EIO;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime address CAM dword2=");
  k1_early_puthex(k1_rtl8852bs_read_le32(content + 8));
  k1_early_puts(" dword13=");
  k1_early_puthex(k1_rtl8852bs_read_le32(content + 52));
  k1_early_puts(" H2C=1/6/0\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime address CAM "
                "serialization complete\r\n");
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_rx_read
 *
 * Description:
 *   Read one pending RTL8852B SDIO RX FIFO transfer.  The RX request length
 *   is owned by the device; callers provide a persistent buffer and must
 *   parse every aggregate in the returned transfer before issuing another
 *   read.  RXFF is a fixed-address FIFO, therefore CMD53 increment is false.
 ****************************************************************************/

int k1_rtl8852bs_runtime_rx_read(FAR uint8_t *buffer, size_t buffer_size,
                                 FAR size_t *transfer_length)
{
  uint32_t request_length;
  int ret;

  if (buffer == NULL || transfer_length == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_sdio_local_read32(K1_RTL8852BS_SDIO_RX_REQ_LEN,
                                        &request_length);
  if (ret < 0)
    {
      return ret;
    }

  request_length &= K1_RTL8852BS_SDIO_RX_REQ_LEN_MASK;
  if (request_length == 0)
    {
      *transfer_length = 0;
      return -EAGAIN;
    }

  if (request_length > buffer_size)
    {
      /* Report the device-owned length so the caller can log how much room
       * the pending aggregate would have needed.  The FIFO is left intact.
       */

      *transfer_length = request_length;
      return -ENOSPC;
    }

  /* The announced length regularly exceeds the 512-byte CMD53 byte-mode
   * limit, so the aggregate is read through the dedicated fixed-address RX
   * FIFO route, which splits it into block-mode transfers plus one byte-mode
   * remainder exactly as the SDIO core does for the vendor driver.
   */

  ret = k1_sdio_wifi_rxfifo_read(K1_RTL8852BS_H2C_LOOPBACK_RXFIFO,
                                 buffer, request_length);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: runtime RX read error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" length=");
      k1_early_puthex(request_length);
      k1_early_puts("\r\n");
      return ret;
    }

  *transfer_length = request_length;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_rx_parse
 *
 * Description:
 *   Parse one RTL8852B RX descriptor in an SDIO RX aggregate.  The original
 *   driver advances each aggregate entry to an eight-byte boundary.  Retain
 *   that rule so the future RX worker can visit every frame exactly once.
 ****************************************************************************/

int k1_rtl8852bs_runtime_rx_parse(FAR const uint8_t *buffer,
                                  size_t transfer_length, size_t offset,
                                  FAR struct k1_rtl8852bs_rx_frame_s *frame)
{
  uint32_t descriptor0;
  uint32_t descriptor3 = 0;
  size_t descriptor_length;
  size_t payload_offset;
  size_t payload_end;
  size_t next_offset;

  if (buffer == NULL || frame == NULL || offset > transfer_length ||
      transfer_length - offset < K1_RTL8852BS_RXDESC_SHORT_SIZE)
    {
      return -EMSGSIZE;
    }

  descriptor0 = k1_rtl8852bs_read_le32(buffer + offset);
  descriptor_length = (descriptor0 & K1_RTL8852BS_RXDESC_LONG) != 0 ?
                      K1_RTL8852BS_RXDESC_LONG_SIZE :
                      K1_RTL8852BS_RXDESC_SHORT_SIZE;
  if (descriptor_length > transfer_length - offset)
    {
      return -EMSGSIZE;
    }

  payload_offset = offset + descriptor_length +
                   (((descriptor0 >> K1_RTL8852BS_RXDESC_DRIVER_INFO_SHIFT) &
                     K1_RTL8852BS_RXDESC_DRIVER_INFO_MASK) * 8u) +
                   (((descriptor0 >> K1_RTL8852BS_RXDESC_SHIFT) &
                     K1_RTL8852BS_RXDESC_SHIFT_MASK) * 2u);
  if (payload_offset > transfer_length)
    {
      return -EMSGSIZE;
    }

  frame->payload_length = descriptor0 & K1_RTL8852BS_RXDESC_LENGTH_MASK;
  if (frame->payload_length == 0 ||
      frame->payload_length > transfer_length - payload_offset)
    {
      return -EBADMSG;
    }

  payload_end = payload_offset + frame->payload_length;
  next_offset = (payload_end + K1_RTL8852BS_RX_AGG_ALIGNMENT - 1) &
                ~(K1_RTL8852BS_RX_AGG_ALIGNMENT - 1);
  if (next_offset > transfer_length)
    {
      /* A single final packet may omit aggregate padding. */

      if (payload_end != transfer_length)
        {
          return -EMSGSIZE;
        }

      next_offset = payload_end;
    }

  if (descriptor_length >= K1_RTL8852BS_RXDESC_SHORT_SIZE)
    {
      descriptor3 = k1_rtl8852bs_read_le32(buffer + offset + 12);
    }

  frame->payload_offset = payload_offset;
  frame->next_offset = next_offset;
  frame->descriptor0 = descriptor0;
  frame->descriptor3 = descriptor3;
  frame->packet_type =
    (descriptor0 >> K1_RTL8852BS_RXDESC_PACKET_TYPE_SHIFT) &
    K1_RTL8852BS_RXDESC_PACKET_TYPE_MASK;
  frame->crc_error = (descriptor3 & K1_RTL8852BS_RXDESC_CRC_ERROR) != 0;
  frame->icv_error = (descriptor3 & K1_RTL8852BS_RXDESC_ICV_ERROR) != 0;
  return OK;
}

/* The self MAC the active sweep names as transmitter address of its offloaded
 * Probe Request.  The receive accounting needs it where the frame observer can
 * reach it: A1 of a Probe Response has to be compared against it before that
 * response may count as an answer to this host, and A2 equal to it is what
 * identifies a frame this part transmitted itself.  A zero address disables
 * both comparisons rather than matching everything.
 */

static uint8_t g_k1_rtl8852bs_scan_self_mac[6];
static bool g_k1_rtl8852bs_scan_self_mac_valid;

static void k1_rtl8852bs_scanofld_set_self_mac(FAR const uint8_t *self_mac)
{
  if (self_mac == NULL)
    {
      g_k1_rtl8852bs_scan_self_mac_valid = false;
      return;
    }

  memcpy(g_k1_rtl8852bs_scan_self_mac, self_mac,
         sizeof(g_k1_rtl8852bs_scan_self_mac));
  g_k1_rtl8852bs_scan_self_mac_valid = true;
}

static bool k1_rtl8852bs_scanofld_is_self_mac(FAR const uint8_t *address)
{
  return g_k1_rtl8852bs_scan_self_mac_valid && address != NULL &&
         memcmp(address, g_k1_rtl8852bs_scan_self_mac,
                sizeof(g_k1_rtl8852bs_scan_self_mac)) == 0;
}

/* The one Authentication Request this component transmits, and the account of
 * what came back.  It is module state rather than a parameter because the
 * frame has to be handed to the hardware from inside the scan-offload receive
 * loop: that loop is the only place that knows the radio is parked on the
 * access point's channel, since the firmware holds a channel from its
 * enter-channel notification until the host asks for the next one.  Arming it
 * before a sweep and disarming it afterwards keeps every existing caller of
 * the sweep unchanged and keeps the sweep passive when nothing is armed.
 */

struct k1_rtl8852bs_auth_action_s
{
  uint8_t bssid[6];
  uint8_t channel;
  bool armed;
  bool transmitted;
  int transmit_status;
  uint16_t requests;
  uint16_t responses_to_self;
  uint16_t responses_from_target;
  uint16_t frames_seen;
  bool response_valid;
  uint16_t response_algorithm;
  uint16_t response_sequence;
  uint16_t response_status;
  uint8_t response_a2[6];
};

static struct k1_rtl8852bs_auth_action_s g_k1_rtl8852bs_auth_action;

static int k1_rtl8852bs_runtime_mgmt_tx_frame(FAR const uint8_t *frame,
                                              size_t frame_length,
                                              uint16_t sequence,
                                              bool broadcast);

static void k1_rtl8852bs_runtime_auth_arm(FAR const uint8_t *bssid,
                                          uint8_t channel)
{
  memset(&g_k1_rtl8852bs_auth_action, 0, sizeof(g_k1_rtl8852bs_auth_action));
  if (bssid == NULL || channel == 0)
    {
      return;
    }

  memcpy(g_k1_rtl8852bs_auth_action.bssid, bssid,
         sizeof(g_k1_rtl8852bs_auth_action.bssid));
  g_k1_rtl8852bs_auth_action.channel = channel;
  g_k1_rtl8852bs_auth_action.transmit_status = -ENODATA;
  g_k1_rtl8852bs_auth_action.armed = true;
}

static void k1_rtl8852bs_runtime_auth_disarm(void)
{
  g_k1_rtl8852bs_auth_action.armed = false;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_mgmt_parse
 *
 * Description:
 *   Parse the bounded 802.11 management-frame fields needed by the passive
 *   scan diagnostic.  Beacon and probe-response fixed parameters are
 *   followed by tagged IEs; malformed management frames are rejected, while
 *   other 802.11 frame types return an empty, non-management result.
 ****************************************************************************/

int k1_rtl8852bs_runtime_mgmt_parse(
  FAR const uint8_t *payload, size_t payload_length,
  FAR struct k1_rtl8852bs_mgmt_frame_s *frame)
{
  uint16_t frame_control;
  uint8_t frame_type;
  uint8_t frame_subtype;
  size_t ie_offset;
  size_t ie_end;
  uint8_t ie_id;
  uint8_t ie_length;
  unsigned int index;
  bool all_zero;
  bool all_ff;

  if (payload == NULL || frame == NULL)
    {
      return -EINVAL;
    }

  memset(frame, 0, sizeof(*frame));
  if (payload_length < sizeof(uint16_t))
    {
      return -EMSGSIZE;
    }

  frame_control = k1_rtl8852bs_read_le16(payload);
  frame->frame_control = frame_control;

  /* The address fields are copied ahead of every early return, so a control
   * or data frame reports them as well.  That matters for two separate
   * questions this diagnostic has to answer: whether the receiver is passing
   * up frames addressed to other stations, which is visible only in A1 of
   * ordinary traffic, and whether a frame carries this host's own address as
   * transmitter, which is how a frame this part emitted is recognised.  A
   * control frame can be ten bytes and carries A1 alone, so each address is
   * copied only when the payload actually reaches it.
   */

  if (payload_length >= 10)
    {
      memcpy(frame->addr1, payload + 4, sizeof(frame->addr1));
      frame->addr1_valid = true;
    }

  if (payload_length >= 16)
    {
      memcpy(frame->addr2, payload + 10, sizeof(frame->addr2));
      frame->addr2_valid = true;
    }

  if (payload_length >= 22)
    {
      memcpy(frame->addr3, payload + 16, sizeof(frame->addr3));
      frame->addr3_valid = true;
    }

  frame_type = (frame_control >> 2) & K1_RTL8852BS_IEEE80211_TYPE_MASK;
  frame_subtype = (frame_control >> K1_RTL8852BS_IEEE80211_SUBTYPE_SHIFT) &
                  K1_RTL8852BS_IEEE80211_SUBTYPE_MASK;
  if (frame_type != K1_RTL8852BS_IEEE80211_TYPE_MANAGEMENT)
    {
      return OK;
    }

  frame->is_management = true;
  if (payload_length < K1_RTL8852BS_IEEE80211_HEADER_SIZE)
    {
      return -EMSGSIZE;
    }

  frame->is_probe_response =
    frame_subtype == K1_RTL8852BS_IEEE80211_SUBTYPE_PROBE_RESPONSE;
  frame->is_beacon = frame_subtype == K1_RTL8852BS_IEEE80211_SUBTYPE_BEACON;
  if (!frame->is_beacon && !frame->is_probe_response)
    {
      return OK;
    }

  if (payload_length < K1_RTL8852BS_IEEE80211_HEADER_SIZE +
                      K1_RTL8852BS_IEEE80211_BEACON_FIXED_SIZE)
    {
      return -EMSGSIZE;
    }

  frame->beacon_interval = k1_rtl8852bs_read_le16(
    payload + K1_RTL8852BS_IEEE80211_HEADER_SIZE + 8);
  frame->capability = k1_rtl8852bs_read_le16(
    payload + K1_RTL8852BS_IEEE80211_HEADER_SIZE + 10);
  memcpy(frame->bssid, payload + 16, sizeof(frame->bssid));
  all_zero = true;
  all_ff = true;
  for (index = 0; index < sizeof(frame->bssid); index++)
    {
      all_zero = all_zero && frame->bssid[index] == 0;
      all_ff = all_ff && frame->bssid[index] == 0xff;
    }

  frame->bssid_valid = !all_zero && !all_ff &&
                       (frame->bssid[0] & 1u) == 0;
  ie_offset = K1_RTL8852BS_IEEE80211_HEADER_SIZE +
              K1_RTL8852BS_IEEE80211_BEACON_FIXED_SIZE;
  ie_end = payload_length;

  /* The reported packet length may cover a trailing FCS or hardware padding,
   * and a Beacon captured at the edge of the dwell can end mid-element.
   * Neither voids the fields already decoded, so stop walking the element
   * list on a malformed tail instead of rejecting the whole frame.
   */

  while (ie_offset < ie_end)
    {
      if (ie_end - ie_offset < 2)
        {
          break;
        }

      ie_id = payload[ie_offset];
      ie_length = payload[ie_offset + 1];
      ie_offset += 2;
      if (ie_length > ie_end - ie_offset)
        {
          break;
        }

      if (ie_id == K1_RTL8852BS_IEEE80211_SSID_IE &&
          !frame->ssid_present &&
          ie_length <= sizeof(frame->ssid))
        {
          memcpy(frame->ssid, payload + ie_offset, ie_length);
          frame->ssid_length = ie_length;
          frame->ssid_present = true;
        }
      else if (ie_id == K1_RTL8852BS_IEEE80211_CHANNEL_IE &&
               ie_length >= 1 && frame->channel == 0)
        {
          frame->channel = payload[ie_offset];
        }

      ie_offset += ie_length;
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_c2h_parse
 *
 * Description:
 *   Decode and bound-check the common Realtek C2H firmware-command header.
 *   The firmware header's total length includes the eight-byte header
 *   itself, whereas content points to the following bytes.  The caller
 *   retains the RX aggregate buffer and therefore owns the C2H content
 *   lifetime.
 ****************************************************************************/

int k1_rtl8852bs_runtime_c2h_parse(FAR const uint8_t *buffer,
                                   size_t buffer_length,
                                   FAR struct k1_rtl8852bs_c2h_s *c2h)
{
  uint32_t header0;
  uint32_t header1;
  uint16_t total_length;
  uint8_t delivery_type;

  if (buffer == NULL || c2h == NULL)
    {
      return -EINVAL;
    }

  if (buffer_length < K1_RTL8852BS_C2H_HEADER_SIZE)
    {
      return -EMSGSIZE;
    }

  header0 = k1_rtl8852bs_read_le32(buffer);
  header1 = k1_rtl8852bs_read_le32(buffer + 4);
  delivery_type =
    (header0 >> K1_RTL8852BS_C2H_DELIVERY_TYPE_SHIFT) &
    K1_RTL8852BS_C2H_DELIVERY_TYPE_MASK;
  if (delivery_type != K1_RTL8852BS_C2H_DELIVERY_TYPE_C2H)
    {
      return -EPROTO;
    }

  total_length = header1 & K1_RTL8852BS_C2H_TOTAL_LENGTH_MASK;
  if (total_length < K1_RTL8852BS_C2H_HEADER_SIZE)
    {
      return -EBADMSG;
    }

  if (total_length > buffer_length)
    {
      return -EMSGSIZE;
    }

  c2h->content = buffer + K1_RTL8852BS_C2H_HEADER_SIZE;
  c2h->total_length = total_length;
  c2h->content_length = total_length - K1_RTL8852BS_C2H_HEADER_SIZE;
  c2h->category = header0 & K1_RTL8852BS_C2H_CATEGORY_MASK;
  c2h->class_id = (header0 >> K1_RTL8852BS_C2H_CLASS_SHIFT) &
                  K1_RTL8852BS_C2H_CLASS_MASK;
  c2h->function = (header0 >> K1_RTL8852BS_C2H_FUNCTION_SHIFT) &
                  K1_RTL8852BS_C2H_FUNCTION_MASK;
  c2h->sequence = (header0 >> K1_RTL8852BS_C2H_SEQUENCE_SHIFT) &
                  K1_RTL8852BS_C2H_SEQUENCE_MASK;
  c2h->receive_ack = (header1 & K1_RTL8852BS_C2H_RECEIVE_ACK) != 0;
  c2h->done_ack = (header1 & K1_RTL8852BS_C2H_DONE_ACK) != 0;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_c2h_dispatch
 *
 * Description:
 *   Parse one C2H frame and synchronously dispatch its bounded content to
 *   the caller.  This keeps the Realtek header format in the GPL chip layer
 *   while allowing a future control plane to select its own handlers.
 ****************************************************************************/

int k1_rtl8852bs_runtime_c2h_dispatch(FAR const uint8_t *buffer,
                                      size_t buffer_length,
                                      k1_rtl8852bs_c2h_handler_t handler,
                                      FAR void *arg)
{
  struct k1_rtl8852bs_c2h_s c2h;
  int ret;

  if (handler == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_runtime_c2h_parse(buffer, buffer_length, &c2h);
  if (ret < 0)
    {
      return ret;
    }

  return handler(&c2h, arg);
}

static int k1_rtl8852bs_h2c_resource_wait(uint16_t required_pages,
                                           FAR uint16_t *available_pages)
{
  unsigned int attempt;
  int ret;

  for (attempt = 0; attempt < K1_RTL8852BS_H2C_RESOURCE_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_h2c_resource_read(available_pages);
      if (ret < 0)
        {
          return ret;
        }

      if (*available_pages >= required_pages)
        {
          return OK;
        }

      if (attempt + 1 < K1_RTL8852BS_H2C_RESOURCE_POLL_COUNT)
        {
          up_mdelay(K1_RTL8852BS_H2C_RESOURCE_POLL_MSEC);
        }
    }

  k1_early_puts("K1 Wi-Fi GPL: H2C TX pages unavailable required=");
  k1_early_puthex(required_pages);
  k1_early_puts(" available=");
  k1_early_puthex(*available_pages);
  k1_early_puts(" polls=");
  k1_early_puthex(attempt);
  k1_early_puts("\r\n");
  return -ENOSPC;
}

static int k1_rtl8852bs_h2c_descriptor_build(FAR uint8_t *descriptor,
                                              FAR uint8_t *header,
                                              uint32_t payload_size,
                                              FAR uint32_t *fifo_address,
                                              FAR uint16_t *required_pages)
{
  uint32_t packet_size = K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + payload_size;
  uint32_t transfer_size = K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + packet_size;
  uint32_t length_units;
  unsigned int index;

  /* __fwhdr_download() prepends this FWDL H2C header before invoking
   * h2c_pkt_build_txd().  The caller chooses whether the header carries no
   * payload for accounting or the fixed, 80-byte static firmware header.
   */

  for (index = 0; index < K1_RTL8852BS_H2C_DESCRIPTOR_SIZE; index++)
    {
      descriptor[index] = 0;
    }

  k1_rtl8852bs_write_le32(header, K1_RTL8852BS_H2C_FWCMD_IDENTIFIER);
  k1_rtl8852bs_write_le32(header + 4, packet_size);
  k1_rtl8852bs_write_le32(
    descriptor, K1_RTL8852BS_H2C_CHANNEL <<
                K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT);
  k1_rtl8852bs_write_le32(descriptor + 8,
                           packet_size & K1_RTL8852BS_H2C_TXD_LENGTH_MASK);

  length_units = (transfer_size + K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) /
                 K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  if (length_units > K1_RTL8852BS_H2C_TX_UNIT_MASK)
    {
      return -E2BIG;
    }

  *fifo_address = K1_RTL8852BS_H2C_TX_FIFO_BASE |
                  (K1_RTL8852BS_H2C_CHANNEL <<
                   K1_RTL8852BS_H2C_TX_FIFO_SHIFT) |
                  length_units;
  *required_pages = ((packet_size + K1_RTL8852BS_H2C_PLE_PAGE_SIZE - 1) /
                     K1_RTL8852BS_H2C_PLE_PAGE_SIZE) << 1;

  if (k1_rtl8852bs_read_le32(header) != K1_RTL8852BS_H2C_FWCMD_IDENTIFIER ||
      k1_rtl8852bs_read_le32(header + 4) != packet_size ||
      k1_rtl8852bs_read_le32(descriptor) !=
        (K1_RTL8852BS_H2C_CHANNEL << K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT) ||
      k1_rtl8852bs_read_le32(descriptor + 8) != packet_size)
    {
      return -EIO;
    }

  return OK;
}

static void k1_rtl8852bs_h2c_loopback_content_fill(FAR uint8_t *content)
{
  unsigned int index;

  for (index = 0; index < K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE; index++)
    {
      content[index] = (uint8_t)index;
    }
}

static int k1_rtl8852bs_h2c_loopback_packet_build(
  FAR uint8_t *packet, FAR uint32_t *fifo_address,
  FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *header = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  FAR uint8_t *content = header + K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE;
  uint32_t header0;
  uint32_t header1;
  uint32_t length_units;

  if (packet == NULL || fifo_address == NULL || required_pages == NULL)
    {
      return -EINVAL;
    }

  /* This is the original mac_fwcmd_lb() packet path after the firmware has
   * started.  It is an ordinary H2C packet: unlike FWDL, TXD FWDL_EN stays
   * clear.  The firmware returns the content as a C2H CMD_PATH loopback.
   */

  memset(packet, 0, K1_RTL8852BS_H2C_LOOPBACK_TRANSFER_SIZE);
  k1_rtl8852bs_h2c_loopback_content_fill(content);

  header0 = K1_RTL8852BS_H2C_LOOPBACK_H2C_CATEGORY |
            (K1_RTL8852BS_H2C_LOOPBACK_H2C_CLASS <<
             K1_RTL8852BS_H2C_HEADER_CLASS_SHIFT) |
            (K1_RTL8852BS_H2C_LOOPBACK_H2C_FUNCTION <<
             K1_RTL8852BS_H2C_HEADER_FUNCTION_SHIFT) |
            (K1_RTL8852BS_H2C_LOOPBACK_H2C_SEQUENCE <<
             K1_RTL8852BS_H2C_HEADER_SEQUENCE_SHIFT);
  header1 = K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE;
  k1_rtl8852bs_write_le32(header, header0);
  k1_rtl8852bs_write_le32(header + 4, header1);
  k1_rtl8852bs_write_le32(
    descriptor, K1_RTL8852BS_H2C_CHANNEL <<
                K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT);
  k1_rtl8852bs_write_le32(descriptor + 8,
                           K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE);

  length_units = K1_RTL8852BS_H2C_LOOPBACK_TRANSFER_SIZE /
                 K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  *fifo_address = K1_RTL8852BS_H2C_TX_FIFO_BASE |
                  (K1_RTL8852BS_H2C_CHANNEL <<
                   K1_RTL8852BS_H2C_TX_FIFO_SHIFT) |
                  length_units;
  *required_pages = ((K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE +
                      K1_RTL8852BS_H2C_PLE_PAGE_SIZE - 1) /
                     K1_RTL8852BS_H2C_PLE_PAGE_SIZE) << 1;

  if (k1_rtl8852bs_read_le32(header) != header0 ||
      k1_rtl8852bs_read_le32(header + 4) != header1 ||
      k1_rtl8852bs_read_le32(descriptor) !=
        (K1_RTL8852BS_H2C_CHANNEL <<
         K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT) ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE)
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_h2c_loopback_c2h_validate(
  FAR const uint8_t *buffer, uint32_t length)
{
  struct k1_rtl8852bs_c2h_s c2h;
  struct k1_rtl8852bs_rx_frame_s frame;
  unsigned int index;
  int ret;

  ret = k1_rtl8852bs_runtime_rx_parse(buffer, length, 0, &frame);
  if (ret < 0)
    {
      return ret;
    }

  if (frame.packet_type != K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE ||
      frame.payload_length < K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE)
    {
      return -EPROTO;
    }

  ret = k1_rtl8852bs_runtime_c2h_parse(buffer + frame.payload_offset,
                                        frame.payload_length, &c2h);
  if (ret < 0)
    {
      return ret;
    }

  if (c2h.category != K1_RTL8852BS_H2C_LOOPBACK_C2H_CATEGORY ||
      c2h.class_id != K1_RTL8852BS_H2C_LOOPBACK_C2H_CLASS ||
      c2h.function != K1_RTL8852BS_H2C_LOOPBACK_C2H_FUNCTION ||
      c2h.total_length != K1_RTL8852BS_H2C_LOOPBACK_PACKET_SIZE ||
      c2h.content_length != K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE)
    {
      return -EPROTO;
    }

  for (index = 0; index < K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE; index++)
    {
      if (c2h.content[index] != (uint8_t)index)
        {
          return -EBADMSG;
        }
    }

  return OK;
}

static int k1_rtl8852bs_fw_header_packet_build(FAR uint8_t *packet,
                                               FAR uint32_t *fifo_address,
                                               FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *header = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  unsigned int index;
  int ret;

  ret = k1_rtl8852bs_h2c_descriptor_build(
    descriptor, header, K1_RTL8852BS_FW_HEADER_STATIC_SIZE, fifo_address,
    required_pages);
  if (ret < 0)
    {
      return ret;
    }

  for (index = 0; index < K1_RTL8852BS_FW_HEADER_STATIC_SIZE; index++)
    {
      header[K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + index] =
        g_k1_rtl8852bs_u2_nicce_image[index];
    }

  if (k1_rtl8852bs_read_le32(header) != K1_RTL8852BS_H2C_FWCMD_IDENTIFIER ||
      k1_rtl8852bs_read_le32(header + 4) !=
        K1_RTL8852BS_FW_HEADER_PACKET_SIZE ||
      k1_rtl8852bs_read_le32(descriptor) != 0x000c0000u ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_FW_HEADER_PACKET_SIZE ||
      *fifo_address != K1_RTL8852BS_FW_HEADER_FIFO_ADDRESS ||
      *required_pages != K1_RTL8852BS_FW_HEADER_REQUIRED_PAGES)
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fw_section_packet_build(
  FAR uint8_t *packet, FAR const uint8_t *source, uint32_t payload_size,
  FAR uint32_t *fifo_address, FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  uint32_t transfer_size;
  uint32_t length_units;
  unsigned int index;

  /* __sections_download() uses a FWDL descriptor followed directly by
   * section bytes.  Unlike __fwhdr_download(), these packets have no H2C
   * header.  FWDL_SECTION_PER_PKT_LEN is 2020 for the RTL8852B path.
   */

  if (source == NULL || payload_size == 0 ||
      payload_size > K1_RTL8852BS_FW_SECTION0_PACKET_SIZE)
    {
      return -E2BIG;
    }

  transfer_size = K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + payload_size;
  transfer_size = (transfer_size + K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) /
                  K1_RTL8852BS_H2C_TX_UNIT_SIZE *
                  K1_RTL8852BS_H2C_TX_UNIT_SIZE;

  for (index = 0; index < transfer_size; index++)
    {
      packet[index] = 0;
    }

  k1_rtl8852bs_write_le32(
    descriptor, (K1_RTL8852BS_H2C_CHANNEL <<
                 K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT) |
                K1_RTL8852BS_H2C_TXD_FWDL_ENABLE);
  k1_rtl8852bs_write_le32(descriptor + 8, payload_size);

  for (index = 0; index < payload_size; index++)
    {
      payload[index] = source[index];
    }

  length_units = transfer_size /
                 K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  if (length_units == 0 || length_units > K1_RTL8852BS_H2C_TX_UNIT_MASK)
    {
      return -E2BIG;
    }

  *fifo_address = K1_RTL8852BS_H2C_TX_FIFO_BASE |
                  (K1_RTL8852BS_H2C_CHANNEL <<
                   K1_RTL8852BS_H2C_TX_FIFO_SHIFT) |
                  length_units;
  *required_pages = ((payload_size +
                      K1_RTL8852BS_H2C_PLE_PAGE_SIZE - 1) /
                     K1_RTL8852BS_H2C_PLE_PAGE_SIZE) << 1;

  return OK;
}

static int k1_rtl8852bs_fw_section0_packet_build(
  FAR uint8_t *packet, FAR uint32_t *fifo_address,
  FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  int ret;

  ret = k1_rtl8852bs_fw_section_packet_build(
    packet, g_k1_rtl8852bs_u2_nicce_image + K1_RTL8852BS_FW_SECTION0_OFFSET,
    K1_RTL8852BS_FW_SECTION0_PACKET_SIZE, fifo_address, required_pages);
  if (ret < 0)
    {
      return ret;
    }

  if (k1_rtl8852bs_read_le32(descriptor) != 0x001c0000u ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_FW_SECTION0_PACKET_SIZE ||
      *fifo_address != K1_RTL8852BS_FW_SECTION0_FIFO_ADDRESS ||
      *required_pages != K1_RTL8852BS_FW_SECTION0_REQUIRED_PAGES ||
      payload[0] != g_k1_rtl8852bs_u2_nicce_image[
                      K1_RTL8852BS_FW_SECTION0_OFFSET] ||
      payload[K1_RTL8852BS_FW_SECTION0_PACKET_SIZE - 1] !=
        g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_SECTION0_OFFSET +
          K1_RTL8852BS_FW_SECTION0_PACKET_SIZE - 1])
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fw_section0_tail_packet_build(
  FAR uint8_t *packet, FAR uint32_t *fifo_address,
  FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  int ret;

  ret = k1_rtl8852bs_fw_section_packet_build(
    packet, g_k1_rtl8852bs_u2_nicce_image +
            K1_RTL8852BS_FW_SECTION0_TAIL_OFFSET,
    K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_SIZE, fifo_address,
    required_pages);
  if (ret < 0)
    {
      return ret;
    }

  if (k1_rtl8852bs_read_le32(descriptor) != 0x001c0000u ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_SIZE ||
      *fifo_address != K1_RTL8852BS_FW_SECTION0_TAIL_FIFO_ADDRESS ||
      *required_pages != K1_RTL8852BS_FW_SECTION0_TAIL_REQUIRED_PAGES ||
      payload[0] != g_k1_rtl8852bs_u2_nicce_image[
                      K1_RTL8852BS_FW_SECTION0_TAIL_OFFSET] ||
      payload[K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_SIZE - 1] !=
        g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_SECTION0_TAIL_OFFSET +
          K1_RTL8852BS_FW_SECTION0_TAIL_PACKET_SIZE - 1])
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fw_section0_second_packet_build(
  FAR uint8_t *packet, FAR uint32_t *fifo_address,
  FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  int ret;

  ret = k1_rtl8852bs_fw_section_packet_build(
    packet, g_k1_rtl8852bs_u2_nicce_image +
            K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_OFFSET,
    K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE, fifo_address,
    required_pages);
  if (ret < 0)
    {
      return ret;
    }

  if (k1_rtl8852bs_read_le32(descriptor) != 0x001c0000u ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE ||
      *fifo_address != K1_RTL8852BS_FW_SECTION0_SECOND_FIFO_ADDRESS ||
      *required_pages != K1_RTL8852BS_FW_SECTION0_SECOND_REQUIRED_PAGES ||
      payload[0] != g_k1_rtl8852bs_u2_nicce_image[
                      K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_OFFSET] ||
      payload[K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE - 1] !=
        g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_OFFSET +
          K1_RTL8852BS_FW_SECTION0_SECOND_PACKET_SIZE - 1])
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fw_section0_third_packet_build(
  FAR uint8_t *packet, FAR uint32_t *fifo_address,
  FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  int ret;

  ret = k1_rtl8852bs_fw_section_packet_build(
    packet, g_k1_rtl8852bs_u2_nicce_image +
            K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_OFFSET,
    K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE, fifo_address,
    required_pages);
  if (ret < 0)
    {
      return ret;
    }

  if (k1_rtl8852bs_read_le32(descriptor) != 0x001c0000u ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE ||
      *fifo_address != K1_RTL8852BS_FW_SECTION0_THIRD_FIFO_ADDRESS ||
      *required_pages != K1_RTL8852BS_FW_SECTION0_THIRD_REQUIRED_PAGES ||
      payload[0] != g_k1_rtl8852bs_u2_nicce_image[
                      K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_OFFSET] ||
      payload[K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE - 1] !=
        g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_OFFSET +
          K1_RTL8852BS_FW_SECTION0_THIRD_PACKET_SIZE - 1])
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fw_section0_fourth_packet_build(
  FAR uint8_t *packet, FAR uint32_t *fifo_address,
  FAR uint16_t *required_pages)
{
  FAR uint8_t *descriptor = packet;
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  int ret;

  ret = k1_rtl8852bs_fw_section_packet_build(
    packet, g_k1_rtl8852bs_u2_nicce_image +
            K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_OFFSET,
    K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_SIZE, fifo_address,
    required_pages);
  if (ret < 0)
    {
      return ret;
    }

  if (k1_rtl8852bs_read_le32(descriptor) != 0x001c0000u ||
      k1_rtl8852bs_read_le32(descriptor + 8) !=
        K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_SIZE ||
      *fifo_address != K1_RTL8852BS_FW_SECTION0_FOURTH_FIFO_ADDRESS ||
      *required_pages != K1_RTL8852BS_FW_SECTION0_FOURTH_REQUIRED_PAGES ||
      payload[0] != g_k1_rtl8852bs_u2_nicce_image[
                      K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_OFFSET] ||
      payload[K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_SIZE - 1] !=
        g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_OFFSET +
          K1_RTL8852BS_FW_SECTION0_FOURTH_PACKET_SIZE - 1])
    {
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fwdl_cleanup(FAR uint16_t *failed_address)
{
  uint32_t value;
  int ret;

  /* This is the state cleanup order from mac_disable_cpu(). */

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_PLATFORM_ENABLE,
                                     K1_RTL8852BS_WCPU_ENABLE, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      return ret;
    }

  ret = k1_rtl8852bs_mac_update_bits(
    K1_RTL8852BS_WCPU_FW_CTRL,
    K1_RTL8852BS_WCPU_FWDL_ENABLE | K1_RTL8852BS_H2C_PATH_READY |
    K1_RTL8852BS_FWDL_PATH_READY, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      return ret;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_SYS_CLK_CTRL,
                                     K1_RTL8852BS_CPU_CLK_ENABLE, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_SYS_CLK_CTRL;
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PLATFORM_ENABLE, &value);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      return ret;
    }

  if ((value & K1_RTL8852BS_WCPU_ENABLE) != 0)
    {
      *failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      return -EIO;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL, &value);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      return ret;
    }

  if ((value & (K1_RTL8852BS_WCPU_FWDL_ENABLE |
                K1_RTL8852BS_H2C_PATH_READY |
                K1_RTL8852BS_FWDL_PATH_READY)) != 0)
    {
      *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      return -EIO;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SYS_CLK_CTRL, &value);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_SYS_CLK_CTRL;
      return ret;
    }

  if ((value & K1_RTL8852BS_CPU_CLK_ENABLE) != 0)
    {
      *failed_address = K1_RTL8852BS_SYS_CLK_CTRL;
      return -EIO;
    }

  return OK;
}

static int k1_rtl8852bs_fwdl_prepare(FAR uint16_t *failed_address,
                                     FAR bool *cleanup_needed)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  if (failed_address == NULL || cleanup_needed == NULL)
    {
      return -EINVAL;
    }

  *failed_address = 0;
  *cleanup_needed = false;

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PLATFORM_ENABLE, &value);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      return ret;
    }

  if ((value & K1_RTL8852BS_WCPU_ENABLE) != 0)
    {
      *failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      return -EBUSY;
    }

  /* Mirror the state-clearing writes in mac_enable_cpu().  The UDM0 read in
   * that routine only obtains a value for disabled debug code, so it has no
   * corresponding state change in this diagnostic.
   */

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_LDM, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_LDM;
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_H2C_CTRL, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_HALT_H2C_CTRL;
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_C2H_CTRL, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_HALT_C2H_CTRL;
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_H2C, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_HALT_H2C;
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_C2H, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_HALT_C2H;
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_HISR0, &value);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_HISR0;
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HISR0, value);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_HISR0;
      return ret;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_SYS_CLK_CTRL, 0,
                                     K1_RTL8852BS_CPU_CLK_ENABLE);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_SYS_CLK_CTRL;
      return ret;
    }

  *cleanup_needed = true;

  ret = k1_rtl8852bs_mac_update_bits(
    K1_RTL8852BS_WCPU_FW_CTRL,
    K1_RTL8852BS_WCPU_FWDL_ENABLE | K1_RTL8852BS_H2C_PATH_READY |
    K1_RTL8852BS_FWDL_PATH_READY | K1_RTL8852BS_WCPU_FWDL_STATUS_MASK, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      return ret;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_BOOT_REASON,
                                     K1_RTL8852BS_BOOT_REASON_MASK, 0);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_BOOT_REASON;
      return ret;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_WCPU_FW_CTRL, 0,
                                     K1_RTL8852BS_WCPU_FWDL_ENABLE);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      return ret;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_PLATFORM_ENABLE, 0,
                                     K1_RTL8852BS_WCPU_ENABLE);
  if (ret < 0)
    {
      *failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      return ret;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_FW_PREBOOT_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL, &value);
      if (ret < 0)
        {
          *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
          return ret;
        }

      if ((value & K1_RTL8852BS_H2C_PATH_READY) != 0)
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_FW_PREBOOT_POLL_USEC);
    }

  *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
  return -ETIMEDOUT;
}

static int k1_rtl8852bs_fwdl_parse_full_image(
  FAR struct k1_rtl8852bs_fwdl_section_s *sections,
  FAR unsigned int *section_count, FAR const uint8_t **signature,
  FAR uint8_t *signature_index, FAR uint32_t *signature_source,
  FAR uint32_t *signature_target)
{
  uint32_t dynamic_consumed;
  uint32_t dynamic_count;
  uint32_t dynamic_length;
  uint32_t dynamic_offset;
  uint32_t header_length;
  uint32_t header_words;
  uint32_t image_words;
  uint32_t section_length;
  uint32_t section_offset;
  uint32_t section_words;
  uint32_t trailer_offset;
  uint32_t section_index;
  uint16_t dynamic_entry_length;
  uint8_t customer;
  uint8_t decoded_customer;
  uint8_t decoded_external_pn;
  uint8_t decoded_serial;
  uint8_t external_pn;
  bool mapped;
  unsigned int index;
  int ret;

  if (sections == NULL || section_count == NULL || signature == NULL ||
      signature_index == NULL || signature_source == NULL ||
      signature_target == NULL)
    {
      return -EINVAL;
    }

  header_words = k1_rtl8852bs_read_le32(g_k1_rtl8852bs_u2_nicce_image + 12);
  header_length = (header_words >> 16) & 0xffu;
  image_words = k1_rtl8852bs_read_le32(g_k1_rtl8852bs_u2_nicce_image + 24);
  *section_count = (image_words >> 8) & 0xffu;
  if (header_length != K1_RTL8852BS_FW_HEADER_SIZE ||
      *section_count <= K1_RTL8852BS_FW_SECURITY_SECTION_INDEX ||
      *section_count > K1_RTL8852BS_FW_SECTION_MAX_COUNT ||
      (k1_rtl8852bs_read_le32(g_k1_rtl8852bs_u2_nicce_image + 28) &
       (1u << 16)) == 0)
    {
      return -EPROTO;
    }

  dynamic_length = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + K1_RTL8852BS_FW_HEADER_STATIC_SIZE);
  dynamic_count = k1_rtl8852bs_read_le32(
    g_k1_rtl8852bs_u2_nicce_image + K1_RTL8852BS_FW_HEADER_STATIC_SIZE + 4);
  if (dynamic_length != K1_RTL8852BS_FW_HEADER_DYNAMIC_SIZE ||
      dynamic_count == 0)
    {
      return -EPROTO;
    }

  dynamic_offset = 8;
  dynamic_consumed = 8;
  for (index = 0; index < dynamic_count; index++)
    {
      if (dynamic_offset + 4 > dynamic_length)
        {
          return -EPROTO;
        }

      dynamic_entry_length =
        (uint16_t)g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_HEADER_STATIC_SIZE + dynamic_offset] |
        (uint16_t)g_k1_rtl8852bs_u2_nicce_image[
          K1_RTL8852BS_FW_HEADER_STATIC_SIZE + dynamic_offset + 1] << 8;
      if (dynamic_entry_length < 4 ||
          dynamic_offset + dynamic_entry_length > dynamic_length)
        {
          return -EPROTO;
        }

      dynamic_offset += dynamic_entry_length;
      dynamic_consumed += dynamic_entry_length;
    }

  dynamic_consumed = (dynamic_consumed + 15u) & ~15u;
  if (dynamic_consumed != dynamic_length)
    {
      return -EPROTO;
    }

  section_offset = header_length;
  for (section_index = 0; section_index < *section_count; section_index++)
    {
      header_words = k1_rtl8852bs_read_le32(
        g_k1_rtl8852bs_u2_nicce_image +
        K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
        section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE + 4);
      section_words = k1_rtl8852bs_read_le32(
        g_k1_rtl8852bs_u2_nicce_image +
        K1_RTL8852BS_FW_SECTION_HEADERS_OFFSET +
        section_index * K1_RTL8852BS_FW_SECTION_HEADER_SIZE + 8);
      section_length = header_words & K1_RTL8852BS_FW_SECTION_SIZE_MASK;
      if ((header_words & K1_RTL8852BS_FW_SECTION_CHECKSUM) != 0)
        {
          section_length += K1_RTL8852BS_FW_SECTION_CHECKSUM_SIZE;
        }

      if (section_length == 0 ||
          section_offset > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
          section_length > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - section_offset)
        {
          return -EPROTO;
        }

      sections[section_index].offset = section_offset;
      sections[section_index].length = section_length;
      sections[section_index].mssc = section_words;
      sections[section_index].type =
        (header_words >> K1_RTL8852BS_FW_SECTION_TYPE_SHIFT) &
        K1_RTL8852BS_FW_SECTION_TYPE_MASK;
      section_offset += section_length;
    }

  if (sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].type !=
        K1_RTL8852BS_FW_SECURITY_SECTION_TYPE ||
      sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].length !=
        K1_RTL8852BS_FW_SECURITY_SECTION_SIZE ||
      sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].mssc !=
        K1_RTL8852BS_FW_LEGACY_MSS_SIG_COUNT ||
      section_offset > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
      K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - section_offset !=
        K1_RTL8852BS_FW_LEGACY_MSS_TRAILER_SIZE)
    {
      return -EPROTO;
    }

  trailer_offset = section_offset;

  *signature_target =
    sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].offset +
    K1_RTL8852BS_FW_SECURITY_SIG_OFFSET;
  if (*signature_target >
        sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].offset +
        sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].length ||
      K1_RTL8852BS_FW_SECURITY_SIG_SIZE >
        sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].offset +
        sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].length -
        *signature_target)
    {
      return -EPROTO;
    }

  ret = k1_rtl8852bs_read_mss_efuse(&external_pn, &customer);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_legacy_mss_index(
    external_pn, customer, signature_index, &decoded_external_pn,
    &decoded_customer, &decoded_serial, &mapped);
  /* __mss_index() defaults to entry zero when none of its two OTP rows
   * matches, so preserve that original fallback instead of rejecting it.
   */

  if (ret < 0 ||
      *signature_index >= K1_RTL8852BS_FW_LEGACY_MSS_SIG_COUNT)
    {
      return ret < 0 ? ret : -EPROTO;
    }

  *signature_source = trailer_offset +
    (uint32_t)*signature_index * K1_RTL8852BS_FW_SECURITY_SIG_SIZE;
  if (*signature_source > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
      K1_RTL8852BS_FW_SECURITY_SIG_SIZE >
        K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - *signature_source)
    {
      return -EPROTO;
    }

  *signature = g_k1_rtl8852bs_u2_nicce_image + *signature_source;

  /* fwhdr_parser() shortens this RTL8852B secure section for a non-default
   * legacy MSS signature.  The section's source span remains 2048 bytes, but
   * only the original 960-byte download span is transmitted.
   */

  if (*signature_index > 0)
    {
      sections[K1_RTL8852BS_FW_SECURITY_SECTION_INDEX].length =
        K1_RTL8852BS_FW_SECURITY_ALT_SECTION_SIZE;
    }

  return OK;
}

static int k1_rtl8852bs_fwdl_full_packet_build(
  FAR uint8_t *packet, uint32_t image_offset, uint32_t payload_size,
  uint32_t signature_target, FAR const uint8_t *signature,
  FAR uint32_t *fifo_address, FAR uint16_t *required_pages)
{
  FAR uint8_t *payload = packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE;
  uint32_t copy_length;
  uint32_t copy_start;
  uint32_t image_end;
  unsigned int index;
  int ret;

  if (image_offset > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE ||
      payload_size > K1_RTL8852BS_U2_NICCE_IMAGE_SIZE - image_offset)
    {
      return -EPROTO;
    }

  ret = k1_rtl8852bs_fw_section_packet_build(
    packet, g_k1_rtl8852bs_u2_nicce_image + image_offset, payload_size,
    fifo_address, required_pages);
  if (ret < 0 || signature == NULL)
    {
      return ret;
    }

  image_end = image_offset + payload_size;
  if (signature_target >= image_end ||
      signature_target + K1_RTL8852BS_FW_SECURITY_SIG_SIZE <= image_offset)
    {
      return OK;
    }

  copy_start = signature_target > image_offset ? signature_target :
               image_offset;
  if (image_end < signature_target + K1_RTL8852BS_FW_SECURITY_SIG_SIZE)
    {
      copy_length = image_end - copy_start;
    }
  else
    {
      copy_length = signature_target + K1_RTL8852BS_FW_SECURITY_SIG_SIZE -
                    copy_start;
    }

  for (index = 0; index < copy_length; index++)
    {
      payload[copy_start - image_offset + index] =
        signature[copy_start - signature_target + index];
    }

  return OK;
}

static int k1_rtl8852bs_fwdl_wait_path_ready(FAR uint32_t *failed_address)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  for (attempt = 0; attempt < K1_RTL8852BS_FW_PREBOOT_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL, &value);
      if (ret < 0)
        {
          *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
          return ret;
        }

      if ((value & K1_RTL8852BS_FWDL_PATH_READY) != 0)
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_FW_PREBOOT_POLL_USEC);
    }

  *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
  return -ETIMEDOUT;
}

static int k1_rtl8852bs_fwdl_wait_ready(FAR uint32_t *failed_address,
                                         FAR uint8_t *last_status)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  for (attempt = 0; attempt < K1_RTL8852BS_FW_PREBOOT_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL, &value);
      if (ret < 0)
        {
          *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
          return ret;
        }

      *last_status = (value & K1_RTL8852BS_WCPU_FWDL_STATUS_MASK) >>
                     K1_RTL8852BS_WCPU_FWDL_STATUS_SHIFT;
      if (*last_status == K1_RTL8852BS_FWDL_READY)
        {
          return OK;
        }

      if (*last_status == K1_RTL8852BS_FWDL_CHECKSUM_FAIL)
        {
          return -EBADMSG;
        }

      if (*last_status == K1_RTL8852BS_FWDL_SECURITY_FAIL)
        {
          return -EACCES;
        }

      if (*last_status == K1_RTL8852BS_FWDL_CUT_NOT_MATCH)
        {
          return -ENODEV;
        }

      up_udelay(K1_RTL8852BS_FW_PREBOOT_POLL_USEC);
    }

  *failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
  return -ETIMEDOUT;
}

static int k1_rtl8852bs_fwdl_send_full_section(
  FAR uint8_t *packet, FAR const struct k1_rtl8852bs_fwdl_section_s *section,
  uint32_t signature_target, FAR const uint8_t *signature,
  FAR uint32_t *failed_address)
{
  uint16_t available_pages;
  uint16_t required_pages;
  uint32_t fifo_address;
  uint32_t image_offset;
  uint32_t packet_length;
  uint32_t remaining;
  uint32_t transfer_size;
  unsigned int packets = 0;
  int ret;

  if (packet == NULL || section == NULL || failed_address == NULL)
    {
      return -EINVAL;
    }

  image_offset = section->offset;
  remaining = section->length;
  while (remaining != 0)
    {
      packet_length = remaining > K1_RTL8852BS_FW_SECTION0_PACKET_SIZE ?
        K1_RTL8852BS_FW_SECTION0_PACKET_SIZE : remaining;
      ret = k1_rtl8852bs_fwdl_full_packet_build(
        packet, image_offset, packet_length, signature_target, signature,
        &fifo_address, &required_pages);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_h2c_resource_wait(required_pages, &available_pages);
      if (ret < 0)
        {
          *failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
          return ret;
        }

      transfer_size = K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + packet_length;
      transfer_size = (transfer_size + K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1) &
                      ~(K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1);
      ret = k1_sdio_wifi_fwdl_write(fifo_address, packet, transfer_size);
      if (ret < 0)
        {
          *failed_address = fifo_address;
          return ret;
        }

      image_offset += packet_length;
      remaining -= packet_length;
      packets++;
    }

  k1_early_puts("K1 Wi-Fi GPL: full FWDL section bytes=");
  k1_early_puthex(section->length);
  k1_early_puts(" packets=");
  k1_early_puthex(packets);
  k1_early_puts(" type=");
  k1_early_puthex(section->type);
  k1_early_puts("\r\n");
  return OK;
}

int k1_rtl8852bs_fwdl_full_download(void)
{
  struct k1_rtl8852bs_fwdl_section_s
    sections[K1_RTL8852BS_FW_SECTION_MAX_COUNT];
  FAR const uint8_t *signature;
  FAR uint8_t *packet = NULL;
  uint16_t available_pages;
  uint16_t cleanup_failed_address;
  uint16_t prepare_failed_address;
  uint16_t required_pages;
  uint32_t failed_address = 0;
  uint32_t fifo_address;
  uint32_t signature_source;
  uint32_t signature_target;
  uint8_t last_status = 0;
  uint8_t signature_index;
  bool cleanup_needed = false;
  unsigned int section_count;
  unsigned int section_index;
  int cleanup_ret;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 full FWDL begin\r\n");
  packet = kmm_zalloc(K1_RTL8852BS_FW_SECTION0_TRANSFER_SIZE);
  if (packet == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  ret = k1_rtl8852bs_fwdl_parse_full_image(
    sections, &section_count, &signature, &signature_index,
    &signature_source, &signature_target);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: full FWDL MSS index=");
  k1_early_puthex(signature_index);
  k1_early_puts(" source=");
  k1_early_puthex(signature_source);
  k1_early_puts(" target=");
  k1_early_puthex(signature_target);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_fwdl_prepare(&prepare_failed_address, &cleanup_needed);
  if (ret < 0)
    {
      failed_address = prepare_failed_address;
      goto error;
    }

  ret = k1_rtl8852bs_fw_header_packet_build(packet, &fifo_address,
                                             &required_pages);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_h2c_resource_wait(required_pages, &available_pages);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
      goto error;
    }

  ret = k1_sdio_wifi_write(1, fifo_address, false, packet,
                           K1_RTL8852BS_FW_HEADER_TRANSFER_SIZE);
  if (ret < 0)
    {
      failed_address = fifo_address;
      goto error;
    }

  ret = k1_rtl8852bs_fwdl_wait_path_ready(&failed_address);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_H2C_CTRL, 0);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HALT_H2C_CTRL;
      goto error;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_C2H_CTRL, 0);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HALT_C2H_CTRL;
      goto error;
    }

  for (section_index = 0; section_index < section_count; section_index++)
    {
      ret = k1_rtl8852bs_fwdl_send_full_section(
        packet, &sections[section_index], signature_target, signature,
        &failed_address);
      if (ret < 0)
        {
          goto error;
        }
    }

  ret = k1_rtl8852bs_fwdl_wait_ready(&failed_address, &last_status);
  if (ret < 0)
    {
      goto error;
    }

  cleanup_needed = false;
  kmm_free(packet);
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 full FWDL ready status=");
  k1_early_puthex(last_status);
  k1_early_puts("\r\n");
  return OK;

error:
  if (cleanup_needed)
    {
      cleanup_failed_address = 0;
      cleanup_ret = k1_rtl8852bs_fwdl_cleanup(&cleanup_failed_address);
      if (cleanup_ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: full FWDL cleanup error address=");
          k1_early_puthex(cleanup_failed_address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-cleanup_ret);
          k1_early_puts("\r\n");
        }
    }

  kmm_free(packet);
  k1_early_puts("K1 Wi-Fi GPL: full FWDL error address=");
  k1_early_puthex(failed_address);
  k1_early_puts(" status=");
  k1_early_puthex(last_status);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_runtime_status_diagnostic(void)
{
  uint16_t failed_address = 0;
  uint32_t firmware_control;
  uint32_t platform_enable;
  uint32_t sys_clock_control;
  uint8_t firmware_status;
  int ret;

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL,
                                &firmware_control);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      goto error;
    }

  firmware_status =
    (firmware_control & K1_RTL8852BS_WCPU_FWDL_STATUS_MASK) >>
    K1_RTL8852BS_WCPU_FWDL_STATUS_SHIFT;
  if (firmware_status != K1_RTL8852BS_FWDL_READY)
    {
      failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      ret = -EIO;
      goto error;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PLATFORM_ENABLE,
                                &platform_enable);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      goto error;
    }

  if ((platform_enable & K1_RTL8852BS_WCPU_ENABLE) == 0)
    {
      failed_address = K1_RTL8852BS_PLATFORM_ENABLE;
      ret = -EIO;
      goto error;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SYS_CLK_CTRL,
                                &sys_clock_control);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SYS_CLK_CTRL;
      goto error;
    }

  if ((sys_clock_control & K1_RTL8852BS_CPU_CLK_ENABLE) == 0)
    {
      failed_address = K1_RTL8852BS_SYS_CLK_CTRL;
      ret = -EIO;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime FW status=");
  k1_early_puthex(firmware_status);
  k1_early_puts(" WCPU=");
  k1_early_puthex(platform_enable);
  k1_early_puts(" clock=");
  k1_early_puthex(sys_clock_control);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 firmware runtime diagnostic "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: firmware runtime diagnostic error address=");
  k1_early_puthex(failed_address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_runtime_transport_diagnostic(void)
{
  uint16_t available_pages;
  uint16_t failed_address = 0;
  uint32_t himr;
  uint32_t hisr;
  uint32_t rx_request_length;
  int ret;

  /* The derived runtime handler only reads these state registers before it
   * decides whether an RX FIFO transfer is needed.  This diagnostic stops
   * before enabling or acknowledging an interrupt and never reads RXFF.
   */

  ret = k1_rtl8852bs_sdio_local_read32(K1_RTL8852BS_SDIO_HIMR, &himr);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_HIMR;
      goto error;
    }

  ret = k1_rtl8852bs_sdio_local_read32(K1_RTL8852BS_SDIO_HISR, &hisr);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_HISR;
      goto error;
    }

  ret = k1_rtl8852bs_sdio_local_read32(K1_RTL8852BS_SDIO_RX_REQ_LEN,
                                        &rx_request_length);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_RX_REQ_LEN;
      goto error;
    }

  ret = k1_rtl8852bs_h2c_resource_read(&available_pages);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime SDIO HIMR=");
  k1_early_puthex(himr);
  k1_early_puts(" HISR=");
  k1_early_puthex(hisr);
  k1_early_puts(" RXREQ=");
  k1_early_puthex(rx_request_length & K1_RTL8852BS_SDIO_RX_REQ_LEN_MASK);
  k1_early_puts(" H2C-pages=");
  k1_early_puthex(available_pages);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime transport diagnostic "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: runtime transport diagnostic error address=");
  k1_early_puthex(failed_address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_runtime_h2c_loopback_submit(void)
{
  uint8_t packet[K1_RTL8852BS_H2C_LOOPBACK_TRANSFER_SIZE];
  uint16_t available_pages;
  uint16_t required_pages;
  uint32_t fifo_address;
  int ret;

  ret = k1_rtl8852bs_h2c_loopback_packet_build(packet, &fifo_address,
                                                &required_pages);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_h2c_resource_wait(required_pages, &available_pages);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_sdio_wifi_fwdl_write(fifo_address, packet,
                                 sizeof(packet));
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime H2C loopback queued pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");
  return OK;
}

int k1_rtl8852bs_fwdl_runtime_h2c_loopback_diagnostic(void)
{
  FAR uint8_t *response = NULL;
  uint32_t rx_request_length = 0;
  size_t response_length;
  unsigned int attempt;
  int ret;

  ret = k1_rtl8852bs_runtime_h2c_loopback_submit();
  if (ret < 0)
    {
      goto error;
    }

  for (attempt = 0;
       attempt < K1_RTL8852BS_H2C_LOOPBACK_RESPONSE_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_sdio_local_read32(K1_RTL8852BS_SDIO_RX_REQ_LEN,
                                            &rx_request_length);
      if (ret < 0)
        {
          goto error;
        }

      rx_request_length &= K1_RTL8852BS_SDIO_RX_REQ_LEN_MASK;
      if (rx_request_length != 0)
        {
          break;
        }

      up_mdelay(K1_RTL8852BS_H2C_LOOPBACK_RESPONSE_POLL_MSEC);
    }

  if (rx_request_length == 0)
    {
      ret = -ETIMEDOUT;
      goto error;
    }

  if (rx_request_length > K1_RTL8852BS_H2C_LOOPBACK_RX_MAX)
    {
      ret = -E2BIG;
      goto error;
    }

  response = kmm_malloc(rx_request_length);
  if (response == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  /* Exercise the generic fixed-address RX FIFO path used by the future
   * runtime worker.  It rereads RXREQ immediately before CMD53 so the frame
   * parser receives the exact transfer size consumed from the FIFO.
   */

  ret = k1_rtl8852bs_runtime_rx_read(response, rx_request_length,
                                     &response_length);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_h2c_loopback_c2h_validate(response, response_length);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime H2C loopback RXREQ=");
  k1_early_puthex(response_length);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime H2C/C2H loopback "
                "complete\r\n");
  kmm_free(response);
  return OK;

error:
  if (response != NULL)
    {
      kmm_free(response);
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime H2C/C2H loopback error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts(" RXREQ=");
  k1_early_puthex(rx_request_length);
  k1_early_puts("\r\n");
  return ret;
}

struct k1_rtl8852bs_done_ack_match_s
{
  uint8_t category;
  uint8_t class_id;
  uint8_t function;
  uint8_t sequence;
  uint8_t firmware_return;
  uint8_t last_c2h_category;
  uint8_t last_c2h_class_id;
  uint8_t last_c2h_function;
  uint8_t last_c2h_sequence;
  uint16_t c2h_frames;
  uint16_t generic_done_ack_frames;
  uint32_t last_done_ack_word;
  bool matched;
  bool saw_done_ack;
};

static int k1_rtl8852bs_runtime_control_h2c_packet_build(
  FAR const uint8_t *content, size_t content_length, uint8_t category,
  uint8_t class_id, uint8_t function, uint8_t sequence, bool done_ack,
  FAR uint8_t *packet, size_t packet_capacity, FAR size_t *packet_length)
{
  uint32_t header0;
  uint32_t header1;
  size_t length;

  if (content == NULL || packet == NULL || packet_length == NULL ||
      content_length == 0 ||
      category > K1_RTL8852BS_H2C_HEADER_CATEGORY_MASK ||
      class_id > K1_RTL8852BS_H2C_HEADER_CLASS_MASK)
    {
      return -EINVAL;
    }

  length = K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + content_length;
  if (length > K1_RTL8852BS_H2C_HEADER_TOTAL_MASK ||
      length > packet_capacity)
    {
      return -E2BIG;
    }

  memset(packet, 0, length);
  memcpy(packet + K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE, content,
         content_length);
  header0 = (uint32_t)category |
            ((uint32_t)class_id << K1_RTL8852BS_H2C_HEADER_CLASS_SHIFT) |
            ((uint32_t)function <<
             K1_RTL8852BS_H2C_HEADER_FUNCTION_SHIFT) |
            ((uint32_t)sequence << K1_RTL8852BS_H2C_HEADER_SEQUENCE_SHIFT);
  header1 = (uint32_t)length;
  if (done_ack)
    {
      header1 |= K1_RTL8852BS_H2C_HEADER_DONE_ACK;
    }

  k1_rtl8852bs_write_le32(packet, header0);
  k1_rtl8852bs_write_le32(packet + sizeof(uint32_t), header1);
  *packet_length = length;
  return OK;
}

static int k1_rtl8852bs_runtime_control_h2c_submit(
  FAR const uint8_t *content, size_t content_length, uint8_t category,
  uint8_t class_id, uint8_t function, uint8_t sequence, bool done_ack,
  FAR uint32_t *fifo_address, FAR uint16_t *available_pages)
{
  uint8_t packet[K1_RTL8852BS_RUNTIME_H2C_TRANSFER_MAX];
  uint32_t packet_size;
  uint32_t transfer_size;
  uint32_t header0;
  uint32_t header1;
  uint32_t length_units;
  uint16_t required_pages;
  int ret;

  if (content == NULL || fifo_address == NULL || available_pages == NULL ||
      content_length == 0 ||
      content_length > K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX ||
      category > K1_RTL8852BS_H2C_HEADER_CATEGORY_MASK ||
      class_id > K1_RTL8852BS_H2C_HEADER_CLASS_MASK)
    {
      return -EINVAL;
    }

  packet_size = K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE + content_length;
  transfer_size = K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + packet_size;
  transfer_size = (transfer_size + K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1u) /
                  K1_RTL8852BS_H2C_TX_UNIT_SIZE *
                  K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  if (packet_size > K1_RTL8852BS_H2C_HEADER_TOTAL_MASK ||
      transfer_size > sizeof(packet))
    {
      return -E2BIG;
    }

  length_units = transfer_size / K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  if (length_units == 0 || length_units > K1_RTL8852BS_H2C_TX_UNIT_MASK)
    {
      return -E2BIG;
    }

  memset(packet, 0, transfer_size);
  memcpy(packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE +
         K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE, content, content_length);
  header0 = (uint32_t)category |
            ((uint32_t)class_id << K1_RTL8852BS_H2C_HEADER_CLASS_SHIFT) |
            ((uint32_t)function <<
             K1_RTL8852BS_H2C_HEADER_FUNCTION_SHIFT) |
            ((uint32_t)sequence << K1_RTL8852BS_H2C_HEADER_SEQUENCE_SHIFT);
  header1 = packet_size;
  if (done_ack)
    {
      header1 |= K1_RTL8852BS_H2C_HEADER_DONE_ACK;
    }

  k1_rtl8852bs_write_le32(packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE,
                           header0);
  k1_rtl8852bs_write_le32(packet + K1_RTL8852BS_H2C_DESCRIPTOR_SIZE + 4,
                           header1);
  k1_rtl8852bs_write_le32(
    packet, K1_RTL8852BS_H2C_CHANNEL <<
            K1_RTL8852BS_H2C_TXD_CHANNEL_SHIFT);
  k1_rtl8852bs_write_le32(packet + 8,
                           packet_size & K1_RTL8852BS_H2C_TXD_LENGTH_MASK);

  *fifo_address = K1_RTL8852BS_H2C_TX_FIFO_BASE |
                  (K1_RTL8852BS_H2C_CHANNEL <<
                   K1_RTL8852BS_H2C_TX_FIFO_SHIFT) |
                  length_units;
  required_pages = ((packet_size + K1_RTL8852BS_H2C_PLE_PAGE_SIZE - 1u) /
                    K1_RTL8852BS_H2C_PLE_PAGE_SIZE) << 1u;

  ret = k1_rtl8852bs_h2c_resource_wait(required_pages, available_pages);
  if (ret < 0)
    {
      return ret;
    }

  return k1_sdio_wifi_fwdl_write(*fifo_address, packet, transfer_size);
}

struct k1_rtl8852bs_runtime_h2c_command_s
{
  FAR const uint8_t *content;
  size_t content_length;
  uint8_t category;
  uint8_t class_id;
  uint8_t function;
  bool done_ack;
};

static int k1_rtl8852bs_runtime_h2c_aggregate_submit(
  FAR const struct k1_rtl8852bs_runtime_h2c_command_s *commands,
  size_t command_count, uint8_t sequence, FAR uint32_t *fifo_address,
  FAR uint16_t *available_pages)
{
  uint8_t aggregate[K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX];
  size_t command_index;
  size_t packet_length;
  size_t aligned_length;
  size_t offset;
  int ret;

  if (commands == NULL || command_count < 2u || fifo_address == NULL ||
      available_pages == NULL)
    {
      return -EINVAL;
    }

  /* mac_h2c_agg_tx() emits a four-byte aligned length followed by each
   * complete inner H2C.  The outer FW_OFLD/H2C_AGG packet carries all inner
   * commands with the current H2C sequence.
   */

  memset(aggregate, 0, sizeof(aggregate));
  offset = 0;
  for (command_index = 0; command_index < command_count; command_index++)
    {
      FAR const struct k1_rtl8852bs_runtime_h2c_command_s *command =
        &commands[command_index];

      if (command->content == NULL || command->content_length == 0 ||
          command->content_length > K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX ||
          command->category > K1_RTL8852BS_H2C_HEADER_CATEGORY_MASK ||
          command->class_id > K1_RTL8852BS_H2C_HEADER_CLASS_MASK ||
          offset > sizeof(aggregate) - K1_RTL8852BS_H2C_AGG_SUB_HEADER_SIZE)
        {
          return -EINVAL;
        }

      ret = k1_rtl8852bs_runtime_control_h2c_packet_build(
        command->content, command->content_length, command->category,
        command->class_id, command->function, sequence, command->done_ack,
        aggregate + offset + K1_RTL8852BS_H2C_AGG_SUB_HEADER_SIZE,
        sizeof(aggregate) - offset - K1_RTL8852BS_H2C_AGG_SUB_HEADER_SIZE,
        &packet_length);
      if (ret < 0)
        {
          return ret;
        }

      aligned_length =
        (packet_length + K1_RTL8852BS_H2C_AGG_ALIGNMENT - 1u) /
        K1_RTL8852BS_H2C_AGG_ALIGNMENT *
        K1_RTL8852BS_H2C_AGG_ALIGNMENT;
      if (aligned_length > sizeof(aggregate) - offset -
                           K1_RTL8852BS_H2C_AGG_SUB_HEADER_SIZE)
        {
          return -E2BIG;
        }

      k1_rtl8852bs_write_le32(aggregate + offset, aligned_length);
      offset += K1_RTL8852BS_H2C_AGG_SUB_HEADER_SIZE + aligned_length;
    }

  return k1_rtl8852bs_runtime_control_h2c_submit(
    aggregate, offset, K1_RTL8852BS_H2C_AGG_CATEGORY,
    K1_RTL8852BS_H2C_AGG_CLASS, K1_RTL8852BS_H2C_AGG_FUNCTION, sequence,
    false, fifo_address, available_pages);
}

static int k1_rtl8852bs_runtime_role_cam_h2c_aggregate_submit(
  FAR const uint8_t *role_content, size_t role_content_length,
  FAR const uint8_t *cam_content, size_t cam_content_length,
  uint8_t sequence, FAR uint32_t *fifo_address,
  FAR uint16_t *available_pages)
{
  const struct k1_rtl8852bs_runtime_h2c_command_s commands[] =
  {
    {
      .content = role_content,
      .content_length = role_content_length,
      .category = K1_RTL8852BS_FWROLE_MAINTAIN_CATEGORY,
      .class_id = K1_RTL8852BS_FWROLE_MAINTAIN_CLASS,
      .function = K1_RTL8852BS_FWROLE_MAINTAIN_FUNCTION,
      .done_ack = true
    },
    {
      .content = cam_content,
      .content_length = cam_content_length,
      .category = K1_RTL8852BS_ADDR_CAM_CATEGORY,
      .class_id = K1_RTL8852BS_ADDR_CAM_CLASS,
      .function = K1_RTL8852BS_ADDR_CAM_FUNCTION,
      .done_ack = true
    }
  };

  if (role_content_length != K1_RTL8852BS_FWROLE_MAINTAIN_SIZE ||
      cam_content_length != K1_RTL8852BS_ADDR_CAM_SIZE)
    {
      return -EINVAL;
    }

  return k1_rtl8852bs_runtime_h2c_aggregate_submit(
    commands, sizeof(commands) / sizeof(commands[0]), sequence, fifo_address,
    available_pages);
}

static int k1_rtl8852bs_runtime_addr_cam_init(void)
{
  uint32_t value;
  unsigned int attempt;
  int ret;

  /* addr_cam_init() programs the band-0 search range, enables address CAM,
   * then waits for the hardware clear request to complete.
   */

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_ADDR_CAM_CTRL, &value);
  if (ret < 0)
    {
      return ret;
    }

  value |= K1_RTL8852BS_ADDR_CAM_RANGE | K1_RTL8852BS_ADDR_CAM_ENABLE |
           K1_RTL8852BS_ADDR_CAM_CLEAR;
  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_ADDR_CAM_CTRL, value);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_ADDR_CAM_INIT_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_ADDR_CAM_CTRL, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & K1_RTL8852BS_ADDR_CAM_CLEAR) == 0)
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_ADDR_CAM_INIT_POLL_USEC);
    }

  return -ETIMEDOUT;
}

static int k1_rtl8852bs_runtime_macid_unpause_build(
  FAR uint8_t *content, size_t content_length)
{
  if (content == NULL ||
      content_length != K1_RTL8852BS_MACID_PAUSE_SLEEP_SIZE)
    {
      return -EINVAL;
    }

  /* role_init() calls set_macid_pause(macid = 0, pause = false).  Once FWDL
   * is ready, the vendor implementation routes this through
   * set_macid_pause_sleep() and sends the 16-dword MACID_PAUSE_SLEEP
   * payload.
   * Pause and sleep are both unpaused, while both group-zero masks select
   * MACID 0.
   */

  memset(content, 0, content_length);
  k1_rtl8852bs_write_le32(
    content + K1_RTL8852BS_MACID_PAUSE_MASK_DWORD * sizeof(uint32_t), 1u);
  k1_rtl8852bs_write_le32(
    content + K1_RTL8852BS_MACID_SLEEP_MASK_DWORD * sizeof(uint32_t), 1u);
  return OK;
}

static int k1_rtl8852bs_runtime_done_ack_match(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg)
{
  FAR struct k1_rtl8852bs_done_ack_match_s *match = arg;
  uint32_t word;
  uint8_t category;
  uint8_t class_id;
  uint8_t function;
  uint8_t sequence;

  if (c2h == NULL || match == NULL)
    {
      return -EINVAL;
    }

  match->c2h_frames++;
  match->last_c2h_category = c2h->category;
  match->last_c2h_class_id = c2h->class_id;
  match->last_c2h_function = c2h->function;
  match->last_c2h_sequence = c2h->sequence;

  if (c2h->category != K1_RTL8852BS_RUNTIME_DONE_ACK_CATEGORY ||
      c2h->class_id != K1_RTL8852BS_RUNTIME_DONE_ACK_CLASS ||
      c2h->function != K1_RTL8852BS_RUNTIME_DONE_ACK_FUNCTION)
    {
      return OK;
    }

  if (c2h->content_length != K1_RTL8852BS_RUNTIME_DONE_ACK_SIZE)
    {
      return -EPROTO;
    }

  word = k1_rtl8852bs_read_le32(c2h->content);
  match->generic_done_ack_frames++;
  match->last_done_ack_word = word;
  match->saw_done_ack = true;
  category = word & K1_RTL8852BS_H2C_HEADER_CATEGORY_MASK;
  class_id = (word >> K1_RTL8852BS_H2C_HEADER_CLASS_SHIFT) &
             K1_RTL8852BS_H2C_HEADER_CLASS_MASK;
  function = (word >> K1_RTL8852BS_H2C_HEADER_FUNCTION_SHIFT) &
             K1_RTL8852BS_H2C_HEADER_FUNCTION_MASK;
  sequence = word >> K1_RTL8852BS_H2C_HEADER_SEQUENCE_SHIFT;
  if (category == match->category && class_id == match->class_id &&
      function == match->function && sequence == match->sequence)
    {
      match->firmware_return = (word >> 16) & 0xffu;
      match->matched = true;
    }

  return OK;
}

struct k1_rtl8852bs_done_ack_pair_match_s
{
  struct k1_rtl8852bs_done_ack_match_s role;
  struct k1_rtl8852bs_done_ack_match_s cam;
};

static int k1_rtl8852bs_runtime_done_ack_pair_match(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg)
{
  FAR struct k1_rtl8852bs_done_ack_pair_match_s *match = arg;
  int ret;

  if (match == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_runtime_done_ack_match(c2h, &match->role);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_runtime_done_ack_match(c2h, &match->cam);
}

static int k1_rtl8852bs_runtime_role_cam_done_ack_wait(
  uint8_t sequence, FAR uint8_t *role_firmware_return,
  FAR uint8_t *cam_firmware_return)
{
  struct k1_rtl8852bs_done_ack_pair_match_s match =
  {
    .role =
    {
      .category = K1_RTL8852BS_FWROLE_MAINTAIN_CATEGORY,
      .class_id = K1_RTL8852BS_FWROLE_MAINTAIN_CLASS,
      .function = K1_RTL8852BS_FWROLE_MAINTAIN_FUNCTION,
      .sequence = sequence
    },
    .cam =
    {
      .category = K1_RTL8852BS_ADDR_CAM_CATEGORY,
      .class_id = K1_RTL8852BS_ADDR_CAM_CLASS,
      .function = K1_RTL8852BS_ADDR_CAM_FUNCTION,
      .sequence = sequence
    }
  };

  struct k1_rtl8852bs_rx_frame_s frame;
  FAR uint8_t *buffer;
  size_t length;
  size_t offset;
  unsigned int attempt;
  int ret;

  if (role_firmware_return == NULL || cam_firmware_return == NULL)
    {
      return -EINVAL;
    }

  buffer = kmm_malloc(K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  /* This bounded poll can issue a thousand successful CMD53 reads.  Retain
   * SDHCI error reporting, but do not make UART trace output lengthen the
   * firmware's one-second acknowledgement window.
   */

  k1_sdio_wifi_suppress_command_trace(true);
  for (attempt = 0; attempt < K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_runtime_rx_read(
        buffer, K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX, &length);
      if (ret == -EAGAIN)
        {
          up_mdelay(K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC);
          continue;
        }

      if (ret < 0)
        {
          goto out;
        }

      offset = 0;
      while (offset < length)
        {
          ret = k1_rtl8852bs_runtime_rx_parse(buffer, length, offset,
                                               &frame);
          if (ret < 0 || frame.next_offset <= offset)
            {
              ret = ret < 0 ? ret : -EPROTO;
              goto out;
            }

          if (!frame.crc_error && !frame.icv_error &&
              frame.packet_type == K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE)
            {
              ret = k1_rtl8852bs_runtime_c2h_dispatch(
                buffer + frame.payload_offset, frame.payload_length,
                k1_rtl8852bs_runtime_done_ack_pair_match, &match);
              if (ret < 0)
                {
                  goto out;
                }

              if (match.role.matched && match.cam.matched)
                {
                  *role_firmware_return = match.role.firmware_return;
                  *cam_firmware_return = match.cam.firmware_return;
                  ret = match.role.firmware_return == 0 &&
                        match.cam.firmware_return == 0 ? OK : -EIO;
                  goto out;
                }
            }

          offset = frame.next_offset;
        }
    }

  ret = -ETIMEDOUT;

out:
  k1_sdio_wifi_suppress_command_trace(false);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: runtime role/CAM done-ack ");
      k1_early_puts("wait sequence=");
      k1_early_puthex(sequence);
      k1_early_puts(" error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" C2H=");
      k1_early_puthex(match.role.c2h_frames);
      k1_early_puts(" done-ack=");
      k1_early_puthex(match.role.generic_done_ack_frames);
      k1_early_puts(" role=");
      k1_early_puthex(match.role.matched ? 1 : 0);
      k1_early_puts(" CAM=");
      k1_early_puthex(match.cam.matched ? 1 : 0);
      k1_early_puts(" last-done-ack=");
      k1_early_puthex(match.role.last_done_ack_word);
      k1_early_puts("\r\n");
    }

  kmm_free(buffer);
  return ret;
}

static int k1_rtl8852bs_runtime_done_ack_wait(
  uint8_t category, uint8_t class_id, uint8_t function, uint8_t sequence,
  FAR uint8_t *firmware_return)
{
  struct k1_rtl8852bs_done_ack_match_s match =
  {
    .category = category,
    .class_id = class_id,
    .function = function,
    .sequence = sequence
  };

  struct k1_rtl8852bs_rx_frame_s frame;
  FAR uint8_t *buffer;
  size_t length;
  size_t offset;
  unsigned int attempt;
  int ret;

  if (firmware_return == NULL)
    {
      return -EINVAL;
    }

  buffer = kmm_malloc(K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  k1_sdio_wifi_suppress_command_trace(true);
  for (attempt = 0; attempt < K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_runtime_rx_read(
        buffer, K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX, &length);
      if (ret == -EAGAIN)
        {
          up_mdelay(K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC);
          continue;
        }

      if (ret < 0)
        {
          goto out;
        }

      offset = 0;
      while (offset < length)
        {
          ret = k1_rtl8852bs_runtime_rx_parse(buffer, length, offset,
                                               &frame);
          if (ret < 0 || frame.next_offset <= offset)
            {
              ret = ret < 0 ? ret : -EPROTO;
              goto out;
            }

          if (!frame.crc_error && !frame.icv_error &&
              frame.packet_type == K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE)
            {
              ret = k1_rtl8852bs_runtime_c2h_dispatch(
                buffer + frame.payload_offset, frame.payload_length,
                k1_rtl8852bs_runtime_done_ack_match, &match);
              if (ret < 0)
                {
                  goto out;
                }

              if (match.matched)
                {
                  *firmware_return = match.firmware_return;
                  ret = match.firmware_return == 0 ? OK : -EIO;
                  goto out;
                }
            }

          offset = frame.next_offset;
        }
    }

  ret = -ETIMEDOUT;

out:
  k1_sdio_wifi_suppress_command_trace(false);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: runtime single done-ack wait ");
      k1_early_puts("category=");
      k1_early_puthex(category);
      k1_early_puts(" class=");
      k1_early_puthex(class_id);
      k1_early_puts(" function=");
      k1_early_puthex(function);
      k1_early_puts(" sequence=");
      k1_early_puthex(sequence);
      k1_early_puts(" error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" C2H=");
      k1_early_puthex(match.c2h_frames);
      k1_early_puts(" done-ack=");
      k1_early_puthex(match.generic_done_ack_frames);
      k1_early_puts(" last=");
      k1_early_puthex(match.last_done_ack_word);
      k1_early_puts("\r\n");
    }

  kmm_free(buffer);
  return ret;
}

/* The fwcmd_cmd_ofld C2H result parser and the per-batch wait are shared by
 * every sequence this component offloads: the BB PHY CR image below, the BB
 * reset that follows it, and the RF radio A/B images further down.
 */

#if defined(CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC) || \
    defined(CONFIG_K1_RTL8852BS2_RUNTIME_BB_RESET_DIAGNOSTIC) || \
    defined(CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC)

struct k1_rtl8852bs_cmd_ofld_match_s
{
  uint32_t offset;
  uint32_t expected;
  uint32_t actual;
  uint16_t c2h_frames;
  uint8_t sequence;
  uint8_t command;
  bool matched;
  bool failed;
};

static int k1_rtl8852bs_runtime_cmd_ofld_match(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg)
{
  FAR struct k1_rtl8852bs_cmd_ofld_match_s *match = arg;
  uint32_t word;

  if (c2h == NULL || match == NULL)
    {
      return -EINVAL;
    }

  match->c2h_frames++;
  if (c2h->category != K1_RTL8852BS_CMD_OFLD_CATEGORY ||
      c2h->class_id != K1_RTL8852BS_CMD_OFLD_C2H_CLASS ||
      c2h->function != K1_RTL8852BS_CMD_OFLD_C2H_FUNCTION)
    {
      return OK;
    }

  /* Realtek c2h_cmd_ofld_rsp_hdl() always consumes the result dword, but
   * supplies the failing command details only for an error response.
   */

  if (c2h->content_length < sizeof(uint32_t))
    {
      return -EPROTO;
    }

  word = k1_rtl8852bs_read_le32(c2h->content);
  match->failed = (word & K1_RTL8852BS_CMD_OFLD_RESPONSE_ERROR) != 0;
  match->command =
    (word >> K1_RTL8852BS_CMD_OFLD_RESPONSE_COMMAND_SHIFT) &
    K1_RTL8852BS_CMD_OFLD_RESPONSE_COMMAND_MASK;
  if (c2h->content_length >= K1_RTL8852BS_CMD_OFLD_RESPONSE_SIZE)
    {
      match->offset = k1_rtl8852bs_read_le32(c2h->content + 4);
      match->expected = k1_rtl8852bs_read_le32(c2h->content + 8);
      match->actual = k1_rtl8852bs_read_le32(c2h->content + 12);
    }
  match->matched = true;
  return OK;
}

static int k1_rtl8852bs_runtime_cmd_ofld_wait(FAR const char *label,
                                               uint8_t sequence,
                                               unsigned int batch,
                                               unsigned int table_index)
{
  struct k1_rtl8852bs_cmd_ofld_match_s match =
  {
    .sequence = sequence
  };

  struct k1_rtl8852bs_rx_frame_s frame;
  FAR uint8_t *buffer;
  size_t length;
  size_t offset;
  unsigned int attempt;
  int ret;

  buffer = kmm_malloc(K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  k1_sdio_wifi_suppress_command_trace(true);
  for (attempt = 0; attempt < K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_runtime_rx_read(
        buffer, K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX, &length);
      if (ret == -EAGAIN)
        {
          up_mdelay(K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC);
          continue;
        }

      if (ret < 0)
        {
          goto out;
        }

      offset = 0;
      while (offset < length)
        {
          ret = k1_rtl8852bs_runtime_rx_parse(buffer, length, offset,
                                               &frame);
          if (ret < 0 || frame.next_offset <= offset)
            {
              ret = ret < 0 ? ret : -EPROTO;
              goto out;
            }

          if (!frame.crc_error && !frame.icv_error &&
              frame.packet_type == K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE)
            {
              ret = k1_rtl8852bs_runtime_c2h_dispatch(
                buffer + frame.payload_offset, frame.payload_length,
                k1_rtl8852bs_runtime_cmd_ofld_match, &match);
              if (ret < 0)
                {
                  goto out;
                }

              if (match.matched)
                {
                  ret = match.failed ? -EIO : OK;
                  goto out;
                }
            }

          offset = frame.next_offset;
        }
    }

  ret = -ETIMEDOUT;

out:
  k1_sdio_wifi_suppress_command_trace(false);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: ");
      k1_early_puts(label);
      k1_early_puts(" offload response batch=");
      k1_early_puthex(batch);
      k1_early_puts(" index=");
      k1_early_puthex(table_index + match.command);
      k1_early_puts(" sequence=");
      k1_early_puthex(sequence);
      k1_early_puts(" error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" C2H=");
      k1_early_puthex(match.c2h_frames);
      k1_early_puts(" result=");
      k1_early_puthex(match.failed ? 1 : 0);
      k1_early_puts(" command=");
      k1_early_puthex(match.command);
      k1_early_puts(" offset=");
      k1_early_puthex(match.offset);
      k1_early_puts(" expected=");
      k1_early_puthex(match.expected);
      k1_early_puts(" actual=");
      k1_early_puthex(match.actual);
      k1_early_puts("\r\n");
    }

  kmm_free(buffer);
  return ret;
}

#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC

static int k1_rtl8852bs_runtime_phy_cr_offload_init(void)
{
  uint8_t content[K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX];
  uint32_t fifo_address;
  uint16_t available_pages;
  unsigned int batch;
  unsigned int entry;
  unsigned int entries;
  unsigned int index;
  size_t table_count;
  int ret;

  table_count = sizeof(g_k1_rtl8852bs_phy_cr_registers) /
                sizeof(g_k1_rtl8852bs_phy_cr_registers[0]);
  if (K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH == 0 ||
      K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH >
      K1_RTL8852BS_CMD_OFLD_RESPONSE_COMMAND_MASK + 1u)
    {
      return -EINVAL;
    }

  for (batch = 0, index = 0; index < table_count; batch++)
    {
      entries = table_count - index;
      if (entries > K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH)
        {
          entries = K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH;
        }

      memset(content, 0, entries * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE);
      for (entry = 0; entry < entries; entry++)
        {
          uint32_t command;

          command =
            (uint32_t)g_k1_rtl8852bs_phy_cr_registers[index + entry].address
            << K1_RTL8852BS_CMD_OFLD_OFFSET_SHIFT;
          command |= entry << K1_RTL8852BS_CMD_OFLD_COMMAND_SHIFT;
          if (entry + 1 == entries)
            {
              command |= K1_RTL8852BS_CMD_OFLD_LAST_COMMAND;
            }

          k1_rtl8852bs_write_le32(
            content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE, command);
          k1_rtl8852bs_write_le32(
            content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 8,
            g_k1_rtl8852bs_phy_cr_registers[index + entry].value);
          k1_rtl8852bs_write_le32(
            content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 12,
            UINT32_MAX);
        }

      if ((batch & 0x0fu) == 0 || index + entries == table_count)
        {
          k1_early_puts("K1 Wi-Fi GPL: PHY CR offload submit batch=");
          k1_early_puthex(batch);
          k1_early_puts(" index=");
          k1_early_puthex(index);
          k1_early_puts(" entries=");
          k1_early_puthex(entries);
          k1_early_puts(" sequence=");
          k1_early_puthex(K1_RTL8852BS_CMD_OFLD_SEQUENCE_BASE + batch);
          k1_early_puts("\r\n");
        }

      ret = k1_rtl8852bs_runtime_control_h2c_submit(
        content, entries * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE,
        K1_RTL8852BS_CMD_OFLD_CATEGORY, K1_RTL8852BS_CMD_OFLD_H2C_CLASS,
        K1_RTL8852BS_CMD_OFLD_H2C_FUNCTION,
        K1_RTL8852BS_CMD_OFLD_SEQUENCE_BASE + batch, false, &fifo_address,
        &available_pages);
      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: PHY CR offload submit error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts(" batch=");
          k1_early_puthex(batch);
          k1_early_puts(" index=");
          k1_early_puthex(index);
          k1_early_puts(" FIFO=");
          k1_early_puthex(fifo_address);
          k1_early_puts(" pages=");
          k1_early_puthex(available_pages);
          k1_early_puts("\r\n");
          return ret;
        }

      ret = k1_rtl8852bs_runtime_cmd_ofld_wait(
        "PHY CR", K1_RTL8852BS_CMD_OFLD_SEQUENCE_BASE + batch, batch, index);
      if (ret < 0)
        {
          return ret;
        }

      index += entries;
    }

  /* The upstream halbb_fw_set_reg() path treats a zero CMD_OFLD C2H result
   * as the completion contract.  PHY CR registers are not read back through
   * the host indirect window after WCPU runtime starts; that window returns
   * an invalid placeholder on this device in this state.
   */

  k1_early_puts("K1 Wi-Fi GPL: PHY CR offload entries=");
  k1_early_puthex(table_count);
  k1_early_puts(" batches=");
  k1_early_puthex(batch);
  k1_early_puts("\r\n");
  return OK;
}

#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_BB_RESET_DIAGNOSTIC

/* The second half of rtw_hal_init_bb_reg(): halbb_init_reg() has written the
 * phy_reg image in the stage above, halbb_reset_bb() now pulses the BB.  One
 * acknowledged offload batch carries the whole vendor sequence, so the
 * firmware applies it without a host access in the middle.
 */

static int k1_rtl8852bs_runtime_bb_reset_offload(void)
{
  uint8_t content[K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX];
  uint32_t fifo_address;
  uint32_t ppdu_stat = 0;
  uint16_t available_pages;
  unsigned int entry;
  size_t count;
  int ret;

  count = sizeof(g_k1_rtl8852bs_bb_reset_fields) /
          sizeof(g_k1_rtl8852bs_bb_reset_fields[0]);
  if (count == 0 || count > K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH)
    {
      return -EINVAL;
    }

  /* The offload sequence leaves R_AX_PPDU_STAT alone, while the host variant
   * of the vendor reset stops and restarts phy-sts reporting through bit 0 of
   * that register.  Nothing in this component programs it, so read it once
   * for the record before the pulse.  This is a MAC register, so the host
   * indirect window still returns a live value here.
   */

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PPDU_STAT, &ppdu_stat);
  if (ret < 0)
    {
      return ret;
    }

  memset(content, 0, count * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE);
  for (entry = 0; entry < count; entry++)
    {
      uint32_t command;

      command = (g_k1_rtl8852bs_bb_reset_fields[entry].address &
                 K1_RTL8852BS_CMD_OFLD_OFFSET_MASK)
                << K1_RTL8852BS_CMD_OFLD_OFFSET_SHIFT;
      command |= entry << K1_RTL8852BS_CMD_OFLD_COMMAND_SHIFT;
      if (entry + 1 == count)
        {
          command |= K1_RTL8852BS_CMD_OFLD_LAST_COMMAND;
        }

      k1_rtl8852bs_write_le32(
        content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE, command);
      k1_rtl8852bs_write_le32(
        content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 8,
        g_k1_rtl8852bs_bb_reset_fields[entry].value);
      k1_rtl8852bs_write_le32(
        content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 12,
        g_k1_rtl8852bs_bb_reset_fields[entry].mask);
    }

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, count * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE,
    K1_RTL8852BS_CMD_OFLD_CATEGORY, K1_RTL8852BS_CMD_OFLD_H2C_CLASS,
    K1_RTL8852BS_CMD_OFLD_H2C_FUNCTION,
    K1_RTL8852BS_BB_RESET_SEQUENCE_BASE, false, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: BB reset offload submit error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" entries=");
      k1_early_puthex(count);
      k1_early_puts(" FIFO=");
      k1_early_puthex(fifo_address);
      k1_early_puts(" pages=");
      k1_early_puthex(available_pages);
      k1_early_puts("\r\n");
      return ret;
    }

  /* One batch only, so a failed response ends the stage: the firmware may
   * already have applied part of the sequence and replaying a BB reset pulse
   * blind is not safe.
   */

  ret = k1_rtl8852bs_runtime_cmd_ofld_wait(
    "BB reset", K1_RTL8852BS_BB_RESET_SEQUENCE_BASE, 0, 0);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: BB reset offload entries=");
  k1_early_puthex(count);
  k1_early_puts(" sequence=");
  k1_early_puthex(K1_RTL8852BS_BB_RESET_SEQUENCE_BASE);
  k1_early_puts(" ppdu-stat=");
  k1_early_puthex(ppdu_stat);
  k1_early_puts(" phy-sts=");
  k1_early_puthex((ppdu_stat & K1_RTL8852BS_PPDU_STAT_RPT_EN) != 0 ? 1 : 0);
  k1_early_puts("\r\n");
  return OK;
}

#endif

int k1_rtl8852bs_runtime_bb_reset(void)
{
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_BB_RESET_DIAGNOSTIC
  return k1_rtl8852bs_runtime_bb_reset_offload();
#else
  return -ENOSYS;
#endif
}

int k1_rtl8852bs_fwdl_runtime_bb_reset_diagnostic(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 BB reset begin\r\n");
  ret = k1_rtl8852bs_runtime_bb_reset();
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 BB reset error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 BB reset complete\r\n");
  return OK;
}

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC

/* Replay halrf_sel_headline_8852b() against the running board.
 *
 * The vendor helper walks the headline block of array_mp_8852b_radio{a,b}[]
 * in five ordered cases and the vendor driver aborts the radio configuration
 * if none of them matches.  The generated headline table preserves the vendor
 * table order, so walking it here reaches the same row the vendor driver
 * would have selected.
 */

static int k1_rtl8852bs_rf_select_headline(uint8_t rfe_type, uint8_t chip_cv,
                                           FAR unsigned int *selected,
                                           FAR unsigned int *case_id)
{
  FAR const struct k1_rtl8852bs_rf_headline_s *rows;
  unsigned int count;
  unsigned int index;
  unsigned int best = 0;
  uint8_t cv_max = 0;
  bool found = false;

  rows = g_k1_rtl8852bs_rf_headlines;
  count = sizeof(g_k1_rtl8852bs_rf_headlines) /
          sizeof(g_k1_rtl8852bs_rf_headlines[0]);

  /* Case 1: {RFE:Match, cv:Match}. */

  for (index = 0; index < count; index++)
    {
      if (rows[index].rfe_type == rfe_type && rows[index].chip_cv == chip_cv)
        {
          *selected = index;
          *case_id = 1;
          return OK;
        }
    }

  /* Case 2: {RFE:Match, cv:Dont_Care}. */

  for (index = 0; index < count; index++)
    {
      if (rows[index].rfe_type == rfe_type &&
          rows[index].chip_cv == K1_RTL8852BS_RF_HEADLINE_DONT_CARE)
        {
          *selected = index;
          *case_id = 2;
          return OK;
        }
    }

  /* Case 3: {RFE:Match, cv:Max_in_Table}. */

  for (index = 0; index < count; index++)
    {
      if (rows[index].rfe_type == rfe_type && rows[index].chip_cv >= cv_max)
        {
          cv_max = rows[index].chip_cv;
          best = index;
          found = true;
        }
    }

  if (found)
    {
      *selected = best;
      *case_id = 3;
      return OK;
    }

  /* Case 4: {RFE:Dont_Care, cv:Max_in_Table}. */

  cv_max = 0;
  for (index = 0; index < count; index++)
    {
      if (rows[index].rfe_type == K1_RTL8852BS_RF_HEADLINE_DONT_CARE &&
          rows[index].chip_cv >= cv_max)
        {
          cv_max = rows[index].chip_cv;
          best = index;
          found = true;
        }
    }

  if (found)
    {
      *selected = best;
      *case_id = 4;
      return OK;
    }

  /* Case 5 is the vendor failure path: no headline describes this board. */

  return -ENOENT;
}

/* Refuse the offload unless the headline the vendor driver would select is
 * exactly the one the compiled images were generated from.
 */

static int k1_rtl8852bs_runtime_rf_cr_guard(void)
{
  FAR const struct k1_rtl8852bs_rf_context_s *context;
  unsigned int selected = 0;
  unsigned int case_id = 0;
  uint8_t images;
  int ret;

  context = &g_k1_rtl8852bs_rf_context;
  if (!context->valid)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF CR guard has no board RF context; "
                    "run the RF context stage first\r\n");
      return -ENODATA;
    }

  ret = k1_rtl8852bs_rf_select_headline(context->rfe_type, context->chip_cv,
                                        &selected, &case_id);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF CR guard no headline for rfe=");
      k1_early_puthex(context->rfe_type);
      k1_early_puts(" cv=");
      k1_early_puthex(context->chip_cv);
      k1_early_puts("; the vendor driver aborts here too\r\n");
      return ret;
    }

  images = g_k1_rtl8852bs_rf_headlines[selected].images;

  k1_early_puts("K1 Wi-Fi GPL: RF CR guard rfe=");
  k1_early_puthex(context->rfe_type);
  k1_early_puts(" cv=");
  k1_early_puthex(context->chip_cv);
  k1_early_puts(" case=");
  k1_early_puthex(case_id);
  k1_early_puts(" headline=");
  k1_early_puthex(selected);
  k1_early_puts(" images=");
  k1_early_puthex(images);
  k1_early_puts(" compiled_for rfe=");
  k1_early_puthex(K1_RTL8852BS_RF_IMAGE_RFE_TYPE);
  k1_early_puts(" cv=");
  k1_early_puthex(K1_RTL8852BS_RF_IMAGE_CHIP_CV);
  k1_early_puts("\r\n");

  if (images != K1_RTL8852BS_RF_HEADLINE_IMAGE_BOTH)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF CR guard refused: this board selects a "
                    "vendor branch that is not the compiled image\r\n");
      k1_early_puts("K1 Wi-Fi GPL: RF CR guard regenerate the images for "
                    "this rfe/cv with tools/k1_rtl8852bs_rf_table_gen.py"
                    "\r\n");
      return -ENOTSUP;
    }

  if (context->rfe_default)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF CR guard note: eFuse rfe cell is "
                    "blank, using the halrf default rfe type\r\n");
    }

  return OK;
}


/* Offload one generated radio parameter image.
 *
 * halrf_cfg_rf_radio_{a,b}_8852b() applies every entry through halrf_wrf(),
 * which routes an entry two different ways:
 *
 *   address & BIT(16)   RF D-die register.  halrf_wrf() masks the address to
 *                       eight bits, turns it into the BB aperture offset
 *                       offset_write_rf[path] + ((address & 0xff) << 2), and
 *                       emits a BB write offload, not an RF one.
 *   otherwise           RF serial-interface register on the given path,
 *                       emitted as an RF write offload.
 *
 * Both forms carry the vendor MASKRF mask, so the firmware performs a
 * read-modify-write of the low 20 bits and leaves the rest of the register
 * alone.
 *
 * Batching, the 16-byte fwcmd_cmd_ofld entry layout, and the C2H result wait
 * are the same mechanism the BB PHY CR image uses.  A batch that fails stops
 * the whole stage: the firmware may already have consumed part of the batch,
 * so replaying it could apply an entry twice.
 */

static int k1_rtl8852bs_runtime_rf_cr_offload_path(
  FAR const char *label, unsigned int path,
  FAR const struct k1_rtl8852bs_rf_register_value_s *table,
  size_t table_count, uint8_t sequence_base)
{
  uint8_t content[K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX];
  uint32_t fifo_address;
  uint16_t available_pages;
  unsigned int batch;
  unsigned int entry;
  unsigned int entries;
  unsigned int index;
  unsigned int sequence;
  int ret;

  if (K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH == 0 ||
      K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH >
      K1_RTL8852BS_CMD_OFLD_RESPONSE_COMMAND_MASK + 1u)
    {
      return -EINVAL;
    }

  for (batch = 0, index = 0; index < table_count; batch++)
    {
      sequence = (unsigned int)sequence_base + batch;
      if (sequence > K1_RTL8852BS_RF_CR_SEQUENCE_MAX)
        {
          k1_early_puts("K1 Wi-Fi GPL: ");
          k1_early_puts(label);
          k1_early_puts(" offload sequence window exhausted\r\n");
          return -ERANGE;
        }

      entries = table_count - index;
      if (entries > K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH)
        {
          entries = K1_RTL8852BS_CMD_OFLD_ENTRIES_PER_BATCH;
        }

      memset(content, 0, entries * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE);
      for (entry = 0; entry < entries; entry++)
        {
          uint32_t address = table[index + entry].address;
          uint32_t value = table[index + entry].value;
          uint32_t offset;
          uint32_t command;

          /* halrf_cfg_rf_radio_{a,b}_8852b() writes every table entry as a
           * register, so 0xf9..0xfe carries no vendor delay meaning here;
           * only halrf_cfg_rf_nctl_8852b() treats that range as a delay.
           * No entry of either vendor radio array, in any branch, falls in
           * it.  Refuse rather than write an address that could only come
           * from a table this walk does not understand.
           */

          if ((address & K1_RTL8852BS_RF_DDIE_FLAG) == 0 &&
              address >= K1_RTL8852BS_RF_DELAY_FIRST &&
              address <= K1_RTL8852BS_RF_DELAY_LAST)
            {
              k1_early_puts("K1 Wi-Fi GPL: ");
              k1_early_puts(label);
              k1_early_puts(" offload unexpected address=");
              k1_early_puthex(address);
              k1_early_puts("\r\n");
              return -EINVAL;
            }

          if (value > K1_RTL8852BS_RF_MASK)
            {
              k1_early_puts("K1 Wi-Fi GPL: ");
              k1_early_puts(label);
              k1_early_puts(" offload value exceeds MASKRF address=");
              k1_early_puthex(address);
              k1_early_puts(" value=");
              k1_early_puthex(value);
              k1_early_puts("\r\n");
              return -EINVAL;
            }

          if ((address & K1_RTL8852BS_RF_DDIE_FLAG) != 0)
            {
              offset = (path == K1_RTL8852BS_RF_PATH_A ?
                        K1_RTL8852BS_RF_DDIE_BB_BASE_A :
                        K1_RTL8852BS_RF_DDIE_BB_BASE_B) +
                       ((address & K1_RTL8852BS_RF_DDIE_ADDRESS_MASK) << 2);
              command = K1_RTL8852BS_CMD_OFLD_SOURCE_BB
                        << K1_RTL8852BS_CMD_OFLD_SOURCE_SHIFT;
            }
          else
            {
              offset = address & K1_RTL8852BS_RF_SI_ADDRESS_MASK;
              command = K1_RTL8852BS_CMD_OFLD_SOURCE_RF
                        << K1_RTL8852BS_CMD_OFLD_SOURCE_SHIFT;
              command |= path << K1_RTL8852BS_CMD_OFLD_PATH_SHIFT;
            }

          if (offset > K1_RTL8852BS_CMD_OFLD_OFFSET_MASK)
            {
              return -EINVAL;
            }

          command |= K1_RTL8852BS_CMD_OFLD_TYPE_WRITE
                     << K1_RTL8852BS_CMD_OFLD_TYPE_SHIFT;
          command |= entry << K1_RTL8852BS_CMD_OFLD_COMMAND_SHIFT;
          command |= offset << K1_RTL8852BS_CMD_OFLD_OFFSET_SHIFT;
          if (entry + 1 == entries)
            {
              command |= K1_RTL8852BS_CMD_OFLD_LAST_COMMAND;
            }

          k1_rtl8852bs_write_le32(
            content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE, command);
          k1_rtl8852bs_write_le32(
            content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 8, value);
          k1_rtl8852bs_write_le32(
            content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 12,
            K1_RTL8852BS_RF_MASK);
        }

      if ((batch & 0x0fu) == 0 || index + entries == table_count)
        {
          k1_early_puts("K1 Wi-Fi GPL: ");
          k1_early_puts(label);
          k1_early_puts(" offload submit batch=");
          k1_early_puthex(batch);
          k1_early_puts(" index=");
          k1_early_puthex(index);
          k1_early_puts(" entries=");
          k1_early_puthex(entries);
          k1_early_puts(" sequence=");
          k1_early_puthex(sequence);
          k1_early_puts("\r\n");
        }

      ret = k1_rtl8852bs_runtime_control_h2c_submit(
        content, entries * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE,
        K1_RTL8852BS_CMD_OFLD_CATEGORY, K1_RTL8852BS_CMD_OFLD_H2C_CLASS,
        K1_RTL8852BS_CMD_OFLD_H2C_FUNCTION, (uint8_t)sequence, false,
        &fifo_address, &available_pages);
      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: ");
          k1_early_puts(label);
          k1_early_puts(" offload submit error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts(" batch=");
          k1_early_puthex(batch);
          k1_early_puts(" index=");
          k1_early_puthex(index);
          k1_early_puts(" FIFO=");
          k1_early_puthex(fifo_address);
          k1_early_puts(" pages=");
          k1_early_puthex(available_pages);
          k1_early_puts("\r\n");
          return ret;
        }

      /* Stop on the first failing batch.  Do not replay a batch the
       * firmware may already have accepted.
       */

      ret = k1_rtl8852bs_runtime_cmd_ofld_wait(label, (uint8_t)sequence,
                                                batch, index);
      if (ret < 0)
        {
          return ret;
        }

      index += entries;
    }

  k1_early_puts("K1 Wi-Fi GPL: ");
  k1_early_puts(label);
  k1_early_puts(" offload entries=");
  k1_early_puthex(table_count);
  k1_early_puts(" batches=");
  k1_early_puthex(batch);
  k1_early_puts("\r\n");
  return OK;
}


/* The RF init config H2C that closes halrf_dm_init(), the rf_dm_init stage of
 * hal_start_8852b().
 *
 * halrf_dm_init() ends with
 *
 *   data_to_fw[0] = rf->phl_com->dev_cap.rfe_type;
 *   halrf_fill_h2c_cmd(rf, 4, FWCMD_H2C_RF_INIT_CFG, 0xa, H2CB_TYPE_DATA,
 *                      data_to_fw);
 *
 * so the payload is one little-endian word holding the board RFE type.  The
 * firmware does its own channel setting during scan offload, so without this
 * command it configures the RF front end for whatever RFE type it defaults to
 * instead of this board's.  Everything else in halrf_dm_init() - the NCTL
 * table, si_reset, AACK, LCK, RCK and DACK - is still missing and is a much
 * larger separate stage; this is the one part of it that is a single H2C on a
 * path that already works.
 *
 * The vendor clears both rec_ack and done_ack for this command, so there is no
 * C2H to match and no sequence number to correlate.  Sending it blind is still
 * verifiable one step short of an acknowledgement: the SDIO TX pages the
 * packet occupies are only returned to the channel 12 free-page count once the
 * firmware has dequeued it, so sample that count before the submit and poll it
 * back afterwards.  A count that never recovers means the firmware never took
 * the command, which is reported rather than retried.
 */

static int k1_rtl8852bs_runtime_rf_init_cfg_h2c(void)
{
  FAR const struct k1_rtl8852bs_rf_context_s *context;
  uint8_t content[K1_RTL8852BS_RF_INIT_CFG_SIZE];
  uint32_t fifo_address = 0;
  uint16_t before = 0;
  uint16_t granted = 0;
  uint16_t reclaimed = 0;
  unsigned int attempt;
  int ret;

  context = &g_k1_rtl8852bs_rf_context;
  if (!context->valid)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF init cfg H2C has no board RF context; "
                    "run the RF context stage first\r\n");
      return -ENODATA;
    }

  ret = k1_rtl8852bs_h2c_resource_read(&before);
  if (ret < 0)
    {
      return ret;
    }

  k1_rtl8852bs_write_le32(content, context->rfe_type);

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, sizeof(content), K1_RTL8852BS_RF_INIT_CFG_CATEGORY,
    K1_RTL8852BS_RF_INIT_CFG_CLASS, K1_RTL8852BS_RF_INIT_CFG_FUNCTION,
    K1_RTL8852BS_RF_INIT_CFG_H2C_SEQUENCE, false, &fifo_address, &granted);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF init cfg H2C submit error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" FIFO=");
      k1_early_puthex(fifo_address);
      k1_early_puts(" pages=");
      k1_early_puthex(granted);
      k1_early_puts("\r\n");
      return ret;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_RF_INIT_CFG_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_h2c_resource_read(&reclaimed);
      if (ret < 0)
        {
          return ret;
        }

      if (reclaimed >= before)
        {
          break;
        }

      up_mdelay(K1_RTL8852BS_RF_INIT_CFG_POLL_MSEC);
    }

  k1_early_puts("K1 Wi-Fi GPL: RF init cfg H2C rfe=");
  k1_early_puthex(context->rfe_type);
  k1_early_puts(" category=");
  k1_early_puthex(K1_RTL8852BS_RF_INIT_CFG_CATEGORY);
  k1_early_puts(" class=");
  k1_early_puthex(K1_RTL8852BS_RF_INIT_CFG_CLASS);
  k1_early_puts(" function=");
  k1_early_puthex(K1_RTL8852BS_RF_INIT_CFG_FUNCTION);
  k1_early_puts(" pages=");
  k1_early_puthex(before);
  k1_early_puts("->");
  k1_early_puthex(reclaimed);
  k1_early_puts(" polls=");
  k1_early_puthex(attempt);
  k1_early_puts("\r\n");

  if (reclaimed < before)
    {
      k1_early_puts("K1 Wi-Fi GPL: RF init cfg H2C page never returned; the "
                    "firmware has not dequeued the command\r\n");
      return -ETIMEDOUT;
    }

  return OK;
}


/* The init_rf_reg stage of hal_start_8852b().
 *
 * halrf_config_rf_parameter() runs halrf_config_radio(), which for 8852B is
 * halrf_cfg_rf_radio_a_8852b() followed by halrf_cfg_rf_radio_b_8852b(), and
 * then the power-by-rate, power limit, power limit RU, power track, and xtal
 * track tables.  Only the two radio images are reproduced here; the power
 * tables and the NCTL/RFK sequences are separate later stages.
 *
 * The vendor OUTSRC class 8 / class 9 radio-to-FW page upload is deliberately
 * not sent.  rtw_hal_rf_config_radio_to_fw() sits behind USE_TRUE_PHY and is
 * not reached from halrf_config_rf_parameter(), so the init_rf_reg stage never
 * uploads it; it also has no C2H completion contract, which would make it an
 * unverifiable blind H2C.  If a later stage turns out to need it, it belongs
 * with that stage and needs its own acknowledgement story.
 */

static int k1_rtl8852bs_runtime_rf_cr_offload_init(void)
{
  int ret;

  ret = k1_rtl8852bs_runtime_rf_cr_guard();
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_runtime_rf_cr_offload_path(
    "RF radio A", K1_RTL8852BS_RF_PATH_A,
    g_k1_rtl8852bs_rf_radio_a_registers,
    sizeof(g_k1_rtl8852bs_rf_radio_a_registers) /
    sizeof(g_k1_rtl8852bs_rf_radio_a_registers[0]),
    K1_RTL8852BS_RF_CR_SEQUENCE_BASE_A);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_runtime_rf_cr_offload_path(
    "RF radio B", K1_RTL8852BS_RF_PATH_B,
    g_k1_rtl8852bs_rf_radio_b_registers,
    sizeof(g_k1_rtl8852bs_rf_radio_b_registers) /
    sizeof(g_k1_rtl8852bs_rf_radio_b_registers[0]),
    K1_RTL8852BS_RF_CR_SEQUENCE_BASE_B);
  if (ret < 0)
    {
      return ret;
    }

  /* halrf_dm_init() runs after this stage in hal_start_8852b() and hands the
   * firmware the board RFE type on its way out.  The rest of that function is
   * not implemented yet.
   */

  return k1_rtl8852bs_runtime_rf_init_cfg_h2c();
}

#endif

int k1_rtl8852bs_runtime_rf_cr_init(void)
{
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC
  return k1_rtl8852bs_runtime_rf_cr_offload_init();
#else
  return -ENOSYS;
#endif
}

int k1_rtl8852bs_fwdl_runtime_rf_cr_diagnostic(void)
{
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 RF radio image begin\r\n");
  ret = k1_rtl8852bs_runtime_rf_cr_init();
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 RF radio image error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 RF radio image complete\r\n");
  return OK;
}

struct k1_rtl8852bs_loopback_aggregate_match_s
{
  uint16_t c2h_frames;
  uint16_t loopback_frames;
};

static int k1_rtl8852bs_runtime_loopback_aggregate_match(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg)
{
  FAR struct k1_rtl8852bs_loopback_aggregate_match_s *match = arg;
  unsigned int index;

  if (c2h == NULL || match == NULL)
    {
      return -EINVAL;
    }

  match->c2h_frames++;
  if (c2h->category != K1_RTL8852BS_H2C_LOOPBACK_C2H_CATEGORY ||
      c2h->class_id != K1_RTL8852BS_H2C_LOOPBACK_C2H_CLASS ||
      c2h->function != K1_RTL8852BS_H2C_LOOPBACK_C2H_FUNCTION)
    {
      return OK;
    }

  if (c2h->content_length != K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE)
    {
      return -EPROTO;
    }

  for (index = 0; index < c2h->content_length; index++)
    {
      if (c2h->content[index] != (uint8_t)index)
        {
          return -EIO;
        }
    }

  match->loopback_frames++;
  return OK;
}

static int k1_rtl8852bs_runtime_loopback_aggregate_wait(void)
{
  struct k1_rtl8852bs_loopback_aggregate_match_s match;
  struct k1_rtl8852bs_rx_frame_s frame;
  FAR uint8_t *buffer;
  size_t length;
  size_t offset;
  unsigned int attempt;
  int ret;

  memset(&match, 0, sizeof(match));
  buffer = kmm_malloc(K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  k1_sdio_wifi_suppress_command_trace(true);
  for (attempt = 0; attempt < K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_COUNT;
       attempt++)
    {
      ret = k1_rtl8852bs_runtime_rx_read(
        buffer, K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX, &length);
      if (ret == -EAGAIN)
        {
          up_mdelay(K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC);
          continue;
        }

      if (ret < 0)
        {
          goto out;
        }

      offset = 0;
      while (offset < length)
        {
          ret = k1_rtl8852bs_runtime_rx_parse(buffer, length, offset,
                                               &frame);
          if (ret < 0 || frame.next_offset <= offset)
            {
              ret = ret < 0 ? ret : -EPROTO;
              goto out;
            }

          if (!frame.crc_error && !frame.icv_error &&
              frame.packet_type == K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE)
            {
              ret = k1_rtl8852bs_runtime_c2h_dispatch(
                buffer + frame.payload_offset, frame.payload_length,
                k1_rtl8852bs_runtime_loopback_aggregate_match, &match);
              if (ret < 0)
                {
                  goto out;
                }

              if (match.loopback_frames ==
                  K1_RTL8852BS_H2C_AGG_LOOPBACK_COUNT)
                {
                  ret = OK;
                  goto out;
                }
            }

          offset = frame.next_offset;
        }
    }

  ret = -ETIMEDOUT;

out:
  k1_sdio_wifi_suppress_command_trace(false);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: runtime aggregate loopback wait error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" C2H=");
      k1_early_puthex(match.c2h_frames);
      k1_early_puts(" loopback=");
      k1_early_puthex(match.loopback_frames);
      k1_early_puts("\r\n");
    }

  kmm_free(buffer);
  return ret;
}

static int k1_rtl8852bs_runtime_h2c_aggregate_loopback_diagnostic(void)
{
  uint8_t content[K1_RTL8852BS_H2C_LOOPBACK_CONTENT_SIZE];
  const struct k1_rtl8852bs_runtime_h2c_command_s commands[] =
  {
    {
      .content = content,
      .content_length = sizeof(content),
      .category = K1_RTL8852BS_H2C_LOOPBACK_H2C_CATEGORY,
      .class_id = K1_RTL8852BS_H2C_LOOPBACK_H2C_CLASS,
      .function = K1_RTL8852BS_H2C_LOOPBACK_H2C_FUNCTION
    },
    {
      .content = content,
      .content_length = sizeof(content),
      .category = K1_RTL8852BS_H2C_LOOPBACK_H2C_CATEGORY,
      .class_id = K1_RTL8852BS_H2C_LOOPBACK_H2C_CLASS,
      .function = K1_RTL8852BS_H2C_LOOPBACK_H2C_FUNCTION
    }
  };

  uint16_t available_pages;
  uint32_t fifo_address;
  int ret;

  k1_rtl8852bs_h2c_loopback_content_fill(content);

  ret = k1_rtl8852bs_runtime_h2c_aggregate_submit(
    commands, sizeof(commands) / sizeof(commands[0]),
    K1_RTL8852BS_H2C_AGG_LOOPBACK_SEQUENCE, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime aggregate loopback H2C queued ");
  k1_early_puts("outer=1/9/0x15 sequence=2 pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_loopback_aggregate_wait();
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime aggregate loopback ");
  k1_early_puts("complete\r\n");
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_role_cam_done_ack_diagnostic
 *
 * Description:
 *   Create one volatile no-link station role with the eFuse self MAC, then
 *   configure its corresponding no-link address/BSSID CAM record.  The
 *   commands and their done acknowledgements use the recorded Realtek
 *   role.c, addr_cam.c and fwcmd.c layouts.  No BSSID, peer, scan,
 *   association, network device, or persistent state is created.
 ****************************************************************************/

int k1_rtl8852bs_fwdl_runtime_role_cam_done_ack_diagnostic(
  FAR const uint8_t *self_mac)
{
  struct k1_rtl8852bs_fwrole_maintain_info_s role =
  {
    .mac_id = 0u,
    .self_role = 0u,
    .update_mode = 0u,
    .wifi_role = 1u,
    .band = 0u,
    .port = 0u
  };

  struct k1_rtl8852bs_addr_cam_info_s cam =
  {
    .address_cam_index = 0u,
    .bssid_cam_index = 0u,
    .mac_id = 0u,
    .port = 0u,
    .network_type = 0u,
    .self_role = 0u,
    .address_mask = 0u,
    .mask_selection = 0u
  };

  uint8_t role_content[K1_RTL8852BS_FWROLE_MAINTAIN_SIZE];
  uint8_t cam_content[K1_RTL8852BS_ADDR_CAM_SIZE];
  uint8_t macid_content[K1_RTL8852BS_MACID_PAUSE_SLEEP_SIZE];
  uint8_t role_firmware_return = 0;
  uint8_t cam_firmware_return = 0;
  uint16_t available_pages;
  uint32_t fifo_address;
  int ret;

  if (self_mac == NULL || !k1_rtl8852bs_addr_cam_mac_valid(self_mac))
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_runtime_mac_function_enable();
  if (ret < 0)
    {
      goto error;
    }

  /* mac_trx_init() follows mac_sys_init() with DLE and HFC setup.  The
   * pre-firmware copies run before the runtime MAC function enable and are
   * reset by that transition, so restore them before any role/CAM H2C.
   */

  ret = k1_rtl8852bs_dle_scc_init();
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_hci_fc_init();
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_h2c_resource_read(&available_pages);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime role preflight H2C-pages=");
  k1_early_puthex(available_pages);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_addr_cam_init();
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime address CAM init complete\r\n");

  ret = k1_rtl8852bs_runtime_macid_unpause_build(
    macid_content, sizeof(macid_content));
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    macid_content, sizeof(macid_content),
    K1_RTL8852BS_MACID_PAUSE_SLEEP_CATEGORY,
    K1_RTL8852BS_MACID_PAUSE_SLEEP_CLASS,
    K1_RTL8852BS_MACID_PAUSE_SLEEP_FUNCTION, 1u, false, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime MACID unpause H2C queued category=");
  k1_early_puthex(K1_RTL8852BS_MACID_PAUSE_SLEEP_CATEGORY);
  k1_early_puts(" class=");
  k1_early_puthex(K1_RTL8852BS_MACID_PAUSE_SLEEP_CLASS);
  k1_early_puts(" function=");
  k1_early_puthex(K1_RTL8852BS_MACID_PAUSE_SLEEP_FUNCTION);
  k1_early_puts(" sequence=1 pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_h2c_aggregate_loopback_diagnostic();
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_runtime_fwrole_maintain_build(
    &role, role_content, sizeof(role_content));
  if (ret < 0)
    {
      goto error;
    }

  memcpy(cam.self_mac, self_mac, sizeof(cam.self_mac));

  /* The same address the receive accounting compares A1 and A2 against.  It is
   * recorded here rather than in the active sweep so that every sweep in the
   * run, including the passive ones that come first, reports its
   * address-checked counters against the address the hardware was actually
   * configured with.
   */

  k1_rtl8852bs_scanofld_set_self_mac(self_mac);
  ret = k1_rtl8852bs_runtime_addr_cam_build(&cam, cam_content,
                                             sizeof(cam_content));
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    role_content, sizeof(role_content),
    K1_RTL8852BS_FWROLE_MAINTAIN_CATEGORY,
    K1_RTL8852BS_FWROLE_MAINTAIN_CLASS,
    K1_RTL8852BS_FWROLE_MAINTAIN_FUNCTION,
    K1_RTL8852BS_ROLE_CAM_H2C_SEQUENCE, true, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime role direct H2C queued ");
  k1_early_puts("sequence=3 pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_done_ack_wait(
    K1_RTL8852BS_FWROLE_MAINTAIN_CATEGORY,
    K1_RTL8852BS_FWROLE_MAINTAIN_CLASS,
    K1_RTL8852BS_FWROLE_MAINTAIN_FUNCTION,
    K1_RTL8852BS_ROLE_CAM_H2C_SEQUENCE, &role_firmware_return);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime role direct done-ack return=");
  k1_early_puthex(role_firmware_return);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    cam_content, sizeof(cam_content), K1_RTL8852BS_ADDR_CAM_CATEGORY,
    K1_RTL8852BS_ADDR_CAM_CLASS, K1_RTL8852BS_ADDR_CAM_FUNCTION, 4u, true,
    &fifo_address, &available_pages);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime CAM direct H2C queued ");
  k1_early_puts("sequence=4 pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_done_ack_wait(
    K1_RTL8852BS_ADDR_CAM_CATEGORY, K1_RTL8852BS_ADDR_CAM_CLASS,
    K1_RTL8852BS_ADDR_CAM_FUNCTION, 4u, &cam_firmware_return);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: runtime CAM direct done-ack return=");
  k1_early_puthex(cam_firmware_return);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: runtime role/CAM done ack pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts(" sequences=1,2,3,4\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 runtime role/CAM done-ack "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: runtime role/CAM done-ack error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts(" firmware-return=");
  k1_early_puthex(role_firmware_return);
  k1_early_puts(":");
  k1_early_puthex(cam_firmware_return);
  k1_early_puts("\r\n");
  return ret;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_passive_ch1_build
 *
 * Description:
 *   Serialize one Realtek MAC/FW_OFLD/ADD_SCANOFLD_CH command.  The channel
 *   table is deliberately fixed to a single passive 2.4 GHz channel-1 entry:
 *   20 MHz and a 100 ms dwell period.  It retains the upstream no-link scan
 *   settings: notify firmware host code on channel entry and pause ordinary
 *   data TX while away from the operating channel.  It does not request a
 *   probe, null, data, or additional-frame transmission.  This bounded
 *   primitive does not submit an H2C.
 ****************************************************************************/

int k1_rtl8852bs_runtime_scanofld_passive_ch1_build(
  FAR uint8_t *content, size_t content_length)
{
  if (content == NULL ||
      content_length != K1_RTL8852BS_SCAN_OFLD_CH1_CONTENT_SIZE)
    {
      return -EINVAL;
    }

  memset(content, 0, K1_RTL8852BS_SCAN_OFLD_CH1_CONTENT_SIZE);

  /* ADD_SCANOFLD_CH dword0: one 28-byte (seven-dword) band-0 entry. */

  content[0] = 1u;
  content[1] = K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE / sizeof(uint32_t);

  /* The packed channel-info dword0 is period, dwell, central, primary. */

  content[4] = K1_RTL8852BS_SCAN_OFLD_PASSIVE_PERIOD_MSEC;
  content[6] = K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL;
  content[7] = K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL;

  /* The upstream no-link path sets c2h_notify_enterCH and pause_tx_data
   * before serializing struct mac_ax_scanofld_chinfo.  They are respectively
   * dword1 bits 6 and 13, stored in little-endian bytes 0 and 1 here.
   */

  content[8] = K1_RTL8852BS_SCAN_OFLD_NOTIFY_ENTER_CHANNEL;
  content[9] = K1_RTL8852BS_SCAN_OFLD_PAUSE_TX_DATA;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_probe_request_build
 *
 * Description:
 *   Serialize one wildcard Probe Request for firmware packet offload.  The
 *   frame is a bare 802.11 management frame: the firmware prepends its own
 *   transmit descriptor to an offloaded packet, so no host descriptor, no
 *   transmit page and no transmit ring is involved here, and the
 *   sequence-control field is left zero for the transmit hardware to fill.
 *
 * Input Parameters:
 *   frame        - Receives the frame.
 *   frame_length - Size of frame in bytes.
 *   self_mac     - The eFuse self MAC, used as transmitter address.  It is
 *                  the address the no-link role and the address CAM already
 *                  carry, so a Probe Response sent back to it is accepted by
 *                  the receive filter instead of being dropped as
 *                  unaddressed.
 *   length       - Receives the number of bytes written.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_probe_request_build(
  FAR uint8_t *frame, size_t frame_length, FAR const uint8_t *self_mac,
  FAR size_t *length)
{
  /* 1, 2, 5.5, 11, 6, 9, 12 and 18 Mbit/s in the 500 kbit/s units of the
   * Supported Rates element, then 24, 36, 48 and 54 Mbit/s in the Extended
   * Supported Rates element.  No rate carries the basic-rate bit: a Probe
   * Request only advertises what its transmitter supports.
   */

  static const uint8_t supported_rates[] =
    {
      0x02u, 0x04u, 0x0bu, 0x16u, 0x0cu, 0x12u, 0x18u, 0x24u
    };

  static const uint8_t extended_rates[] =
    {
      0x30u, 0x48u, 0x60u, 0x6cu
    };

  const size_t required = K1_RTL8852BS_IEEE80211_HEADER_SIZE + 2u +
                          2u + sizeof(supported_rates) +
                          2u + sizeof(extended_rates);
  size_t offset;

  if (frame == NULL || length == NULL || self_mac == NULL ||
      !k1_rtl8852bs_addr_cam_mac_valid(self_mac) || frame_length < required)
    {
      return -EINVAL;
    }

  memset(frame, 0, frame_length);
  frame[0] = (uint8_t)(K1_RTL8852BS_PROBE_REQUEST_FRAME_CONTROL & 0xffu);
  frame[1] = (uint8_t)(K1_RTL8852BS_PROBE_REQUEST_FRAME_CONTROL >> 8);

  /* Duration stays zero.  Address 1 is the broadcast destination, address 2
   * the transmitter and address 3 the wildcard BSSID.
   */

  memset(frame + 4, 0xff, 6);
  memcpy(frame + 10, self_mac, 6);
  memset(frame + 16, 0xff, 6);
  offset = K1_RTL8852BS_IEEE80211_HEADER_SIZE;

  frame[offset++] = K1_RTL8852BS_IEEE80211_SSID_IE;
  frame[offset++] = 0u;

  frame[offset++] = K1_RTL8852BS_PROBE_REQUEST_SUPPORTED_RATES_IE;
  frame[offset++] = (uint8_t)sizeof(supported_rates);
  memcpy(frame + offset, supported_rates, sizeof(supported_rates));
  offset += sizeof(supported_rates);

  frame[offset++] = K1_RTL8852BS_PROBE_REQUEST_EXTENDED_RATES_IE;
  frame[offset++] = (uint8_t)sizeof(extended_rates);
  memcpy(frame + offset, extended_rates, sizeof(extended_rates));
  offset += sizeof(extended_rates);

  *length = offset;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_auth_request_build
 *
 * Description:
 *   Serialize one open-system Authentication Request for the host transmit
 *   path.  Unlike the Probe Request this is never handed to firmware packet
 *   offload: the frame is unicast to one access point on one channel, so it is
 *   written into the band-0 management FIFO with a host descriptor in front of
 *   it, which is the descriptor k1_rtl8852bs_runtime_mgmt_tx_build() already
 *   proved reaches the protocol engine.
 *
 *   The sequence-control field is left zero.  The management descriptor
 *   selects neither hardware sequence numbering nor a MAC-table sequence
 *   counter, so the number in the descriptor owns the frame and no station
 *   record has to have been configured for the transmit to be legal.
 *
 * Input Parameters:
 *   frame        - Receives the frame.
 *   frame_length - Size of frame in bytes.
 *   self_mac     - The eFuse self MAC, used as transmitter address.
 *   bssid        - The access point selected from a scan result.  It is both
 *                  the destination and the BSSID of the frame.
 *   length       - Receives the number of bytes written.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_auth_request_build(
  FAR uint8_t *frame, size_t frame_length, FAR const uint8_t *self_mac,
  FAR const uint8_t *bssid, FAR size_t *length)
{
  size_t offset;

  if (frame == NULL || length == NULL || self_mac == NULL || bssid == NULL ||
      !k1_rtl8852bs_addr_cam_mac_valid(self_mac) ||
      !k1_rtl8852bs_addr_cam_mac_valid(bssid) ||
      frame_length < K1_RTL8852BS_AUTH_FRAME_SIZE)
    {
      return -EINVAL;
    }

  memset(frame, 0, frame_length);
  frame[0] = (uint8_t)(K1_RTL8852BS_AUTH_FRAME_CONTROL & 0xffu);
  frame[1] = (uint8_t)(K1_RTL8852BS_AUTH_FRAME_CONTROL >> 8);

  /* Duration stays zero for the hardware to fill.  Address 1 is the access
   * point, address 2 this host and address 3 the BSSID, which for an
   * infrastructure management frame is the access point again.
   */

  memcpy(frame + 4, bssid, 6);
  memcpy(frame + 10, self_mac, 6);
  memcpy(frame + 16, bssid, 6);
  offset = K1_RTL8852BS_IEEE80211_HEADER_SIZE;

  k1_rtl8852bs_write_le16(frame + offset, K1_RTL8852BS_AUTH_ALGORITHM_OPEN);
  k1_rtl8852bs_write_le16(frame + offset + 2,
                          K1_RTL8852BS_AUTH_SEQUENCE_REQUEST);
  k1_rtl8852bs_write_le16(frame + offset + 4, 0u);
  offset += K1_RTL8852BS_AUTH_BODY_SIZE;

  *length = offset;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_auth_transmit
 *
 * Description:
 *   Transmit the armed Authentication Request.  This is called from the
 *   scan-offload receive loop at the moment the firmware reports it has
 *   entered the access point's channel, because that notification is the only
 *   point at which this port knows what the radio is tuned to: the firmware
 *   holds a channel until the host submits the next-channel command, so the
 *   whole dwell is available to transmit into and to receive the answer on.
 *
 *   It is deliberately quiet.  The console is polled, and the dwell it runs
 *   inside is the same dwell the answer has to arrive in, so a full
 *   descriptor dump here would spend a large part of that dwell printing.
 *   One line before the transfer and the result line the diagnostic prints
 *   afterwards are enough; the verbose descriptor evidence already exists in
 *   k1_rtl8852bs_runtime_mgmt_tx_probe().
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_auth_transmit(void)
{
  uint8_t frame[K1_RTL8852BS_AUTH_FRAME_SIZE];
  size_t frame_length = 0;
  int ret;

  g_k1_rtl8852bs_auth_action.transmitted = true;
  if (!g_k1_rtl8852bs_scan_self_mac_valid)
    {
      g_k1_rtl8852bs_auth_action.transmit_status = -EINVAL;
      return;
    }

  ret = k1_rtl8852bs_runtime_auth_request_build(
    frame, sizeof(frame), g_k1_rtl8852bs_scan_self_mac,
    g_k1_rtl8852bs_auth_action.bssid, &frame_length);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_runtime_mgmt_tx_frame(frame, frame_length, 0u,
                                                false);
      if (ret >= 0)
        {
          g_k1_rtl8852bs_auth_action.requests++;
        }
    }

  g_k1_rtl8852bs_auth_action.transmit_status = ret;

  k1_early_puts("K1 Wi-Fi GPL: auth request tx channel=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.channel);
  k1_early_puts(" bytes=");
  k1_early_puthex((uintreg_t)frame_length);
  k1_early_puts(" status=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_pkt_ofld_add
 *
 * Description:
 *   Hand one packet to the firmware packet-offload table with the original
 *   MAC/FW_OFLD/PACKET_OFLD host command and require its generic done
 *   acknowledgement.  mac_add_pkt_ofld() sends the same command with both a
 *   receive and a done acknowledgement requested, so a firmware that stored
 *   the packet reports it here and a firmware that did not cannot be mistaken
 *   for one that did.
 *
 *   The identifier is not read back from the FWCMD_C2H_FUNC_PKT_OFLD_RSP
 *   report: this host allocates it before sending, exactly as the original
 *   allocates from its own identifier bitmap, and the done acknowledgement is
 *   what says whether that allocation was accepted.  One identifier is used
 *   for the lifetime of the image and reused by every sweep, so no delete
 *   command is needed between sweeps.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_pkt_ofld_add(
  FAR const uint8_t *packet, size_t packet_length, uint8_t packet_id)
{
  uint8_t content[K1_RTL8852BS_PKT_OFLD_HEADER_SIZE +
                  K1_RTL8852BS_PROBE_REQUEST_MAX_SIZE];
  uint8_t firmware_return = 0;
  uint16_t available_pages;
  uint32_t fifo_address;
  size_t content_length;
  int ret;

  if (packet == NULL || packet_length == 0 ||
      packet_length > K1_RTL8852BS_PROBE_REQUEST_MAX_SIZE ||
      packet_id == K1_RTL8852BS_PKT_OFLD_ID_NONE)
    {
      return -EINVAL;
    }

  content_length = K1_RTL8852BS_PKT_OFLD_HEADER_SIZE + packet_length;
  memset(content, 0, sizeof(content));
  k1_rtl8852bs_write_le32(
    content,
    (uint32_t)packet_id |
    ((uint32_t)K1_RTL8852BS_PKT_OFLD_OP_ADD <<
     K1_RTL8852BS_PKT_OFLD_OP_SHIFT) |
    ((uint32_t)packet_length << K1_RTL8852BS_PKT_OFLD_LENGTH_SHIFT));
  memcpy(content + K1_RTL8852BS_PKT_OFLD_HEADER_SIZE, packet, packet_length);

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, content_length, K1_RTL8852BS_PKT_OFLD_CATEGORY,
    K1_RTL8852BS_PKT_OFLD_CLASS, K1_RTL8852BS_PKT_OFLD_FUNCTION,
    K1_RTL8852BS_PKT_OFLD_H2C_SEQUENCE, true, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: probe-request packet-offload H2C queued id=");
  k1_early_puthex(packet_id);
  k1_early_puts(" bytes=");
  k1_early_puthex((uintreg_t)packet_length);
  k1_early_puts(" sequence=9 pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_done_ack_wait(
    K1_RTL8852BS_PKT_OFLD_CATEGORY, K1_RTL8852BS_PKT_OFLD_CLASS,
    K1_RTL8852BS_PKT_OFLD_FUNCTION, K1_RTL8852BS_PKT_OFLD_H2C_SEQUENCE,
    &firmware_return);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: probe-request packet-offload done-ack "
                "return=");
  k1_early_puthex(firmware_return);
  k1_early_puts("\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: probe-request packet-offload error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts(" firmware-return=");
  k1_early_puthex(firmware_return);
  k1_early_puts("\r\n");
  return ret;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_chlist_build
 *
 * Description:
 *   Serialize the same upstream scan channel-info layout for every 2.4 GHz
 *   primary channel, 1 through 13.  Firmware receives the complete list once
 *   before SCANOFLD starts; each channel is advanced only after its matching
 *   enter-channel C2H is observed below.
 *
 *   probe_id selects which of the two sweeps this component can request.
 *   K1_RTL8852BS_PKT_OFLD_ID_NONE leaves every entry byte-identical to the
 *   passive table validated on the board: no transmit bit and no
 *   probe-request identifier, so nothing is radiated.  An offloaded packet
 *   identifier sets the original chinfo tx_pkt bit and writes that identifier
 *   to probe_req_pkt_id, which is the pair rtw_hal_mac_scan_ofld_add_ch()
 *   fills in for an active scan and nothing else.  num_addition_pkt stays
 *   zero: in the original layout the probe request is not one of the eight
 *   additional packets, and the original passes none of those either.
 ****************************************************************************/

static int k1_rtl8852bs_runtime_scanofld_chlist_build(
  FAR uint8_t *content, size_t content_length, uint8_t probe_id)
{
  static const uint8_t channels[K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT] =
    {
      1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u
    };
  unsigned int index;

  if (content == NULL || content_length != K1_RTL8852BS_SCAN_OFLD_CONTENT_SIZE)
    {
      return -EINVAL;
    }

  memset(content, 0, content_length);
  content[0] = K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT;
  content[1] = K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE / sizeof(uint32_t);
  for (index = 0; index < K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT;
       index++)
    {
      FAR uint8_t *entry = content + K1_RTL8852BS_SCAN_OFLD_CONTENT_HEADER_SIZE +
                           index * K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE;

      entry[0] = K1_RTL8852BS_SCAN_OFLD_PASSIVE_PERIOD_MSEC;
      entry[2] = channels[index];
      entry[3] = channels[index];
      entry[4] = K1_RTL8852BS_SCAN_OFLD_NOTIFY_ENTER_CHANNEL;
      entry[5] = K1_RTL8852BS_SCAN_OFLD_PAUSE_TX_DATA;

      /* Byte 1 of channel-info dword 1 holds tx_pkt in its bit 4 and byte 2
       * holds probe_req_pkt_id, so an active entry differs from the passive
       * one by exactly those two fields.  pause_tx_data is kept in both: it
       * suspends the data queues during a dwell, not the scan's own
       * management transmission, and it belongs to the configuration that
       * was already validated.
       */

      if (probe_id != K1_RTL8852BS_PKT_OFLD_ID_NONE)
        {
          entry[5] |= K1_RTL8852BS_SCAN_OFLD_TX_PKT;
          entry[6] = probe_id;

          /* An active entry also asks for the pre- and post-transmit and the
           * leave-channel notifications.  They are the firmware's own account
           * of the transmission this entry requests, and they are the only
           * observation that separates a firmware that never reached the
           * transmit step from one that reached it and failed.  The passive
           * table keeps asking for the enter-channel notification alone, so
           * the validated sweep sees exactly the events it saw before.
           */

          entry[4] |= K1_RTL8852BS_SCAN_OFLD_NOTIFY_PRE_TX |
                      K1_RTL8852BS_SCAN_OFLD_NOTIFY_POST_TX |
                      K1_RTL8852BS_SCAN_OFLD_NOTIFY_LEAVE_CHANNEL;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_chlist_submit
 *
 * Description:
 *   Submit the 2.4 GHz 1-13 scan-offload table after the no-link role/CAM
 *   setup has completed, then validate the generic firmware done
 *   acknowledgement.  It does not issue the subsequent SCANOFLD start, create
 *   a wireless interface, or retain state after reset.
 *
 *   probe_id is passed straight to the channel-list builder, so
 *   K1_RTL8852BS_PKT_OFLD_ID_NONE submits the validated passive table that
 *   transmits nothing, and an offloaded packet identifier submits the same
 *   table with the original tx_pkt/probe_req_pkt_id pair set.
 ****************************************************************************/

static int k1_rtl8852bs_runtime_scanofld_chlist_submit(uint8_t probe_id)
{
  uint8_t content[K1_RTL8852BS_SCAN_OFLD_CONTENT_SIZE];
  uint8_t firmware_return = 0;
  uint16_t available_pages;
  uint32_t fifo_address;
  int ret;

  ret = k1_rtl8852bs_runtime_scanofld_chlist_build(content, sizeof(content),
                                                   probe_id);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, sizeof(content), K1_RTL8852BS_SCAN_OFLD_CATEGORY,
    K1_RTL8852BS_SCAN_OFLD_CLASS, K1_RTL8852BS_SCAN_OFLD_FUNCTION,
    K1_RTL8852BS_SCAN_OFLD_H2C_SEQUENCE, true, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan channel-list H2C queued ");
  k1_early_puts("channels=1-13 sequence=5 probe-id=");
  k1_early_puthex(probe_id);
  k1_early_puts(" pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  /* Dump the first serialized channel entry as it was handed to firmware.
   * Every entry differs from it only in the two channel bytes, and the whole
   * transmit question turns on three of its bit fields, so the bytes
   * themselves belong in the log rather than a decoded summary of them.
   */

  k1_early_puts("K1 Wi-Fi GPL: passive scan channel-list entry0=");
  k1_rtl8852bs_scanofld_log_bytes(
    content + K1_RTL8852BS_SCAN_OFLD_CONTENT_HEADER_SIZE,
    K1_RTL8852BS_SCAN_OFLD_CHANNEL_INFO_SIZE);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_done_ack_wait(
    K1_RTL8852BS_SCAN_OFLD_CATEGORY, K1_RTL8852BS_SCAN_OFLD_CLASS,
    K1_RTL8852BS_SCAN_OFLD_FUNCTION, K1_RTL8852BS_SCAN_OFLD_H2C_SEQUENCE,
    &firmware_return);
  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan channel-list done-ack return=");
  k1_early_puthex(firmware_return);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 passive scan channel-list "
                "done-ack complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: passive scan channel-list done-ack error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts(" firmware-return=");
  k1_early_puthex(firmware_return);
  k1_early_puts("\r\n");
  return ret;
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_scanofld_ch_done_ack_diagnostic
 *
 * Description:
 *   Submit the passive 2.4 GHz 1-13 scan-offload table and validate its
 *   generic firmware done acknowledgement.  This is the passive entry point:
 *   the table it submits carries no probe-request identifier and no transmit
 *   bit, so it transmits no probe, creates no wireless interface, and retains
 *   no state after reset.
 ****************************************************************************/

int k1_rtl8852bs_fwdl_runtime_scanofld_ch_done_ack_diagnostic(void)
{
  return k1_rtl8852bs_runtime_scanofld_chlist_submit(
    K1_RTL8852BS_PKT_OFLD_ID_NONE);
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_passive_start_build
 *
 * Description:
 *   Serialize the original MAC/FW_OFLD/SCANOFLD command for the queued
 *   passive channel list.  The original no-link path has no
 *   current channel definition, so target-channel mode remains clear.  A
 *   zero TSF selects immediate start and zero scan type selects one scan.
 ****************************************************************************/

static int k1_rtl8852bs_runtime_scanofld_passive_start_build(
  FAR uint8_t *content, size_t content_length)
{
  uint32_t dword0;

  if (content == NULL ||
      content_length != K1_RTL8852BS_SCAN_OFLD_START_CONTENT_SIZE)
    {
      return -EINVAL;
    }

  memset(content, 0, content_length);
  dword0 = K1_RTL8852BS_SCAN_OFLD_START_OPERATION <<
           K1_RTL8852BS_SCAN_OFLD_START_OPERATION_SHIFT;
  k1_rtl8852bs_write_le32(content, dword0);
  k1_rtl8852bs_write_le32(content + sizeof(uint32_t),
                           K1_RTL8852BS_SCAN_OFLD_START_NOTIFY_END);
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_next_channel_submit
 *
 * Description:
 *   Perform the host side of the original scan-offload state machine after
 *   the channel-entry C2H.  SCANOFLD holds the current channel until this
 *   FW_OFLD/SCANOFLD_DRV_CTRL/NEXT_CH command is received.  It requests no
 *   probe, null, data, or other transmitted frame.
 ****************************************************************************/

static int k1_rtl8852bs_runtime_scanofld_next_channel_submit(uint8_t channel)
{
  uint8_t content[K1_RTL8852BS_SCAN_OFLD_NEXT_CONTENT_SIZE];
  uint16_t available_pages;
  uint32_t fifo_address;
  int ret;

  memset(content, 0, sizeof(content));
  content[0] = channel;
  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, sizeof(content), K1_RTL8852BS_SCAN_OFLD_CATEGORY,
    K1_RTL8852BS_SCAN_OFLD_CLASS, K1_RTL8852BS_SCAN_OFLD_NEXT_FUNCTION,
    K1_RTL8852BS_SCAN_OFLD_NEXT_H2C_SEQUENCE, false, &fifo_address,
    &available_pages);
  if (ret >= 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: passive scan-offload next-channel H2C ");
      k1_early_puts("queued channel=");
      k1_early_puthex(channel);
      k1_early_puts(" sequence=7 pages=");
      k1_early_puthex(available_pages);
      k1_early_puts(" FIFO=");
      k1_early_puthex(fifo_address);
      k1_early_puts("\r\n");
    }

  return ret;
}

struct k1_rtl8852bs_scan_rx_filter_state_s
{
  uint32_t rcr;
  uint32_t plcp_header_filter;
  uint32_t rx_filter_option;
  uint32_t management_filter;
  bool active;
};

static void k1_rtl8852bs_scan_rx_filter_log(
  FAR const char *phase,
  FAR const struct k1_rtl8852bs_scan_rx_filter_state_s *state)
{
  uint32_t rcr = 0;
  uint32_t plcp_header_filter = 0;
  uint32_t rx_filter_option = 0;
  uint32_t management_filter = 0;

  if (state != NULL)
    {
      rcr = state->rcr;
      plcp_header_filter = state->plcp_header_filter;
      rx_filter_option = state->rx_filter_option;
      management_filter = state->management_filter;
    }

  k1_early_puts("K1 Wi-Fi GPL: scan RX filter ");
  k1_early_puts(phase);
  k1_early_puts(" ce00=");
  k1_early_puthex(rcr);
  k1_early_puts(" ce04=");
  k1_early_puthex(plcp_header_filter);
  k1_early_puts(" ce20=");
  k1_early_puthex(rx_filter_option);
  k1_early_puts(" ce28=");
  k1_early_puthex(management_filter);
  k1_early_puts("\r\n");
}

static int k1_rtl8852bs_scan_rx_filter_read(
  FAR struct k1_rtl8852bs_scan_rx_filter_state_s *state)
{
  int ret;

  if (state == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_RCR, &state->rcr);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PLCP_HDR_FLTR,
                                &state->plcp_header_filter);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_RX_FLTR_OPT,
                                &state->rx_filter_option);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_mac_read32(K1_RTL8852BS_MGNT_FLTR,
                                 &state->management_filter);
}

static int k1_rtl8852bs_scan_rx_filter_restore(
  FAR struct k1_rtl8852bs_scan_rx_filter_state_s *state)
{
  struct k1_rtl8852bs_scan_rx_filter_state_s current;
  int ret;
  int first_error = OK;

  if (state == NULL || !state->active)
    {
      return OK;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_RX_FLTR_OPT,
                                 state->rx_filter_option);
  if (ret < 0)
    {
      first_error = ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_MGNT_FLTR,
                                 state->management_filter);
  if (ret < 0 && first_error == OK)
    {
      first_error = ret;
    }

  ret = k1_rtl8852bs_scan_rx_filter_read(&current);
  if (ret < 0)
    {
      if (first_error == OK)
        {
          first_error = ret;
        }

      k1_early_puts("K1 Wi-Fi GPL: scan RX filter restore read error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
  }
  else
    {
      k1_rtl8852bs_scan_rx_filter_log("after", &current);
      if (current.rx_filter_option != state->rx_filter_option ||
          current.management_filter != state->management_filter)
        {
          first_error = -EIO;
        }
    }

  state->active = false;
  return first_error;
}

static int k1_rtl8852bs_scan_rx_filter_enable(
  FAR struct k1_rtl8852bs_scan_rx_filter_state_s *state)
{
  struct k1_rtl8852bs_scan_rx_filter_state_s current;
  uint32_t current_rx_filter_option;
  int ret;

  if (state == NULL)
    {
      return -EINVAL;
    }

  memset(state, 0, sizeof(*state));
  ret = k1_rtl8852bs_scan_rx_filter_read(state);
  if (ret < 0)
    {
      return ret;
    }

  k1_rtl8852bs_scan_rx_filter_log("before", state);
  state->active = true;

  /* RX_FLTR_OPT_MODE_SCAN accepts A1/broadcast/multicast traffic without
   * CAM matching and disables beacon CAM checks.  The host RX path also
   * needs every management subtype forwarded from RMAC to the SDIO host.
   * These are the minimal band-0 changes made by the vendor scan path.
   */

  current_rx_filter_option = state->rx_filter_option;
  current_rx_filter_option &= ~K1_RTL8852BS_SCAN_RX_FLTR_OPT_MASK;
  current_rx_filter_option |= K1_RTL8852BS_SCAN_RX_FLTR_OPT_VALUE;

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_RX_FLTR_OPT,
                                 current_rx_filter_option);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_MGNT_FLTR,
                                 K1_RTL8852BS_SCAN_MGNT_FLTR_TO_HOST);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_scan_rx_filter_read(&current);
  if (ret < 0)
    {
      goto error;
    }

  k1_rtl8852bs_scan_rx_filter_log("scan", &current);
  if ((current.rx_filter_option & K1_RTL8852BS_SCAN_RX_FLTR_OPT_MASK) !=
      K1_RTL8852BS_SCAN_RX_FLTR_OPT_VALUE ||
      current.management_filter != K1_RTL8852BS_SCAN_MGNT_FLTR_TO_HOST)
    {
      ret = -EIO;
      goto error;
    }

  return OK;

error:
  (void)k1_rtl8852bs_scan_rx_filter_restore(state);
  return ret;
}

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_PHY_COUNTER_DIAGNOSTIC

/****************************************************************************
 * Name: k1_rtl8852bs_rx_counter_read
 *
 * Description:
 *   Read one RMAC receive counter the way the original mac_rx_cnt() does:
 *   select the counter in byte 0 of R_AX_RX_DBG_CNT_SEL and take the value
 *   back out of the upper half word.  The counters are 16 bit and wrap, so
 *   only differences between two samples of the same counter are meaningful.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rx_counter_select(uint8_t index, FAR uint32_t *raw)
{
  uint32_t select = 0;
  int ret;

  if (raw == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_RX_DBG_CNT_SEL, &select);
  if (ret < 0)
    {
      return ret;
    }

  select &= K1_RTL8852BS_RX_DBG_CNT_KEEP_MASK;
  select &= ~K1_RTL8852BS_RX_DBG_CNT_RESET;
  select |= (uint32_t)index & K1_RTL8852BS_RX_DBG_CNT_INDEX_MASK;

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_RX_DBG_CNT_SEL, select);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_RX_DBG_CNT_SEL, &select);
  if (ret < 0)
    {
      return ret;
    }

  *raw = select;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_rx_counter_read
 *
 * Description:
 *   Return one counter value out of the selection window.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rx_counter_read(uint8_t index, FAR uint16_t *value)
{
  uint32_t raw = 0;
  int ret;

  if (value == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_rx_counter_select(index, &raw);
  if (ret < 0)
    {
      return ret;
    }

  *value = (uint16_t)((raw >> K1_RTL8852BS_RX_DBG_CNT_VALUE_SHIFT) &
                      K1_RTL8852BS_RX_DBG_CNT_VALUE_MASK);
  return OK;
}

struct k1_rtl8852bs_scan_phy_counters_s
{
  uint32_t crc_ok;
  uint32_t crc_fail;
  uint32_t false_alarm;
  uint16_t crc_ok_type[K1_RTL8852BS_RX_PPDU_TYPES];
  uint16_t crc_fail_type[K1_RTL8852BS_RX_PPDU_TYPES];
  uint16_t false_alarm_type[K1_RTL8852BS_RX_PPDU_TYPES];
  uint16_t clear_channel;
  uint16_t invalid;
  uint16_t full_drop;
  uint16_t full_drop_pkt;
  uint16_t rxdma;
  uint16_t filter_drop;
  uint32_t select_raw;
};

/****************************************************************************
 * Name: k1_rtl8852bs_scan_phy_counters_read
 *
 * Description:
 *   Sample the CRC-pass, CRC-fail and false-alarm counters of all eight PPDU
 *   types, the three sets rtw_hal_mac_get_rx_cnt() sums.  These count what
 *   RMAC was handed by the baseband, so they separate a baseband that never
 *   demodulates anything from frames that are demodulated and then dropped
 *   on the way to this host.
 *
 ****************************************************************************/

static int k1_rtl8852bs_scan_phy_counters_read(
  FAR struct k1_rtl8852bs_scan_phy_counters_s *counters)
{
  unsigned int type;
  int ret;

  if (counters == NULL)
    {
      return -EINVAL;
    }

  memset(counters, 0, sizeof(*counters));
  for (type = 0; type < K1_RTL8852BS_RX_PPDU_TYPES; type++)
    {
      ret = k1_rtl8852bs_rx_counter_read(
        g_k1_rtl8852bs_rx_crc_ok_index[type],
        &counters->crc_ok_type[type]);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_rx_counter_read(
        g_k1_rtl8852bs_rx_crc_fail_index[type],
        &counters->crc_fail_type[type]);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_rx_counter_read(
        g_k1_rtl8852bs_rx_false_alarm_index[type],
        &counters->false_alarm_type[type]);
      if (ret < 0)
        {
          return ret;
        }

      counters->crc_ok += counters->crc_ok_type[type];
      counters->crc_fail += counters->crc_fail_type[type];
      counters->false_alarm += counters->false_alarm_type[type];
    }

  /* The clear channel counter is read through the selection helper directly so
   * the whole selection word can be kept.  Its low bits must read back as the
   * index just written; that is the only proof this component has that the
   * counter window answers at all, and without it a snapshot of zeroes cannot
   * be told apart from a window that never responded.
   */

  ret = k1_rtl8852bs_rx_counter_select(K1_RTL8852BS_RX_CNT_RECCA,
                                       &counters->select_raw);
  if (ret < 0)
    {
      return ret;
    }

  counters->clear_channel =
    (uint16_t)((counters->select_raw >>
                K1_RTL8852BS_RX_DBG_CNT_VALUE_SHIFT) &
               K1_RTL8852BS_RX_DBG_CNT_VALUE_MASK);

  ret = k1_rtl8852bs_rx_counter_read(K1_RTL8852BS_RX_CNT_INVALID,
                                     &counters->invalid);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_rx_counter_read(K1_RTL8852BS_RX_CNT_FULL_DROP,
                                     &counters->full_drop);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_rx_counter_read(K1_RTL8852BS_RX_CNT_FULL_DROP_PKT,
                                     &counters->full_drop_pkt);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_rx_counter_read(K1_RTL8852BS_RX_CNT_RXDMA,
                                     &counters->rxdma);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_rx_counter_read(K1_RTL8852BS_RX_CNT_FILTER_DROP,
                                     &counters->filter_drop);
  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_scan_phy_counters_log
 *
 * Description:
 *   Print one counter snapshot.  When a baseline is supplied the differences
 *   are printed as well, because the counters are 16 bit, wrap, and are not
 *   reset by this component.  The per-type lines cover CCK, OFDM and HT: a
 *   2.4 GHz beacon or probe response arrives as one of those three.
 *
 ****************************************************************************/

static void k1_rtl8852bs_scan_phy_counters_log(
  FAR const char *phase,
  FAR const struct k1_rtl8852bs_scan_phy_counters_s *now,
  FAR const struct k1_rtl8852bs_scan_phy_counters_s *base)
{
  static const char *names[] =
  {
    "cck", "ofdm", "ht"
  };

  unsigned int type;

  k1_early_puts("K1 Wi-Fi GPL: scan PHY counters ");
  k1_early_puts(phase);
  k1_early_puts(" crc-ok=");
  k1_early_puthex(now->crc_ok);
  k1_early_puts(" crc-fail=");
  k1_early_puthex(now->crc_fail);
  k1_early_puts(" fa=");
  k1_early_puthex(now->false_alarm);
  if (base != NULL)
    {
      k1_early_puts(" delta-ok=");
      k1_early_puthex(now->crc_ok - base->crc_ok);
      k1_early_puts(" delta-fail=");
      k1_early_puthex(now->crc_fail - base->crc_fail);
      k1_early_puts(" delta-fa=");
      k1_early_puthex(now->false_alarm - base->false_alarm);
    }

  k1_early_puts("\r\n");

  /* The stage line says where a frame was lost when one was received at all,
   * and its raw= field is the selection word read back after the clear channel
   * index was written: its low six bits must be 0x1f.
   */

  k1_early_puts("K1 Wi-Fi GPL: scan PHY stage ");
  k1_early_puts(phase);
  k1_early_puts(" recca=");
  k1_early_puthex(now->clear_channel);
  k1_early_puts(" invd=");
  k1_early_puthex(now->invalid);
  k1_early_puts(" fulldrp=");
  k1_early_puthex(now->full_drop);
  k1_early_puts(" fulldrp-pkt=");
  k1_early_puthex(now->full_drop_pkt);
  k1_early_puts(" rxdma=");
  k1_early_puthex(now->rxdma);
  k1_early_puts(" pktfltr-drp=");
  k1_early_puthex(now->filter_drop);
  k1_early_puts(" raw=");
  k1_early_puthex(now->select_raw);
  if (base != NULL)
    {
      k1_early_puts(" delta-recca=");
      k1_early_puthex((uint32_t)(uint16_t)(now->clear_channel -
                                           base->clear_channel));
      k1_early_puts(" delta-rxdma=");
      k1_early_puthex((uint32_t)(uint16_t)(now->rxdma - base->rxdma));
      k1_early_puts(" delta-pktfltr-drp=");
      k1_early_puthex((uint32_t)(uint16_t)(now->filter_drop -
                                           base->filter_drop));
    }

  k1_early_puts("\r\n");

  for (type = 0; type < sizeof(names) / sizeof(names[0]); type++)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan PHY type ");
      k1_early_puts(phase);
      k1_early_puts(" ");
      k1_early_puts(names[type]);
      k1_early_puts(" ok=");
      k1_early_puthex(now->crc_ok_type[type]);
      k1_early_puts(" fail=");
      k1_early_puthex(now->crc_fail_type[type]);
      k1_early_puts(" fa=");
      k1_early_puthex(now->false_alarm_type[type]);
      k1_early_puts("\r\n");
    }
}

#endif

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC

/****************************************************************************
 * Name: k1_rtl8852bs_bb_read32 / k1_rtl8852bs_bb_write32
 *
 * Description:
 *   Reach a baseband control register from this host.  The register numbers in
 *   the original baseband sources are relative to bb0_cr_offset, so the offset
 *   has to be added before the shared indirect window sees them; without it the
 *   access lands on an unmapped MAC address and reads back nothing.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: k1_rtl8852bs_rf_read
 *
 * Description:
 *   Read one A-die RF register from this host, the way the original
 *   halbb_read_rf_reg_8852b_a() does: wait for the RF serial interface to be
 *   idle in both directions, write the path and offset into the read select
 *   register and confirm it latched, wait for the done bit, and take the 20
 *   bit result out of the status word.  This is a read-only path; every RF
 *   write in this component stays with the firmware through CMD_OFLD.
 *
 ****************************************************************************/

struct k1_rtl8852bs_rf_read_trace_s
{
  uint32_t pre_status;
  uint32_t select;
  uint32_t status;
  uint16_t select_polls;
  uint16_t done_polls;
};

static int k1_rtl8852bs_rf_read(uint8_t path, uint8_t address,
                                FAR uint32_t *value,
                                FAR struct k1_rtl8852bs_rf_read_trace_s
                                *trace)
{
  const uint32_t busy = K1_RTL8852BS_RF_READ_WRITE_BUSY |
                        K1_RTL8852BS_RF_READ_READ_BUSY;
  uint32_t select = 0;
  uint32_t status = 0;
  uint32_t wanted;
  unsigned int select_polls = 0;
  unsigned int attempt;
  int ret;

  if (value == NULL || path >= K1_RTL8852BS_RF_PATHS)
    {
      return -EINVAL;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_RF_READ_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_READ_STATUS, &status);
      if (ret < 0)
        {
          return ret;
        }

      if ((status & busy) == 0)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_RF_READ_POLL_USEC);
    }

  if ((status & busy) != 0)
    {
      return -EBUSY;
    }

  /* The status word as it stood before this transaction selected anything.
   * If the done bit is already set here then the done bit the read below
   * waits for is a leftover from an earlier transaction rather than evidence
   * that this one completed, and a value of zero taken from the same word
   * would say nothing about the radio.
   */

  if (trace != NULL)
    {
      trace->pre_status = status;
    }

  ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_READ_ADDRESS, &select);
  if (ret < 0)
    {
      return ret;
    }

  wanted = (((uint32_t)path << 8) | (uint32_t)address) &
           K1_RTL8852BS_RF_READ_SELECT_MASK;
  select &= ~K1_RTL8852BS_RF_READ_SELECT_MASK;
  select |= wanted;

  ret = k1_rtl8852bs_bb_write32(K1_RTL8852BS_RF_READ_ADDRESS, select);
  if (ret < 0)
    {
      return ret;
    }

  /* The vendor loops on the select register until it reads back what was
   * written, because the serial interface latches it only once it is free.
   */

  for (attempt = 0; attempt < K1_RTL8852BS_RF_READ_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_READ_ADDRESS, &select);
      if (ret < 0)
        {
          return ret;
        }

      if ((select & K1_RTL8852BS_RF_READ_SELECT_MASK) == wanted)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_RF_READ_POLL_USEC);
    }

  select_polls = attempt;
  if ((select & K1_RTL8852BS_RF_READ_SELECT_MASK) != wanted)
    {
      return -EIO;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_RF_READ_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_READ_STATUS, &status);
      if (ret < 0)
        {
          return ret;
        }

      if ((status & K1_RTL8852BS_RF_READ_DONE) != 0)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_RF_READ_POLL_USEC);
    }

  if ((status & K1_RTL8852BS_RF_READ_DONE) == 0)
    {
      return -ETIMEDOUT;
    }

  /* The whole status word and both poll counts are the only evidence that
   * the transaction happened rather than that this window reads a constant.
   * A done bit that was already set before the select was written, that is
   * a zero done poll count on every register, would make a value of zero
   * meaningless, so the caller gets to see them.
   */

  if (trace != NULL)
    {
      trace->select = select;
      trace->status = status;
      trace->select_polls = (uint16_t)select_polls;
      trace->done_polls = (uint16_t)attempt;
    }

  *value = status & K1_RTL8852BS_RF_READ_VALUE_MASK;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_rf_ddie_read
 *
 * Description:
 *   Read one RF register out of the D-die mirror, the way the original
 *   halbb_read_rf_reg_8852b_d() does: the mirror is a plain baseband register
 *   at 0xe000 or 0xf000 plus four times the offset, so it needs no serial
 *   interface transaction at all.  It is a different register set from the
 *   A-die, so it is not a second opinion on an A-die value; it is a second
 *   opinion on whether anything in the radio block answers.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rf_ddie_read(uint8_t path, uint8_t address,
                                     FAR uint32_t *value)
{
  uint32_t base;
  int ret;

  if (value == NULL || path >= K1_RTL8852BS_RF_PATHS)
    {
      return -EINVAL;
    }

  base = path == K1_RTL8852BS_RF_PATH_A ?
         K1_RTL8852BS_RF_DDIE_BB_BASE_A :
         K1_RTL8852BS_RF_DDIE_BB_BASE_B;

  ret = k1_rtl8852bs_bb_read32(base + ((uint32_t)address << 2), value);
  if (ret < 0)
    {
      return ret;
    }

  *value &= K1_RTL8852BS_RF_READ_VALUE_MASK;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_bb_update_field
 *
 * Description:
 *   Read modify write one baseband field.  The value is already positioned
 *   inside the mask.
 *
 ****************************************************************************/

static int k1_rtl8852bs_bb_update_field(uint32_t address, uint32_t mask,
                                        uint32_t value)
{
  uint32_t current;
  int ret;

  if ((value & ~mask) != 0)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_bb_read32(address, &current);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_bb_write32(address, (current & ~mask) | value);
}

/****************************************************************************
 * Name: k1_rtl8852bs_rf_si_reset
 *
 * Description:
 *   halrf_si_reset_8852b() followed by the 1000 us of halrf_si_reset().  The
 *   vendor calls this from halrf_dm_init() after the radio images and before
 *   the RC and LC calibrations, and comments the delay "wa for S0 RCK val = 0
 *   issue".  This component skipped the whole of halrf_dm_init() except its
 *   closing RF init config H2C, and the board reports exactly that symptom:
 *   every radio register reads back as zero on both paths.  Running the reset
 *   here, before the scan, is the vendor order relative to the radio images
 *   and it is the smallest part of halrf_dm_init() that can explain the
 *   symptom on its own.
 *
 *   Nothing here is a radio write: the two trigger fields, the two D die
 *   enables and the two A die interface bits are all interface control, and
 *   every one of them is put back to its enabled state before returning.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rf_si_reset(void)
{
  uint32_t hwsi_a;
  uint32_t hwsi_b;
  uint32_t ddie_a;
  uint32_t ddie_b;
  uint8_t adie_a;
  uint8_t adie_b;
  int ret;

  ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_HWSI_CTRL_A,
                                     K1_RTL8852BS_RF_HWSI_TRIGGER_MASK,
                                     K1_RTL8852BS_RF_HWSI_TRIGGER_OFF);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_HWSI_CTRL_B,
                                         K1_RTL8852BS_RF_HWSI_TRIGGER_MASK,
                                         K1_RTL8852BS_RF_HWSI_TRIGGER_OFF);
    }

  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_RF_SI_SETTLE_USEC);

  /* halrf_arfc_si_reset_8852b(rf, true): hold the A die interface of both
   * paths, path B first, exactly as the original does.
   */

  ret = k1_rtl8852bs_xtal_si_read(K1_RTL8852BS_XTAL_SI_WL_RFC_S1, &adie_b);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_write(
        K1_RTL8852BS_XTAL_SI_WL_RFC_S1,
        (uint8_t)(adie_b & ~K1_RTL8852BS_RF_ADIE_SI_MASK));
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_read(K1_RTL8852BS_XTAL_SI_WL_RFC_S0,
                                      &adie_a);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_write(
        K1_RTL8852BS_XTAL_SI_WL_RFC_S0,
        (uint8_t)(adie_a & ~K1_RTL8852BS_RF_ADIE_SI_MASK));
    }

  if (ret < 0)
    {
      return ret;
    }

  /* Hold the D die interface of both paths. */

  ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_DDIE_SI_CTRL_A,
                                     K1_RTL8852BS_RF_DDIE_SI_ENABLE, 0);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_DDIE_SI_CTRL_B,
                                         K1_RTL8852BS_RF_DDIE_SI_ENABLE, 0);
    }

  if (ret < 0)
    {
      return ret;
    }

  /* halrf_arfc_si_reset_8852b(rf, false): release the A die interface. */

  ret = k1_rtl8852bs_xtal_si_write(
    K1_RTL8852BS_XTAL_SI_WL_RFC_S1,
    (uint8_t)(adie_b | K1_RTL8852BS_RF_ADIE_SI_MASK));
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_write(
        K1_RTL8852BS_XTAL_SI_WL_RFC_S0,
        (uint8_t)(adie_a | K1_RTL8852BS_RF_ADIE_SI_MASK));
    }

  if (ret < 0)
    {
      return ret;
    }

  /* Re-enable the hardware trigger, then release the D die interface. */

  ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_HWSI_CTRL_A,
                                     K1_RTL8852BS_RF_HWSI_TRIGGER_MASK, 0);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_HWSI_CTRL_B,
                                         K1_RTL8852BS_RF_HWSI_TRIGGER_MASK,
                                         0);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_update_field(
        K1_RTL8852BS_RF_DDIE_SI_CTRL_A, K1_RTL8852BS_RF_DDIE_SI_ENABLE,
        K1_RTL8852BS_RF_DDIE_SI_ENABLE);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_update_field(
        K1_RTL8852BS_RF_DDIE_SI_CTRL_B, K1_RTL8852BS_RF_DDIE_SI_ENABLE,
        K1_RTL8852BS_RF_DDIE_SI_ENABLE);
    }

  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_RF_SI_RESET_USEC);

  ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_HWSI_CTRL_A, &hwsi_a);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_HWSI_CTRL_B, &hwsi_b);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_DDIE_SI_CTRL_A, &ddie_a);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_DDIE_SI_CTRL_B, &ddie_b);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_read(K1_RTL8852BS_XTAL_SI_WL_RFC_S0,
                                      &adie_a);
    }

  if (ret >= 0)
    {
      ret = k1_rtl8852bs_xtal_si_read(K1_RTL8852BS_XTAL_SI_WL_RFC_S1,
                                      &adie_b);
    }

  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: scan RF si-reset hwsi=");
  k1_early_puthex(hwsi_a);
  k1_early_puts(",");
  k1_early_puthex(hwsi_b);
  k1_early_puts(" ddie=");
  k1_early_puthex(ddie_a);
  k1_early_puts(",");
  k1_early_puthex(ddie_b);
  k1_early_puts(" adie=");
  k1_early_puthex(adie_a);
  k1_early_puts(",");
  k1_early_puthex(adie_b);
  k1_early_puts("\r\n");
  return OK;
}

struct k1_rtl8852bs_scan_rf_readback_s
{
  uint32_t mode[K1_RTL8852BS_RF_PATHS];
  uint32_t mode_save[K1_RTL8852BS_RF_PATHS];
  uint32_t channel[K1_RTL8852BS_RF_PATHS];
  uint32_t rck_trigger[K1_RTL8852BS_RF_PATHS];
  uint32_t rck_status[K1_RTL8852BS_RF_PATHS];
  uint32_t static_reg[K1_RTL8852BS_RF_PATHS];
  uint32_t ddie_channel[K1_RTL8852BS_RF_PATHS];
  struct k1_rtl8852bs_rf_read_trace_s trace[K1_RTL8852BS_RF_PATHS];
  uint32_t sentinel[K1_RTL8852BS_PHY_CR_SENTINELS];
};

/****************************************************************************
 * Name: k1_rtl8852bs_scan_rf_readback_read
 *
 * Description:
 *   Sample the RF mode, channel and RC calibration registers of both radio
 *   paths.  The low byte of the channel register is the channel the radio is
 *   actually tuned to, so a sample taken right after a scan says whether the
 *   firmware tuned the radio for the dwells at all, which is the question a
 *   receive path that reports no clear channel assessment raises first.
 *
 ****************************************************************************/

static int k1_rtl8852bs_scan_rf_readback_read(
  FAR struct k1_rtl8852bs_scan_rf_readback_s *readback)
{
  unsigned int index;
  unsigned int path;
  int ret;

  if (readback == NULL)
    {
      return -EINVAL;
    }

  memset(readback, 0, sizeof(*readback));
  for (path = 0; path < K1_RTL8852BS_RF_PATHS; path++)
    {
      ret = k1_rtl8852bs_rf_read((uint8_t)path, K1_RTL8852BS_RF_REG_MODE,
                                 &readback->mode[path], NULL);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_rf_read((uint8_t)path,
                                 K1_RTL8852BS_RF_REG_MODE_SAVE,
                                 &readback->mode_save[path], NULL);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_rf_read((uint8_t)path, K1_RTL8852BS_RF_REG_CHANNEL,
                                 &readback->channel[path],
                                 &readback->trace[path]);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_rf_read((uint8_t)path,
                                 K1_RTL8852BS_RF_REG_RCK_TRIGGER,
                                 &readback->rck_trigger[path], NULL);
      if (ret < 0)
        {
          return ret;
        }

      ret = k1_rtl8852bs_rf_read((uint8_t)path,
                                 K1_RTL8852BS_RF_REG_RCK_STATUS,
                                 &readback->rck_status[path], NULL);
      if (ret < 0)
        {
          return ret;
        }

      /* The register whose value the radio image of this path set and that
       * nothing since then has written.  A channel register that reads zero
       * after a scan has two explanations, a radio that was never tuned and a
       * read path that returns zero for everything; this register separates
       * them, because its value cannot have changed.
       */

      ret = k1_rtl8852bs_rf_read((uint8_t)path, K1_RTL8852BS_RF_REG_STATIC,
                                 &readback->static_reg[path], NULL);
      if (ret < 0)
        {
          return ret;
        }

      /* The same register number read the other way the original driver
       * offers: halbb_read_rf_reg_8852b_d() reads the D-die mirror as a
       * plain baseband register at 0xe000 or 0xf000 plus four times the
       * offset.  It is a different register set, so it is not a second
       * opinion on the channel value; it is a second opinion on whether
       * anything in the radio block answers at all.
       */

      ret = k1_rtl8852bs_rf_ddie_read((uint8_t)path,
                                      K1_RTL8852BS_RF_REG_CHANNEL,
                                      &readback->ddie_channel[path]);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Read the baseband registers whose value the PHY CR image is known to
   * have set.  They answer two questions at once: whether this baseband
   * read window returns real content, and whether the firmware side PHY CR
   * offload actually landed in the baseband.
   */

  if (sizeof(g_k1_rtl8852bs_phy_cr_sentinels) /
      sizeof(g_k1_rtl8852bs_phy_cr_sentinels[0]) !=
      K1_RTL8852BS_PHY_CR_SENTINELS)
    {
      return -EINVAL;
    }

  for (index = 0; index < K1_RTL8852BS_PHY_CR_SENTINELS; index++)
    {
      ret = k1_rtl8852bs_bb_read32(
        g_k1_rtl8852bs_phy_cr_sentinels[index].address,
        &readback->sentinel[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_scan_rf_readback_log
 *
 * Description:
 *   Print one RF snapshot, one line per radio path.
 *
 ****************************************************************************/

static void k1_rtl8852bs_scan_rf_readback_log(
  FAR const char *phase,
  FAR const struct k1_rtl8852bs_scan_rf_readback_s *readback)
{
  unsigned int index;
  unsigned int path;

  for (path = 0; path < K1_RTL8852BS_RF_PATHS; path++)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF readback ");
      k1_early_puts(phase);
      k1_early_puts(" path=");
      k1_early_puthex(path);
      k1_early_puts(" mode=");
      k1_early_puthex(readback->mode[path]);
      k1_early_puts(" r05=");
      k1_early_puthex(readback->mode_save[path]);
      k1_early_puts(" ch-reg=");
      k1_early_puthex(readback->channel[path]);
      k1_early_puts(" ch=");
      k1_early_puthex(readback->channel[path] &
                      K1_RTL8852BS_RF_CHANNEL_MASK);
      k1_early_puts(" rck=");
      k1_early_puthex(readback->rck_trigger[path]);
      k1_early_puts(" rck-sts=");
      k1_early_puthex(readback->rck_status[path]);
      k1_early_puts(" r5a=");
      k1_early_puthex(readback->static_reg[path]);
      k1_early_puts(" r5a-expect=");
      k1_early_puthex(path == K1_RTL8852BS_RF_PATH_A ?
                      K1_RTL8852BS_RF_STATIC_VALUE_A :
                      K1_RTL8852BS_RF_STATIC_VALUE_B);
      k1_early_puts("\r\n");

      k1_early_puts("K1 Wi-Fi GPL: scan RF readback trace ");
      k1_early_puts(phase);
      k1_early_puts(" path=");
      k1_early_puthex(path);
      k1_early_puts(" pre-status=");
      k1_early_puthex(readback->trace[path].pre_status);
      k1_early_puts(" select=");
      k1_early_puthex(readback->trace[path].select);
      k1_early_puts(" status=");
      k1_early_puthex(readback->trace[path].status);
      k1_early_puts(" select-polls=");
      k1_early_puthex(readback->trace[path].select_polls);
      k1_early_puts(" done-polls=");
      k1_early_puthex(readback->trace[path].done_polls);
      k1_early_puts(" ddie-ch=");
      k1_early_puthex(readback->ddie_channel[path]);
      k1_early_puts("\r\n");
    }

  for (index = 0; index < K1_RTL8852BS_PHY_CR_SENTINELS; index++)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan BB image ");
      k1_early_puts(phase);
      k1_early_puts(" index=");
      k1_early_puthex(index);
      k1_early_puts(" address=");
      k1_early_puthex(g_k1_rtl8852bs_phy_cr_sentinels[index].address);
      k1_early_puts(" expect=");
      k1_early_puthex(g_k1_rtl8852bs_phy_cr_sentinels[index].value);
      k1_early_puts(" actual=");
      k1_early_puthex(readback->sentinel[index]);
      k1_early_puts("\r\n");
    }
}

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC

/****************************************************************************
 * Name: k1_rtl8852bs_rf_write
 *
 * Description:
 *   Write one A-die RF register from this host, the way the original
 *   halbb_write_rf_reg_8852b_a() does: wait for the serial interface to be
 *   idle in both directions, put the bit mask in 0x374 and shift the value to
 *   the mask when the write is partial, build the command word out of the
 *   mask enable, the path, the offset and the value, and loop on writing 0x370
 *   and reading it back until it reads what was written.
 *
 *   Every RF write this component applies to the radio stays with the
 *   firmware through CMD_OFLD.  This primitive exists for one question the
 *   firmware side cannot answer: a CMD_OFLD write is acknowledged by the
 *   firmware whether or not the radio took it, so a radio that reads back as
 *   zero after the images were offloaded is equally consistent with a radio
 *   that never received them and with a radio that cannot be reached at all.
 *   A host write followed by a host read separates the two.
 *
 ****************************************************************************/

struct k1_rtl8852bs_rf_write_trace_s
{
  uint32_t pre_status;
  uint32_t command;
  uint32_t command_readback;
  uint16_t polls;
};

static int k1_rtl8852bs_rf_write(uint8_t path, uint8_t address,
                                 uint32_t mask, uint32_t value,
                                 FAR struct k1_rtl8852bs_rf_write_trace_s
                                 *trace)
{
  const uint32_t busy = K1_RTL8852BS_RF_READ_WRITE_BUSY |
                        K1_RTL8852BS_RF_READ_READ_BUSY;
  uint32_t status = 0;
  uint32_t command;
  uint32_t readback = 0;
  unsigned int shift;
  unsigned int attempt;
  bool mask_enable = false;
  int ret;

  if (path >= K1_RTL8852BS_RF_PATHS)
    {
      return -EINVAL;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_RF_WRITE_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_READ_STATUS, &status);
      if (ret < 0)
        {
          return ret;
        }

      if ((status & busy) == 0)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_RF_SI_SETTLE_USEC);
    }

  if ((status & busy) != 0)
    {
      return -EBUSY;
    }

  if (trace != NULL)
    {
      trace->pre_status = status;
    }

  value &= K1_RTL8852BS_RF_READ_VALUE_MASK;
  mask &= K1_RTL8852BS_RF_READ_VALUE_MASK;

  if (mask != K1_RTL8852BS_RF_READ_VALUE_MASK)
    {
      mask_enable = true;
      ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_WRITE_MASK_ADDRESS,
                                         K1_RTL8852BS_RF_READ_VALUE_MASK,
                                         mask);
      if (ret < 0)
        {
          return ret;
        }

      for (shift = 0; shift <= 19; shift++)
        {
          if (((mask >> shift) & 1u) != 0)
            {
              break;
            }
        }

      value = (value << shift) & K1_RTL8852BS_RF_READ_VALUE_MASK;
    }

  command = (mask_enable ? K1_RTL8852BS_RF_WRITE_MASK_ENABLE : 0u) |
            ((uint32_t)path << K1_RTL8852BS_RF_WRITE_PATH_SHIFT) |
            ((uint32_t)address << K1_RTL8852BS_RF_WRITE_OFFSET_SHIFT) |
            value;

  for (attempt = 0; attempt < K1_RTL8852BS_RF_WRITE_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_bb_write32(K1_RTL8852BS_RF_WRITE_ADDRESS, command);
      if (ret < 0)
        {
          return ret;
        }

      up_udelay(K1_RTL8852BS_RF_WRITE_POLL_USEC);

      ret = k1_rtl8852bs_bb_read32(K1_RTL8852BS_RF_WRITE_ADDRESS, &readback);
      if (ret < 0)
        {
          return ret;
        }

      if (readback == command)
        {
          break;
        }
    }

  up_udelay(K1_RTL8852BS_RF_WRITE_POLL_USEC);

  if (trace != NULL)
    {
      trace->command = command;
      trace->command_readback = readback;
      trace->polls = (uint16_t)attempt;
    }

  return readback == command ? OK : -EIO;
}

/****************************************************************************
 * Name: k1_rtl8852bs_rf_compare_probe
 *
 * Description:
 *   Ask the firmware to read one radio register itself and compare it with
 *   the value this host measured, through a single entry COMPARE offload on
 *   the RF source.  A match means both readers agree.  A mismatch is the
 *   informative case: c2h_cmd_ofld_rsp_hdl() reports the offset, the expected
 *   value and the value the firmware read, and the shared wait below prints
 *   all three, so the firmware's own reading of the register is recovered
 *   even when this host cannot reach the serial interface.
 *
 *   The transaction writes no register, and a failed compare leaves nothing
 *   half applied, so unlike a write batch there is nothing here that must not
 *   be repeated.  It is still not repeated: one probe per register.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rf_compare_probe(unsigned int path,
                                         uint8_t source,
                                         uint8_t address,
                                         uint32_t expected,
                                         uint8_t sequence)
{
  uint8_t content[K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE];
  uint32_t fifo_address = 0;
  uint16_t available_pages = 0;
  uint32_t command;
  int ret;

  if (path >= K1_RTL8852BS_RF_PATHS)
    {
      return -EINVAL;
    }

  memset(content, 0, sizeof(content));
  command = (uint32_t)source <<
            K1_RTL8852BS_CMD_OFLD_SOURCE_SHIFT;
  command |= K1_RTL8852BS_CMD_OFLD_TYPE_COMPARE <<
             K1_RTL8852BS_CMD_OFLD_TYPE_SHIFT;
  command |= (uint32_t)path << K1_RTL8852BS_CMD_OFLD_PATH_SHIFT;
  command |= K1_RTL8852BS_CMD_OFLD_LAST_COMMAND;
  command |= (uint32_t)address << K1_RTL8852BS_CMD_OFLD_OFFSET_SHIFT;

  k1_rtl8852bs_write_le32(content, command);
  k1_rtl8852bs_write_le32(content + 8, expected & K1_RTL8852BS_RF_MASK);
  k1_rtl8852bs_write_le32(content + 12, K1_RTL8852BS_RF_MASK);

  k1_early_puts("K1 Wi-Fi GPL: scan RF probe path=");
  k1_early_puthex(path);
  k1_early_puts(" source=");
  k1_early_puthex(source);
  k1_early_puts(" address=");
  k1_early_puthex(address);
  k1_early_puts(" expect=");
  k1_early_puthex(expected & K1_RTL8852BS_RF_MASK);
  k1_early_puts(" sequence=");
  k1_early_puthex(sequence);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, sizeof(content),
    K1_RTL8852BS_CMD_OFLD_CATEGORY, K1_RTL8852BS_CMD_OFLD_H2C_CLASS,
    K1_RTL8852BS_CMD_OFLD_H2C_FUNCTION, sequence, false,
    &fifo_address, &available_pages);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF probe submit error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" FIFO=");
      k1_early_puthex(fifo_address);
      k1_early_puts(" pages=");
      k1_early_puthex(available_pages);
      k1_early_puts("\r\n");
      return ret;
    }

  return k1_rtl8852bs_runtime_cmd_ofld_wait("scan RF probe", sequence,
                                            0, 0);
}

/****************************************************************************
 * Name: k1_rtl8852bs_scan_rf_compare_probe_all
 *
 * Description:
 *   Ask the firmware to read the radio itself, four compares per path, after
 *   the scan.  Only after: the compare runs on the same firmware I/O offload
 *   pipeline the radio images used, so it stays clear of the scan itself.
 *
 *   The four together are a decision table rather than four checks.  Probe 0
 *   compares the channel register with what this host read from it.  Probe 1
 *   compares the static register with the value this path's radio image wrote
 *   into it, which cannot have changed since.  Probe 2 compares the same
 *   register with what this host read from it.  Probe 3 compares it with a
 *   value no 20 bit register can hold and is the control on the compare
 *   itself.
 *
 *     probe 3 agrees                  the compare proves nothing, ignore the
 *                                     other three
 *     probe 1 agrees, probe 2 differs  the radio holds the image and this
 *                                     host cannot read the serial interface
 *     probe 1 differs with actual 0    both readers see zero, so the radio
 *     and probe 2 agrees               really lost the images
 *
 *   A mismatch is reported by the shared wait with the offset, the expected
 *   value and the value the firmware read, so each of these outcomes is
 *   visible in the log.  A compare writes nothing, so a mismatch leaves
 *   nothing half applied; a transport failure, which is not a mismatch, stops
 *   the series instead of being retried.
 *
 ****************************************************************************/

static void k1_rtl8852bs_scan_rf_compare_probe_all(
  FAR const struct k1_rtl8852bs_scan_rf_readback_s *readback)
{
  unsigned int index;
  unsigned int path;
  uint32_t expected;
  uint8_t address;
  bool agreement_expected;
  int ret;

  for (path = 0; path < K1_RTL8852BS_RF_PATHS; path++)
    {
      for (index = 0; index < K1_RTL8852BS_RF_PROBES_PER_PATH; index++)
        {
          switch (index)
            {
              case 0:
                address = K1_RTL8852BS_RF_REG_CHANNEL;
                expected = readback->channel[path];
                agreement_expected = true;
                break;

              case 1:
                address = K1_RTL8852BS_RF_REG_STATIC;
                expected = path == K1_RTL8852BS_RF_PATH_A ?
                           K1_RTL8852BS_RF_STATIC_VALUE_A :
                           K1_RTL8852BS_RF_STATIC_VALUE_B;
                agreement_expected = true;
                break;

              case 2:
                address = K1_RTL8852BS_RF_REG_STATIC;
                expected = readback->static_reg[path];
                agreement_expected = true;
                break;

              default:
                address = K1_RTL8852BS_RF_REG_STATIC;
                expected = K1_RTL8852BS_RF_IMPOSSIBLE_VALUE;
                agreement_expected = false;
                break;
            }

          ret = k1_rtl8852bs_rf_compare_probe(
            path, K1_RTL8852BS_CMD_OFLD_SOURCE_RF, address, expected,
            (uint8_t)(K1_RTL8852BS_RF_PROBE_SEQUENCE_BASE +
                      path * K1_RTL8852BS_RF_PROBES_PER_PATH + index));

          k1_early_puts("K1 Wi-Fi GPL: scan RF probe result path=");
          k1_early_puthex(path);
          k1_early_puts(" probe=");
          k1_early_puthex(index);
          k1_early_puts(" address=");
          k1_early_puthex(address);
          k1_early_puts(" expect=");
          k1_early_puthex(expected & K1_RTL8852BS_RF_MASK);
          k1_early_puts(" agree=");
          k1_early_puthex(ret == OK ? 1 : 0);
          k1_early_puts(" agree-expected=");
          k1_early_puthex(agreement_expected ? 1 : 0);
          k1_early_puts(" error=");
          k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
          k1_early_puts("\r\n");

          /* -EIO is the compare disagreeing, which is the informative case.
           * Anything else is the offload pipeline itself failing, and the
           * remaining probes would only add noise to it.
           */

          if (ret < 0 && ret != -EIO)
            {
              return;
            }
        }
    }
}

/****************************************************************************
 * Name: k1_rtl8852bs_rf_hwsi_trigger_set
 *
 * Description:
 *   Hold or release the hardware serial interface trigger of both paths, the
 *   two fields the original comments "Protest SW-SI" in
 *   halbb_bb_reset_all_8852b() and holds while it touches the interface from
 *   software.  Holding it is what the vendor does around its own software
 *   serial interface work, so a host read taken with it held is the read the
 *   vendor would have taken.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rf_hwsi_trigger_set(bool hold)
{
  uint32_t value = hold ? K1_RTL8852BS_RF_HWSI_TRIGGER_OFF : 0u;
  int ret;

  ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_HWSI_CTRL_A,
                                     K1_RTL8852BS_RF_HWSI_TRIGGER_MASK,
                                     value);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_bb_update_field(K1_RTL8852BS_RF_HWSI_CTRL_B,
                                     K1_RTL8852BS_RF_HWSI_TRIGGER_MASK,
                                     value);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_RF_SI_SETTLE_USEC);
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_rf_write_compare_probe
 *
 * Description:
 *   Ask the firmware to write one radio register and then read the same
 *   register back and compare it, both in one batch so nothing runs between
 *   them.  This is the firmware side counterpart of the host write below and
 *   answers the question a plain CMD_OFLD write cannot: the firmware
 *   acknowledges a write offload whether or not the radio took it, but it
 *   cannot fake a compare that reads the register it just wrote.
 *
 *   The value written is the value this path's radio image already wrote to
 *   this same register, so a repeat programs nothing new; and because the
 *   batch is one transaction, a failure leaves nothing half applied that a
 *   later batch would have to avoid replaying.
 *
 ****************************************************************************/

static int k1_rtl8852bs_rf_write_compare_probe(unsigned int path,
                                               uint8_t address,
                                               uint32_t value,
                                               uint8_t sequence)
{
  uint8_t content[K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE * 2u];
  uint32_t fifo_address = 0;
  uint16_t available_pages = 0;
  uint32_t command;
  unsigned int entry;
  int ret;

  if (path >= K1_RTL8852BS_RF_PATHS)
    {
      return -EINVAL;
    }

  memset(content, 0, sizeof(content));
  for (entry = 0; entry < 2u; entry++)
    {
      command = K1_RTL8852BS_CMD_OFLD_SOURCE_RF <<
                K1_RTL8852BS_CMD_OFLD_SOURCE_SHIFT;
      command |= (entry == 0 ? K1_RTL8852BS_CMD_OFLD_TYPE_WRITE :
                  K1_RTL8852BS_CMD_OFLD_TYPE_COMPARE) <<
                 K1_RTL8852BS_CMD_OFLD_TYPE_SHIFT;
      command |= (uint32_t)path << K1_RTL8852BS_CMD_OFLD_PATH_SHIFT;
      command |= (uint32_t)entry << K1_RTL8852BS_CMD_OFLD_COMMAND_SHIFT;
      command |= (uint32_t)address << K1_RTL8852BS_CMD_OFLD_OFFSET_SHIFT;
      if (entry == 1u)
        {
          command |= K1_RTL8852BS_CMD_OFLD_LAST_COMMAND;
        }

      k1_rtl8852bs_write_le32(
        content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE, command);
      k1_rtl8852bs_write_le32(
        content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 8,
        value & K1_RTL8852BS_RF_MASK);
      k1_rtl8852bs_write_le32(
        content + entry * K1_RTL8852BS_CMD_OFLD_ENTRY_SIZE + 12,
        K1_RTL8852BS_RF_MASK);
    }

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, sizeof(content),
    K1_RTL8852BS_CMD_OFLD_CATEGORY, K1_RTL8852BS_CMD_OFLD_H2C_CLASS,
    K1_RTL8852BS_CMD_OFLD_H2C_FUNCTION, sequence, false,
    &fifo_address, &available_pages);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF access write submit error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" FIFO=");
      k1_early_puthex(fifo_address);
      k1_early_puts(" pages=");
      k1_early_puthex(available_pages);
      k1_early_puts("\r\n");
      return ret;
    }

  return k1_rtl8852bs_runtime_cmd_ofld_wait("scan RF access write",
                                            sequence, 0, 0);
}

/****************************************************************************
 * Name: k1_rtl8852bs_scan_rf_access_probe_all
 *
 * Description:
 *   Decide why every A-die radio register on this board reads back as zero.
 *   The compare probes above established that the firmware's own reader sees
 *   the same zero this host sees, with a negative control proving the compare
 *   discriminates, and that the D-die mirror and the baseband window are both
 *   alive.  Two explanations survive that: the radio never received the
 *   images, because a CMD_OFLD write offload is acknowledged whether or not
 *   the radio took it; or the A-die cannot be reached by anything.
 *
 *   Six measurements per path separate them.  Each writes at most the value
 *   this path's radio image already wrote to the same register, so the radio
 *   is never programmed with anything new.
 *
 *     ddie      the D-die mirror of the same register number, for reference
 *     write     a host write of the image value through the serial interface,
 *               with the command word read back out of 0x370
 *     read      a host read of the register straight after that write
 *     fw-cmp    the firmware reading the register after the host write
 *     held      a host read taken with the hardware trigger held, the state
 *               the vendor puts the interface in for its own software access
 *     fw-rw     the firmware writing and then reading the register itself,
 *               both in one batch
 *     fw-ddie   the firmware reading the D-die mirror, compared with what
 *               this host read from it
 *
 *   If the host write sticks and the read that follows it returns the value,
 *   the interface works and the images never landed.  If the host write is
 *   accepted by 0x370 but the read still returns zero while the firmware
 *   compare after it agrees, the write works and this host cannot read.  If
 *   neither reader ever sees the value, the A-die is unreachable and the
 *   missing part of the vendor init sequence has to come first.
 *
 ****************************************************************************/

static void k1_rtl8852bs_scan_rf_access_probe_all(void)
{
  struct k1_rtl8852bs_rf_write_trace_s write_trace;
  struct k1_rtl8852bs_rf_read_trace_s read_trace;
  unsigned int path;
  uint32_t image;
  uint32_t ddie = 0;
  uint32_t value;
  uint8_t sequence;
  int ret;

  for (path = 0; path < K1_RTL8852BS_RF_PATHS; path++)
    {
      image = path == K1_RTL8852BS_RF_PATH_A ?
              K1_RTL8852BS_RF_STATIC_VALUE_A :
              K1_RTL8852BS_RF_STATIC_VALUE_B;

      ret = k1_rtl8852bs_rf_ddie_read((uint8_t)path,
                                      K1_RTL8852BS_RF_REG_STATIC, &ddie);
      k1_early_puts("K1 Wi-Fi GPL: scan RF access ddie path=");
      k1_early_puthex(path);
      k1_early_puts(" r5a=");
      k1_early_puthex(ddie);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");

      memset(&write_trace, 0, sizeof(write_trace));
      ret = k1_rtl8852bs_rf_write((uint8_t)path,
                                  K1_RTL8852BS_RF_REG_STATIC,
                                  K1_RTL8852BS_RF_MASK, image,
                                  &write_trace);
      k1_early_puts("K1 Wi-Fi GPL: scan RF access write path=");
      k1_early_puthex(path);
      k1_early_puts(" value=");
      k1_early_puthex(image);
      k1_early_puts(" pre-status=");
      k1_early_puthex(write_trace.pre_status);
      k1_early_puts(" command=");
      k1_early_puthex(write_trace.command);
      k1_early_puts(" readback=");
      k1_early_puthex(write_trace.command_readback);
      k1_early_puts(" polls=");
      k1_early_puthex(write_trace.polls);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");

      memset(&read_trace, 0, sizeof(read_trace));
      value = 0;
      ret = k1_rtl8852bs_rf_read((uint8_t)path,
                                 K1_RTL8852BS_RF_REG_STATIC, &value,
                                 &read_trace);
      k1_early_puts("K1 Wi-Fi GPL: scan RF access read path=");
      k1_early_puthex(path);
      k1_early_puts(" r5a=");
      k1_early_puthex(value);
      k1_early_puts(" expect=");
      k1_early_puthex(image);
      k1_early_puts(" pre-status=");
      k1_early_puthex(read_trace.pre_status);
      k1_early_puts(" status=");
      k1_early_puthex(read_trace.status);
      k1_early_puts(" select-polls=");
      k1_early_puthex(read_trace.select_polls);
      k1_early_puts(" done-polls=");
      k1_early_puthex(read_trace.done_polls);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");

      sequence = (uint8_t)(K1_RTL8852BS_RF_ACCESS_SEQUENCE_BASE +
                           path * K1_RTL8852BS_RF_ACCESS_PROBES_PER_PATH);
      ret = k1_rtl8852bs_rf_compare_probe(
        path, K1_RTL8852BS_CMD_OFLD_SOURCE_RF,
        K1_RTL8852BS_RF_REG_STATIC, image, sequence);
      k1_early_puts("K1 Wi-Fi GPL: scan RF access fw-cmp path=");
      k1_early_puthex(path);
      k1_early_puts(" expect=");
      k1_early_puthex(image);
      k1_early_puts(" agree=");
      k1_early_puthex(ret == OK ? 1 : 0);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");
      if (ret < 0 && ret != -EIO)
        {
          return;
        }

      ret = k1_rtl8852bs_rf_hwsi_trigger_set(true);
      if (ret >= 0)
        {
          memset(&read_trace, 0, sizeof(read_trace));
          value = 0;
          ret = k1_rtl8852bs_rf_read((uint8_t)path,
                                     K1_RTL8852BS_RF_REG_STATIC, &value,
                                     &read_trace);
        }

      k1_early_puts("K1 Wi-Fi GPL: scan RF access held path=");
      k1_early_puthex(path);
      k1_early_puts(" r5a=");
      k1_early_puthex(value);
      k1_early_puts(" status=");
      k1_early_puthex(read_trace.status);
      k1_early_puts(" pre-status=");
      k1_early_puthex(read_trace.pre_status);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");

      ret = k1_rtl8852bs_rf_hwsi_trigger_set(false);
      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: scan RF access release error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts("\r\n");
          return;
        }

      ret = k1_rtl8852bs_rf_write_compare_probe(
        path, K1_RTL8852BS_RF_REG_STATIC, image, (uint8_t)(sequence + 1u));
      k1_early_puts("K1 Wi-Fi GPL: scan RF access fw-rw path=");
      k1_early_puthex(path);
      k1_early_puts(" value=");
      k1_early_puthex(image);
      k1_early_puts(" agree=");
      k1_early_puthex(ret == OK ? 1 : 0);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");
      if (ret < 0 && ret != -EIO)
        {
          return;
        }

      ret = k1_rtl8852bs_rf_compare_probe(
        path, K1_RTL8852BS_CMD_OFLD_SOURCE_RF_DDIE,
        K1_RTL8852BS_RF_REG_STATIC, ddie, (uint8_t)(sequence + 2u));
      k1_early_puts("K1 Wi-Fi GPL: scan RF access fw-ddie path=");
      k1_early_puthex(path);
      k1_early_puts(" expect=");
      k1_early_puthex(ddie);
      k1_early_puts(" agree=");
      k1_early_puthex(ret == OK ? 1 : 0);
      k1_early_puts(" error=");
      k1_early_puthex(ret < 0 ? (uintreg_t)-ret : 0);
      k1_early_puts("\r\n");
      if (ret < 0 && ret != -EIO)
        {
          return;
        }
    }
}

#endif

#endif

struct k1_rtl8852bs_scanofld_bss_s
{
  uint8_t bssid[6];
  uint8_t ssid[32];
  uint8_t ssid_length;
  uint8_t ds_channel;
  uint8_t dwell_channel;
  uint16_t dwell_mask;
  uint16_t capability;
  uint16_t beacon_interval;
  uint16_t beacon_frames;
  uint16_t probe_response_frames;
  uint16_t management_frames;
  uint16_t data_frames;
  bool ssid_present;
};

struct k1_rtl8852bs_scanofld_passive_match_s
{
  struct k1_rtl8852bs_done_ack_match_s done_ack;
  struct k1_rtl8852bs_scanofld_bss_s bss[K1_RTL8852BS_SCAN_OFLD_BSS_MAX];
  clock_t dwell_deadline;
  uint32_t rx_reads;
  uint32_t rx_oversize;
  uint32_t rx_oversize_max;
  uint32_t rx_frames_logged;
  uint32_t rx_frames_suppressed;
  uint32_t rx_parse_errors;
  uint32_t data_frames_seen;
  uint32_t control_frames;
  uint32_t rx_frames_total;
  uint32_t rx_crc_errors;
  uint32_t rx_icv_errors;
  uint16_t rx_types[16];
  uint16_t dwell_frames[K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT];
  uint16_t dwell_management[K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT];
  uint16_t dwell_beacons[K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT];
  uint16_t mgmt_subtypes[16];
  uint32_t rx_data_frames;
  uint32_t management_frames;
  uint32_t beacon_frames;
  uint32_t probe_response_frames;

  /* Probe Responses split by whether A1 is this host's own address.  Only the
   * first of the two may be treated as evidence that the offloaded Probe
   * Request was radiated: during a sweep this receiver runs with sniffer mode
   * set and unicast CAM matching cleared, so a Probe Response an access point
   * sent to a different station arrives here too.
   */

  uint32_t probe_response_to_self;
  uint32_t probe_response_to_others;

  /* Frames whose transmitter address is this host's own, which is what a MAC
   * loopback delivers, and how many of those were Probe Requests.
   */

  uint32_t self_transmitted_frames;
  uint32_t self_probe_requests;

  /* Unicast frames addressed to some other station.  A non-zero count is the
   * positive control for the receive side: it proves the sweep would have
   * delivered a Probe Response addressed to this host had one arrived.
   */

  uint32_t unicast_to_others;
  uint32_t scan_report_events;
  uint32_t scan_report_rx_count;
  uint32_t scan_report_channels;
  uint32_t pre_tx_events;
  uint32_t post_tx_events;
  uint32_t post_tx_failures;
  uint32_t leave_channel_events;
  uint32_t active_channel_notifies;
  uint32_t transmit_fail_reported;
  uint16_t scan_events;
  uint8_t last_channel;
  uint8_t last_reason;
  uint8_t last_status;
  uint8_t last_channel_band;
  uint8_t first_bssid[6];
  uint8_t probe_response_a1[6];
  uint8_t probe_response_a2[6];
  uint8_t self_tx_a1[6];
  uint16_t self_tx_frame_control;
  uint8_t first_ssid[32];
  uint8_t first_ssid_length;
  uint8_t first_channel;
  uint16_t entered_channels;
  uint16_t advanced_channels;
  uint8_t bss_count;
  uint8_t bss_dropped;
  uint8_t dwell_channel;
  bool dwell_pending;
  bool first_bss_valid;
  bool first_ssid_present;
  bool probe_response_seen;
  bool self_tx_seen;
  bool saw_scan_end;
};

static int k1_rtl8852bs_scanofld_passive_channel_index(uint8_t channel)
{
  if (channel < 1u ||
      channel > K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT)
    {
      return -ENOENT;
    }

  return (int)channel - 1;
}

static uint8_t k1_rtl8852bs_scanofld_passive_channel(unsigned int index)
{
  if (index >= K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT)
    {
      return 0u;
    }

  return (uint8_t)(index + 1u);
}

/****************************************************************************
 * Name: k1_rtl8852bs_scanofld_bss_lookup
 *
 * Description:
 *   Find, or append, the bounded scan BSS table entry for one BSSID.  The
 *   dwell channel the frame arrived in is folded into the entry so that a
 *   BSS advertising a DS-Parameter channel other than the dwell channel
 *   stays visible as such.  Returns NULL when the table is full.
 ****************************************************************************/

static FAR struct k1_rtl8852bs_scanofld_bss_s *
k1_rtl8852bs_scanofld_bss_lookup(
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match,
  FAR const uint8_t *bssid)
{
  FAR struct k1_rtl8852bs_scanofld_bss_s *entry;
  unsigned int index;
  int dwell_index;
  bool all_zero = true;
  bool all_ff = true;

  for (index = 0; index < 6u; index++)
    {
      all_zero = all_zero && bssid[index] == 0;
      all_ff = all_ff && bssid[index] == 0xff;
    }

  if (all_zero || all_ff || (bssid[0] & 1u) != 0)
    {
      return NULL;
    }

  for (index = 0; index < match->bss_count; index++)
    {
      if (memcmp(match->bss[index].bssid, bssid, 6u) == 0)
        {
          break;
        }
    }

  if (index == match->bss_count)
    {
      if (match->bss_count >= K1_RTL8852BS_SCAN_OFLD_BSS_MAX)
        {
          match->bss_dropped++;
          return NULL;
        }

      entry = &match->bss[index];
      memset(entry, 0, sizeof(*entry));
      memcpy(entry->bssid, bssid, sizeof(entry->bssid));
      entry->dwell_channel = match->dwell_channel;
      match->bss_count++;
    }
  else
    {
      entry = &match->bss[index];
    }

  dwell_index = k1_rtl8852bs_scanofld_passive_channel_index(
    match->dwell_channel);
  if (dwell_index >= 0)
    {
      entry->dwell_mask |= 1u << dwell_index;
    }

  return entry;
}

/****************************************************************************
 * Name: k1_rtl8852bs_scanofld_bss_record
 *
 * Description:
 *   Fold one decoded Beacon or Probe Response into the scan BSS table.
 ****************************************************************************/

static void k1_rtl8852bs_scanofld_bss_record(
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match,
  FAR const struct k1_rtl8852bs_mgmt_frame_s *mgmt)
{
  FAR struct k1_rtl8852bs_scanofld_bss_s *entry;

  if (!mgmt->bssid_valid)
    {
      return;
    }

  entry = k1_rtl8852bs_scanofld_bss_lookup(match, mgmt->bssid);
  if (entry == NULL)
    {
      return;
    }

  if (mgmt->is_beacon)
    {
      entry->beacon_frames++;
    }

  if (mgmt->is_probe_response)
    {
      entry->probe_response_frames++;
    }

  entry->beacon_interval = mgmt->beacon_interval;
  entry->capability = mgmt->capability;
  if (mgmt->channel != 0)
    {
      entry->ds_channel = mgmt->channel;
    }

  if (mgmt->ssid_present && !entry->ssid_present)
    {
      memcpy(entry->ssid, mgmt->ssid, sizeof(entry->ssid));
      entry->ssid_length = mgmt->ssid_length;
      entry->ssid_present = true;
    }
}

/****************************************************************************
 * Name: k1_rtl8852bs_scanofld_traffic_record
 *
 * Description:
 *   Attribute a received non-Beacon frame to a BSS.  A management frame
 *   carries the BSSID in address 3; a data frame carries it in address 1
 *   when it travels to the distribution system and in address 2 when it
 *   comes from it.  This separates an infrastructure BSS that is only heard
 *   through its clients from one that is not heard at all.
 ****************************************************************************/

static void k1_rtl8852bs_scanofld_traffic_record(
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match,
  FAR const uint8_t *payload, size_t payload_length, uint16_t frame_control)
{
  FAR struct k1_rtl8852bs_scanofld_bss_s *entry;
  FAR const uint8_t *bssid;
  uint8_t frame_type;

  frame_type = (frame_control >> 2) & K1_RTL8852BS_IEEE80211_TYPE_MASK;
  if (payload_length < K1_RTL8852BS_IEEE80211_HEADER_SIZE)
    {
      return;
    }

  if (frame_type == K1_RTL8852BS_IEEE80211_TYPE_MANAGEMENT)
    {
      bssid = payload + 16;
    }
  else if (frame_type == K1_RTL8852BS_IEEE80211_TYPE_DATA)
    {
      /* Bit 8 is to-DS and bit 9 is from-DS in the frame-control field. */

      if ((frame_control & 0x0100u) != 0 && (frame_control & 0x0200u) == 0)
        {
          bssid = payload + 4;
        }
      else if ((frame_control & 0x0100u) == 0 &&
               (frame_control & 0x0200u) != 0)
        {
          bssid = payload + 10;
        }
      else
        {
          return;
        }
    }
  else
    {
      return;
    }

  entry = k1_rtl8852bs_scanofld_bss_lookup(match, bssid);
  if (entry == NULL)
    {
      return;
    }

  if (frame_type == K1_RTL8852BS_IEEE80211_TYPE_MANAGEMENT)
    {
      entry->management_frames++;
    }
  else
    {
      entry->data_frames++;
    }
}

static void k1_rtl8852bs_scanofld_observe_wifi(
  FAR const uint8_t *payload, size_t payload_length,
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match)
{
  struct k1_rtl8852bs_mgmt_frame_s mgmt;
  uint8_t frame_type;
  uint8_t frame_subtype;
  int dwell_index;
  int ret;

  match->rx_data_frames++;
  dwell_index = k1_rtl8852bs_scanofld_passive_channel_index(
    match->dwell_channel);
  if (dwell_index >= 0)
    {
      match->dwell_frames[dwell_index]++;
    }

  ret = k1_rtl8852bs_runtime_mgmt_parse(payload, payload_length, &mgmt);
  if (ret < 0)
    {
      /* Only a genuine decode failure is printed.  Every other 802.11 frame
       * type is expected traffic and is counted instead, because the polled
       * console would otherwise spend the dwell printing.
       */

      match->rx_parse_errors++;
      if (match->rx_parse_errors <=
          K1_RTL8852BS_SCAN_OFLD_PARSE_ERROR_LOG_MAX)
        {
          k1_early_puts("K1 Wi-Fi GPL: passive scan frame error ret=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts(" fc=");
          k1_early_puthex(mgmt.frame_control);
          k1_early_puts(" len=");
          k1_early_puthex(payload_length);
          k1_early_puts("\r\n");
        }

      return;
    }

  frame_type = (mgmt.frame_control >> 2) & K1_RTL8852BS_IEEE80211_TYPE_MASK;
  frame_subtype = (mgmt.frame_control >>
                   K1_RTL8852BS_IEEE80211_SUBTYPE_SHIFT) &
                  K1_RTL8852BS_IEEE80211_SUBTYPE_MASK;

  /* A frame whose transmitter address is this host's own is a frame this part
   * emitted and the receiver got back, which is exactly what a MAC loopback
   * produces.  It is counted for every 802.11 type, before the management
   * branch, because it is the only observation in this component that can
   * separate a transmit that reached the MAC from one that never happened.
   */

  if (mgmt.addr2_valid && k1_rtl8852bs_scanofld_is_self_mac(mgmt.addr2))
    {
      match->self_transmitted_frames++;
      if (mgmt.is_management &&
          frame_subtype == K1_RTL8852BS_IEEE80211_SUBTYPE_PROBE_REQUEST)
        {
          match->self_probe_requests++;
        }

      if (!match->self_tx_seen)
        {
          match->self_tx_seen = true;
          match->self_tx_frame_control = mgmt.frame_control;
          if (mgmt.addr1_valid)
            {
              memcpy(match->self_tx_a1, mgmt.addr1,
                     sizeof(match->self_tx_a1));
            }
        }
    }

  /* A unicast frame addressed to some other station is the receive side's own
   * positive control: it proves this sweep passes up frames that do not match
   * this host's address, so a Probe Response that never appeared cannot be
   * blamed on address filtering.
   */

  if (mgmt.addr1_valid && (mgmt.addr1[0] & 1u) == 0 &&
      !k1_rtl8852bs_scanofld_is_self_mac(mgmt.addr1))
    {
      match->unicast_to_others++;
    }

  if (!mgmt.is_management)
    {
      if (frame_type == K1_RTL8852BS_IEEE80211_TYPE_DATA)
        {
          match->data_frames_seen++;
        }
      else
        {
          match->control_frames++;
        }

      k1_rtl8852bs_scanofld_traffic_record(match, payload, payload_length,
                                           mgmt.frame_control);
      return;
    }

  match->management_frames++;
  match->mgmt_subtypes[frame_subtype]++;
  if (dwell_index >= 0)
    {
      match->dwell_management[dwell_index]++;
    }

  /* An Authentication frame is only evidence of an answer to this host when
   * its A1 is this host's own address, on exactly the same reasoning the
   * Probe Response accounting below uses: the sweep runs with unicast address
   * matching off, so an Authentication frame an access point sent to a
   * different station is delivered here as well.  The three body fields are
   * read from the payload rather than from the parsed header subset, which
   * carries no Authentication body, and only the first answer is kept: what
   * matters is the algorithm the access point agreed to, the transaction it
   * believes it is in, and its status code.
   */

  if (frame_subtype == K1_RTL8852BS_IEEE80211_SUBTYPE_AUTHENTICATION &&
      g_k1_rtl8852bs_auth_action.armed)
    {
      g_k1_rtl8852bs_auth_action.frames_seen++;
      if (mgmt.addr2_valid &&
          memcmp(mgmt.addr2, g_k1_rtl8852bs_auth_action.bssid,
                 sizeof(g_k1_rtl8852bs_auth_action.bssid)) == 0)
        {
          g_k1_rtl8852bs_auth_action.responses_from_target++;
        }

      if (mgmt.addr1_valid && k1_rtl8852bs_scanofld_is_self_mac(mgmt.addr1))
        {
          g_k1_rtl8852bs_auth_action.responses_to_self++;
          if (!g_k1_rtl8852bs_auth_action.response_valid &&
              payload_length >= K1_RTL8852BS_AUTH_FRAME_SIZE)
            {
              FAR const uint8_t *body =
                payload + K1_RTL8852BS_IEEE80211_HEADER_SIZE;

              g_k1_rtl8852bs_auth_action.response_valid = true;
              g_k1_rtl8852bs_auth_action.response_algorithm =
                k1_rtl8852bs_read_le16(body);
              g_k1_rtl8852bs_auth_action.response_sequence =
                k1_rtl8852bs_read_le16(body + 2);
              g_k1_rtl8852bs_auth_action.response_status =
                k1_rtl8852bs_read_le16(body + 4);
              if (mgmt.addr2_valid)
                {
                  memcpy(g_k1_rtl8852bs_auth_action.response_a2, mgmt.addr2,
                         sizeof(g_k1_rtl8852bs_auth_action.response_a2));
                }
            }
        }
    }

  if (mgmt.is_beacon)
    {
      match->beacon_frames++;
      if (dwell_index >= 0)
        {
          match->dwell_beacons[dwell_index]++;
        }
    }

  if (mgmt.is_probe_response)
    {
      match->probe_response_frames++;
      if (mgmt.addr1_valid && k1_rtl8852bs_scanofld_is_self_mac(mgmt.addr1))
        {
          match->probe_response_to_self++;
        }
      else
        {
          match->probe_response_to_others++;
        }

      if (!match->probe_response_seen)
        {
          match->probe_response_seen = true;
          memcpy(match->probe_response_a1, mgmt.addr1,
                 sizeof(match->probe_response_a1));
          memcpy(match->probe_response_a2, mgmt.addr2,
                 sizeof(match->probe_response_a2));
        }
    }

  if (!match->first_bss_valid && mgmt.bssid_valid)
    {
      memcpy(match->first_bssid, mgmt.bssid, sizeof(match->first_bssid));
      match->first_bss_valid = true;
      match->first_channel = mgmt.channel;
      if (mgmt.ssid_present)
        {
          memcpy(match->first_ssid, mgmt.ssid, sizeof(match->first_ssid));
          match->first_ssid_length = mgmt.ssid_length;
          match->first_ssid_present = true;
        }
    }

  if (mgmt.is_beacon || mgmt.is_probe_response)
    {
      k1_rtl8852bs_scanofld_bss_record(match, &mgmt);
    }
  else
    {
      k1_rtl8852bs_scanofld_traffic_record(match, payload, payload_length,
                                          mgmt.frame_control);
    }
}

static int k1_rtl8852bs_scanofld_observe_report(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, uint32_t dword3,
  uint8_t reason, FAR struct k1_rtl8852bs_scanofld_passive_match_s *match)
{
  uint8_t channel_count;
  uint8_t report_size_dwords;
  size_t report_offset;
  size_t report_bytes;
  unsigned int index;

  if (reason != K1_RTL8852BS_SCAN_OFLD_C2H_GET_REPORT &&
      reason != K1_RTL8852BS_SCAN_OFLD_C2H_SCAN_END)
    {
      return OK;
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan report dword3=");
  k1_early_puthex(dword3);
  k1_early_puts(" bytes=");
  k1_early_puthex(c2h->content_length);
  k1_early_puts("\r\n");
  match->scan_report_events++;
  channel_count = (dword3 >> 8) & 0xffu;
  report_size_dwords = (dword3 >> 16) & 0xffu;
  if (channel_count == 0)
    {
      return OK;
    }

  if (report_size_dwords == 0)
    {
      return -EPROTO;
    }

  report_bytes = (size_t)channel_count * report_size_dwords *
                 K1_RTL8852BS_SCAN_OFLD_REPORT_SIZE;
  if (c2h->content_length < K1_RTL8852BS_SCAN_OFLD_C2H_HEADER_SIZE ||
      report_bytes > c2h->content_length -
                     K1_RTL8852BS_SCAN_OFLD_C2H_HEADER_SIZE)
    {
      return -EPROTO;
    }

  report_offset = K1_RTL8852BS_SCAN_OFLD_C2H_HEADER_SIZE;
  for (index = 0; index < channel_count; index++)
    {
      FAR const uint8_t *report = c2h->content + report_offset;

      match->scan_report_channels++;
      match->scan_report_rx_count += report[1];
      k1_early_puts("K1 Wi-Fi GPL: passive scan report channel=");
      k1_early_puthex(report[0]);
      k1_early_puts(" rx=");
      k1_early_puthex(report[1]);
      k1_early_puts(" txfail=");
      k1_early_puthex(report[2] & 0x7fu);
      k1_early_puts(" parsed=");
      k1_early_puthex((report[2] >> 7) & 1u);
      k1_early_puts("\r\n");
      report_offset += (size_t)report_size_dwords *
                       K1_RTL8852BS_SCAN_OFLD_REPORT_SIZE;
    }

  return OK;
}

static void k1_rtl8852bs_scanofld_log_rx(
  FAR const struct k1_rtl8852bs_scanofld_passive_match_s *match)
{
  unsigned int index;

  k1_early_puts("K1 Wi-Fi GPL: passive scan RX data=");
  k1_early_puthex(match->rx_data_frames);
  k1_early_puts(" mgmt=");
  k1_early_puthex(match->management_frames);
  k1_early_puts(" beacon=");
  k1_early_puthex(match->beacon_frames);
  k1_early_puts(" probe-rsp=");
  k1_early_puthex(match->probe_response_frames);
  k1_early_puts(" scan-report=");
  k1_early_puthex(match->scan_report_events);
  k1_early_puts(" report-rx=");
  k1_early_puthex(match->scan_report_rx_count);
  k1_early_puts(" report-ch=");
  k1_early_puthex(match->scan_report_channels);
  k1_early_puts(" bss=");
  k1_early_puthex(match->first_bss_valid ? 1 : 0);
  k1_early_puts(" channel=");
  k1_early_puthex(match->first_channel);
  k1_early_puts(" ssid-len=");
  k1_early_puthex(match->first_ssid_present ? match->first_ssid_length : 0);
  k1_early_puts(" bssid=");
  if (match->first_bss_valid)
    {
      for (index = 0; index < sizeof(match->first_bssid); index++)
        {
          k1_early_puthex(match->first_bssid[index]);
        }
    }
  else
    {
      k1_early_puthex(0);
    }

  k1_early_puts("\r\n");

  /* The firmware's own account of the transmission side of the sweep.  A
   * passive table asks for none of these notifications, so every counter
   * stays zero there; on an active table pre-tx counts the channels the
   * firmware reached the transmit step on, post-tx-fail counts the ones it
   * reported as failed, active-ch counts the notifications where the firmware
   * itself marked the channel as one that transmits, and fw-txfail is the
   * transmit failure count it reports in its notifications.
   */

  k1_early_puts("K1 Wi-Fi GPL: passive scan TX notify pre-tx=");
  k1_early_puthex(match->pre_tx_events);
  k1_early_puts(" post-tx=");
  k1_early_puthex(match->post_tx_events);
  k1_early_puts(" post-tx-fail=");
  k1_early_puthex(match->post_tx_failures);
  k1_early_puts(" leave-ch=");
  k1_early_puthex(match->leave_channel_events);
  k1_early_puts(" active-ch=");
  k1_early_puthex(match->active_channel_notifies);
  k1_early_puts(" fw-txfail=");
  k1_early_puthex(match->transmit_fail_reported);
  k1_early_puts("\r\n");

  /* The same receive, judged on the 802.11 address fields instead of on the
   * frame subtype alone.  rsp-self counts Probe Responses whose A1 is this
   * host's own address and is the only number here that may be read as
   * evidence that the offloaded Probe Request was radiated; rsp-other counts
   * the ones an access point addressed to a different station, which this
   * sweep also receives and which must never be mistaken for a reply to us.
   * uc-other is the receive side's positive control: unicast frames addressed
   * to some other station, proving A1 filtering is not what is losing a reply.
   * self-tx counts frames carrying this host as transmitter, which is what a
   * MAC loopback delivers, and self-preq counts the Probe Request among them.
   */

  k1_early_puts("K1 Wi-Fi GPL: passive scan RX addr rsp-self=");
  k1_early_puthex(match->probe_response_to_self);
  k1_early_puts(" rsp-other=");
  k1_early_puthex(match->probe_response_to_others);
  k1_early_puts(" uc-other=");
  k1_early_puthex(match->unicast_to_others);
  k1_early_puts(" self-tx=");
  k1_early_puthex(match->self_transmitted_frames);
  k1_early_puts(" self-preq=");
  k1_early_puthex(match->self_probe_requests);
  k1_early_puts(" self-tx-fc=");
  k1_early_puthex(match->self_tx_frame_control);
  k1_early_puts(" self-tx-a1=");
  for (index = 0; index < sizeof(match->self_tx_a1); index++)
    {
      k1_early_puthex(match->self_tx_a1[index]);
    }

  k1_early_puts(" rsp-a1=");
  for (index = 0; index < sizeof(match->probe_response_a1); index++)
    {
      k1_early_puthex(match->probe_response_a1[index]);
    }

  k1_early_puts(" rsp-a2=");
  for (index = 0; index < sizeof(match->probe_response_a2); index++)
    {
      k1_early_puthex(match->probe_response_a2[index]);
    }

  k1_early_puts("\r\n");
}

static void k1_rtl8852bs_scanofld_log_bytes(FAR const uint8_t *data,
                                            size_t length)
{
  static const char digits[] = "0123456789abcdef";
  char text[3];
  size_t index;

  text[2] = '\0';
  for (index = 0; index < length; index++)
    {
      text[0] = digits[(data[index] >> 4) & 0xfu];
      text[1] = digits[data[index] & 0xfu];
      k1_early_puts(text);
    }
}

static void k1_rtl8852bs_scanofld_log_bss_table(
  FAR const struct k1_rtl8852bs_scanofld_passive_match_s *match)
{
  FAR const struct k1_rtl8852bs_scanofld_bss_s *bss;
  unsigned int index;

  k1_early_puts("K1 Wi-Fi GPL: passive scan BSS entries=");
  k1_early_puthex(match->bss_count);
  k1_early_puts(" dropped=");
  k1_early_puthex(match->bss_dropped);
  k1_early_puts(" reads=");
  k1_early_puthex(match->rx_reads);
  k1_early_puts(" oversize=");
  k1_early_puthex(match->rx_oversize);
  k1_early_puts(" oversize-max=");
  k1_early_puthex(match->rx_oversize_max);
  k1_early_puts(" logged=");
  k1_early_puthex(match->rx_frames_logged);
  k1_early_puts(" suppressed=");
  k1_early_puthex(match->rx_frames_suppressed);
  k1_early_puts(" parse-err=");
  k1_early_puthex(match->rx_parse_errors);
  k1_early_puts(" data=");
  k1_early_puthex(match->data_frames_seen);
  k1_early_puts(" ctrl=");
  k1_early_puthex(match->control_frames);
  k1_early_puts(" rx-total=");
  k1_early_puthex(match->rx_frames_total);
  k1_early_puts(" crc-err=");
  k1_early_puthex(match->rx_crc_errors);
  k1_early_puts(" icv-err=");
  k1_early_puthex(match->rx_icv_errors);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: passive scan rpkt types=");
  for (index = 0; index < 16u; index++)
    {
      if (index != 0)
        {
          k1_early_puts(",");
        }

      k1_early_puthex(match->rx_types[index]);
    }

  k1_early_puts("\r\n");

  for (index = 0; index < K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT;
       index++)
    {
      k1_early_puts("K1 Wi-Fi GPL: passive scan dwell[");
      k1_early_puthex(index);
      k1_early_puts("] ch=");
      k1_early_puthex(k1_rtl8852bs_scanofld_passive_channel(index));
      k1_early_puts(" frames=");
      k1_early_puthex(match->dwell_frames[index]);
      k1_early_puts(" mgmt=");
      k1_early_puthex(match->dwell_management[index]);
      k1_early_puts(" beacon=");
      k1_early_puthex(match->dwell_beacons[index]);
      k1_early_puts("\r\n");
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan mgmt subtypes=");
  for (index = 0; index < 16u; index++)
    {
      if (index != 0)
        {
          k1_early_puts(",");
        }

      k1_early_puthex(match->mgmt_subtypes[index]);
    }

  k1_early_puts("\r\n");

  for (index = 0; index < match->bss_count; index++)
    {
      bss = &match->bss[index];

      k1_early_puts("K1 Wi-Fi GPL: passive scan BSS[");
      k1_early_puthex(index);
      k1_early_puts("] bssid=");
      k1_rtl8852bs_scanofld_log_bytes(bss->bssid, sizeof(bss->bssid));
      k1_early_puts(" ds-ch=");
      k1_early_puthex(bss->ds_channel);
      k1_early_puts(" dwell-ch=");
      k1_early_puthex(bss->dwell_channel);
      k1_early_puts(" dwell-mask=");
      k1_early_puthex(bss->dwell_mask);
      k1_early_puts(" beacons=");
      k1_early_puthex(bss->beacon_frames);
      k1_early_puts(" probe-rsp=");
      k1_early_puthex(bss->probe_response_frames);
      k1_early_puts(" mgmt-other=");
      k1_early_puthex(bss->management_frames);
      k1_early_puts(" data=");
      k1_early_puthex(bss->data_frames);
      k1_early_puts(" cap=");
      k1_early_puthex(bss->capability);
      k1_early_puts(" interval=");
      k1_early_puthex(bss->beacon_interval);
      k1_early_puts(" ssid-len=");
      k1_early_puthex(bss->ssid_present ? bss->ssid_length : 0);
      k1_early_puts(" ssid=");
      if (bss->ssid_present && bss->ssid_length > 0)
        {
          k1_rtl8852bs_scanofld_log_bytes(bss->ssid, bss->ssid_length);
        }
      else
        {
          k1_early_puthex(0);
        }

      k1_early_puts("\r\n");
    }
}

static void k1_rtl8852bs_scanofld_log_frame(
  FAR const uint8_t *buffer, size_t length, size_t offset,
  FAR const struct k1_rtl8852bs_rx_frame_s *frame)
{
  size_t descriptor_length;
  size_t dump_offset;
  size_t dump_length;

  k1_early_puts("K1 Wi-Fi GPL: passive scan RXD0=");
  k1_early_puthex(frame->descriptor0);
  k1_early_puts(" RXD3=");
  k1_early_puthex(frame->descriptor3);
  k1_early_puts(" type=");
  k1_early_puthex(frame->packet_type);
  k1_early_puts(" len=");
  k1_early_puthex(frame->payload_length);
  k1_early_puts(" crc=");
  k1_early_puthex(frame->crc_error ? 1 : 0);
  k1_early_puts(" icv=");
  k1_early_puthex(frame->icv_error ? 1 : 0);
  k1_early_puts("\r\n");

  if (frame->packet_type != K1_RTL8852BS_RXDESC_PACKET_TYPE_WIFI)
    {
      return;
    }

  /* The MAC reports the 802.11 header plus IV length in RXD0[21:16] and its
   * own frame-type decode in RXD4[1:0], so the descriptor metadata alone
   * tells a management frame from a data frame.  Dump the bytes from the end
   * of the descriptor rather than from the payload so that a wrong payload
   * offset stays visible: the shift and driver-info padding is printed
   * before the frame control.
   */

  descriptor_length = (frame->descriptor0 & K1_RTL8852BS_RXDESC_LONG) != 0 ?
                      K1_RTL8852BS_RXDESC_LONG_SIZE :
                      K1_RTL8852BS_RXDESC_SHORT_SIZE;
  k1_early_puts("K1 Wi-Fi GPL: passive scan RXD meta hdr-iv=");
  k1_early_puthex((frame->descriptor0 >> 16) & 0x3fu);
  k1_early_puts(" bb-sel=");
  k1_early_puthex((frame->descriptor0 >> 22) & 1u);
  k1_early_puts(" mac-info=");
  k1_early_puthex((frame->descriptor0 >> 23) & 1u);
  k1_early_puts(" shift=");
  k1_early_puthex((frame->descriptor0 >> K1_RTL8852BS_RXDESC_SHIFT) &
                  K1_RTL8852BS_RXDESC_SHIFT_MASK);
  k1_early_puts(" drv=");
  k1_early_puthex((frame->descriptor0 >>
                   K1_RTL8852BS_RXDESC_DRIVER_INFO_SHIFT) &
                  K1_RTL8852BS_RXDESC_DRIVER_INFO_MASK);
  k1_early_puts(" rxd-len=");
  k1_early_puthex(descriptor_length);
  k1_early_puts(" payload-offset=");
  k1_early_puthex(frame->payload_offset - offset);
  k1_early_puts("\r\n");

  if (descriptor_length == K1_RTL8852BS_RXDESC_LONG_SIZE)
    {
      k1_early_puts("K1 Wi-Fi GPL: passive scan RXD1=");
      k1_early_puthex(k1_rtl8852bs_read_le32(buffer + offset + 4));
      k1_early_puts(" RXD4=");
      k1_early_puthex(k1_rtl8852bs_read_le32(buffer + offset + 16));
      k1_early_puts(" RXD5=");
      k1_early_puthex(k1_rtl8852bs_read_le32(buffer + offset + 20));
      k1_early_puts(" RXD6=");
      k1_early_puthex(k1_rtl8852bs_read_le32(buffer + offset + 24));
      k1_early_puts(" RXD7=");
      k1_early_puthex(k1_rtl8852bs_read_le32(buffer + offset + 28));
      k1_early_puts("\r\n");
    }

  dump_offset = offset + descriptor_length;
  dump_length = K1_RTL8852BS_SCAN_OFLD_FRAME_DUMP_BYTES;
  if (dump_offset >= length)
    {
      return;
    }

  if (dump_length > length - dump_offset)
    {
      dump_length = length - dump_offset;
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan frame=");
  k1_rtl8852bs_scanofld_log_bytes(buffer + dump_offset, dump_length);
  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_passive_match
 *
 * Description:
 *   Observe generic done acknowledgements and scan-offload notifications in
 *   the same RX pass.  The firmware is allowed to send either notification
 *   before the H2C done acknowledgement, so a separate done-ack wait would
 *   lose a valid early scan event.
 ****************************************************************************/

static int k1_rtl8852bs_runtime_scanofld_passive_match(
  FAR const struct k1_rtl8852bs_c2h_s *c2h, FAR void *arg)
{
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match = arg;
  uint32_t dword0;
  uint32_t dword3;
  uint8_t channel;
  uint8_t reason;
  uint8_t status;
  uint8_t channel_band;
  uint16_t channel_mask;
  int channel_index;
  int ret;

  if (c2h == NULL || match == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_runtime_done_ack_match(c2h, &match->done_ack);
  if (ret < 0)
    {
      return ret;
    }

  if (c2h->category != K1_RTL8852BS_SCAN_OFLD_C2H_CATEGORY ||
      c2h->class_id != K1_RTL8852BS_SCAN_OFLD_C2H_CLASS ||
      c2h->function != K1_RTL8852BS_SCAN_OFLD_C2H_FUNCTION)
    {
      return OK;
    }

  if (c2h->content_length < K1_RTL8852BS_SCAN_OFLD_C2H_CONTENT_SIZE)
    {
      return -EPROTO;
    }

  dword0 = k1_rtl8852bs_read_le32(c2h->content);
  dword3 = k1_rtl8852bs_read_le32(c2h->content + 3u * sizeof(uint32_t));
  channel = dword0 & K1_RTL8852BS_SCAN_OFLD_C2H_PRIMARY_MASK;
  reason = (dword0 >> K1_RTL8852BS_SCAN_OFLD_C2H_REASON_SHIFT) &
           K1_RTL8852BS_SCAN_OFLD_C2H_REASON_MASK;
  status = (dword0 >> K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_SHIFT) &
           K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_MASK;
  channel_band = (dword3 >> K1_RTL8852BS_SCAN_OFLD_C2H_CHANNEL_BAND_SHIFT) &
                 K1_RTL8852BS_SCAN_OFLD_C2H_CHANNEL_BAND_MASK;

  match->scan_events++;
  match->last_channel = channel;
  match->last_reason = reason;
  match->last_status = status;
  match->last_channel_band = channel_band;
  if (reason == K1_RTL8852BS_SCAN_OFLD_C2H_PRE_TX)
    {
      match->pre_tx_events++;
    }
  else if (reason == K1_RTL8852BS_SCAN_OFLD_C2H_POST_TX)
    {
      match->post_tx_events++;
      if (status == K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_FAILURE)
        {
          match->post_tx_failures++;
        }
    }
  else if (reason == K1_RTL8852BS_SCAN_OFLD_C2H_LEAVE_CHANNEL)
    {
      match->leave_channel_events++;
    }

  if ((dword3 & K1_RTL8852BS_SCAN_OFLD_C2H_ACTIVE_CHANNEL) != 0)
    {
      match->active_channel_notifies++;
    }

  match->transmit_fail_reported += dword3 &
                                   K1_RTL8852BS_SCAN_OFLD_C2H_TX_FAIL_MASK;

  k1_early_puts("K1 Wi-Fi GPL: passive scan C2H channel=");
  k1_early_puthex(channel);
  k1_early_puts(" reason=");
  k1_early_puthex(reason);
  k1_early_puts(" status=");
  k1_early_puthex(status);
  k1_early_puts(" band=");
  k1_early_puthex(channel_band);
  k1_early_puts(" dword0=");
  k1_early_puthex(dword0);
  k1_early_puts(" dword3=");
  k1_early_puthex(dword3);
  k1_early_puts("\r\n");
  ret = k1_rtl8852bs_scanofld_observe_report(c2h, dword3, reason, match);
  if (ret < 0)
    {
      return ret;
    }

  /* A failing status on a transmit notification is exactly the result this
   * sweep exists to measure, so it is counted above and the sweep continues:
   * abandoning it here would hide the remaining channels and the firmware's
   * end-of-scan report, which is where the transmit failure count lives.  A
   * failing status on any other notification still ends the sweep.
   */

  if (status == K1_RTL8852BS_SCAN_OFLD_C2H_STATUS_FAILURE &&
      reason != K1_RTL8852BS_SCAN_OFLD_C2H_PRE_TX &&
      reason != K1_RTL8852BS_SCAN_OFLD_C2H_POST_TX)
    {
      return -EIO;
    }

  channel_index = k1_rtl8852bs_scanofld_passive_channel_index(channel);
  if (reason == K1_RTL8852BS_SCAN_OFLD_C2H_ENTER_CHANNEL &&
      channel_index >= 0 && channel_band == 0 &&
      (dword3 & K1_RTL8852BS_SCAN_OFLD_C2H_HARDWARE_BAND) == 0)
    {
      channel_mask = 1u << channel_index;
      match->entered_channels |= channel_mask;
      if ((match->advanced_channels & channel_mask) == 0 &&
          !match->dwell_pending)
        {
          /* The original PHL scan state machine waits for the configured
           * channel dwell, then tells firmware to advance.  Arm a deadline
           * instead of sleeping here: this handler runs from the RX read
           * loop, so a blocking dwell stops the host from draining the RX
           * FIFO for the whole dwell and throws away every frame the
           * firmware receives on the channel but the first.
           */

          match->dwell_pending = true;
          match->dwell_channel = channel;
          match->dwell_deadline = clock_systime_ticks() +
            MSEC2TICK(K1_RTL8852BS_SCAN_OFLD_PASSIVE_PERIOD_MSEC);

          /* Give every dwell its own small frame-dump budget so the decode
           * evidence covers every channel instead of only the first.
           */

          match->rx_frames_logged = 0;

          /* The radio is now parked on this channel and stays there until the
           * dwell deadline above submits the next-channel command, so this is
           * the one point in the sweep at which a host-built management frame
           * can be handed to the hardware knowing what it will be radiated
           * on.  The transmit is armed by a diagnostic and is a no-op in
           * every other sweep, so the passive path is unchanged when nothing
           * armed it.
           */

          if (g_k1_rtl8852bs_auth_action.armed &&
              !g_k1_rtl8852bs_auth_action.transmitted &&
              channel == g_k1_rtl8852bs_auth_action.channel)
            {
              k1_rtl8852bs_runtime_auth_transmit();
            }
        }
    }
  else if (reason == K1_RTL8852BS_SCAN_OFLD_C2H_SCAN_END)
    {
      match->saw_scan_end = true;
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_scanofld_dwell_poll
 *
 * Description:
 *   Advance the scan when the armed dwell deadline has expired.  Called from
 *   the RX read loop so the dwell is a deadline rather than a blocking sleep.
 ****************************************************************************/

static int k1_rtl8852bs_scanofld_dwell_poll(
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match)
{
  int channel_index;
  int ret;

  if (!match->dwell_pending ||
      (sclock_t)(clock_systime_ticks() - match->dwell_deadline) < 0)
    {
      return OK;
    }

  channel_index = k1_rtl8852bs_scanofld_passive_channel_index(
    match->dwell_channel);
  if (channel_index < 0)
    {
      return -EINVAL;
    }

  match->dwell_pending = false;
  ret = k1_rtl8852bs_runtime_scanofld_next_channel_submit(
    match->dwell_channel);
  if (ret < 0)
    {
      return ret;
    }

  match->advanced_channels |= 1u << channel_index;
  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_export_result
 *
 * Description:
 *   Copy the accepted access points out of the internal sweep state before
 *   it is released.  Only an entry backed by a Beacon or a Probe Response
 *   becomes a reported BSS: the sweep also records BSSIDs seen solely in
 *   data frames, and reporting one of those as a scan result would claim a
 *   beacon the receiver never observed.  Those are counted instead, so an
 *   empty report stays distinguishable from a filtered one.
 *
 *   The reported channel prefers the DS Parameter Set carried by the frame
 *   over the channel the sweep was dwelling on when it arrived.
 ****************************************************************************/

static void k1_rtl8852bs_runtime_scanofld_export_result(
  FAR const struct k1_rtl8852bs_scanofld_passive_match_s *match,
  FAR struct k1_rtl8852bs_scan_result_s *result)
{
  FAR const struct k1_rtl8852bs_scanofld_bss_s *source;
  FAR struct k1_rtl8852bs_scan_bss_s *entry;
  unsigned int index;

  memset(result, 0, sizeof(*result));
  result->entered_channels = match->entered_channels;
  result->advanced_channels = match->advanced_channels;
  result->dropped_count = match->bss_dropped;
  result->scan_end = match->saw_scan_end;

  for (index = 0; index < match->bss_count &&
       index < K1_RTL8852BS_SCAN_OFLD_BSS_MAX; index++)
    {
      source = &match->bss[index];
      if (source->beacon_frames == 0 && source->probe_response_frames == 0)
        {
          result->data_only_count++;
          continue;
        }

      if (result->bss_count >= K1_RTL8852BS_SCAN_BSS_MAX)
        {
          result->dropped_count++;
          continue;
        }

      entry = &result->bss[result->bss_count++];
      memcpy(entry->bssid, source->bssid, sizeof(entry->bssid));
      memcpy(entry->ssid, source->ssid, sizeof(entry->ssid));
      entry->ssid_length = source->ssid_length > sizeof(entry->ssid) ?
                           sizeof(entry->ssid) : source->ssid_length;
      entry->channel = source->ds_channel != 0 ?
                       source->ds_channel : source->dwell_channel;
      entry->capability = source->capability;
      entry->beacon_interval = source->beacon_interval;
      entry->beacon_frames = source->beacon_frames;
      entry->probe_response_frames = source->probe_response_frames;
      entry->ssid_present = source->ssid_present;
    }
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_dle_dfi_read
 *
 * Description:
 *   One transaction of the dispatcher debug function interface, the read path
 *   of dle_dfi_ctrl_8852b().  The control word carries the target, the address
 *   and an active bit that the hardware clears when the data word holds the
 *   answer; nothing here writes dispatcher state.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_dle_dfi_read(uint32_t control,
                                              uint32_t data,
                                              uint32_t target,
                                              uint32_t address,
                                              FAR uint32_t *value)
{
  unsigned int attempt;
  uint32_t word;
  int ret;

  if (value == NULL || (address & ~K1_RTL8852BS_DFI_ADDRESS_MASK) != 0)
    {
      return -EINVAL;
    }

  word = K1_RTL8852BS_DFI_ACTIVE |
         ((target << K1_RTL8852BS_DFI_TARGET_SHIFT) &
          K1_RTL8852BS_DFI_TARGET_MASK) | address;
  ret = k1_rtl8852bs_mac_write32(control, word);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_DFI_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(control, &word);
      if (ret < 0)
        {
          return ret;
        }

      if ((word & K1_RTL8852BS_DFI_ACTIVE) == 0)
        {
          return k1_rtl8852bs_mac_read32(data, value);
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_dbg_port_read
 *
 * Description:
 *   One sample of a protocol or scheduler debug port, the loop body of
 *   print_dbg_port().  The selector goes into the low byte of the select
 *   register with the port enable bit kept set, and the data register is read
 *   after the same short delay the original waits.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_dbg_port_read(uint32_t select,
                                               uint32_t enable,
                                               uint32_t selector,
                                               uint32_t data,
                                               FAR uint32_t *value)
{
  uint32_t word;
  int ret;

  ret = k1_rtl8852bs_mac_read32(select, &word);
  if (ret < 0)
    {
      return ret;
    }

  word &= ~0x000000ffu;
  word |= enable | (selector & 0x000000ffu);
  ret = k1_rtl8852bs_mac_write32(select, word);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_DBG_PORT_DELAY_US);
  return k1_rtl8852bs_mac_read32(data, value);
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_dbg_port_read_mask
 *
 * Description:
 *   One sample of a debug port whose selector is not the low byte of its
 *   select register.  print_dbg_port() writes the selector into the field the
 *   port table names and reads the data register after the same short delay;
 *   the two dispatcher engines, the release block and the transmit packet
 *   control block carry one selector per output half, and the two direct
 *   memory access ports carry theirs high inside a control register whose
 *   other bits have to survive the write.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_dbg_port_read_mask(uint32_t select,
                                                   uint32_t enable,
                                                   uint32_t mask,
                                                   unsigned int shift,
                                                   uint32_t selector,
                                                   uint32_t data,
                                                   FAR uint32_t *value)
{
  uint32_t word;
  int ret;

  ret = k1_rtl8852bs_mac_read32(select, &word);
  if (ret < 0)
    {
      return ret;
    }

  word &= ~(mask << shift);
  word |= enable | ((selector & mask) << shift);
  ret = k1_rtl8852bs_mac_write32(select, word);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_DBG_PORT_DELAY_US);
  return k1_rtl8852bs_mac_read32(data, value);
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_put_word
 *
 * Description:
 *   One thirty two bit value as eight hexadecimal digits.  The early console
 *   helper prints a register width value, which doubles the length of a line
 *   carrying eight of them, and a debug port sweep is several hundred values
 *   per stage on a serial link that is the slowest part of a board run.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_put_word(uint32_t value)
{
  static const char digits[] = "0123456789abcdef";
  char text[9];
  int index;

  for (index = 7; index >= 0; index--)
    {
      text[index] = digits[value & 0xfu];
      value >>= 4;
    }

  text[8] = '\0';
  k1_early_puts(text);
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_tx_witness
 *
 * Description:
 *   Watch, while a sweep runs, the state that decides whether a frame the
 *   firmware handed to the transmit path ever reaches the air.
 *
 *   A single sample proves nothing here, because a frame that is transmitted
 *   normally leaves its queue in microseconds and no sampling rate would see
 *   it.  What a sample can show is the opposite: a queue that is never empty
 *   again, a protocol state machine that never returns to idle, a transmit NAV
 *   abort state that stays asserted.  So each field is accumulated both ways,
 *   as an OR over every sample and as an AND over every sample, and the pair
 *   separates a bit that was momentarily set from a bit that was always set.
 *
 *   The sweep itself is the control.  The same watch runs during the passive
 *   sweep, which transmits nothing and is known to work, so a queue that is
 *   busy only in the active sweep is a difference the sweep caused, and the
 *   free page count read alongside is the positive control for the debug
 *   interface: a plausible non-zero page count proves it answers at all.
 *
 *   The two strides keep the watch from changing what it watches.  The three
 *   directly readable words are sampled often, and the debug interface
 *   transactions, which cost a write, a poll and a read each, are sampled
 *   rarely.
 *
 ****************************************************************************/

#define K1_RTL8852BS_WITNESS_STRIDE          16u
#define K1_RTL8852BS_WITNESS_DEEP_STRIDE     256u
#define K1_RTL8852BS_WITNESS_SCH_SELECTORS   5u
#define K1_RTL8852BS_WITNESS_MID_SAMPLE      4u

struct k1_rtl8852bs_tx_witness_s
{
  uint32_t samples;
  uint32_t deep_samples;
  uint32_t empty0_or;
  uint32_t empty0_and;
  uint32_t empty1_or;
  uint32_t empty1_and;
  uint32_t ctn_sel_or;
  uint32_t ctn_sel_and;
  uint32_t ctn_txen_and;
  uint32_t qempty_or;
  uint32_t qempty_and;
  uint32_t qempty_error;
  uint32_t cpumgq_busy;
  uint32_t mgq_busy;
  uint32_t public_pages;
  uint32_t public_pages_error;
  uint32_t wde_wlcpu_pages;
  uint32_t ple_txpl_pages;
  uint32_t ple_wlcpu_pages;
  uint32_t ptcl_fsm0_or;
  uint32_t ptcl_fsm0;
  uint32_t ptcl_fsm1;
  uint32_t ptcl_phy;
  uint32_t sch_or[K1_RTL8852BS_WITNESS_SCH_SELECTORS];
  uint32_t sch_last[K1_RTL8852BS_WITNESS_SCH_SELECTORS];
  uint32_t nav_abort_first;
  uint32_t nav_abort_last;
  uint32_t cca_abort_first;
  uint32_t cca_abort_last;
  uint32_t resp_abort_first;
  uint32_t resp_abort_last;
  bool mid_snapshot_done;
};

static struct k1_rtl8852bs_tx_witness_s g_k1_rtl8852bs_tx_witness;

static void k1_rtl8852bs_runtime_tx_witness_reset(void)
{
  memset(&g_k1_rtl8852bs_tx_witness, 0, sizeof(g_k1_rtl8852bs_tx_witness));
  g_k1_rtl8852bs_tx_witness.empty0_and = UINT32_MAX;
  g_k1_rtl8852bs_tx_witness.empty1_and = UINT32_MAX;
  g_k1_rtl8852bs_tx_witness.ctn_sel_and = UINT32_MAX;
  g_k1_rtl8852bs_tx_witness.ctn_txen_and = UINT32_MAX;
  g_k1_rtl8852bs_tx_witness.qempty_and = UINT32_MAX;
}

/* The snapshot the sweep itself takes, declared here because the queue sweep
 * behind it is defined further down this file.  The four snapshots that bracket
 * the active path all run with the sweep either not started or already
 * finished; this one runs while it is in progress, which is the only moment at
 * which a queue that holds a frame for the duration of one channel dwell is
 * still holding it.
 */

static void k1_rtl8852bs_runtime_fault_snapshot(FAR const char *stage,
                                                bool deep);

static void k1_rtl8852bs_runtime_tx_witness_deep(void)
{
  FAR struct k1_rtl8852bs_tx_witness_s *w = &g_k1_rtl8852bs_tx_witness;
  bool first = w->deep_samples == 0;
  uint32_t value;
  unsigned int index;

  w->deep_samples++;

  if (k1_rtl8852bs_runtime_dle_dfi_read(
        K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
        K1_RTL8852BS_DFI_TYPE_QEMPTY,
        K1_RTL8852BS_DFI_QEMPTY_MGQ_GROUP, &value) == OK)
    {
      w->qempty_or |= value;
      w->qempty_and &= value;
      if ((value & K1_RTL8852BS_DFI_QEMPTY_CMAC0_CPUMGQ) == 0)
        {
          w->cpumgq_busy++;
        }

      if ((value & K1_RTL8852BS_DFI_QEMPTY_CMAC0_MGQ) == 0)
        {
          w->mgq_busy++;
        }
    }
  else
    {
      w->qempty_error++;
    }

  if (k1_rtl8852bs_runtime_dle_dfi_read(
        K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
        K1_RTL8852BS_DFI_TYPE_FREEPG,
        K1_RTL8852BS_DFI_FREEPG_PUBNUM, &value) == OK)
    {
      w->public_pages = value & K1_RTL8852BS_DFI_PUB_PGNUM_MASK;
    }
  else
    {
      w->public_pages_error++;
    }

  if (k1_rtl8852bs_runtime_dle_dfi_read(
        K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
        K1_RTL8852BS_DFI_TYPE_QUOTA,
        K1_RTL8852BS_WDE_QTAID_WLAN_CPU, &value) == OK)
    {
      value = (value & K1_RTL8852BS_DFI_USE_PGNUM_MASK) >>
              K1_RTL8852BS_DFI_USE_PGNUM_SHIFT;
      if (value > w->wde_wlcpu_pages)
        {
          w->wde_wlcpu_pages = value;
        }
    }

  if (k1_rtl8852bs_runtime_dle_dfi_read(
        K1_RTL8852BS_PLE_DFI_CTRL, K1_RTL8852BS_PLE_DFI_DATA,
        K1_RTL8852BS_DFI_TYPE_QUOTA,
        K1_RTL8852BS_PLE_QTAID_B0_TXPL, &value) == OK)
    {
      value = (value & K1_RTL8852BS_DFI_USE_PGNUM_MASK) >>
              K1_RTL8852BS_DFI_USE_PGNUM_SHIFT;
      if (value > w->ple_txpl_pages)
        {
          w->ple_txpl_pages = value;
        }
    }

  if (k1_rtl8852bs_runtime_dle_dfi_read(
        K1_RTL8852BS_PLE_DFI_CTRL, K1_RTL8852BS_PLE_DFI_DATA,
        K1_RTL8852BS_DFI_TYPE_QUOTA,
        K1_RTL8852BS_PLE_QTAID_WLAN_CPU, &value) == OK)
    {
      value = (value & K1_RTL8852BS_DFI_USE_PGNUM_MASK) >>
              K1_RTL8852BS_DFI_USE_PGNUM_SHIFT;
      if (value > w->ple_wlcpu_pages)
        {
          w->ple_wlcpu_pages = value;
        }
    }

  if (k1_rtl8852bs_runtime_dbg_port_read(
        K1_RTL8852BS_PTCL_DBG_SEL, K1_RTL8852BS_PTCL_DBG_ENABLE,
        K1_RTL8852BS_PTCL_DBG_SEL_FSM_0,
        K1_RTL8852BS_PTCL_DBG_INFO, &value) == OK)
    {
      w->ptcl_fsm0 = value;
      w->ptcl_fsm0_or |= value;
    }

  if (k1_rtl8852bs_runtime_dbg_port_read(
        K1_RTL8852BS_PTCL_DBG_SEL, K1_RTL8852BS_PTCL_DBG_ENABLE,
        K1_RTL8852BS_PTCL_DBG_SEL_FSM_1,
        K1_RTL8852BS_PTCL_DBG_INFO, &value) == OK)
    {
      w->ptcl_fsm1 = value;
    }

  if (k1_rtl8852bs_runtime_dbg_port_read(
        K1_RTL8852BS_PTCL_DBG_SEL, K1_RTL8852BS_PTCL_DBG_ENABLE,
        K1_RTL8852BS_PTCL_DBG_SEL_PHY_DBG,
        K1_RTL8852BS_PTCL_DBG_INFO, &value) == OK)
    {
      w->ptcl_phy = value;
    }

  for (index = 0; index < K1_RTL8852BS_WITNESS_SCH_SELECTORS; index++)
    {
      if (k1_rtl8852bs_runtime_dbg_port_read(
            K1_RTL8852BS_SCH_DBG_SEL, K1_RTL8852BS_SCH_DBG_ENABLE,
            K1_RTL8852BS_SCH_DBG_SEL_PREBKF_1 + index,
            K1_RTL8852BS_SCH_DBG_INFO, &value) == OK)
        {
          w->sch_last[index] = value;
          w->sch_or[index] |= value;
        }
    }

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_RESP_TX_NAV_ABORT_CNT,
                              &value) == OK)
    {
      w->nav_abort_last = value;
      if (first)
        {
          w->nav_abort_first = value;
        }
    }

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_RESP_TX_CCA_ABORT_CNT,
                              &value) == OK)
    {
      w->cca_abort_last = value;
      if (first)
        {
          w->cca_abort_first = value;
        }
    }

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_TRXPTCL_RESP_TX_ABT_CNT,
                              &value) == OK)
    {
      w->resp_abort_last = value;
      if (first)
        {
          w->resp_abort_first = value;
        }
    }
}

static void k1_rtl8852bs_runtime_tx_witness_sample(unsigned int attempt)
{
  FAR struct k1_rtl8852bs_tx_witness_s *w = &g_k1_rtl8852bs_tx_witness;
  uint32_t value;

  if ((attempt % K1_RTL8852BS_WITNESS_STRIDE) != 0)
    {
      return;
    }

  w->samples++;

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_DLE_EMPTY0, &value) == OK)
    {
      w->empty0_or |= value;
      w->empty0_and &= value;
    }

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_DLE_EMPTY1, &value) == OK)
    {
      w->empty1_or |= value;
      w->empty1_and &= value;
    }

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_PTCL_TX_CTN_SEL, &value) == OK)
    {
      w->ctn_sel_or |= value;
      w->ctn_sel_and &= value;
    }

  if (k1_rtl8852bs_mac_read32(K1_RTL8852BS_CTN_TXEN, &value) == OK)
    {
      w->ctn_txen_and &= value;
    }

  if ((attempt % K1_RTL8852BS_WITNESS_DEEP_STRIDE) == 0)
    {
      k1_rtl8852bs_runtime_tx_witness_deep();
    }

  /* One snapshot from inside the sweep, once.  The queue sweep it carries is
   * the part that names a queue index, and it is deliberately taken without
   * the debug port sweep: several hundred register reads in the middle of a
   * channel dwell would move the very timing being measured, while a queue
   * link table read is a handful of reads.
   */

  if (!w->mid_snapshot_done && w->samples >= K1_RTL8852BS_WITNESS_MID_SAMPLE)
    {
      w->mid_snapshot_done = true;
      k1_rtl8852bs_runtime_fault_snapshot("mid-sweep", false);
    }
}

static void k1_rtl8852bs_runtime_tx_witness_log(FAR const char *stage)
{
  FAR const struct k1_rtl8852bs_tx_witness_s *w = &g_k1_rtl8852bs_tx_witness;
  unsigned int index;

  k1_early_puts("K1 Wi-Fi GPL: witness ");
  k1_early_puts(stage);
  k1_early_puts(" samples=");
  k1_early_puthex(w->samples);
  k1_early_puts(" deep=");
  k1_early_puthex(w->deep_samples);
  k1_early_puts(" empty0-or=");
  k1_early_puthex(w->empty0_or);
  k1_early_puts(" empty0-and=");
  k1_early_puthex(w->samples != 0 ? w->empty0_and : 0);
  k1_early_puts(" empty1-or=");
  k1_early_puthex(w->empty1_or);
  k1_early_puts(" empty1-and=");
  k1_early_puthex(w->samples != 0 ? w->empty1_and : 0);
  k1_early_puts(" ctn-sel-or=");
  k1_early_puthex(w->ctn_sel_or);
  k1_early_puts(" ctn-sel-and=");
  k1_early_puthex(w->samples != 0 ? w->ctn_sel_and : 0);
  k1_early_puts(" ctn-txen-and=");
  k1_early_puthex(w->samples != 0 ? w->ctn_txen_and : 0);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: witness ");
  k1_early_puts(stage);
  k1_early_puts(" qempty-or=");
  k1_early_puthex(w->qempty_or);
  k1_early_puts(" qempty-and=");
  k1_early_puthex(w->deep_samples != 0 ? w->qempty_and : 0);
  k1_early_puts(" cpumgq-busy=");
  k1_early_puthex(w->cpumgq_busy);
  k1_early_puts(" mgq-busy=");
  k1_early_puthex(w->mgq_busy);
  k1_early_puts(" qempty-err=");
  k1_early_puthex(w->qempty_error);
  k1_early_puts(" pub-pages=");
  k1_early_puthex(w->public_pages);
  k1_early_puts(" pub-err=");
  k1_early_puthex(w->public_pages_error);
  k1_early_puts(" wde-wlcpu-pages=");
  k1_early_puthex(w->wde_wlcpu_pages);
  k1_early_puts(" ple-txpl-pages=");
  k1_early_puthex(w->ple_txpl_pages);
  k1_early_puts(" ple-wlcpu-pages=");
  k1_early_puthex(w->ple_wlcpu_pages);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: witness ");
  k1_early_puts(stage);
  k1_early_puts(" ptcl-fsm0=");
  k1_early_puthex(w->ptcl_fsm0);
  k1_early_puts(" ptcl-fsm0-or=");
  k1_early_puthex(w->ptcl_fsm0_or);
  k1_early_puts(" ptcl-fsm1=");
  k1_early_puthex(w->ptcl_fsm1);
  k1_early_puts(" ptcl-phy=");
  k1_early_puthex(w->ptcl_phy);
  k1_early_puts(" nav-abort=");
  k1_early_puthex(w->nav_abort_first);
  k1_early_puts("->");
  k1_early_puthex(w->nav_abort_last);
  k1_early_puts(" cca-abort=");
  k1_early_puthex(w->cca_abort_first);
  k1_early_puts("->");
  k1_early_puthex(w->cca_abort_last);
  k1_early_puts(" resp-abort=");
  k1_early_puthex(w->resp_abort_first);
  k1_early_puts("->");
  k1_early_puthex(w->resp_abort_last);
  k1_early_puts("\r\n");

  /* The scheduler port is reported selector by selector because only one of
   * the five is a hypothesis under test: selector 7 is the transmit NAV abort
   * state, and it is the last of them.
   */

  k1_early_puts("K1 Wi-Fi GPL: witness ");
  k1_early_puts(stage);
  k1_early_puts(" sch");

  for (index = 0; index < K1_RTL8852BS_WITNESS_SCH_SELECTORS; index++)
    {
      k1_early_puts("[");
      k1_early_puthex(K1_RTL8852BS_SCH_DBG_SEL_PREBKF_1 + index);
      k1_early_puts("]=");
      k1_early_puthex(w->sch_last[index]);
      k1_early_puts("/");
      k1_early_puthex(w->sch_or[index]);
    }

  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_err_isr_log
 *
 * Description:
 *   One labelled pass over every error interrupt status word the original
 *   reads after a fault.  Read only: these are write one to clear registers,
 *   so a phase that reports the same bits as the phase before it did not
 *   introduce them, and a bit that appears between two phases was raised
 *   between them.
 *
 ****************************************************************************/

struct k1_rtl8852bs_err_isr_s
{
  FAR const char *name;
  uint32_t address;
};

static const struct k1_rtl8852bs_err_isr_s g_k1_rtl8852bs_err_isr[] =
{
  {
    "ser", K1_RTL8852BS_SER_DBG_INFO
  },
  {
    "dmac", K1_RTL8852BS_DMAC_ERR_ISR
  },
  {
    "disp-other", K1_RTL8852BS_DISP_OTHER_ERR_ISR
  },
  {
    "disp-host", K1_RTL8852BS_DISP_HOST_ERR_ISR
  },
  {
    "disp-cpu", K1_RTL8852BS_DISP_CPU_ERR_ISR
  },
  {
    "wde", K1_RTL8852BS_WDE_ERR_ISR
  },
  {
    "ple", K1_RTL8852BS_PLE_ERR_FLAG_ISR
  },
  {
    "wdrls", K1_RTL8852BS_WDRLS_ERR_ISR
  },
  {
    "cpuio", K1_RTL8852BS_CPUIO_ERR_ISR
  },
  {
    "pktin", K1_RTL8852BS_PKTIN_ERR_ISR
  },
  {
    "mpdu-tx", K1_RTL8852BS_MPDU_TX_ERR_ISR
  },
  {
    "mpdu-rx", K1_RTL8852BS_MPDU_RX_ERR_ISR
  },
  {
    "sta-sch", K1_RTL8852BS_STA_SCH_ERR_ISR
  },
  {
    "txpktctl", K1_RTL8852BS_TXPKTCTL_ERR_ISR
  },
  {
    "cmac", K1_RTL8852BS_CMAC_ERR_ISR
  },
  {
    "sch-err", K1_RTL8852BS_SCHEDULE_ERR_ISR
  },
  {
    "tmac", K1_RTL8852BS_TMAC_ERR_ISR
  },
  {
    "phyinfo", K1_RTL8852BS_PHYINFO_ERR_ISR
  },
  {
    "rmac", K1_RTL8852BS_RMAC_ERR_ISR
  },
  {
    "bbrpt-com", K1_RTL8852BS_BBRPT_COM_ERR_ISR
  },
  {
    "bbrpt-chinfo", K1_RTL8852BS_BBRPT_CHINFO_ERR_ISR
  },
  {
    "bbrpt-dfs", K1_RTL8852BS_BBRPT_DFS_ERR_ISR
  },
  {
    "dmac-imr", K1_RTL8852BS_DMAC_ERR_IMR
  },
  {
    "cmac-imr", K1_RTL8852BS_CMAC_ERR_IMR
  }
};

#define K1_RTL8852BS_ERR_ISR_COUNT \
  (sizeof(g_k1_rtl8852bs_err_isr) / sizeof(g_k1_rtl8852bs_err_isr[0]))
#define K1_RTL8852BS_ERR_ISR_PER_LINE 8u

static void k1_rtl8852bs_runtime_err_isr_log(FAR const char *stage)
{
  unsigned int index;
  uint32_t value;

  for (index = 0; index < K1_RTL8852BS_ERR_ISR_COUNT; index++)
    {
      if ((index % K1_RTL8852BS_ERR_ISR_PER_LINE) == 0)
        {
          if (index != 0)
            {
              k1_early_puts("\r\n");
            }

          k1_early_puts("K1 Wi-Fi GPL: err ");
          k1_early_puts(stage);
        }

      k1_early_puts(" ");
      k1_early_puts(g_k1_rtl8852bs_err_isr[index].name);
      k1_early_puts("=");

      if (k1_rtl8852bs_mac_read32(g_k1_rtl8852bs_err_isr[index].address,
                                  &value) == OK)
        {
          k1_early_puthex(value);
        }
      else
        {
          k1_early_puts("rd-err");
        }
    }

  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_dle_snapshot_log
 *
 * Description:
 *   One labelled snapshot of the two dispatcher empty words and the page
 *   accounting behind them.  The transmit witness reports these as minimums
 *   and maximums over a whole sweep, which cannot say when a queue stopped
 *   being empty; this prints them at one instant so consecutive calls bracket
 *   a single step of the active path.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_dle_snapshot_register(
  FAR const char *label, uint32_t address)
{
  uint32_t value;

  k1_early_puts(label);

  if (k1_rtl8852bs_mac_read32(address, &value) == OK)
    {
      k1_early_puthex(value);
    }
  else
    {
      k1_early_puts("rd-err");
    }
}

static void k1_rtl8852bs_runtime_dle_snapshot_quota(
  FAR const char *label, uint32_t control, uint32_t data, uint32_t type,
  uint32_t address, uint32_t mask, unsigned int shift)
{
  uint32_t value;

  k1_early_puts(label);

  if (k1_rtl8852bs_runtime_dle_dfi_read(control, data, type, address,
                                        &value) == OK)
    {
      k1_early_puthex((value & mask) >> shift);
    }
  else
    {
      k1_early_puts("rd-err");
    }
}

static void k1_rtl8852bs_runtime_dle_snapshot_log(FAR const char *stage)
{
  k1_early_puts("K1 Wi-Fi GPL: dle ");
  k1_early_puts(stage);

  k1_rtl8852bs_runtime_dle_snapshot_register(" empty0=",
                                             K1_RTL8852BS_DLE_EMPTY0);
  k1_rtl8852bs_runtime_dle_snapshot_register(" empty1=",
                                             K1_RTL8852BS_DLE_EMPTY1);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " qempty4=", K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QEMPTY, K1_RTL8852BS_DFI_QEMPTY_MGQ_GROUP,
    UINT32_MAX, 0);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " pub-pages=", K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_FREEPG, K1_RTL8852BS_DFI_FREEPG_PUBNUM,
    K1_RTL8852BS_DFI_PUB_PGNUM_MASK, 0);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " wde-hif=", K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QUOTA, K1_RTL8852BS_WDE_QTAID_HOST_IF,
    K1_RTL8852BS_DFI_USE_PGNUM_MASK, K1_RTL8852BS_DFI_USE_PGNUM_SHIFT);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " wde-wlcpu=", K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QUOTA, K1_RTL8852BS_WDE_QTAID_WLAN_CPU,
    K1_RTL8852BS_DFI_USE_PGNUM_MASK, K1_RTL8852BS_DFI_USE_PGNUM_SHIFT);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " wde-pktin=", K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QUOTA, K1_RTL8852BS_WDE_QTAID_PKTIN,
    K1_RTL8852BS_DFI_USE_PGNUM_MASK, K1_RTL8852BS_DFI_USE_PGNUM_SHIFT);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " ple-txpl=", K1_RTL8852BS_PLE_DFI_CTRL, K1_RTL8852BS_PLE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QUOTA, K1_RTL8852BS_PLE_QTAID_B0_TXPL,
    K1_RTL8852BS_DFI_USE_PGNUM_MASK, K1_RTL8852BS_DFI_USE_PGNUM_SHIFT);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " ple-wlcpu=", K1_RTL8852BS_PLE_DFI_CTRL, K1_RTL8852BS_PLE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QUOTA, K1_RTL8852BS_PLE_QTAID_WLAN_CPU,
    K1_RTL8852BS_DFI_USE_PGNUM_MASK, K1_RTL8852BS_DFI_USE_PGNUM_SHIFT);
  k1_rtl8852bs_runtime_dle_snapshot_quota(
    " ple-h2c=", K1_RTL8852BS_PLE_DFI_CTRL, K1_RTL8852BS_PLE_DFI_DATA,
    K1_RTL8852BS_DFI_TYPE_QUOTA, K1_RTL8852BS_PLE_QTAID_H2C,
    K1_RTL8852BS_DFI_USE_PGNUM_MASK, K1_RTL8852BS_DFI_USE_PGNUM_SHIFT);

  k1_early_puts("\r\n");
}

/* The selector values of the four ports whose selector is not a plain range.
 * print_dbg_port() walks each vendor table entry from its start to its end by
 * its increment, and for these ports the increment carries the same value in
 * both halves of the field, one selector per output half; the lists below are
 * those walks flattened.  The dispatcher engine lists cover the buffer manager
 * control and arbiter, the queue manager control, information and arbiter and
 * every port interface; the release block list covers its control, both report
 * generators and both payload engine channels; the transmit packet control
 * list covers the fetch engine, the band zero command parser, the command
 * block DMA interface and all four packet units.
 */

static const uint32_t g_k1_rtl8852bs_dbg_wde_sel[] =
{
  0x00000000u, 0x00000101u, 0x00000e0eu, 0x00001010u, 0x00001111u,
  0x00001414u, 0x00001e1eu, 0x00008080u, 0x00008181u, 0x00008282u,
  0x00009090u, 0x00009191u, 0x00009292u, 0x0000b0b0u, 0x0000b1b1u,
  0x0000b2b2u, 0x0000c0c0u, 0x0000c1c1u, 0x0000c2c2u, 0x0000e0e0u,
  0x0000e1e1u, 0x0000e2e2u, 0x0000f0f0u, 0x0000f1f1u, 0x0000f2f2u
};

static const uint32_t g_k1_rtl8852bs_dbg_ple_sel[] =
{
  0x00000000u, 0x00000101u, 0x00000e0eu, 0x00001010u, 0x00001111u,
  0x00001414u, 0x00001e1eu, 0x00008080u, 0x00008181u, 0x00008282u,
  0x00009090u, 0x00009191u, 0x00009292u, 0x0000a0a0u, 0x0000a1a1u,
  0x0000a2a2u, 0x0000b0b0u, 0x0000b1b1u, 0x0000b2b2u, 0x0000c0c0u,
  0x0000c1c1u, 0x0000c2c2u, 0x0000d0d0u, 0x0000d1d1u, 0x0000d2d2u,
  0x0000e0e0u, 0x0000e1e1u, 0x0000e2e2u
};

static const uint32_t g_k1_rtl8852bs_dbg_wdrls_sel[] =
{
  0x00000000u, 0x00000808u, 0x00000909u, 0x00000c0cu, 0x00000d0du,
  0x00001010u, 0x00001414u, 0x00001818u, 0x00001919u, 0x00001a1au,
  0x00001b1bu
};

static const uint32_t g_k1_rtl8852bs_dbg_txpkt_sel[] =
{
  0x00000000u, 0x00010001u, 0x00800080u, 0x00810081u, 0x00880088u,
  0x01000100u, 0x01010101u, 0x01020102u, 0x01080108u, 0x01090109u,
  0x010a010au, 0x010b010bu, 0x010c010cu, 0x010d010du, 0x01100110u,
  0x01110111u, 0x01120112u, 0x01180118u, 0x01190119u, 0x011a011au,
  0x011b011bu, 0x011c011cu, 0x011d011du, 0x01200120u, 0x01210121u,
  0x01220122u, 0x01280128u, 0x01290129u, 0x012a012au, 0x012b012bu,
  0x012c012cu, 0x012d012du, 0x01300130u, 0x01310131u, 0x01320132u,
  0x01380138u, 0x01390139u, 0x013a013au, 0x013b013bu, 0x013c013cu,
  0x013d013du
};

struct k1_rtl8852bs_dbg_port_s
{
  FAR const char *name;
  uint32_t select;
  uint32_t enable;
  uint32_t mask;
  unsigned int shift;
  uint32_t data;
  FAR const uint32_t *selectors;
  uint32_t count;
  uint32_t first;
  uint32_t step;
};

static const struct k1_rtl8852bs_dbg_port_s g_k1_rtl8852bs_dbg_port[] =
{
  {
    "ptcl", K1_RTL8852BS_PTCL_DBG_SEL, K1_RTL8852BS_PTCL_DBG_ENABLE,
    K1_RTL8852BS_DBG_SEL_BYTE_MASK, 0, K1_RTL8852BS_PTCL_DBG_INFO,
    NULL, 0x40u, 0x00u, 1u
  },
  {
    "sch", K1_RTL8852BS_SCH_DBG_SEL, K1_RTL8852BS_SCH_DBG_ENABLE,
    K1_RTL8852BS_DBG_SEL_BYTE_MASK, 0, K1_RTL8852BS_SCH_DBG_INFO,
    NULL, 0x30u, 0x00u, 1u
  },
  {
    "mactx", K1_RTL8852BS_MACTX_DBG_SEL_CNT, 0,
    K1_RTL8852BS_MACTX_DBG_SEL_MASK, 0, K1_RTL8852BS_DBG_PORT_SEL,
    NULL, 0x1au, 0x00u, 1u
  },
  {
    "trxptcl", K1_RTL8852BS_TRXPTCL_DBG_SEL, 0,
    K1_RTL8852BS_DBG_SEL_BYTE_MASK, 0, K1_RTL8852BS_DBG_PORT_SEL,
    NULL, 0x09u, 0x08u, 1u
  },
  {
    "txdma", K1_RTL8852BS_TXDMA_DBG_SEL, 0,
    K1_RTL8852BS_TXDMA_DBG_SEL_MASK, K1_RTL8852BS_TXDMA_DBG_SEL_SHIFT,
    K1_RTL8852BS_DBG_PORT_SEL, NULL, 0x04u, 0x00u, 1u
  },
  {
    "cmacdma", K1_RTL8852BS_RXDMA_CTRL0, 0,
    K1_RTL8852BS_RXDMA_DBG_SEL_MASK, K1_RTL8852BS_RXDMA_DBG_SEL_SHIFT,
    K1_RTL8852BS_DBG_PORT_SEL, NULL, 0x40u, 0x00u, 1u
  },
  {
    "txinfo0", K1_RTL8852BS_WMAC_TX_CTRL_DBG, 0,
    K1_RTL8852BS_WMAC_TX_CTRL_SEL_MASK, 0, K1_RTL8852BS_WMAC_TX_INFO0_DBG,
    NULL, 0x08u, 0x00u, 1u
  },
  {
    "txinfo1", K1_RTL8852BS_WMAC_TX_CTRL_DBG, 0,
    K1_RTL8852BS_WMAC_TX_CTRL_SEL_MASK, 0, K1_RTL8852BS_WMAC_TX_INFO1_DBG,
    NULL, 0x08u, 0x00u, 1u
  },
  {
    "tfinfo0", K1_RTL8852BS_WMAC_TX_TF_INFO_0, 0,
    K1_RTL8852BS_WMAC_TX_TF_SEL_MASK, 0, K1_RTL8852BS_WMAC_TX_TF_INFO_1,
    NULL, 0x05u, 0x00u, 1u
  },
  {
    "tfinfo1", K1_RTL8852BS_WMAC_TX_TF_INFO_0, 0,
    K1_RTL8852BS_WMAC_TX_TF_SEL_MASK, 0, K1_RTL8852BS_WMAC_TX_TF_INFO_2,
    NULL, 0x05u, 0x00u, 1u
  },
  {
    "wde", K1_RTL8852BS_WDE_DBG_CTL, 0, K1_RTL8852BS_DBG_SEL_WORD_MASK, 0,
    K1_RTL8852BS_WDE_DBG_OUT, g_k1_rtl8852bs_dbg_wde_sel,
    sizeof(g_k1_rtl8852bs_dbg_wde_sel) /
    sizeof(g_k1_rtl8852bs_dbg_wde_sel[0]), 0, 0
  },
  {
    "ple", K1_RTL8852BS_PLE_DBG_CTL, 0, K1_RTL8852BS_DBG_SEL_WORD_MASK, 0,
    K1_RTL8852BS_PLE_DBG_OUT, g_k1_rtl8852bs_dbg_ple_sel,
    sizeof(g_k1_rtl8852bs_dbg_ple_sel) /
    sizeof(g_k1_rtl8852bs_dbg_ple_sel[0]), 0, 0
  },
  {
    "wdrls", K1_RTL8852BS_WDRLS_DBG_CTL, 0, K1_RTL8852BS_DBG_SEL_WORD_MASK, 0,
    K1_RTL8852BS_WDRLS_DBG_OUT, g_k1_rtl8852bs_dbg_wdrls_sel,
    sizeof(g_k1_rtl8852bs_dbg_wdrls_sel) /
    sizeof(g_k1_rtl8852bs_dbg_wdrls_sel[0]), 0, 0
  },
  {
    "txpkt", K1_RTL8852BS_TXPKT_DBG_CTL, 0, K1_RTL8852BS_DBG_SEL_LONG_MASK, 0,
    K1_RTL8852BS_TXPKT_DBG_OUT, g_k1_rtl8852bs_dbg_txpkt_sel,
    sizeof(g_k1_rtl8852bs_dbg_txpkt_sel) /
    sizeof(g_k1_rtl8852bs_dbg_txpkt_sel[0]), 0, 0
  }
};

#define K1_RTL8852BS_DBG_PORT_COUNT \
  (sizeof(g_k1_rtl8852bs_dbg_port) / sizeof(g_k1_rtl8852bs_dbg_port[0]))

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_dbg_port_sweep
 *
 * Description:
 *   Every selector of every transmit side debug port at one instant, the walk
 *   dbg_port_dump() makes.  The values are read in batches and each batch is
 *   printed as one whole line afterwards, because a MAC register read emits a
 *   command trace of its own and a line printed while reads are in flight is
 *   shredded by it.  The label carries the first selector of the batch, so a
 *   value maps back to a selector by its position in the line.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_dbg_port_sweep(FAR const char *stage)
{
  uint32_t values[K1_RTL8852BS_DBG_PORT_PER_LINE];
  bool good[K1_RTL8852BS_DBG_PORT_PER_LINE];
  unsigned int port;

  for (port = 0; port < K1_RTL8852BS_DBG_PORT_COUNT; port++)
    {
      FAR const struct k1_rtl8852bs_dbg_port_s *info =
        &g_k1_rtl8852bs_dbg_port[port];
      uint32_t batch_first = 0;
      unsigned int batch = 0;
      unsigned int index;
      unsigned int slot;

      for (index = 0; index < info->count; index++)
        {
          uint32_t selector = info->selectors != NULL ?
                              info->selectors[index] :
                              info->first + index * info->step;

          if (batch == 0)
            {
              batch_first = selector;
            }

          good[batch] = k1_rtl8852bs_runtime_dbg_port_read_mask(
            info->select, info->enable, info->mask, info->shift, selector,
            info->data, &values[batch]) == OK;
          batch++;

          if (batch < K1_RTL8852BS_DBG_PORT_PER_LINE &&
              index + 1 < info->count)
            {
              continue;
            }

          k1_early_puts("K1 Wi-Fi GPL: dbg ");
          k1_early_puts(stage);
          k1_early_puts(" ");
          k1_early_puts(info->name);
          k1_early_puts(" sel=");
          k1_rtl8852bs_runtime_put_word(batch_first);

          for (slot = 0; slot < batch; slot++)
            {
              k1_early_puts(" ");

              if (good[slot])
                {
                  k1_rtl8852bs_runtime_put_word(values[slot]);
                }
              else
                {
                  k1_early_puts("rd-err--");
                }
            }

          k1_early_puts("\r\n");
          batch = 0;
        }
    }
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_dle_engine_sweep
 *
 * Description:
 *   One dispatcher engine described completely: the page count every quota
 *   identifier holds, the free page head, tail and public count, the empty
 *   word of every queue group, and for every queue a group reports as not
 *   empty the queue link table entry behind it.  The empty words this port
 *   already prints say that some queue of some group holds something; the link
 *   table says which queue and how many frames, which is the difference
 *   between a localisation and an identification.  Only queues with a non zero
 *   packet count are printed, and the probe count is bounded so a group whose
 *   unimplemented bits read as not empty cannot turn into an unbounded walk.
 *   Nothing here writes a queue: the debug interface is read only.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_dle_q_prefix(FAR const char *stage,
                                              FAR const char *engine)
{
  k1_early_puts("K1 Wi-Fi GPL: dle-q ");
  k1_early_puts(stage);
  k1_early_puts(" ");
  k1_early_puts(engine);
}

static void k1_rtl8852bs_runtime_dle_engine_sweep(FAR const char *stage,
                                                  FAR const char *engine,
                                                  uint32_t control,
                                                  uint32_t data,
                                                  unsigned int qtaids,
                                                  unsigned int groups)
{
  uint32_t words[K1_RTL8852BS_PLE_QTAID_COUNT];
  bool good[K1_RTL8852BS_PLE_QTAID_COUNT];
  unsigned int probes = 0;
  unsigned int group;
  unsigned int index;

  for (index = 0; index < qtaids; index++)
    {
      good[index] = k1_rtl8852bs_runtime_dle_dfi_read(
        control, data, K1_RTL8852BS_DFI_TYPE_QUOTA, index,
        &words[index]) == OK;
    }

  k1_rtl8852bs_runtime_dle_q_prefix(stage, engine);
  k1_early_puts(" quota");

  for (index = 0; index < qtaids; index++)
    {
      k1_early_puts(" ");

      if (good[index])
        {
          k1_rtl8852bs_runtime_put_word(words[index]);
        }
      else
        {
          k1_early_puts("rd-err--");
        }
    }

  k1_early_puts("\r\n");

  good[0] = k1_rtl8852bs_runtime_dle_dfi_read(
    control, data, K1_RTL8852BS_DFI_TYPE_FREEPG,
    K1_RTL8852BS_DFI_FREEPG_INDEX, &words[0]) == OK;
  good[1] = k1_rtl8852bs_runtime_dle_dfi_read(
    control, data, K1_RTL8852BS_DFI_TYPE_FREEPG,
    K1_RTL8852BS_DFI_FREEPG_PUBNUM, &words[1]) == OK;

  k1_rtl8852bs_runtime_dle_q_prefix(stage, engine);
  k1_early_puts(" freepg idx=");
  if (good[0])
    {
      k1_rtl8852bs_runtime_put_word(words[0]);
    }
  else
    {
      k1_early_puts("rd-err--");
    }

  k1_early_puts(" pub=");
  if (good[1])
    {
      k1_rtl8852bs_runtime_put_word(words[1]);
    }
  else
    {
      k1_early_puts("rd-err--");
    }

  k1_early_puts("\r\n");

  for (group = 0; group < groups; group++)
    {
      uint32_t empty;
      unsigned int bit;

      good[0] = k1_rtl8852bs_runtime_dle_dfi_read(
        control, data, K1_RTL8852BS_DFI_TYPE_QEMPTY, group, &empty) == OK;

      k1_rtl8852bs_runtime_dle_q_prefix(stage, engine);
      k1_early_puts(" qempty g=");
      k1_rtl8852bs_runtime_put_word(group);
      k1_early_puts(" ");

      if (good[0])
        {
          k1_rtl8852bs_runtime_put_word(empty);
        }
      else
        {
          k1_early_puts("rd-err--");
        }

      k1_early_puts("\r\n");

      if (!good[0])
        {
          continue;
        }

      for (bit = 0; bit < K1_RTL8852BS_QEMPTY_GROUP_BITS; bit++)
        {
          uint32_t queue;
          uint32_t low;
          uint32_t high;
          uint32_t count;

          if ((empty & (1u << bit)) != 0)
            {
              continue;
            }

          if (probes >= K1_RTL8852BS_QLNKTBL_MAX_PROBE)
            {
              k1_rtl8852bs_runtime_dle_q_prefix(stage, engine);
              k1_early_puts(" probe-cap g=");
              k1_rtl8852bs_runtime_put_word(group);
              k1_early_puts("\r\n");
              return;
            }

          probes++;
          queue = group * K1_RTL8852BS_QEMPTY_GROUP_BITS + bit;

          if (k1_rtl8852bs_runtime_dle_dfi_read(
                control, data, K1_RTL8852BS_DFI_TYPE_QLNKTBL,
                (queue << 1) | 1u, &low) != OK)
            {
              continue;
            }

          count = low & K1_RTL8852BS_QLNKTBL_PKT_CNT_MASK;
          if (count == 0)
            {
              continue;
            }

          if (k1_rtl8852bs_runtime_dle_dfi_read(
                control, data, K1_RTL8852BS_DFI_TYPE_QLNKTBL,
                queue << 1, &high) != OK)
            {
              high = 0;
            }

          k1_rtl8852bs_runtime_dle_q_prefix(stage, engine);
          k1_early_puts(" q=");
          k1_rtl8852bs_runtime_put_word(queue);
          k1_early_puts(" cnt=");
          k1_rtl8852bs_runtime_put_word(count);
          k1_early_puts(" tail=");
          k1_rtl8852bs_runtime_put_word(
            (low & K1_RTL8852BS_QLNKTBL_TAIL_MASK) >>
            K1_RTL8852BS_QLNKTBL_TAIL_SHIFT);
          k1_early_puts(" head=");
          k1_rtl8852bs_runtime_put_word(
            ((high & K1_RTL8852BS_QLNKTBL_HEAD_HIGH_MASK) << 8) |
            ((low & K1_RTL8852BS_QLNKTBL_HEAD_MASK) >>
             K1_RTL8852BS_QLNKTBL_HEAD_SHIFT));
          k1_early_puts("\r\n");
        }
    }
}

static void k1_rtl8852bs_runtime_dle_queue_sweep(FAR const char *stage)
{
  k1_rtl8852bs_runtime_dle_engine_sweep(
    stage, "wde", K1_RTL8852BS_WDE_DFI_CTRL, K1_RTL8852BS_WDE_DFI_DATA,
    K1_RTL8852BS_WDE_QTAID_COUNT, K1_RTL8852BS_WDE_QEMPTY_GROUPS);
  k1_rtl8852bs_runtime_dle_engine_sweep(
    stage, "ple", K1_RTL8852BS_PLE_DFI_CTRL, K1_RTL8852BS_PLE_DFI_DATA,
    K1_RTL8852BS_PLE_QTAID_COUNT, K1_RTL8852BS_PLE_QEMPTY_GROUPS);
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_fault_snapshot
 *
 * Description:
 *   The pair of snapshots above under one stage label, so a caller brackets a
 *   step of the active path with a single call on each side of it.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_fault_snapshot(FAR const char *stage,
                                                bool deep)
{
  k1_rtl8852bs_runtime_dle_snapshot_log(stage);
  k1_rtl8852bs_runtime_err_isr_log(stage);
  k1_rtl8852bs_runtime_dle_queue_sweep(stage);

  /* The debug port sweep is several hundred register reads, so it is taken at
   * the two stages that bracket everything between them: the state before the
   * active path starts and the state it leaves behind.  A stage in the middle
   * of that path is described by the queue sweep alone, which is cheap enough
   * not to move the timing it measures.
   */

  if (deep)
    {
      k1_rtl8852bs_runtime_dbg_port_sweep(stage);
    }
}

static int k1_rtl8852bs_runtime_scanofld_passive_wait(
  FAR uint8_t *firmware_return, bool require_bss,
  bool require_probe_response,
  FAR struct k1_rtl8852bs_scan_result_s *result)
{
  FAR struct k1_rtl8852bs_scanofld_passive_match_s *match;
  struct k1_rtl8852bs_rx_frame_s frame;
  FAR uint8_t *buffer;
  size_t length;
  size_t offset;
  unsigned int attempt;
  unsigned int drain_polls = 0;
  unsigned int idle_polls = 0;
  bool complete = false;
  int complete_ret = OK;
  int ret;

  if (firmware_return == NULL)
    {
      return -EINVAL;
    }

  buffer = kmm_malloc(K1_RTL8852BS_SCAN_OFLD_RX_MAX);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  /* The BSS table makes this structure too large for the bring-up stack. */

  match = kmm_zalloc(sizeof(*match));
  if (match == NULL)
    {
      kmm_free(buffer);
      return -ENOMEM;
    }

  /* The transmit watch runs over both sweeps, so it is armed here rather than
   * in the active diagnostic: the passive sweep, which transmits nothing and
   * is known to work, is what gives the active sweep's numbers a baseline.
   */

  k1_rtl8852bs_runtime_tx_witness_reset();
  k1_rtl8852bs_runtime_tx_witness_deep();

  match->done_ack.category = K1_RTL8852BS_SCAN_OFLD_CATEGORY;
  match->done_ack.class_id = K1_RTL8852BS_SCAN_OFLD_CLASS;
  match->done_ack.function = K1_RTL8852BS_SCAN_OFLD_START_FUNCTION;
  match->done_ack.sequence = K1_RTL8852BS_SCAN_OFLD_START_H2C_SEQUENCE;

  k1_sdio_wifi_suppress_command_trace(true);

  /* Each dwell now runs inside this loop instead of blocking the handler, so
   * the budget has to cover the whole channel list plus the drain.
   */

  for (attempt = 0; attempt < K1_RTL8852BS_SCAN_OFLD_PASSIVE_POLL_COUNT;
       attempt++)
    {
      k1_rtl8852bs_runtime_tx_witness_sample(attempt);

      ret = k1_rtl8852bs_scanofld_dwell_poll(match);
      if (ret < 0)
        {
          goto out;
        }

      ret = k1_rtl8852bs_runtime_rx_read(
        buffer, K1_RTL8852BS_SCAN_OFLD_RX_MAX, &length);
      if (ret == -EAGAIN)
        {
          if (complete &&
              ++idle_polls >= K1_RTL8852BS_SCAN_OFLD_DRAIN_IDLE_POLLS)
            {
              ret = complete_ret;
              goto out;
            }

          up_mdelay(K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC);
          continue;
        }

      if (ret == -ENOSPC)
        {
          /* The pending aggregate is larger than this path's buffer.  Count
           * it and keep polling instead of aborting: the FIFO keeps the
           * transfer, so any loss stays visible in the counters.
           */

          match->rx_oversize++;
          if (length > match->rx_oversize_max)
            {
              match->rx_oversize_max = (uint32_t)length;
            }

          up_mdelay(K1_RTL8852BS_RUNTIME_DONE_ACK_POLL_MSEC);
          continue;
        }

      if (ret < 0)
        {
          goto out;
        }

      idle_polls = 0;
      match->rx_reads++;
      offset = 0;
      while (offset < length)
        {
          ret = k1_rtl8852bs_runtime_rx_parse(buffer, length, offset,
                                               &frame);
          if (ret < 0 || frame.next_offset <= offset)
            {
              ret = ret < 0 ? ret : -EPROTO;
              goto out;
            }

          match->rx_frames_total++;
          match->rx_types[frame.packet_type & 0xfu]++;
          if (frame.crc_error)
            {
              match->rx_crc_errors++;
            }

          if (frame.icv_error)
            {
              match->rx_icv_errors++;
            }

          /* The early console is polled, so a full dump of every frame would
           * cost more milliseconds than the dwell has.  Dump one air frame per
           * dwell for decode evidence and keep only counters afterwards; the
           * C2H and PPDU-status descriptors are already covered by the scan
           * C2H trace and the per-type counters.
           */

          if (frame.packet_type == 0 &&
              match->rx_frames_logged < K1_RTL8852BS_SCAN_OFLD_FRAME_LOG_MAX)
            {
              match->rx_frames_logged++;
              k1_rtl8852bs_scanofld_log_frame(buffer, length, offset,
                                              &frame);
            }
          else
            {
              match->rx_frames_suppressed++;
            }

          if (!frame.crc_error && !frame.icv_error &&
              frame.packet_type == K1_RTL8852BS_H2C_LOOPBACK_RXD_C2H_TYPE)
            {
              ret = k1_rtl8852bs_runtime_c2h_dispatch(
                buffer + frame.payload_offset, frame.payload_length,
                k1_rtl8852bs_runtime_scanofld_passive_match, match);
              if (ret < 0)
                {
                  goto out;
                }

              if (!complete && match->done_ack.matched &&
                  match->entered_channels ==
                    (1u << K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT) - 1u &&
                  match->advanced_channels ==
                    (1u << K1_RTL8852BS_SCAN_OFLD_PASSIVE_CHANNEL_COUNT) - 1u &&
                  match->saw_scan_end)
                {
                  /* Firmware is done with the channel list.  Frames received
                   * during the last dwell can still be queued, so record the
                   * verdict and keep draining until the FIFO goes quiet.
                   */

                  *firmware_return = match->done_ack.firmware_return;
                  complete_ret =
                    match->done_ack.firmware_return == 0 ? OK : -EIO;
                  complete = true;
                }
            }
          else if (!frame.crc_error && !frame.icv_error &&
                   frame.packet_type == 0)
            {
              k1_rtl8852bs_scanofld_observe_wifi(
                buffer + frame.payload_offset, frame.payload_length, match);
            }

          offset = frame.next_offset;
        }

      if (complete && ++drain_polls >= K1_RTL8852BS_SCAN_OFLD_DRAIN_POLL_COUNT)
        {
          ret = complete_ret;
          goto out;
        }
    }

  ret = -ETIMEDOUT;

out:
  k1_sdio_wifi_suppress_command_trace(false);

  k1_rtl8852bs_runtime_tx_witness_deep();
  k1_rtl8852bs_runtime_tx_witness_log(require_probe_response ?
                                      "active" : "passive");

  /* A Beacon may still arrive during the post-scan drain, so the BSS
   * requirement is judged once, after the loop has finished.
   */

  if (ret == OK && require_bss && !match->first_bss_valid)
    {
      ret = -ENODATA;
    }

  /* A Probe Response is the only frame in this sweep that cannot exist
   * unless the Probe Request this host offloaded was actually radiated, so
   * it, and not a firmware acknowledgement, is what an active sweep is
   * required to observe.  Beacons are deliberately not accepted here: they
   * arrive on a passive sweep too and would prove nothing about transmit.
   *
   * The response also has to be addressed to this host.  A sweep runs with
   * sniffer mode set and unicast CAM matching cleared, so a Probe Response an
   * access point sent to some other station is delivered here as well;
   * counting one of those would report a transmit that never happened.  The
   * comparison needs the self MAC to have been recorded, so a sweep that asks
   * for a Probe Response without one is an error rather than a pass.
   */

  if (ret == OK && require_probe_response &&
      (!g_k1_rtl8852bs_scan_self_mac_valid ||
       match->probe_response_to_self == 0))
    {
      ret = -ENODATA;
    }

  /* Every path that reaches here has the sweep state, so one export before
   * the release covers the timeout and error cases too: a caller that gets
   * an error can still see what the sweep did observe.
   */

  if (result != NULL)
    {
      k1_rtl8852bs_runtime_scanofld_export_result(match, result);
    }

  k1_rtl8852bs_scanofld_log_rx(match);
  k1_rtl8852bs_scanofld_log_bss_table(match);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: passive scan-offload wait error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts(" C2H=");
      k1_early_puthex(match->done_ack.c2h_frames);
      k1_early_puts(" done-ack=");
      k1_early_puthex(match->done_ack.generic_done_ack_frames);
      k1_early_puts(" scan-events=");
      k1_early_puthex(match->scan_events);
      k1_early_puts(" enter-mask=");
      k1_early_puthex(match->entered_channels);
      k1_early_puts(" next=");
      k1_early_puthex(match->advanced_channels);
      k1_early_puts(" end=");
      k1_early_puthex(match->saw_scan_end ? 1 : 0);
      k1_early_puts(" last=ch");
      k1_early_puthex(match->last_channel);
      k1_early_puts(" reason=");
      k1_early_puthex(match->last_reason);
      k1_early_puts(" status=");
      k1_early_puthex(match->last_status);
      k1_early_puts(" band=");
      k1_early_puthex(match->last_channel_band);
      k1_early_puts("\r\n");
    }

  kmm_free(match);
  kmm_free(buffer);
  return ret;
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic
 *
 * Description:
 *   Start the already accepted passive 1-13 channel-list and require the
 *   corresponding H2C done acknowledgement, each channel-enter event and
 *   scan-end event.  This is a one-shot firmware offload diagnostic only:
 *   it transmits no probe, associates with no AP, creates no network device,
 *   and leaves no state after reset.
 ****************************************************************************/

static int k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common(
  bool require_bss, bool require_probe_response,
  FAR struct k1_rtl8852bs_scan_result_s *result)
{
  struct k1_rtl8852bs_scan_rx_filter_state_s scan_filter;
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_PHY_COUNTER_DIAGNOSTIC
  struct k1_rtl8852bs_scan_phy_counters_s phy_counters;
  struct k1_rtl8852bs_scan_phy_counters_s phy_baseline;
  bool phy_baseline_valid = false;
  int phy_ret;
#endif
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC
  struct k1_rtl8852bs_scan_rf_readback_s rf_readback;
  int rf_readback_ret;
#endif
  uint8_t content[K1_RTL8852BS_SCAN_OFLD_START_CONTENT_SIZE];
  uint8_t firmware_return = 0;
  uint16_t available_pages;
  uint32_t fifo_address;
  int restore_ret;
  int ret;

  ret = k1_rtl8852bs_runtime_scanofld_passive_start_build(content,
                                                           sizeof(content));
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_scan_rx_filter_enable(&scan_filter);
  if (ret < 0)
    {
      goto error;
    }

#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_PHY_COUNTER_DIAGNOSTIC

  /* Baseline the RMAC receive counters with the scan filter already in
   * place, so the difference reported after the scan belongs to the dwell
   * on channels 1, 6 and 11 alone.  A failed sample is reported and then
   * ignored: it must not change the result of the scan itself.
   */

  phy_ret = k1_rtl8852bs_scan_phy_counters_read(&phy_baseline);
  if (phy_ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan PHY counters error=");
      k1_early_puthex((uintreg_t)-phy_ret);
      k1_early_puts("\r\n");
    }
  else
    {
      phy_baseline_valid = true;
      k1_rtl8852bs_scan_phy_counters_log("before", &phy_baseline, NULL);
    }

#endif
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC

  /* Read the radio state once before the scan for comparison.  Like the
   * counters this is reported and then ignored on failure: a read-only RF
   * sample must not change the result of the scan.
   */

  rf_readback_ret = k1_rtl8852bs_scan_rf_readback_read(&rf_readback);
  if (rf_readback_ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF readback error=");
      k1_early_puthex((uintreg_t)-rf_readback_ret);
      k1_early_puts("\r\n");
    }
  else
    {
      k1_rtl8852bs_scan_rf_readback_log("pre-si-reset", &rf_readback);
    }

  /* Run the serial interface reset of halrf_dm_init() between the two
   * samples.  It is the vendor workaround for a radio that reads back as
   * zero, so the pair of samples measures it directly.  A failure is
   * reported and then ignored, like the samples themselves: it must not
   * change the result of the scan.
   */

  rf_readback_ret = k1_rtl8852bs_rf_si_reset();
  if (rf_readback_ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF si-reset error=");
      k1_early_puthex((uintreg_t)-rf_readback_ret);
      k1_early_puts("\r\n");
    }

  rf_readback_ret = k1_rtl8852bs_scan_rf_readback_read(&rf_readback);
  if (rf_readback_ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF readback error=");
      k1_early_puthex((uintreg_t)-rf_readback_ret);
      k1_early_puts("\r\n");
    }
  else
    {
      k1_rtl8852bs_scan_rf_readback_log("before", &rf_readback);
    }

#endif

  ret = k1_rtl8852bs_runtime_control_h2c_submit(
    content, sizeof(content), K1_RTL8852BS_SCAN_OFLD_CATEGORY,
    K1_RTL8852BS_SCAN_OFLD_CLASS, K1_RTL8852BS_SCAN_OFLD_START_FUNCTION,
    K1_RTL8852BS_SCAN_OFLD_START_H2C_SEQUENCE, true, &fifo_address,
    &available_pages);
  if (ret < 0)
    {
      goto out;
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan-offload H2C queued ");
  k1_early_puts("channels=1-13 sequence=6 pages=");
  k1_early_puthex(available_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_scanofld_passive_wait(&firmware_return,
                                                   require_bss,
                                                   require_probe_response,
                                                   result);
  if (ret < 0)
    {
      goto out;
    }

out:
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_PHY_COUNTER_DIAGNOSTIC

  /* Sample the counters again while the scan filter is still applied.  The
   * differences say whether the baseband handed RMAC any PPDU at all during
   * the three dwells, which is the question a scan that ends with no BSS
   * cannot answer on its own.
   */

  phy_ret = k1_rtl8852bs_scan_phy_counters_read(&phy_counters);
  if (phy_ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan PHY counters error=");
      k1_early_puthex((uintreg_t)-phy_ret);
      k1_early_puts("\r\n");
    }
  else
    {
      k1_rtl8852bs_scan_phy_counters_log(
        "after", &phy_counters,
        phy_baseline_valid ? &phy_baseline : NULL);
    }

#endif
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC

  /* The radio keeps the last channel the firmware tuned for the dwells, so
   * this sample is the one that says whether the scan reached the radio.
   */

  rf_readback_ret = k1_rtl8852bs_scan_rf_readback_read(&rf_readback);
  if (rf_readback_ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: scan RF readback error=");
      k1_early_puthex((uintreg_t)-rf_readback_ret);
      k1_early_puts("\r\n");
    }
  else
    {
      k1_rtl8852bs_scan_rf_readback_log("after", &rf_readback);
#ifdef CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC
      k1_rtl8852bs_scan_rf_compare_probe_all(&rf_readback);
      k1_rtl8852bs_scan_rf_access_probe_all();
#endif
    }

#endif

  restore_ret = k1_rtl8852bs_scan_rx_filter_restore(&scan_filter);
  if (ret == OK && restore_ret < 0)
    {
      ret = restore_ret;
    }

  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: passive scan-offload done-ack return=");
  k1_early_puthex(firmware_return);
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 passive scan-offload "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: passive scan-offload error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts(" firmware-return=");
  k1_early_puthex(firmware_return);
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common(
    false, false, NULL);
}

int k1_rtl8852bs_fwdl_runtime_scanofld_passive_rx_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common(
    true, false, NULL);
}

/****************************************************************************
 * Name: k1_rtl8852bs_tx_ppdu_counter_read
 *
 * Description:
 *   Read one TMAC transmit PPDU counter.  This is the transmit twin of
 *   k1_rtl8852bs_rx_counter_read() and follows the same selection rule: keep
 *   the read index byte, clear the reset trigger so a read never restarts the
 *   counters, and overwrite the read-only count with zero.
 *
 * Input Parameters:
 *   index - A vendor tx_cnt_type_g6() index: 0 LCCK, 1 SCCK, 2 OFDM, 3 HT,
 *           4 HT-GF, 5 VHT-SU, 6 VHT-MU, 7 HE-SU, 8 HE-ER-SU, 9 HE-MU and
 *           10 HE-TB.
 *   value - Receives the 16 bit count.
 *   raw   - Optional, receives the selection word as it reads back.  A count
 *           of zero only means "nothing was transmitted" when the index this
 *           word carries is the index that was asked for, so the caller needs
 *           the raw word to tell a flat counter from a refused selection.
 *
 * Returned Value:
 *   OK or a negated errno.
 *
 ****************************************************************************/

static int k1_rtl8852bs_tx_ppdu_counter_read(uint8_t index,
                                            FAR uint16_t *value,
                                            FAR uint32_t *raw)
{
  uint32_t select = 0;
  int ret;

  if (value == NULL)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_TX_PPDU_CNT_SEL, &select);
  if (ret < 0)
    {
      return ret;
    }

  select &= ~K1_RTL8852BS_TX_PPDU_CNT_RESET;
  select &= ~K1_RTL8852BS_TX_PPDU_CNT_INDEX_MASK;
  select |= (uint32_t)index & K1_RTL8852BS_TX_PPDU_CNT_INDEX_MASK;

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_TX_PPDU_CNT_SEL, select);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(K1_RTL8852BS_TX_PPDU_CNT_SETTLE_US);

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_TX_PPDU_CNT_SEL, &select);
  if (ret < 0)
    {
      return ret;
    }

  *value = (uint16_t)((select >> K1_RTL8852BS_TX_PPDU_CNT_VALUE_SHIFT) &
                      K1_RTL8852BS_TX_PPDU_CNT_VALUE_MASK);
  if (raw != NULL)
    {
      *raw = select;
    }

  return OK;
}

/* One transmit side snapshot: the eleven transmit PPDU counters followed by
 * the registers mac_tx_status_dump() reads when a frame does not reach the
 * air.  The selection word of one counter and the number of counters whose
 * selection did not read back are kept as well, so a table of zeroes can be
 * read as evidence instead of being taken on trust.
 */

struct k1_rtl8852bs_tx_state_s
{
  uint16_t ppdu[K1_RTL8852BS_TX_PPDU_TYPES];
  uint32_t ppdu_sel;
  uint32_t ppdu_sel_bad;
  uint32_t mactx_cnt;
  uint32_t ctn_txen;
  uint32_t ptcl_common;
  uint32_t macid_sleep;
  uint32_t macid_pause;
  uint32_t cmac_drop;
  uint32_t dmac_drop;
  uint32_t loopback;
  uint32_t cca_abort;
};

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_tx_state_sample
 *
 * Description:
 *   Take one transmit side snapshot.  Every access is a read except the
 *   counter index selection, so the chip keeps the state it had.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_tx_state_sample(
  FAR struct k1_rtl8852bs_tx_state_s *state)
{
  static const uint32_t addresses[] =
  {
    K1_RTL8852BS_CTN_TXEN, K1_RTL8852BS_PTCL_COMMON_SETTING_0,
    K1_RTL8852BS_MACID_SLEEP_0, K1_RTL8852BS_SS_MACID_PAUSE_0,
    K1_RTL8852BS_CMAC_MACID_DROP_0, K1_RTL8852BS_DMAC_MACID_DROP_0,
    K1_RTL8852BS_MAC_LOOPBACK_STATE, K1_RTL8852BS_RESP_TX_CCA_ABORT_CNT,
    K1_RTL8852BS_MACTX_DBG_SEL_CNT
  };

  FAR uint32_t *words[sizeof(addresses) / sizeof(addresses[0])];
  unsigned int index;
  int ret;

  if (state == NULL)
    {
      return -EINVAL;
    }

  memset(state, 0, sizeof(*state));

  words[0] = &state->ctn_txen;
  words[1] = &state->ptcl_common;
  words[2] = &state->macid_sleep;
  words[3] = &state->macid_pause;
  words[4] = &state->cmac_drop;
  words[5] = &state->dmac_drop;
  words[6] = &state->loopback;
  words[7] = &state->cca_abort;
  words[8] = &state->mactx_cnt;

  for (index = 0; index < K1_RTL8852BS_TX_PPDU_TYPES; index++)
    {
      uint32_t raw = 0;

      ret = k1_rtl8852bs_tx_ppdu_counter_read((uint8_t)index,
                                              &state->ppdu[index], &raw);
      if (ret < 0)
        {
          return ret;
        }

      if ((raw & K1_RTL8852BS_TX_PPDU_CNT_INDEX_MASK) != index)
        {
          state->ppdu_sel_bad++;
        }

      if (index == K1_RTL8852BS_TX_PPDU_CNT_INDEX_OFDM)
        {
          state->ppdu_sel = raw;
        }
    }

  for (index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++)
    {
      ret = k1_rtl8852bs_mac_read32(addresses[index], words[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_tx_state_log
 *
 * Description:
 *   Print one transmit side snapshot.  The counters are printed with their
 *   differences against a baseline when one is supplied, because they are 16
 *   bit, wrap, and are not reset by this component.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_tx_state_log(
  FAR const char *phase,
  FAR const struct k1_rtl8852bs_tx_state_s *now,
  FAR const struct k1_rtl8852bs_tx_state_s *base)
{
  static const char *names[K1_RTL8852BS_TX_PPDU_TYPES] =
  {
    "lcck", "scck", "ofdm", "ht", "ht-gf", "vhtsu", "vhtmu", "hesu",
    "heersu", "hemu", "hetb"
  };

  unsigned int index;

  k1_early_puts("K1 Wi-Fi GPL: active scan TX PPDU ");
  k1_early_puts(phase);
  for (index = 0; index < K1_RTL8852BS_TX_PPDU_TYPES; index++)
    {
      k1_early_puts(" ");
      k1_early_puts(names[index]);
      k1_early_puts("=");
      k1_early_puthex(now->ppdu[index]);
    }

  if (base != NULL)
    {
      for (index = 0; index < K1_RTL8852BS_TX_PPDU_TYPES; index++)
        {
          k1_early_puts(" delta-");
          k1_early_puts(names[index]);
          k1_early_puts("=");
          k1_early_puthex((uint32_t)(uint16_t)(now->ppdu[index] -
                                              base->ppdu[index]));
        }
    }

  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: active scan TX state ");
  k1_early_puts(phase);
  k1_early_puts(" ppdu-sel=");
  k1_early_puthex(now->ppdu_sel);
  k1_early_puts(" ppdu-sel-bad=");
  k1_early_puthex(now->ppdu_sel_bad);
  k1_early_puts(" mactx=");
  k1_early_puthex(now->mactx_cnt);
  k1_early_puts(" mactx-mpdu=");
  k1_early_puthex((now->mactx_cnt >> K1_RTL8852BS_MACTX_MPDU_CNT_SHIFT) &
                  K1_RTL8852BS_MACTX_CNT_MASK);
  k1_early_puts(" mactx-dma=");
  k1_early_puthex((now->mactx_cnt >> K1_RTL8852BS_MACTX_DMA_CNT_SHIFT) &
                  K1_RTL8852BS_MACTX_CNT_MASK);
  if (base != NULL)
    {
      k1_early_puts(" delta-mactx-mpdu=");
      k1_early_puthex((uint32_t)(uint8_t)
        (((now->mactx_cnt >> K1_RTL8852BS_MACTX_MPDU_CNT_SHIFT) -
          (base->mactx_cnt >> K1_RTL8852BS_MACTX_MPDU_CNT_SHIFT)) &
         K1_RTL8852BS_MACTX_CNT_MASK));
      k1_early_puts(" delta-mactx-dma=");
      k1_early_puthex((uint32_t)(uint8_t)
        (((now->mactx_cnt >> K1_RTL8852BS_MACTX_DMA_CNT_SHIFT) -
          (base->mactx_cnt >> K1_RTL8852BS_MACTX_DMA_CNT_SHIFT)) &
         K1_RTL8852BS_MACTX_CNT_MASK));
    }

  k1_early_puts(" ctn-txen=");
  k1_early_puthex(now->ctn_txen);
  k1_early_puts(" ptcl-common=");
  k1_early_puthex(now->ptcl_common);
  k1_early_puts(" macid-sleep=");
  k1_early_puthex(now->macid_sleep);
  k1_early_puts(" macid-pause=");
  k1_early_puthex(now->macid_pause);
  k1_early_puts(" cmac-drop=");
  k1_early_puthex(now->cmac_drop);
  k1_early_puts(" dmac-drop=");
  k1_early_puthex(now->dmac_drop);
  k1_early_puts(" loopback=");
  k1_early_puthex(now->loopback);
  k1_early_puts(" cca-abort=");
  k1_early_puthex(now->cca_abort);
  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_sch_tx_en_management
 *
 * Description:
 *   Enable contention transmit for the two management queues in the CMAC
 *   scheduler.
 *
 *   The static dmac_init()/cmac_init() subset this component programs leaves
 *   the scheduler untouched by design, so R_AX_CTN_TXEN still holds whatever
 *   reset left in it, and a queue whose bit is clear there is never served no
 *   matter who filled it.  CMAC itself is not the gate: R_AX_CMAC_FUNC_EN is
 *   already programmed with CMAC_TXEN, SCHEDULER_EN, TMAC_EN and PTCLTOP_EN,
 *   which is why the receive side works.  The original driver reaches its
 *   running state through rtw_hal_tx_pause(hal, band, false, reason), which
 *   collapses to rtw_hal_mac_set_sch_tx_en(band, 0xffff, 0xffff) once no
 *   reason holds a pause, that is every queue enabled.
 *
 *   Only the two management queues are enabled here.  MGQ is where a host
 *   built management frame would go, and CPUMGQ is the queue the firmware
 *   transmits the frames it builds itself from, which is what an offloaded
 *   scan Probe Request is.  The data queues are deliberately left as they
 *   are: this component has no data path, and the original pauses everything
 *   except a management queue for the duration of a scan anyway.
 *
 *   The value read before the write is printed, so the log records what reset
 *   had left behind and whether the write took effect.
 *
 * Returned Value:
 *   OK when both bits read back set, a negated errno otherwise.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_sch_tx_en_management(void)
{
  const uint32_t wanted = K1_RTL8852BS_CTN_TXEN_MGQ |
                          K1_RTL8852BS_CTN_TXEN_CPUMGQ;
  uint32_t before = 0;
  uint32_t after = 0;
  int ret;

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_CTN_TXEN, &before);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_CTN_TXEN, before | wanted);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_CTN_TXEN, &after);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: active scan scheduler TX enable before=");
  k1_early_puthex(before);
  k1_early_puts(" after=");
  k1_early_puthex(after);
  k1_early_puts(" wanted=");
  k1_early_puthex(wanted);
  k1_early_puts("\r\n");

  if ((after & wanted) != wanted)
    {
      return -EIO;
    }

  return OK;
}


/****************************************************************************
 * Name: k1_rtl8852bs_lte_wait
 *
 * Description:
 *   Wait until the coexistence indirect register window reports itself ready.
 *   The two grant configuration words are not part of the ordinary register
 *   space: they live behind a command register, and a command may only be
 *   issued while that register reports ready.  The original polls byte 3 of
 *   the command register every 50 us for 50 ms; this component has 32 bit
 *   access only and polls the same bit in the word.
 *
 * Returned Value:
 *   OK once the window is ready, -ETIMEDOUT when it never becomes ready, or
 *   the bus error that prevented the poll.
 *
 ****************************************************************************/

static int k1_rtl8852bs_lte_wait(void)
{
  unsigned int attempt;

  for (attempt = 0; attempt < K1_RTL8852BS_LTE_POLL_COUNT; attempt++)
    {
      uint32_t value = 0;
      int ret;

      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_LTE_CTRL, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & K1_RTL8852BS_LTE_READY) != 0)
        {
          return OK;
        }

      up_udelay(K1_RTL8852BS_LTE_POLL_USEC);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: k1_rtl8852bs_lte_read32 / k1_rtl8852bs_lte_write32
 *
 * Description:
 *   Read and write one coexistence indirect word.  The command register takes
 *   a fixed byte enable of all four bytes together with the word offset, and
 *   the data travels through a separate write and read data register, exactly
 *   as rtw89_mac_read_lte() and rtw89_mac_write_lte() do it.  The original
 *   reads the data register immediately after issuing the command without
 *   waiting again, so the indirect access completes within the bus access
 *   itself; this port keeps that behaviour rather than inventing a handshake.
 *
 ****************************************************************************/

static int k1_rtl8852bs_lte_read32(uint32_t offset, FAR uint32_t *value)
{
  int ret;

  ret = k1_rtl8852bs_lte_wait();
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_LTE_CTRL,
                                 K1_RTL8852BS_LTE_READ_COMMAND | offset);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_mac_read32(K1_RTL8852BS_LTE_RDATA, value);
}

static int k1_rtl8852bs_lte_write32(uint32_t offset, uint32_t value)
{
  int ret;

  ret = k1_rtl8852bs_lte_wait();
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_LTE_WDATA, value);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_mac_write32(K1_RTL8852BS_LTE_CTRL,
                                  K1_RTL8852BS_LTE_WRITE_COMMAND | offset);
}

/* The arbitration state this component reads before and after programming it.
 * Every word is reported on both sides of the change.  Three of them carry the
 * question this block exists to answer - whether the Bluetooth side of a
 * combined part is holding the medium away from the Wi-Fi side - and the rest
 * are the context needed to read those three: the channel assessment enables,
 * the medium reservation cap, and which of the two grant paths is selected.
 */

struct k1_rtl8852bs_coex_state_s
{
  uint32_t muxcfg;
  uint32_t btc_func_en;
  uint32_t coex_cfg_2;
  uint32_t csr_mode;
  uint32_t tdma_mode;
  uint32_t coex_cfg_5;
  uint32_t cca_cfg_0;
  uint32_t cca_control;
  uint32_t nav_ctl;
  uint32_t trxptcl_resp0;
  uint32_t sdio_ctrl;
  uint32_t gnt;
  uint32_t rx_ctrl;
  int lte_status;
};

static int k1_rtl8852bs_runtime_coex_sample(
  FAR struct k1_rtl8852bs_coex_state_s *state)
{
  static const uint32_t addresses[] =
  {
    K1_RTL8852BS_GPIO_MUXCFG, K1_RTL8852BS_BTC_FUNC_EN,
    K1_RTL8852BS_BT_COEX_CFG_2, K1_RTL8852BS_CSR_MODE,
    K1_RTL8852BS_TDMA_MODE, K1_RTL8852BS_BT_COEX_CFG_5,
    K1_RTL8852BS_CCA_CFG_0, K1_RTL8852BS_CCA_CONTROL,
    K1_RTL8852BS_WMAC_NAV_CTL, K1_RTL8852BS_TRXPTCL_RESP0,
    K1_RTL8852BS_SYS_SDIO_CTRL
  };

  FAR uint32_t *words[sizeof(addresses) / sizeof(addresses[0])];
  unsigned int index;
  int ret;

  if (state == NULL)
    {
      return -EINVAL;
    }

  memset(state, 0, sizeof(*state));

  words[0] = &state->muxcfg;
  words[1] = &state->btc_func_en;
  words[2] = &state->coex_cfg_2;
  words[3] = &state->csr_mode;
  words[4] = &state->tdma_mode;
  words[5] = &state->coex_cfg_5;
  words[6] = &state->cca_cfg_0;
  words[7] = &state->cca_control;
  words[8] = &state->nav_ctl;
  words[9] = &state->trxptcl_resp0;
  words[10] = &state->sdio_ctrl;

  for (index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++)
    {
      ret = k1_rtl8852bs_mac_read32(addresses[index], words[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* The two indirect words are reported best effort.  Their window is a
   * separate block and a component that cannot reach it still has to be able
   * to report the ordinary registers, which are the ones the transmit enable
   * lives in.
   */

  state->lte_status = k1_rtl8852bs_lte_read32(K1_RTL8852BS_LTE_SW_CFG_1,
                                              &state->gnt);
  if (state->lte_status == OK)
    {
      state->lte_status = k1_rtl8852bs_lte_read32(K1_RTL8852BS_LTE_SW_CFG_2,
                                                  &state->rx_ctrl);
    }

  return OK;
}

static void k1_rtl8852bs_runtime_coex_log(
  FAR const char *stage, FAR const struct k1_rtl8852bs_coex_state_s *state)
{
  k1_early_puts("K1 Wi-Fi GPL: coex ");
  k1_early_puts(stage);
  k1_early_puts(" muxcfg=");
  k1_early_puthex(state->muxcfg);
  k1_early_puts(" btc-func=");
  k1_early_puthex(state->btc_func_en);
  k1_early_puts(" wl-tx-en=");
  k1_early_puthex((state->btc_func_en &
                   K1_RTL8852BS_PTA_WL_TX_EN) != 0 ? 1 : 0);
  k1_early_puts(" coex2=");
  k1_early_puthex(state->coex_cfg_2);
  k1_early_puts(" csr=");
  k1_early_puthex(state->csr_mode);
  k1_early_puts(" tdma=");
  k1_early_puthex(state->tdma_mode);
  k1_early_puts(" coex5=");
  k1_early_puthex(state->coex_cfg_5);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: coex ");
  k1_early_puts(stage);
  k1_early_puts(" cca=");
  k1_early_puthex(state->cca_cfg_0);
  k1_early_puts(" btcca-en=");
  k1_early_puthex((state->cca_cfg_0 &
                   K1_RTL8852BS_BTCCA_EN) != 0 ? 1 : 0);
  k1_early_puts(" cca-control=");
  k1_early_puthex(state->cca_control);
  k1_early_puts(" nav=");
  k1_early_puthex(state->nav_ctl);
  k1_early_puts(" resp0=");
  k1_early_puthex(state->trxptcl_resp0);
  k1_early_puts(" sdio-ctrl=");
  k1_early_puthex(state->sdio_ctrl);
  k1_early_puts(" wl-ctrl-path=");
  k1_early_puthex((state->sdio_ctrl &
                   K1_RTL8852BS_LTE_MUX_CTRL_PATH) != 0 ? 1 : 0);
  k1_early_puts(" gnt=");
  k1_early_puthex(state->gnt);
  k1_early_puts(" rx-ctrl=");
  k1_early_puthex(state->rx_ctrl);
  k1_early_puts(" lte-status=");
  k1_early_puthex((uintreg_t)(state->lte_status < 0 ?
                              -state->lte_status : 0));
  k1_early_puts("\r\n");
}

/* What a Wi-Fi only image has to say to the arbitration block.  The first
 * board run programmed the block the way rtw89_mac_coex_init() does, which is
 * the sequence for a part whose Bluetooth core is running, and it changed
 * nothing: the firmware still reported thirteen transmits with no failures and
 * no access point answered.  Reading that run back showed why the sequence was
 * the wrong one to copy.
 *
 * R_AX_CCA_CFG_0 came out of reset with BTCCA_EN set, and it was still set
 * after coex_init, because coex_init sets it: it is telling the Wi-Fi core to
 * treat the Bluetooth core's channel assessment as the medium being busy.
 * mainline's own scheduler_init_ax() clears that bit, and it runs for every
 * image, coex or not.  With no Bluetooth core driving the signal, a Wi-Fi core
 * that honours it can see the medium as permanently occupied - which stops
 * transmits, leaves receive untouched, and costs the firmware's scan state
 * machine nothing to report as success.  That is the shape of this failure.
 *
 * The grant words were the second half of the mistake.  They were written with
 * the Wi-Fi grant asserted and the Bluetooth grant withdrawn, and the readback
 * confirmed the write, but R_AX_SYS_SDIO_CTRL showed the path multiplexer
 * still selecting the hardware arbiter, so the values were stored and ignored.
 *
 * So this table is the opposite of the first one: nothing enables the arbiter,
 * no arbitration mode is chosen, no Bluetooth statistics are started.  Two
 * bits stop the Bluetooth side from holding the medium, one bit hands the
 * grant to software, and the grant itself follows below.
 */

struct k1_rtl8852bs_coex_field_s
{
  uint32_t address;
  uint32_t mask;
  uint32_t value;
};

static const struct k1_rtl8852bs_coex_field_s g_k1_rtl8852bs_coex_fields[] =
{
  {K1_RTL8852BS_CCA_CFG_0,
   K1_RTL8852BS_BTCCA_EN | K1_RTL8852BS_BTCCA_BRK_TXOP_EN, 0},
  {K1_RTL8852BS_TRXPTCL_RESP0, K1_RTL8852BS_RSP_CHK_BTCCA, 0},
  {K1_RTL8852BS_SYS_SDIO_CTRL, K1_RTL8852BS_LTE_MUX_CTRL_PATH,
   K1_RTL8852BS_LTE_MUX_CTRL_PATH}
};

static int k1_rtl8852bs_runtime_coex_wl_only(void)
{
  uint32_t rx_ctrl = 0;
  unsigned int index;
  int ret;

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_coex_fields) /
               sizeof(g_k1_rtl8852bs_coex_fields[0]);
       index++)
    {
      ret = k1_rtl8852bs_mac_update_field_checked(
        g_k1_rtl8852bs_coex_fields[index].address,
        g_k1_rtl8852bs_coex_fields[index].mask,
        g_k1_rtl8852bs_coex_fields[index].value);
      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: coex field ");
          k1_early_puthex((uintreg_t)index);
          k1_early_puts(" address=");
          k1_early_puthex(g_k1_rtl8852bs_coex_fields[index].address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts("\r\n");
          return ret;
        }
    }

  /* The grant words live behind the indirect window, and the multiplexer bit
   * set above is what makes them mean anything.  Software control with the
   * Wi-Fi grant asserted and the Bluetooth grant withdrawn is the state a
   * Wi-Fi only image wants from a shared front end, and it is the same state
   * the original writes through its cfg_gnt operation.  The receive control
   * bit is the only bit the original keeps in the second word.
   */

  ret = k1_rtl8852bs_lte_write32(K1_RTL8852BS_LTE_SW_CFG_1,
                                 K1_RTL8852BS_GNT_WL_SW_BAND0);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_lte_read32(K1_RTL8852BS_LTE_SW_CFG_2, &rx_ctrl);
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_lte_write32(K1_RTL8852BS_LTE_SW_CFG_2,
                                  rx_ctrl & K1_RTL8852BS_LTE_WL_RX_CTRL);
}

/****************************************************************************
 * Name: k1_rtl8852bs_bb_update_field_checked
 *
 * Description:
 *   The baseband counterpart of k1_rtl8852bs_mac_update_field_checked(): read,
 *   replace one field, write, and read back.  Baseband register numbers are
 *   not bus addresses, so every access goes through the baseband helpers.
 *
 ****************************************************************************/

static int k1_rtl8852bs_bb_update_field_checked(uint32_t address,
                                                 uint32_t mask,
                                                 uint32_t value)
{
  uint32_t actual = 0;
  uint32_t updated = 0;
  int ret;

  if ((value & ~mask) != 0)
    {
      return -EINVAL;
    }

  ret = k1_rtl8852bs_bb_read32(address, &updated);
  if (ret < 0)
    {
      return ret;
    }

  updated &= ~mask;
  updated |= value;
  ret = k1_rtl8852bs_bb_write32(address, updated);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_bb_read32(address, &actual);
  if (ret < 0)
    {
      return ret;
    }

  return (actual & mask) == value ? OK : -EIO;
}

/* The transmit power window.  The original never leaves this to the register
 * image: hal_start_8852b() ends in rtw_hal_rf_dm_init(), and rtw_hal_start()
 * then calls rtw_hal_rf_set_power(PWR_BY_RATE) before anything is transmitted,
 * outside every CONFIG_BTCOEX guard, while halrf_chl_rfk_trigger() calls it
 * again on each channel change.  This port runs none of that, and the phy_reg
 * image it does load writes the forced power word as zero, so the four words
 * below are reported before and after the sweep, and the last one is what the
 * baseband recorded for the frame it transmitted most recently.
 */

struct k1_rtl8852bs_txpwr_state_s
{
  uint32_t force_ctrl;
  uint32_t force_value;
  uint32_t cck_index_a;
  uint32_t cck_index_b;
  uint32_t txinfo;
  uint32_t rate_ctrl;
  uint32_t coext_ctrl;
  uint32_t by_rate0;
  uint32_t by_rate_1ss_max;
  uint32_t by_rate_max;
};

static int k1_rtl8852bs_runtime_txpwr_sample(
  FAR struct k1_rtl8852bs_txpwr_state_s *state)
{
  static const uint32_t addresses[] =
  {
    K1_RTL8852BS_BB_TXPWR_FORCE_CTRL, K1_RTL8852BS_BB_TXPWR_FORCE_VALUE,
    K1_RTL8852BS_BB_CCK_TXPWR_INDEX_A, K1_RTL8852BS_BB_CCK_TXPWR_INDEX_B,
    K1_RTL8852BS_BB_TXINFO_TXPWR
  };

  static const uint32_t mac_addresses[] =
  {
    K1_RTL8852BS_PWR_RATE_CTRL, K1_RTL8852BS_PWR_COEXT_CTRL,
    K1_RTL8852BS_PWR_BY_RATE_TABLE0, K1_RTL8852BS_PWR_BY_RATE_1SS_MAX,
    K1_RTL8852BS_PWR_BY_RATE_MAX
  };

  FAR uint32_t *words[sizeof(addresses) / sizeof(addresses[0])];
  unsigned int index;
  int ret;

  if (state == NULL)
    {
      return -EINVAL;
    }

  memset(state, 0, sizeof(*state));

  words[0] = &state->force_ctrl;
  words[1] = &state->force_value;
  words[2] = &state->cck_index_a;
  words[3] = &state->cck_index_b;
  words[4] = &state->txinfo;

  for (index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++)
    {
      ret = k1_rtl8852bs_bb_read32(addresses[index], words[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* The MAC side of the same question.  These five are ordinary MAC registers:
   * the original reaches them through a power register accessor that only adds
   * a band offset and a range check, so a plain read is the whole of it.
   */

  words[0] = &state->rate_ctrl;
  words[1] = &state->coext_ctrl;
  words[2] = &state->by_rate0;
  words[3] = &state->by_rate_1ss_max;
  words[4] = &state->by_rate_max;

  for (index = 0; index < sizeof(mac_addresses) / sizeof(mac_addresses[0]);
       index++)
    {
      ret = k1_rtl8852bs_mac_read32(mac_addresses[index], words[index]);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static void k1_rtl8852bs_runtime_txpwr_log(
  FAR const char *stage, FAR const struct k1_rtl8852bs_txpwr_state_s *state)
{
  k1_early_puts("K1 Wi-Fi GPL: TX power ");
  k1_early_puts(stage);
  k1_early_puts(" force-en=");
  k1_early_puthex((state->force_ctrl &
                   K1_RTL8852BS_BB_TXPWR_FORCE_ENABLE) != 0 ? 1 : 0);
  k1_early_puts(" force-dbm=");
  k1_early_puthex((state->force_value &
                   K1_RTL8852BS_BB_TXPWR_VALUE_MASK) >>
                  K1_RTL8852BS_BB_TXPWR_VALUE_SHIFT);
  k1_early_puts(" cck-idx-a=");
  k1_early_puthex((state->cck_index_a &
                   K1_RTL8852BS_BB_CCK_TXPWR_INDEX_MASK) >>
                  K1_RTL8852BS_BB_CCK_TXPWR_INDEX_SHIFT);
  k1_early_puts(" cck-idx-b=");
  k1_early_puthex((state->cck_index_b &
                   K1_RTL8852BS_BB_CCK_TXPWR_INDEX_MASK) >>
                  K1_RTL8852BS_BB_CCK_TXPWR_INDEX_SHIFT);
  k1_early_puts(" txinfo-dbm=");
  k1_early_puthex((state->txinfo & K1_RTL8852BS_BB_TXINFO_TXPWR_MASK) >>
                  K1_RTL8852BS_BB_TXINFO_TXPWR_SHIFT);
  k1_early_puts(" raw-ctrl=");
  k1_early_puthex(state->force_ctrl);
  k1_early_puts(" raw-value=");
  k1_early_puthex(state->force_value);
  k1_early_puts("\r\n");

  k1_early_puts("K1 Wi-Fi GPL: TX power ");
  k1_early_puts(stage);
  k1_early_puts(" mac-force-en=");
  k1_early_puthex((state->rate_ctrl &
                   K1_RTL8852BS_PWR_FORCE_BY_RATE_EN) != 0 ? 1 : 0);
  k1_early_puts(" mac-rate-ctrl=");
  k1_early_puthex(state->rate_ctrl);
  k1_early_puts(" mac-coext=");
  k1_early_puthex(state->coext_ctrl);
  k1_early_puts(" by-rate0=");
  k1_early_puthex(state->by_rate0);
  k1_early_puts(" by-rate-1ss-max=");
  k1_early_puthex(state->by_rate_1ss_max);
  k1_early_puts(" by-rate-max=");
  k1_early_puthex(state->by_rate_max);
  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_txpwr_force
 *
 * Description:
 *   Force one constant transmit power, the way halbb_set_txpwr_dbm_8852b()
 *   does it: the enable bit in the control word, then the value field.  This
 *   is not what the original leaves in place for normal operation - there the
 *   per rate table computed by halrf_set_power() supplies the power - but that
 *   table is filled by RF code this port has not ported yet, and a constant
 *   overrides it in one register pair.
 *
 *   The field is nine signed bits and the original names it dBm without
 *   naming its scale, so the constant is chosen to be a usable transmit power
 *   under every scale the family uses: eight dBm if the step is an eighth of a
 *   dBm, sixteen if a quarter, and clamped by the amplifier above that.  It is
 *   a diagnostic constant, not a regulatory power setting, and the sweep it
 *   serves transmits one Probe Request per channel dwell.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_txpwr_force(void)
{
  int ret;

  ret = k1_rtl8852bs_bb_update_field_checked(
    K1_RTL8852BS_BB_TXPWR_FORCE_CTRL, K1_RTL8852BS_BB_TXPWR_FORCE_ENABLE,
    K1_RTL8852BS_BB_TXPWR_FORCE_ENABLE);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_bb_update_field_checked(
    K1_RTL8852BS_BB_TXPWR_FORCE_VALUE, K1_RTL8852BS_BB_TXPWR_VALUE_MASK,
    (uint32_t)K1_RTL8852BS_BB_TXPWR_FORCE_DBM <<
    K1_RTL8852BS_BB_TXPWR_VALUE_SHIFT);
  if (ret < 0)
    {
      return ret;
    }

  /* The same constant on the MAC side, where the original puts it when it
   * wants one power for every rate: the enable bit and the nine bit value in
   * one word.  This is the override that does not depend on the per rate table
   * being filled, and the table is read alongside it, so the report says
   * whether the override was needed.
   */

  ret = k1_rtl8852bs_mac_update_field_checked(
    K1_RTL8852BS_PWR_RATE_CTRL, K1_RTL8852BS_PWR_FORCE_BY_RATE_ALL,
    K1_RTL8852BS_PWR_FORCE_BY_RATE_EN |
    K1_RTL8852BS_MAC_TXPWR_FORCE_VALUE);
  if (ret < 0)
    {
      return ret;
    }

  /* And the Bluetooth side is taken off the transmit gain, which is the one
   * line the original leaves a comment on: r_txagc_BT_en cleared, and the
   * gain it would have supplied cleared with it.
   */

  return k1_rtl8852bs_mac_update_field_checked(
    K1_RTL8852BS_PWR_COEXT_CTRL, K1_RTL8852BS_PWR_COEXT_TXAGC_BT, 0);
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_medium_access_init
 *
 * Description:
 *   Program the medium access gating that cca_ctrl_init(), nav_ctrl_init() and
 *   the tail of scheduler_init() in trxcfg.c program and this component so far
 *   left at its hardware reset value.
 *
 *   Every entry is an update-and-readback field, so the bits the vendor does
 *   not touch keep whatever the hardware or an earlier step put there, and a
 *   write that the hardware refuses is reported instead of assumed.
 *
 ****************************************************************************/

static const struct k1_rtl8852bs_register_field_s
  g_k1_rtl8852bs_medium_access_fields[] =
{
  {K1_RTL8852BS_CCA_CONTROL, K1_RTL8852BS_CCA_CONTROL_MASK,
   K1_RTL8852BS_CCA_CONTROL_VALUE},
  {K1_RTL8852BS_WMAC_NAV_CTL, K1_RTL8852BS_WMAC_NAV_CTL_MASK,
   K1_RTL8852BS_WMAC_NAV_CTL_VALUE},
  {K1_RTL8852BS_PREBKF_CFG_1, K1_RTL8852BS_SIFS_MACTXEN_T1_MASK,
   K1_RTL8852BS_SIFS_MACTXEN_T1_VALUE},
  {K1_RTL8852BS_SCH_EXT_CTRL, K1_RTL8852BS_PORT_RST_TSF_ADV,
   K1_RTL8852BS_PORT_RST_TSF_ADV},
  {K1_RTL8852BS_EDCA_BCNQ_PARAM, K1_RTL8852BS_EDCA_BCNQ_UPPER_MASK,
   K1_RTL8852BS_EDCA_BCNQ_UPPER_VALUE}
};

static int k1_rtl8852bs_runtime_medium_access_init(void)
{
  unsigned int index;
  int ret;

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_medium_access_fields) /
               sizeof(g_k1_rtl8852bs_medium_access_fields[0]);
       index++)
    {
      ret = k1_rtl8852bs_mac_update_field_checked(
        g_k1_rtl8852bs_medium_access_fields[index].address,
        g_k1_rtl8852bs_medium_access_fields[index].mask,
        g_k1_rtl8852bs_medium_access_fields[index].value);
      if (ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: medium field ");
          k1_early_puthex((uintreg_t)index);
          k1_early_puts(" address=");
          k1_early_puthex(
            g_k1_rtl8852bs_medium_access_fields[index].address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-ret);
          k1_early_puts("\r\n");
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_medium_access_log
 *
 * Description:
 *   Report the scheduler and medium access words that decide whether a queued
 *   management frame may reach the air.  Three of them are read here for the
 *   first time on this port, and R_AX_PREBKF_CFG_0 is among them precisely
 *   because it must keep its reset value on SDIO: reading it is the only way
 *   to say so rather than to assume it.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_medium_access_log(FAR const char *stage)
{
  static const uint32_t addresses[] =
  {
    K1_RTL8852BS_CCA_CONTROL, K1_RTL8852BS_CCA_CONTROL_2,
    K1_RTL8852BS_WMAC_NAV_CTL, K1_RTL8852BS_PREBKF_CFG_0,
    K1_RTL8852BS_PREBKF_CFG_1, K1_RTL8852BS_SCH_EXT_CTRL,
    K1_RTL8852BS_EDCA_MGQ_PARAM, K1_RTL8852BS_EDCA_BCNQ_PARAM,
    K1_RTL8852BS_RSP_CHK_SIG, K1_RTL8852BS_TRXPTCL_RESP0
  };

  static const char *names[] =
  {
    " cca-control=", " cca-control2=", " nav=", " prebkf0=",
    " prebkf1=", " sch-ext=", " edca-mgq=", " edca-bcnq=",
    " rsp-chk-sig=", " resp0="
  };

  uint32_t values[sizeof(addresses) / sizeof(addresses[0])];
  bool valid[sizeof(addresses) / sizeof(addresses[0])];
  unsigned int index;

  /* Every value is collected before anything is printed.  A register read on
   * this port emits its own SDIO command trace, and interleaving the two
   * truncated this whole line on the previous board run, which is why the
   * readbacks it exists to report were never observed.
   */

  k1_sdio_wifi_suppress_command_trace(true);

  for (index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++)
    {
      valid[index] = k1_rtl8852bs_mac_read32(addresses[index],
                                             &values[index]) == OK;
    }

  k1_sdio_wifi_suppress_command_trace(false);

  k1_early_puts("K1 Wi-Fi GPL: medium ");
  k1_early_puts(stage);

  for (index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++)
    {
      k1_early_puts(names[index]);
      if (valid[index])
        {
          k1_early_puthex(values[index]);
        }
      else
        {
          k1_early_puts("err");
        }
    }

  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_sta_sch_init
 *
 * Description:
 *   Perform the original sta_sch_init() on the station scheduler, which the
 *   original runs inside dmac_init() between the host flow control and the
 *   MPDU processor and which this port has never run at all.
 *
 *   The station scheduler is the block that reports a non-empty WDE queue to a
 *   CMAC scheduler.  If its reset value leaves it disabled, a frame the
 *   firmware enqueues is charged to a WDE quota and then waits for a consumer
 *   that never learns it exists - no queue is dropped, no fault is latched and
 *   no CMAC counter moves, which is precisely the state the four-snapshot
 *   witness recorded.  The register is therefore reported before and after:
 *   a run whose "before" value already has both the enable and the
 *   initialisation-done flag set has eliminated this hypothesis by itself.
 *
 *   Hardware transmit mode keeps the non-empty report path cleared; only the
 *   software transmit mode sets it, and the SS2FINFO destination patch the
 *   original applies to two other chips is disabled for 8852B.
 *
 * Returned Value:
 *   OK when the block reports initialisation done, a negated errno otherwise.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_sta_sch_init(void)
{
  uint32_t before = 0;
  uint32_t value = 0;
  unsigned int polls;
  int ret;

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SS_CTRL, &before);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_SS_CTRL,
                                 before | K1_RTL8852BS_SS_CTRL_EN);
  if (ret < 0)
    {
      return ret;
    }

  for (polls = 0; polls < K1_RTL8852BS_SS_CTRL_POLL_COUNT; polls++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SS_CTRL, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & K1_RTL8852BS_SS_CTRL_INIT_DONE_1) != 0)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_SS_CTRL_POLL_USEC);
    }

  k1_early_puts("K1 Wi-Fi GPL: sta-sch ctrl before=");
  k1_early_puthex(before);
  k1_early_puts(" polls=");
  k1_early_puthex((uintreg_t)polls);
  k1_early_puts(" init-done=");
  k1_early_puthex((uintreg_t)
                  ((value & K1_RTL8852BS_SS_CTRL_INIT_DONE_1) != 0 ? 1 : 0));
  k1_early_puts("\r\n");

  if ((value & K1_RTL8852BS_SS_CTRL_INIT_DONE_1) == 0)
    {
      return -ETIMEDOUT;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_SS_CTRL,
                                 value | K1_RTL8852BS_SS_CTRL_WARM_INIT_FLG);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SS_CTRL, &value);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_write32(
    K1_RTL8852BS_SS_CTRL,
    value & ~(uint32_t)K1_RTL8852BS_SS_CTRL_NONEMPTY_SS2F);
  if (ret < 0)
    {
      return ret;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_SS_CTRL, &value);
  if (ret < 0)
    {
      return ret;
    }

  k1_early_puts("K1 Wi-Fi GPL: sta-sch ctrl after=");
  k1_early_puthex(value);
  k1_early_puts("\r\n");

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_tx_prerequisites
 *
 * Description:
 *   Report and program everything this port has never programmed that a
 *   transmit could depend on, immediately before the first transmit.  Both
 *   halves are best effort: their failure is logged and the sweep still runs,
 *   because the criterion that decides this diagnostic is a Probe Response,
 *   not a register write.
 *
 *   The report is the point.  Whatever the sweep does, the log then carries
 *   the reset state of the arbiter and of the transmit power window on a
 *   device where nothing has ever written either of them.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_block_enable_log
 *
 * Description:
 *   Report the four words that decide which DMAC and CMAC blocks are powered
 *   and clocked, read back at the moment a transmit is about to be attempted.
 *   They are written long before this point, so a later write that clears one
 *   of them would otherwise be invisible; printing the effective values makes
 *   any such clobber a single-line finding instead of another board run.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_block_enable_log(FAR const char *stage)
{
  static const uint16_t addresses[] =
    {
      K1_RTL8852BS_DMAC_FUNC_EN, K1_RTL8852BS_DMAC_CLK_EN,
      K1_RTL8852BS_CMAC_FUNC_EN, K1_RTL8852BS_CMAC_CLK_EN
    };

  static const char *names[] =
    {
      " dmac-func=", " dmac-clk=", " cmac-func=", " cmac-clk="
    };

  unsigned int index;

  k1_early_puts("K1 Wi-Fi GPL: block enable ");
  k1_early_puts(stage);

  for (index = 0; index < sizeof(addresses) / sizeof(addresses[0]); index++)
    {
      uint32_t value = 0;
      int ret = k1_rtl8852bs_mac_read32(addresses[index], &value);

      k1_early_puts(names[index]);
      if (ret < 0)
        {
          k1_early_puts("read-error");
        }
      else
        {
          k1_early_puthex(value);
        }
    }

  k1_early_puts("\r\n");
}

static void k1_rtl8852bs_runtime_tx_prerequisites(void)
{
  struct k1_rtl8852bs_coex_state_s coex;
  struct k1_rtl8852bs_txpwr_state_s txpwr;
  int ret;

  if (k1_rtl8852bs_runtime_coex_sample(&coex) == OK)
    {
      k1_rtl8852bs_runtime_coex_log("before", &coex);
    }

  if (k1_rtl8852bs_runtime_txpwr_sample(&txpwr) == OK)
    {
      k1_rtl8852bs_runtime_txpwr_log("before", &txpwr);
    }

  k1_rtl8852bs_runtime_medium_access_log("before");

  k1_rtl8852bs_runtime_block_enable_log("before");

  ret = k1_rtl8852bs_runtime_sta_sch_init();
  k1_early_puts("K1 Wi-Fi GPL: sta-sch init status=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_coex_wl_only();
  k1_early_puts("K1 Wi-Fi GPL: coex init status=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_medium_access_init();
  k1_early_puts("K1 Wi-Fi GPL: medium init status=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");

  ret = k1_rtl8852bs_runtime_txpwr_force();
  k1_early_puts("K1 Wi-Fi GPL: TX power force status=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");

  if (k1_rtl8852bs_runtime_coex_sample(&coex) == OK)
    {
      k1_rtl8852bs_runtime_coex_log("after", &coex);
    }

  if (k1_rtl8852bs_runtime_txpwr_sample(&txpwr) == OK)
    {
      k1_rtl8852bs_runtime_txpwr_log("after", &txpwr);
    }

  k1_rtl8852bs_runtime_medium_access_log("after");
}

static void k1_rtl8852bs_runtime_tx_prerequisites_report(void)
{
  struct k1_rtl8852bs_coex_state_s coex;
  struct k1_rtl8852bs_txpwr_state_s txpwr;

  if (k1_rtl8852bs_runtime_coex_sample(&coex) == OK)
    {
      k1_rtl8852bs_runtime_coex_log("after-sweep", &coex);
    }

  if (k1_rtl8852bs_runtime_txpwr_sample(&txpwr) == OK)
    {
      k1_rtl8852bs_runtime_txpwr_log("after-sweep", &txpwr);
    }

  k1_rtl8852bs_runtime_medium_access_log("after-sweep");
}
/****************************************************************************
 * Name: k1_rtl8852bs_runtime_mgmt_tx_build
 *
 * Description:
 *   Build the forty eight byte RTL8852B management transmit descriptor, a WD
 *   BODY followed by a WD INFO, and calculate its fixed-address SDIO FIFO
 *   encoding.  This is a pure memory operation with a read back check, the
 *   same shape as the normal-data builder next to it.
 *
 *   The values are those of txdes_proc_mgnt_8852b() with the SDIO variant of
 *   its store-and-forward decision, cross checked against the mainline
 *   rtw89 management path:
 *
 *     WD BODY dword0  store and forward, WD INFO present, DMA channel B0MG.
 *                     The WD page bit belongs to the PCIe variant and stays
 *                     clear here.  Hardware sequence selection and hardware
 *                     sequence mode both stay zero, so the sequence in the WD
 *                     BODY owns the frame and no MAC-table sequence counter
 *                     has to have been configured first.
 *     WD BODY dword2  frame length, band-0 management queue selector 0x12 and
 *                     the MACID whose address CAM record this port programs.
 *     WD BODY dword3  the software sequence number.
 *     WD INFO dword0  a fixed rate is selected, that rate is 1 Mbit/s CCK,
 *                     and data rate fallback is disabled, so nothing about
 *                     this transmit depends on a rate table.
 *     WD INFO dword1  the broadcast/multicast bit, set only when the caller
 *                     says the frame is broadcast.  A Probe Request with the
 *                     broadcast destination is not acknowledged and must not
 *                     be retried as if it were, so it needs the bit; a
 *                     unicast Authentication Request is acknowledged by the
 *                     access point and must not carry it, or the hardware
 *                     would neither wait for that acknowledgement nor retry
 *                     the frame when it is missing.
 *
 *   Everything else is zero: no encryption, no aggregation, no RTS, no
 *   lifetime override and no header conversion.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_mgmt_tx_build(
  size_t frame_length, uint16_t sequence, bool broadcast,
  FAR uint8_t *descriptor, size_t descriptor_length,
  FAR struct k1_rtl8852bs_data_tx_layout_s *layout)
{
  uint32_t body0;
  uint32_t body2;
  uint32_t body3;
  uint32_t info0;
  uint32_t info1;
  uint32_t length_units;
  uint32_t total_length;
  uint32_t ple_length;

  if (descriptor == NULL || layout == NULL ||
      descriptor_length < K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE ||
      frame_length < K1_RTL8852BS_IEEE80211_HEADER_SIZE ||
      frame_length > K1_RTL8852BS_H2C_TXD_LENGTH_MASK ||
      sequence > K1_RTL8852BS_DATA_TXD_SEQUENCE_MASK)
    {
      return -EINVAL;
    }

  body0 = K1_RTL8852BS_DATA_TXD_STF_MODE |
          K1_RTL8852BS_MGMT_TXD_WDINFO_EN |
          ((uint32_t)K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG <<
           K1_RTL8852BS_DATA_TXD_CH_DMA_SHIFT);
  body2 = (uint32_t)frame_length |
          ((uint32_t)K1_RTL8852BS_MGMT_TXD_QSEL_B0MG <<
           K1_RTL8852BS_DATA_TXD_QSEL_SHIFT) |
          ((uint32_t)K1_RTL8852BS_MGMT_TX_MACID <<
           K1_RTL8852BS_DATA_TXD_MACID_SHIFT);
  body3 = sequence;
  info0 = K1_RTL8852BS_MGMT_TXI_USERATE_SEL |
          ((uint32_t)K1_RTL8852BS_MGMT_TXI_DATARATE_CCK1 <<
           K1_RTL8852BS_MGMT_TXI_DATARATE_SHIFT) |
          K1_RTL8852BS_MGMT_TXI_DISDATAFB;
  info1 = broadcast ? (uint32_t)K1_RTL8852BS_MGMT_TXI_BMC : 0u;

  memset(descriptor, 0, K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE);
  k1_rtl8852bs_write_le32(descriptor, body0);
  k1_rtl8852bs_write_le32(descriptor + 8, body2);
  k1_rtl8852bs_write_le32(descriptor + 12, body3);
  k1_rtl8852bs_write_le32(descriptor + K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE,
                          info0);
  k1_rtl8852bs_write_le32(descriptor + K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE + 4,
                          info1);

  total_length = K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE + (uint32_t)frame_length;
  length_units = (total_length + K1_RTL8852BS_H2C_TX_UNIT_SIZE - 1u) /
                 K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  if (length_units == 0 || length_units > K1_RTL8852BS_H2C_TX_UNIT_MASK)
    {
      return -E2BIG;
    }

  ple_length = (uint32_t)frame_length + K1_RTL8852BS_DATA_TX_PLE_RESERVED +
               K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE;
  layout->fifo_address = K1_RTL8852BS_H2C_TX_FIFO_BASE |
                         ((uint32_t)K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG <<
                          K1_RTL8852BS_H2C_TX_FIFO_SHIFT) |
                         length_units;
  layout->transfer_length = length_units * K1_RTL8852BS_H2C_TX_UNIT_SIZE;
  layout->required_ple_pages =
    ((ple_length + K1_RTL8852BS_H2C_PLE_PAGE_SIZE - 1u) /
     K1_RTL8852BS_H2C_PLE_PAGE_SIZE) << 1u;
  layout->required_wde_pages = K1_RTL8852BS_DATA_TX_WDE_PAGES;

  if (k1_rtl8852bs_read_le32(descriptor) != body0 ||
      k1_rtl8852bs_read_le32(descriptor + 4) != 0 ||
      k1_rtl8852bs_read_le32(descriptor + 8) != body2 ||
      k1_rtl8852bs_read_le32(descriptor + 12) != body3 ||
      k1_rtl8852bs_read_le32(
        descriptor + K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE) != info0 ||
      k1_rtl8852bs_read_le32(
        descriptor + K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE + 4) != info1)
    {
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_mgmt_tx_probe
 *
 * Description:
 *   Transmit one Probe Request from the host, through the band-0 management
 *   queue, and report what the transmit path did with it.
 *
 *   This is the transmit positive control the transmit counters themselves
 *   need.  Every measurement so far has been of a frame the firmware was
 *   asked to build and send: the packet-offload table holds it, the channel
 *   table names it, and the sweep that should transmit it leaves every
 *   transmit counter at zero without raising a single error status.  A frame
 *   the host writes into the management FIFO takes the same CMAC, protocol
 *   and baseband path from the dispatcher onwards, but none of the firmware
 *   scan machinery, so the two together separate a chip that cannot transmit
 *   at all from a firmware transmit path that never starts.
 *
 *   It also is not throw-away work: association and authentication have to
 *   be transmitted this way as well, so this descriptor is the one the
 *   control plane will use once a scan result exists.
 *
 *   It runs only after the active-scan verdict has already been printed, so
 *   it cannot change that verdict, and it cannot fake one either: it counts
 *   nothing towards the Probe Response accounting the acceptance check reads.
 *   Every step is best effort with a logged status.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_mgmt_tx_probe(FAR const uint8_t *self_mac)
{
  uint8_t packet[K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE +
                 K1_RTL8852BS_PROBE_REQUEST_MAX_SIZE];
  struct k1_rtl8852bs_data_tx_layout_s layout;
  struct k1_rtl8852bs_data_tx_resources_s before_res;
  struct k1_rtl8852bs_data_tx_resources_s after_res;
  struct k1_rtl8852bs_tx_state_s tx_before;
  struct k1_rtl8852bs_tx_state_s tx_after;
  size_t frame_length = 0;
  bool tx_before_valid = false;
  bool after_res_valid = false;
  unsigned int drained = 0;
  int queue_ret;
  int write_ret = -ENODATA;
  int ret;

  ret = k1_rtl8852bs_runtime_probe_request_build(
    packet + K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE,
    sizeof(packet) - K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE,
    self_mac, &frame_length);
  if (ret == OK)
    {
      ret = k1_rtl8852bs_runtime_mgmt_tx_build(
        frame_length, 0u, true, packet, sizeof(packet), &layout);
    }

  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: mgmt tx build error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return;
    }

  /* The management queues are enabled again here because the active-scan path
   * can reach its error exit before it enabled them.  The helper only ORs the
   * two bits in and reads the register back, so a path that already enabled
   * them is unaffected.
   */

  queue_ret = k1_rtl8852bs_runtime_sch_tx_en_management();

  ret = k1_rtl8852bs_data_tx_resources_read(
    K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG, &before_res);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: mgmt tx resource error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return;
    }

  k1_early_puts("K1 Wi-Fi GPL: mgmt tx frame=");
  k1_rtl8852bs_runtime_put_word((uint32_t)frame_length);
  k1_early_puts(" wd0=");
  k1_rtl8852bs_runtime_put_word(k1_rtl8852bs_read_le32(packet));
  k1_early_puts(" wd2=");
  k1_rtl8852bs_runtime_put_word(k1_rtl8852bs_read_le32(packet + 8));
  k1_early_puts(" wi0=");
  k1_rtl8852bs_runtime_put_word(k1_rtl8852bs_read_le32(
    packet + K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE));
  k1_early_puts(" wi1=");
  k1_rtl8852bs_runtime_put_word(k1_rtl8852bs_read_le32(
    packet + K1_RTL8852BS_MGMT_TX_WD_BODY_SIZE + 4));
  k1_early_puts(" fifo=");
  k1_rtl8852bs_runtime_put_word(layout.fifo_address);
  k1_early_puts(" bytes=");
  k1_rtl8852bs_runtime_put_word(layout.transfer_length);
  k1_early_puts(" wp-avail=");
  k1_rtl8852bs_runtime_put_word(before_res.wp_available_pages);
  k1_early_puts(" ch8-used=");
  k1_rtl8852bs_runtime_put_word(before_res.channel_used_pages);
  k1_early_puts(" need-ple=");
  k1_rtl8852bs_runtime_put_word(layout.required_ple_pages);
  k1_early_puts(" mgq-en=");
  k1_early_puthex((uintreg_t)(queue_ret < 0 ? -queue_ret : 0));
  k1_early_puts("\r\n");

  if (before_res.channel_used_pages +
      layout.required_wde_pages > before_res.channel_max_pages ||
      before_res.wp_available_pages <
      layout.required_ple_pages + K1_RTL8852BS_DATA_TX_PLE_RESERVE)
    {
      k1_early_puts("K1 Wi-Fi GPL: mgmt tx no space\r\n");
      return;
    }

  tx_before_valid = k1_rtl8852bs_runtime_tx_state_sample(&tx_before) == OK;
  if (tx_before_valid)
    {
      k1_rtl8852bs_runtime_tx_state_log("mgmt-before", &tx_before, NULL);
    }

  /* One fixed-address CMD53 into the band-0 management FIFO, exactly the
   * transfer the firmware header packet and the H2C path already use.
   */

  write_ret = k1_sdio_wifi_write(1, layout.fifo_address, false,
                                 packet, layout.transfer_length);

  /* A frame that entered the queue and left it returns the page it was
   * charged, so the used-page counter is polled rather than sampled once: a
   * counter that goes up and comes back down is a frame that was consumed,
   * one that goes up and stays up is a frame that is stuck, and one that
   * never moves is a frame the dispatcher refused.
   */

  for (drained = 0; drained < K1_RTL8852BS_MGMT_TX_DRAIN_POLL; drained++)
    {
      after_res_valid = k1_rtl8852bs_data_tx_resources_read(
        K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG, &after_res) == OK;
      if (!after_res_valid ||
          after_res.channel_used_pages == before_res.channel_used_pages)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_MGMT_TX_DRAIN_USEC);
    }

  if (k1_rtl8852bs_runtime_tx_state_sample(&tx_after) == OK)
    {
      k1_rtl8852bs_runtime_tx_state_log("mgmt-after", &tx_after,
                                        tx_before_valid ? &tx_before : NULL);
    }

  k1_rtl8852bs_runtime_fault_snapshot("mgmt-post", true);

  k1_early_puts("K1 Wi-Fi GPL: mgmt tx write=");
  k1_early_puthex((uintreg_t)(write_ret < 0 ? -write_ret : 0));
  k1_early_puts(" polls=");
  k1_rtl8852bs_runtime_put_word(drained);
  k1_early_puts(" ch8-used-after=");
  k1_rtl8852bs_runtime_put_word(
    after_res_valid ? after_res.channel_used_pages : 0xffffffffu);
  k1_early_puts(" wp-avail-after=");
  k1_rtl8852bs_runtime_put_word(
    after_res_valid ? after_res.wp_available_pages : 0xffffffffu);
  k1_early_puts("\r\n");
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_mgmt_tx_frame
 *
 * Description:
 *   Transmit one caller-built management frame through the band-0 management
 *   queue.  This is the same descriptor, the same fixed-address CMD53 and the
 *   same drain poll that k1_rtl8852bs_runtime_mgmt_tx_probe() already proved
 *   reaches the TMAC and radiates, with the instrumentation removed and the
 *   status returned instead of printed.
 *
 *   The instrumentation is what makes it a separate function rather than a
 *   parameter on the existing one.  This path runs inside a scan-offload
 *   dwell, on a polled console, and the answer to the frame has to arrive in
 *   that same dwell; the probe variant prints on the order of ten lines per
 *   transmit, which at the console's byte rate is a large part of the two
 *   hundred and fifty millisecond window.  The verbose evidence is kept where
 *   it belongs, on the positive control that exists to produce it.
 *
 * Returned Value:
 *   OK when the transfer was accepted and the queue drained, a negated errno
 *   otherwise.  -ENOSPC means the queue had no room, so nothing was written.
 *
 ****************************************************************************/

static int k1_rtl8852bs_runtime_mgmt_tx_frame(FAR const uint8_t *frame,
                                              size_t frame_length,
                                              uint16_t sequence,
                                              bool broadcast)
{
  uint8_t packet[K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE +
                 K1_RTL8852BS_PROBE_REQUEST_MAX_SIZE];
  struct k1_rtl8852bs_data_tx_layout_s layout;
  struct k1_rtl8852bs_data_tx_resources_s before_res;
  struct k1_rtl8852bs_data_tx_resources_s after_res;
  unsigned int drained;
  int ret;

  if (frame == NULL || frame_length < K1_RTL8852BS_IEEE80211_HEADER_SIZE ||
      frame_length > sizeof(packet) - K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE)
    {
      return -EINVAL;
    }

  memset(packet, 0, sizeof(packet));
  memcpy(packet + K1_RTL8852BS_MGMT_TX_DESCRIPTOR_SIZE, frame, frame_length);

  ret = k1_rtl8852bs_runtime_mgmt_tx_build(frame_length, sequence, broadcast,
                                           packet, sizeof(packet), &layout);
  if (ret < 0)
    {
      return ret;
    }

  /* Best effort, exactly as in the probe variant: the helper only ORs the two
   * management-queue enables in, so a path that already enabled them is
   * unaffected and its failure is not fatal to the transfer.
   */

  k1_rtl8852bs_runtime_sch_tx_en_management();

  ret = k1_rtl8852bs_data_tx_resources_read(
    K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG, &before_res);
  if (ret < 0)
    {
      return ret;
    }

  if (before_res.channel_used_pages +
      layout.required_wde_pages > before_res.channel_max_pages ||
      before_res.wp_available_pages <
      layout.required_ple_pages + K1_RTL8852BS_DATA_TX_PLE_RESERVE)
    {
      return -ENOSPC;
    }

  ret = k1_sdio_wifi_write(1, layout.fifo_address, false, packet,
                           layout.transfer_length);
  if (ret < 0)
    {
      return ret;
    }

  /* The same page-counter poll the probe variant uses: a counter that goes up
   * and comes back down is a frame the dispatcher consumed.  A frame that is
   * still charged when the poll runs out is not an error here, because the
   * dwell has to be left for the answer either way.
   */

  for (drained = 0; drained < K1_RTL8852BS_MGMT_TX_DRAIN_POLL; drained++)
    {
      if (k1_rtl8852bs_data_tx_resources_read(
            K1_RTL8852BS_MGMT_TXD_CH_DMA_B0MG, &after_res) != OK ||
          after_res.channel_used_pages == before_res.channel_used_pages)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_MGMT_TX_DRAIN_USEC);
    }

  return OK;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_maclbk_probe
 *
 * Description:
 *   Run one more active sweep with the MAC in loopback and report what came
 *   back.  This is a transmit positive control and nothing else: with
 *   B_AX_MACLBK_EN set the CMAC returns the PPDU it would have handed to the
 *   baseband into its own receive path, so a Probe Request that reaches the
 *   MAC reappears as a received frame carrying this host as transmitter, and
 *   the loopback counter counts the PPDUs that were looped.  Nothing is
 *   radiated while the bit is set, and no association, key or network device
 *   is involved.
 *
 *   It exists because the transmit counters this component already samples
 *   fail their own positive control: they read zero even on a sweep whose
 *   receive side demonstrably works, so they cannot say whether the offloaded
 *   Probe Request ever became a PPDU.  A non-zero looped count, or a received
 *   frame whose A2 is this host, places the fault after the MAC, in the
 *   baseband or radio calibration this port does not run yet.  A zero count on
 *   a sweep that otherwise completes places it in the MAC, the scheduler or
 *   the firmware transmit path.
 *
 *   B_AX_MACLBK_EN is the only bit written.  The original's tmac_init sets it
 *   whenever its transmit mode is MAC_AX_TRX_LOOPBACK and clears it otherwise,
 *   with no queue, DLE or baseband prerequisite, and everything else in the
 *   register keeps the ready-period, PLCP-delay and ready-number defaults the
 *   part came up with.  The register is read first and written back to exactly
 *   that value afterwards, the same way the sweep already saves and restores
 *   the receive filter, so nothing outside this window sees the part in
 *   loopback.
 *
 *   Every step is best effort with a logged status.  This runs only after the
 *   real active sweep has already been judged, so it cannot change that
 *   verdict, and a register that refuses a value must not remove the evidence
 *   the rest of the run produced.
 *
 ****************************************************************************/

static void k1_rtl8852bs_runtime_maclbk_probe(void)
{
  uint32_t saved = 0;
  uint32_t engaged = 0;
  uint32_t count_before = 0;
  uint32_t count_cleared = 0;
  uint32_t count_after = 0;
  struct k1_rtl8852bs_tx_state_s tx_before;
  struct k1_rtl8852bs_tx_state_s tx_after;
  bool tx_before_valid;
  int sweep_ret = -ENODATA;
  int restore_ret;
  int ret;

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_MAC_LOOPBACK, &saved);
  if (ret < 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: active scan loopback read error=");
      k1_early_puthex((uintreg_t)-ret);
      k1_early_puts("\r\n");
      return;
    }

  k1_rtl8852bs_mac_read32(K1_RTL8852BS_MAC_LOOPBACK_COUNT, &count_before);

  /* The counter is cleared before the sweep so that whatever it reports
   * afterwards belongs to this sweep alone, and the cleared value is read back
   * because a counter that will not clear cannot be read as a delta either.
   */

  k1_rtl8852bs_mac_write32(K1_RTL8852BS_MAC_LOOPBACK_COUNT,
                           K1_RTL8852BS_MAC_LOOPBACK_COUNT_CLR);
  k1_rtl8852bs_mac_read32(K1_RTL8852BS_MAC_LOOPBACK_COUNT, &count_cleared);

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_MAC_LOOPBACK,
                                 saved | K1_RTL8852BS_MAC_LOOPBACK_EN);
  if (ret == OK)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_MAC_LOOPBACK, &engaged);
    }

  if (ret == OK && (engaged & K1_RTL8852BS_MAC_LOOPBACK_EN) == 0)
    {
      ret = -EIO;
    }

  if (ret == OK)
    {
      /* The firmware clears its channel-list busy state on the scan-end event
       * the previous sweep waited for, so the table can be submitted again,
       * naming the same stored Probe Request.
       */

      ret = k1_rtl8852bs_runtime_scanofld_chlist_submit(
        K1_RTL8852BS_PKT_OFLD_PROBE_REQUEST_ID);
    }

  tx_before_valid = ret == OK &&
                    k1_rtl8852bs_runtime_tx_state_sample(&tx_before) == OK;

  if (ret == OK)
    {
      sweep_ret =
        k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common(
          false, true, NULL);
      k1_rtl8852bs_mac_read32(K1_RTL8852BS_MAC_LOOPBACK_COUNT, &count_after);

      if (k1_rtl8852bs_runtime_tx_state_sample(&tx_after) == OK)
        {
          k1_rtl8852bs_runtime_tx_state_log("lbk-after", &tx_after,
                                            tx_before_valid ? &tx_before
                                                            : NULL);
        }
    }

  restore_ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_MAC_LOOPBACK, saved);

  k1_early_puts("K1 Wi-Fi GPL: active scan loopback saved=");
  k1_early_puthex(saved);
  k1_early_puts(" engaged=");
  k1_early_puthex(engaged);
  k1_early_puts(" count-before=");
  k1_early_puthex(count_before);
  k1_early_puts(" count-cleared=");
  k1_early_puthex(count_cleared);
  k1_early_puts(" count-after=");
  k1_early_puthex(count_after);
  k1_early_puts(" count-after-hi=");
  k1_early_puthex((count_after >> K1_RTL8852BS_MAC_LOOPBACK_COUNT_SHIFT) &
                  K1_RTL8852BS_MAC_LOOPBACK_COUNT_MASK);
  k1_early_puts(" setup=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts(" sweep=");
  k1_early_puthex((uintreg_t)(sweep_ret < 0 ? -sweep_ret : 0));
  k1_early_puts(" restore=");
  k1_early_puthex((uintreg_t)(restore_ret < 0 ? -restore_ret : 0));
  k1_early_puts("\r\n");

  /* The sweep result itself is deliberately not propagated.  A loopback sweep
   * receives no Probe Response by construction, because nothing was radiated,
   * so its own verdict is always a failure and says nothing.  What it produced
   * is in the address-checked receive line the sweep already printed:
   * self-tx and self-preq there, together with count-after above.
   */
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_scanofld_active_diagnostic
 *
 * Description:
 *   Transmit a wildcard Probe Request on 2.4 GHz channels 1 through 13 and
 *   require a Probe Response back.  This is the first transmit path in this
 *   component that puts a frame in the air, and it does so the way both the
 *   original phl and the mainline rtw89 driver do it, so that no host
 *   transmit ring is needed: the frame is stored in the firmware
 *   packet-offload table with MAC/FW_OFLD/PACKET_OFLD, and the scan-offload
 *   channel table then names that stored packet, leaving the firmware to
 *   build the transmit descriptor and to transmit once per dwell.
 *
 *   A Probe Response is required rather than merely a firmware
 *   acknowledgement.  Every acknowledgement in this path only says that the
 *   firmware accepted a command; a Probe Response is a frame an access point
 *   sends solely in reply to a Probe Request, addressed to the eFuse self MAC
 *   this host put in that request, so receiving one is evidence that the
 *   request left the antenna.  Beacons do not count: a passive sweep already
 *   receives those.
 *
 *   This is still a one-shot RAM-only diagnostic.  It associates with no
 *   access point, authenticates with none, installs no key, creates no
 *   network device, carries no data, and writes nothing to eMMC, SPI flash,
 *   eFuse or the U-Boot environment.  A single directed Probe Request per
 *   channel is what any station scan transmits, and nothing is retransmitted
 *   on failure.
 *
 * Input Parameters:
 *   self_mac - The eFuse self MAC.  It has to be the address the no-link role
 *              and address CAM were configured with, or the reply would be
 *              addressed to a station this receiver filters out.
 *
 * Returned Value:
 *   OK when the sweep reached its scan-end event and at least one Probe
 *   Response was received, a negated errno otherwise.  -ENODATA means the
 *   sweep completed without one.
 *
 ****************************************************************************/

int k1_rtl8852bs_fwdl_runtime_scanofld_active_diagnostic(
  FAR const uint8_t *self_mac)
{
  struct k1_rtl8852bs_tx_state_s before;
  struct k1_rtl8852bs_tx_state_s after;
  uint8_t frame[K1_RTL8852BS_PROBE_REQUEST_MAX_SIZE];
  size_t frame_length = 0;
  int sweep_ret;
  int ret;

  ret = k1_rtl8852bs_runtime_probe_request_build(frame, sizeof(frame),
                                                self_mac, &frame_length);
  if (ret < 0)
    {
      goto error;
    }

  /* The address the receive accounting judges A1 of a Probe Response against.
   * The address CAM programming already recorded it; recording it again here
   * keeps this diagnostic correct if it is ever reached on its own, because
   * without it a Probe Response addressed to another station could be counted
   * as a reply to this host.
   */

  k1_rtl8852bs_scanofld_set_self_mac(self_mac);

  k1_early_puts("K1 Wi-Fi GPL: active scan probe request bytes=");
  k1_early_puthex((uintreg_t)frame_length);
  k1_early_puts(" sa=");
  k1_rtl8852bs_scanofld_log_bytes(frame + 10, 6);
  k1_early_puts(" ssid-len=0\r\n");

  /* Everything the vendor does unconditionally before its first transmit and
   * this port does not: the arbiter that decides whether the Wi-Fi core owns
   * the shared front end at all, and the transmit power the baseband applies
   * to what it sends.  Both are read back before and after being programmed,
   * so a run that still gets no Probe Response says which of the two was
   * already right.  Both are best effort with a logged status: a register that
   * refuses to take a value must not remove the rest of the evidence.
   */

  k1_rtl8852bs_runtime_tx_prerequisites();

  /* A queue the scheduler does not serve swallows the frame silently, so the
   * management queues are enabled before anything is offloaded, and the
   * transmit counters are sampled before anything is transmitted.
   */

  ret = k1_rtl8852bs_runtime_sch_tx_en_management();
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_runtime_tx_state_sample(&before);
  if (ret < 0)
    {
      goto error;
    }

  k1_rtl8852bs_runtime_tx_state_log("before", &before, NULL);

  /* The dispatcher and the error status words are read at four points on this
   * path because the run that produced the evidence could not say which step
   * changed them: the packet offload upload happens after the passive sweep
   * that the wide witness sampled, so template residency alone could account
   * for the pages the active sweep holds.  These four snapshots bracket the
   * upload, the channel table and the sweep separately, which is what
   * separates a template sitting in the wireless CPU quota from a frame that
   * entered a queue during the sweep and never left it.
   */

  k1_rtl8852bs_runtime_fault_snapshot("pre-ofld", true);

  ret = k1_rtl8852bs_runtime_pkt_ofld_add(
    frame, frame_length, K1_RTL8852BS_PKT_OFLD_PROBE_REQUEST_ID);
  if (ret < 0)
    {
      goto error;
    }

  k1_rtl8852bs_runtime_fault_snapshot("post-ofld", false);

  /* The channel table has to be resubmitted with the transmit fields set:
   * the table the passive chain left in the firmware names no packet.  The
   * firmware clears its channel-list busy state on the done acknowledgement
   * of the previous table, which the passive submit already required.
   */

  ret = k1_rtl8852bs_runtime_scanofld_chlist_submit(
    K1_RTL8852BS_PKT_OFLD_PROBE_REQUEST_ID);
  if (ret < 0)
    {
      goto error;
    }

  k1_rtl8852bs_runtime_fault_snapshot("post-chlist", false);

  sweep_ret = k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common(
    false, true, NULL);

  k1_rtl8852bs_runtime_fault_snapshot("post-sweep", true);

  /* The counters after the sweep are what separate a Probe Request that was
   * never transmitted from one that was transmitted and not answered, so they
   * are sampled and reported even when the sweep failed: that is precisely
   * the case they have to explain.
   */

  ret = k1_rtl8852bs_runtime_tx_state_sample(&after);
  if (ret == OK)
    {
      k1_rtl8852bs_runtime_tx_state_log("after", &after, &before);
    }

  /* The firmware and its own coexistence logic run during the sweep, so the
   * arbiter and the power words are read once more afterwards: a value that
   * was accepted before the sweep and is different now was overwritten by the
   * part, which is a different failure from never having been set.
   */

  k1_rtl8852bs_runtime_tx_prerequisites_report();

  if (sweep_ret < 0)
    {
      ret = sweep_ret;
      goto error;
    }

  if (ret < 0)
    {
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 active scan probe response "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: active scan probe response error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");

  /* The transmit positive control runs only on the failing path, and only
   * after the verdict above has been printed, so it cannot change the result.
   * It also cannot turn a failure into a pass: a loopback sweep radiates
   * nothing, so no access point answers it and its own Probe Response count
   * stays zero, which is the number the acceptance check reads.  What it is
   * for is separating a Probe Request that never became a PPDU from one that
   * was transmitted into a baseband or radio this port has not calibrated.
   */

  k1_rtl8852bs_runtime_mgmt_tx_probe(self_mac);
  k1_rtl8852bs_runtime_maclbk_probe();
  return ret;
}

/****************************************************************************
 * Name: k1_rtl8852bs_runtime_scanofld_passive_scan
 *
 * Description:
 *   Run one repeatable passive 2.4 GHz 1-13 sweep and report the access
 *   points it received.  This is the same firmware offload the passive
 *   diagnostic validates, made re-runnable for a wireless network device:
 *   the whole channel table is submitted again before every sweep, exactly
 *   as the original driver does.
 *
 *   The original host resubmits its complete table per scan as well: its
 *   ADD_SCANOFLD_CH host command carries the entry count in its own header
 *   and the original clears the host-side list after each send, so a sweep
 *   always describes itself and never extends the previous one.  The
 *   firmware clears the busy state that gates a new table on the scan-end
 *   event, which the wait below requires before returning success.
 *
 *   No probe request is transmitted, no association is attempted and no
 *   persistent state changes.  The sweep is not re-entrant: the caller
 *   serializes it.
 *
 * Input Parameters:
 *   result - Receives the sweep report.  It is filled in even when the
 *            sweep fails, so a caller can report what was observed.
 *
 * Returned Value:
 *   OK on a sweep that reached its scan-end event, a negated errno
 *   otherwise.  An empty report is not an error: no beacon in range is a
 *   legitimate outcome, and the caller decides what to do about it.
 ****************************************************************************/

int k1_rtl8852bs_runtime_scanofld_passive_scan(
  FAR struct k1_rtl8852bs_scan_result_s *result)
{
  int ret;

  if (result == NULL)
    {
      return -EINVAL;
    }

  memset(result, 0, sizeof(*result));

  ret = k1_rtl8852bs_fwdl_runtime_scanofld_ch_done_ack_diagnostic();
  if (ret < 0)
    {
      return ret;
    }

  return k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common(
    false, false, result);
}

/****************************************************************************
 * Name: k1_rtl8852bs_fwdl_runtime_auth_diagnostic
 *
 * Description:
 *   Transmit one open-system Authentication Request to a real access point
 *   and require its Authentication Response back.  This is the first frame
 *   this component addresses to a single peer rather than to the broadcast
 *   address, and the first reply it asks for that only an access point which
 *   accepted the request can send.
 *
 *   Two sweeps are run.  The first is an ordinary passive sweep whose only
 *   job is to choose a target: it reports the access points in range with the
 *   channel each of them advertises, and the strongest evidence available for
 *   "this one is really there" is the number of beacons received from it, so
 *   the access point with the most beacons is chosen.  The Authentication
 *   Request is then armed for that BSSID and channel and a second sweep is
 *   run.  When the firmware reports it has entered that channel, the request
 *   is transmitted from inside the receive loop, and the same sweep's receive
 *   drain is what observes the answer.
 *
 *   The reason the transmit lives inside a sweep is that the firmware parks
 *   the radio.  Scan offload holds the channel it entered until the host
 *   submits the next-channel command, which this port does from a deadline in
 *   the receive loop; so between the enter-channel notification and that
 *   deadline the radio is tuned to the target's channel for a quarter of a
 *   second, which is the vendor state machine's own behaviour and not a
 *   detour around it.  This port has no baseband or radio channel setter yet,
 *   so this is the only way it can currently transmit on a chosen channel.
 *
 *   What this does NOT do, stated plainly because the distinction decides how
 *   the result may be reported: it does not associate, it installs no key, it
 *   creates no network device, it carries no data, and the hardware does not
 *   acknowledge the access point's response.  The sweep runs with the receive
 *   filter opened up and the address CAM still holding a no-link role, so the
 *   answer is received but not acknowledged, and the access point will retry
 *   it and then time the exchange out.  Seeing the response is the milestone;
 *   a link is not claimed.  Nothing is written to eMMC, SPI flash, eFuse or
 *   the U-Boot environment.
 *
 * Input Parameters:
 *   self_mac - The eFuse self MAC.  It has to be the address the no-link role
 *              and the address CAM were configured with, because the response
 *              is only counted as an answer to this host when its A1 matches.
 *
 * Returned Value:
 *   OK when an Authentication frame addressed to this host arrived, a negated
 *   errno otherwise.  -ENODATA means the request was transmitted and no
 *   response addressed to this host arrived; -EHOSTUNREACH means the target
 *   sweep found no access point to aim at.
 *
 ****************************************************************************/

int k1_rtl8852bs_fwdl_runtime_auth_diagnostic(FAR const uint8_t *self_mac)
{
  struct k1_rtl8852bs_scan_result_s result;
  FAR const struct k1_rtl8852bs_scan_bss_s *target = NULL;
  unsigned int index;
  int sweep_ret;
  int ret;

  if (self_mac == NULL || !k1_rtl8852bs_addr_cam_mac_valid(self_mac))
    {
      ret = -EINVAL;
      goto error;
    }

  k1_rtl8852bs_scanofld_set_self_mac(self_mac);

  ret = k1_rtl8852bs_runtime_scanofld_passive_scan(&result);
  if (ret < 0)
    {
      goto error;
    }

  for (index = 0; index < result.bss_count &&
       index < K1_RTL8852BS_SCAN_BSS_MAX; index++)
    {
      FAR const struct k1_rtl8852bs_scan_bss_s *bss = &result.bss[index];

      if (bss->channel == 0 ||
          !k1_rtl8852bs_addr_cam_mac_valid(bss->bssid))
        {
          continue;
        }

      if (target == NULL || bss->beacon_frames > target->beacon_frames)
        {
          target = bss;
        }
    }

  if (target == NULL)
    {
      ret = -EHOSTUNREACH;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: auth target bssid=");
  k1_rtl8852bs_scanofld_log_bytes(target->bssid, 6);
  k1_early_puts(" channel=");
  k1_early_puthex(target->channel);
  k1_early_puts(" beacons=");
  k1_early_puthex(target->beacon_frames);
  k1_early_puts(" ssid-len=");
  k1_early_puthex(target->ssid_length);
  k1_early_puts("\r\n");

  k1_rtl8852bs_runtime_auth_arm(target->bssid, target->channel);
  sweep_ret = k1_rtl8852bs_runtime_scanofld_passive_scan(&result);
  k1_rtl8852bs_runtime_auth_disarm();

  k1_early_puts("K1 Wi-Fi GPL: auth req=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.requests);
  k1_early_puts(" tx-status=");
  k1_early_puthex((uintreg_t)
                  (g_k1_rtl8852bs_auth_action.transmit_status < 0 ?
                   -g_k1_rtl8852bs_auth_action.transmit_status : 0));
  k1_early_puts(" frames=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.frames_seen);
  k1_early_puts(" rsp-self=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.responses_to_self);
  k1_early_puts(" rsp-target=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.responses_from_target);
  k1_early_puts(" alg=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.response_algorithm);
  k1_early_puts(" seq=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.response_sequence);
  k1_early_puts(" status=");
  k1_early_puthex(g_k1_rtl8852bs_auth_action.response_status);
  k1_early_puts(" a2=");
  k1_rtl8852bs_scanofld_log_bytes(g_k1_rtl8852bs_auth_action.response_a2, 6);
  k1_early_puts("\r\n");

  if (sweep_ret < 0)
    {
      ret = sweep_ret;
      goto error;
    }

  if (!g_k1_rtl8852bs_auth_action.response_valid)
    {
      ret = -ENODATA;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 authentication response "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: authentication response error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_preboot_diagnostic(void)
{
  uint16_t failed_address = 0;
  uint16_t cleanup_failed_address;
  bool cleanup_needed;
  int cleanup_ret;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW preboot begin\r\n");

  ret = k1_rtl8852bs_fwdl_prepare(&failed_address, &cleanup_needed);
  if (ret >= 0)
    {
      ret = k1_rtl8852bs_fwdl_cleanup(&failed_address);
      if (ret >= 0)
        {
          cleanup_needed = false;
          k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW preboot H2C path "
                        "complete\r\n");
          return OK;
        }
    }

  if (cleanup_needed)
    {
      cleanup_failed_address = 0;
      cleanup_ret = k1_rtl8852bs_fwdl_cleanup(&cleanup_failed_address);
      if (cleanup_ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: FW preboot cleanup error address=");
          k1_early_puthex(cleanup_failed_address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-cleanup_ret);
          k1_early_puts("\r\n");
        }
    }

  k1_early_puts("K1 Wi-Fi GPL: FW preboot error address=");
  k1_early_puthex(failed_address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_h2c_tx_diagnostic(void)
{
  uint8_t descriptor[K1_RTL8852BS_H2C_DESCRIPTOR_SIZE];
  uint8_t header[K1_RTL8852BS_H2C_FWCMD_HEADER_SIZE];
  uint16_t available_pages;
  uint16_t required_pages;
  uint32_t fifo_address;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 H2C TX resource diagnostic "
                "begin\r\n");

  ret = k1_rtl8852bs_h2c_resource_read(&available_pages);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_h2c_descriptor_build(descriptor, header, 0,
                                           &fifo_address, &required_pages);
  if (ret < 0)
    {
      goto error;
    }

  if (available_pages < required_pages)
    {
      ret = -ENOSPC;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: H2C TX resource available=");
  k1_early_puthex(available_pages);
  k1_early_puts(" required=");
  k1_early_puthex(required_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts(" TXD0=");
  k1_early_puthex(k1_rtl8852bs_read_le32(descriptor));
  k1_early_puts(" TXD2=");
  k1_early_puthex(k1_rtl8852bs_read_le32(descriptor + 8));
  k1_early_puts("\r\n");
  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 H2C TX resource diagnostic "
                "complete\r\n");
  return OK;

error:
  k1_early_puts("K1 Wi-Fi GPL: H2C TX resource diagnostic error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_fw_header_packet_diagnostic(void)
{
  uint8_t packet[K1_RTL8852BS_FW_HEADER_TRANSFER_SIZE];
  uint16_t available_pages;
  uint16_t required_pages;
  uint16_t prepare_failed_address = 0;
  uint16_t cleanup_failed_address;
  uint32_t failed_address = 0;
  uint32_t fifo_address;
  uint32_t value;
  unsigned int attempt;
  bool cleanup_needed;
  int cleanup_ret;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW header packet diagnostic "
                "begin\r\n");

  ret = k1_rtl8852bs_fwdl_prepare(&prepare_failed_address, &cleanup_needed);
  if (ret < 0)
    {
      failed_address = prepare_failed_address;
      goto error;
    }

  ret = k1_rtl8852bs_h2c_resource_read(&available_pages);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
      goto error;
    }

  ret = k1_rtl8852bs_fw_header_packet_build(packet, &fifo_address,
                                             &required_pages);
  if (ret < 0)
    {
      goto error;
    }

  if (available_pages < required_pages)
    {
      failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
      ret = -ENOSPC;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: FW header packet available=");
  k1_early_puthex(available_pages);
  k1_early_puts(" required=");
  k1_early_puthex(required_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts(" bytes=");
  k1_early_puthex(sizeof(packet));
  k1_early_puts("\r\n");

  /* SDIO TX FIFO writes are fixed-address CMD53 transfers.  The Linux
   * reference reaches the same path through sdio_writesb(), not memcpy_toio.
   */

  ret = k1_sdio_wifi_write(1, fifo_address, false, packet, sizeof(packet));
  if (ret < 0)
    {
      failed_address = fifo_address;
      goto error;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_FW_PREBOOT_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL, &value);
      if (ret < 0)
        {
          failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
          goto error;
        }

      if ((value & K1_RTL8852BS_FWDL_PATH_READY) != 0)
        {
          ret = k1_rtl8852bs_fwdl_cleanup(&cleanup_failed_address);
          if (ret < 0)
            {
              failed_address = cleanup_failed_address;
              goto error;
            }

          cleanup_needed = false;
          k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW header packet "
                        "complete\r\n");
          return OK;
        }

      up_udelay(K1_RTL8852BS_FW_PREBOOT_POLL_USEC);
    }

  failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
  ret = -ETIMEDOUT;

error:
  if (cleanup_needed)
    {
      cleanup_failed_address = 0;
      cleanup_ret = k1_rtl8852bs_fwdl_cleanup(&cleanup_failed_address);
      if (cleanup_ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: FW header packet cleanup error "
                        "address=");
          k1_early_puthex(cleanup_failed_address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-cleanup_ret);
          k1_early_puts("\r\n");
        }
    }

  k1_early_puts("K1 Wi-Fi GPL: FW header packet diagnostic error address=");
  k1_early_puthex(failed_address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

static int k1_rtl8852bs_fwdl_section0_packets_diagnostic(
  enum k1_rtl8852bs_section0_follow_e follow_packet)
{
  FAR uint8_t *packet;
  uint16_t available_pages;
  uint16_t required_pages;
  uint16_t prepare_failed_address = 0;
  uint16_t cleanup_failed_address;
  uint32_t failed_address = 0;
  uint32_t fifo_address;
  uint32_t follow_transfer_size;
  uint32_t value;
  unsigned int attempt;
  unsigned int follow_count;
  unsigned int follow_index;
  bool cleanup_needed = false;
  int cleanup_ret;
  int ret;

  if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 fourth packet "
                    "diagnostic begin\r\n");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 third packet "
                    "diagnostic begin\r\n");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_SECOND_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 second packet "
                    "diagnostic begin\r\n");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 tail packet "
                    "diagnostic begin\r\n");
    }
  else
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 packet diagnostic "
                    "begin\r\n");
    }

  /* This runs in board_late_initialize(), where heap allocation and SDIO
   * transfers are allowed.  Avoid placing the 2048-byte packet on its stack.
   */

  packet = kmm_zalloc(K1_RTL8852BS_FW_SECTION0_TRANSFER_SIZE);
  if (packet == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  ret = k1_rtl8852bs_fwdl_prepare(&prepare_failed_address, &cleanup_needed);
  if (ret < 0)
    {
      failed_address = prepare_failed_address;
      goto error;
    }

  ret = k1_rtl8852bs_fw_header_packet_build(packet, &fifo_address,
                                             &required_pages);
  if (ret < 0)
    {
      goto error;
    }

  ret = k1_rtl8852bs_h2c_resource_wait(required_pages, &available_pages);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
      goto error;
    }

  ret = k1_sdio_wifi_write(1, fifo_address, false, packet,
                           K1_RTL8852BS_FW_HEADER_TRANSFER_SIZE);
  if (ret < 0)
    {
      failed_address = fifo_address;
      goto error;
    }

  for (attempt = 0; attempt < K1_RTL8852BS_FW_PREBOOT_POLL_COUNT; attempt++)
    {
      ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WCPU_FW_CTRL, &value);
      if (ret < 0)
        {
          failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
          goto error;
        }

      if ((value & K1_RTL8852BS_FWDL_PATH_READY) != 0)
        {
          break;
        }

      up_udelay(K1_RTL8852BS_FW_PREBOOT_POLL_USEC);
    }

  if (attempt == K1_RTL8852BS_FW_PREBOOT_POLL_COUNT)
    {
      failed_address = K1_RTL8852BS_WCPU_FW_CTRL;
      ret = -ETIMEDOUT;
      goto error;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_H2C_CTRL, 0);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HALT_H2C_CTRL;
      goto error;
    }

  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_HALT_C2H_CTRL, 0);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HALT_C2H_CTRL;
      goto error;
    }

  ret = k1_rtl8852bs_fw_section0_packet_build(packet, &fifo_address,
                                               &required_pages);
  if (ret < 0)
    {
      goto error;
    }

  /* tx_allow_fwcmd_ch() refreshes SDIO H2C availability up to five times
   * when the static header temporarily consumed pages.  Do the same before
   * attempting each explicitly permitted section write.
   */

  ret = k1_rtl8852bs_h2c_resource_wait(required_pages, &available_pages);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: FW section0 packet available=");
  k1_early_puthex(available_pages);
  k1_early_puts(" required=");
  k1_early_puthex(required_pages);
  k1_early_puts(" FIFO=");
  k1_early_puthex(fifo_address);
  k1_early_puts(" bytes=");
  k1_early_puthex(K1_RTL8852BS_FW_SECTION0_TRANSFER_SIZE);
  k1_early_puts(" TXD0=");
  k1_early_puthex(k1_rtl8852bs_read_le32(packet));
  k1_early_puts(" TXD2=");
  k1_early_puthex(k1_rtl8852bs_read_le32(packet + 8));
  k1_early_puts("\r\n");

  ret = k1_sdio_wifi_write(1, fifo_address, false, packet,
                           K1_RTL8852BS_FW_SECTION0_TRANSFER_SIZE);
  if (ret < 0)
    {
      failed_address = fifo_address;
      goto error;
    }

  if (follow_packet != K1_RTL8852BS_SECTION0_FOLLOW_NONE)
    {
      if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE)
        {
          follow_count = 1;
        }
      else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET)
        {
          follow_count = 3;
        }
      else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET)
        {
          follow_count = 2;
        }
      else
        {
          follow_count = 1;
        }

      for (follow_index = 0; follow_index < follow_count; follow_index++)
        {
          if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE)
            {
              ret = k1_rtl8852bs_fw_section0_tail_packet_build(
                packet, &fifo_address, &required_pages);
              follow_transfer_size =
                K1_RTL8852BS_FW_SECTION0_TAIL_TRANSFER_SIZE;
            }
          else if (follow_packet ==
                     K1_RTL8852BS_SECTION0_FOLLOW_SECOND_PACKET ||
                   follow_index == 0)
            {
              ret = k1_rtl8852bs_fw_section0_second_packet_build(
                packet, &fifo_address, &required_pages);
              follow_transfer_size =
                K1_RTL8852BS_FW_SECTION0_SECOND_TRANSFER_SIZE;
            }
          else if (follow_packet ==
                     K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET ||
                   follow_index == 1)
            {
              ret = k1_rtl8852bs_fw_section0_third_packet_build(
                packet, &fifo_address, &required_pages);
              follow_transfer_size =
                K1_RTL8852BS_FW_SECTION0_THIRD_TRANSFER_SIZE;
            }
          else
            {
              ret = k1_rtl8852bs_fw_section0_fourth_packet_build(
                packet, &fifo_address, &required_pages);
              follow_transfer_size =
                K1_RTL8852BS_FW_SECTION0_FOURTH_TRANSFER_SIZE;
            }

          if (ret < 0)
            {
              goto error;
            }

          ret = k1_rtl8852bs_h2c_resource_wait(required_pages,
                                                &available_pages);
          if (ret < 0)
            {
              failed_address = K1_RTL8852BS_SDIO_TXPG_WP;
              goto error;
            }

          if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE)
            {
              k1_early_puts("K1 Wi-Fi GPL: FW section0 tail packet "
                            "available=");
            }
          else if (follow_packet ==
                     K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET &&
                   follow_index == 2)
            {
              k1_early_puts("K1 Wi-Fi GPL: FW section0 fourth packet "
                            "available=");
            }
          else if ((follow_packet ==
                      K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET &&
                    follow_index != 0) ||
                   (follow_packet ==
                      K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET &&
                    follow_index == 1))
            {
              k1_early_puts("K1 Wi-Fi GPL: FW section0 third packet "
                            "available=");
            }
          else
            {
              k1_early_puts("K1 Wi-Fi GPL: FW section0 second packet "
                            "available=");
            }

          k1_early_puthex(available_pages);
          k1_early_puts(" required=");
          k1_early_puthex(required_pages);
          k1_early_puts(" FIFO=");
          k1_early_puthex(fifo_address);
          k1_early_puts(" bytes=");
          k1_early_puthex(follow_transfer_size);
          k1_early_puts(" TXD0=");
          k1_early_puthex(k1_rtl8852bs_read_le32(packet));
          k1_early_puts(" TXD2=");
          k1_early_puthex(k1_rtl8852bs_read_le32(packet + 8));
          k1_early_puts("\r\n");

          ret = k1_sdio_wifi_write(1, fifo_address, false, packet,
                                   follow_transfer_size);
          if (ret < 0)
            {
              failed_address = fifo_address;
              goto error;
            }
        }
    }

  ret = k1_rtl8852bs_fwdl_cleanup(&cleanup_failed_address);
  if (ret < 0)
    {
      failed_address = cleanup_failed_address;
      goto error;
    }

  cleanup_needed = false;
  kmm_free(packet);
  if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 fourth packet "
                    "complete\r\n");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 third packet "
                    "complete\r\n");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_SECOND_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 second packet "
                    "complete\r\n");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE)
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 tail packet "
                    "complete\r\n");
    }
  else
    {
      k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 FW section0 packet "
                    "complete\r\n");
    }

  return OK;

error:
  if (cleanup_needed)
    {
      cleanup_failed_address = 0;
      cleanup_ret = k1_rtl8852bs_fwdl_cleanup(&cleanup_failed_address);
      if (cleanup_ret < 0)
        {
          k1_early_puts("K1 Wi-Fi GPL: FW section0 packet cleanup error "
                        "address=");
          k1_early_puthex(cleanup_failed_address);
          k1_early_puts(" error=");
          k1_early_puthex((uintreg_t)-cleanup_ret);
          k1_early_puts("\r\n");
        }
    }

  kmm_free(packet);
  if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: FW section0 fourth packet diagnostic "
                    "error address=");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: FW section0 third packet diagnostic "
                    "error address=");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_SECOND_PACKET)
    {
      k1_early_puts("K1 Wi-Fi GPL: FW section0 second packet diagnostic "
                    "error address=");
    }
  else if (follow_packet == K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE)
    {
      k1_early_puts("K1 Wi-Fi GPL: FW section0 tail packet diagnostic error "
                    "address=");
    }
  else
    {
      k1_early_puts("K1 Wi-Fi GPL: FW section0 packet diagnostic error "
                    "address=");
    }

  k1_early_puthex(failed_address);
  k1_early_puts(" error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");
  return ret;
}

int k1_rtl8852bs_fwdl_fw_section0_packet_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_section0_packets_diagnostic(
    K1_RTL8852BS_SECTION0_FOLLOW_NONE);
}

int k1_rtl8852bs_fwdl_fw_section0_tail_packet_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_section0_packets_diagnostic(
    K1_RTL8852BS_SECTION0_FOLLOW_LEGACY_28_BYTE);
}

int k1_rtl8852bs_fwdl_fw_section0_second_packet_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_section0_packets_diagnostic(
    K1_RTL8852BS_SECTION0_FOLLOW_SECOND_PACKET);
}

int k1_rtl8852bs_fwdl_fw_section0_third_packet_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_section0_packets_diagnostic(
    K1_RTL8852BS_SECTION0_FOLLOW_THIRD_PACKET);
}

int k1_rtl8852bs_fwdl_fw_section0_fourth_packet_diagnostic(void)
{
  return k1_rtl8852bs_fwdl_section0_packets_diagnostic(
    K1_RTL8852BS_SECTION0_FOLLOW_FOURTH_PACKET);
}

int k1_rtl8852bs_dle_scc_init(void)
{
  uint32_t value;
  unsigned int index;
  uint16_t failed_address = 0;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 DLE SCC init begin\r\n");

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_DMAC_FUNC_EN,
                                     K1_RTL8852BS_DLE_ENABLE_MASK, 0);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_DMAC_FUNC_EN;
      goto error;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_DMAC_CLK_EN, 0,
                                     K1_RTL8852BS_DLE_ENABLE_MASK);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_DMAC_CLK_EN;
      goto error;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_WDE_PKTBUF_CFG, &value);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_WDE_PKTBUF_CFG;
      goto error;
    }

  value &= ~K1_RTL8852BS_DLE_PKTBUF_FIELDS;
  value |= 126u << 16;
  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_WDE_PKTBUF_CFG, value);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_WDE_PKTBUF_CFG;
      goto error;
    }

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_PLE_PKTBUF_CFG, &value);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_PLE_PKTBUF_CFG;
      goto error;
    }

  value &= ~K1_RTL8852BS_DLE_PKTBUF_FIELDS;
  value |= 688u << 16 | 1u << 8 | 1u;
  ret = k1_rtl8852bs_mac_write32(K1_RTL8852BS_PLE_PKTBUF_CFG, value);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_PLE_PKTBUF_CFG;
      goto error;
    }

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_dle_scc_quotas) /
               sizeof(g_k1_rtl8852bs_dle_scc_quotas[0]);
       index++)
    {
      ret = k1_rtl8852bs_mac_write32(
        g_k1_rtl8852bs_dle_scc_quotas[index].address,
        g_k1_rtl8852bs_dle_scc_quotas[index].value);
      if (ret < 0)
        {
          failed_address = g_k1_rtl8852bs_dle_scc_quotas[index].address;
          goto error;
        }
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_DMAC_FUNC_EN, 0,
                                     K1_RTL8852BS_DLE_ENABLE_MASK);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_DMAC_FUNC_EN;
      goto error;
    }

  ret = k1_rtl8852bs_dle_wait_ready(K1_RTL8852BS_WDE_INI_STATUS);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_WDE_INI_STATUS;
      goto error;
    }

  ret = k1_rtl8852bs_dle_wait_ready(K1_RTL8852BS_PLE_INI_STATUS);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_PLE_INI_STATUS;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 DLE SCC init complete\r\n");
  return OK;

error:
  (void)k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_DMAC_FUNC_EN,
                                     K1_RTL8852BS_DLE_ENABLE_MASK, 0);
  k1_rtl8852bs_log_post_power_error(failed_address, ret);
  return ret;
}

int k1_rtl8852bs_hci_fc_init(void)
{
  uint32_t value;
  unsigned int index;
  uint16_t failed_address = 0;
  int ret;

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 HCI flow-control init begin\r\n");

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_HCI_FC_CTRL,
                                     K1_RTL8852BS_HCI_FC_DISABLE_MASK, 0);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HCI_FC_CTRL;
      goto error;
    }

  for (index = 0;
       index < sizeof(g_k1_rtl8852bs_hci_fc_registers) /
               sizeof(g_k1_rtl8852bs_hci_fc_registers[0]);
       index++)
    {
      ret = k1_rtl8852bs_hci_fc_write_checked(
        g_k1_rtl8852bs_hci_fc_registers[index].address,
        g_k1_rtl8852bs_hci_fc_registers[index].value);
      if (ret < 0)
        {
          failed_address = g_k1_rtl8852bs_hci_fc_registers[index].address;
          goto error;
        }
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_HCI_FC_CTRL,
                                     K1_RTL8852BS_HCI_FC_CONFIG_MASK,
                                     K1_RTL8852BS_HCI_FC_MODE_SDIO);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HCI_FC_CTRL;
      goto error;
    }

  ret = k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_HCI_FC_CTRL,
                                     K1_RTL8852BS_HCI_FC_DISABLE_MASK,
                                     K1_RTL8852BS_HCI_FC_ENABLE_VALUE);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HCI_FC_CTRL;
      goto error;
    }

  up_udelay(10);

  ret = k1_rtl8852bs_mac_read32(K1_RTL8852BS_HCI_FC_CTRL, &value);
  if (ret < 0)
    {
      failed_address = K1_RTL8852BS_HCI_FC_CTRL;
      goto error;
    }

  if ((value & (K1_RTL8852BS_HCI_FC_CONFIG_MASK |
                K1_RTL8852BS_HCI_FC_DISABLE_MASK)) !=
      K1_RTL8852BS_HCI_FC_ENABLE_VALUE)
    {
      k1_early_puts("K1 Wi-Fi GPL: HCI FC control readback=");
      k1_early_puthex(value);
      k1_early_puts("\r\n");
      ret = -EIO;
      failed_address = K1_RTL8852BS_HCI_FC_CTRL;
      goto error;
    }

  k1_early_puts("K1 Wi-Fi GPL: RTL8852BS2 HCI flow-control init "
                "complete\r\n");
  return OK;

error:
  (void)k1_rtl8852bs_mac_update_bits(K1_RTL8852BS_HCI_FC_CTRL,
                                     K1_RTL8852BS_HCI_FC_DISABLE_MASK, 0);
  k1_rtl8852bs_log_post_power_error(failed_address, ret);
  return ret;
}

int k1_rtl8852bs_first_cmd53_diagnostic(void)
{
  uint32_t register_value;
  int ret;

  /* This exactly recreates the first CMD53 request produced by the vendor
   * mac_hal_init() sequence.  After mac_pwr_on_sdio_8852b(), hci_func_en()
   * reads R_AX_HCI_FUNC_EN (0x8380).  That is a WLAN-platform register, so
   * r_indir_cmd53_sdio() submits this twelve-byte write to the SDIO indirect
   * window, then polls its result with an eight-byte CMD53 read.  This only
   * reads a MAC register; no firmware or MAC registration is involved here.
   */

  ret = k1_rtl8852bs_cmd53_indirect_read32(K1_RTL8852BS_HCI_FUNC_EN,
                                            &register_value);
  k1_early_puts("K1 Wi-Fi GPL: first vendor CMD53 error=");
  k1_early_puthex((uintreg_t)(ret < 0 ? -ret : 0));
  k1_early_puts("\r\n");

  if (ret >= 0)
    {
      k1_early_puts("K1 Wi-Fi GPL: first vendor CMD53 value=");
      k1_early_puthex(register_value);
      k1_early_puts("\r\n");
    }

  return ret;
}

#endif /* CONFIG_K1_RTL8852BS2_GPL_BOOTSTRAP */
