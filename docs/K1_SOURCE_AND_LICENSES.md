# K1 来源与许可证清单

更新时间：2026-08-29

## 本比赛仓

| 范围 | 来源/作者方式 | 许可证 | 处理 |
|---|---|---|---|
| `chip/k1/`（除下项） | 本项目实现，复用 NuttX RISC-V 公共 API 和 in-tree 驱动模式 | Apache-2.0 | 每个代码文件带 SPDX |
| `chip/k1/k1_rtl8852bs_netdev.[ch]` | 本项目实现的 `wlan0` 扫描 netdev，遵循 NuttX in-tree bcmf 的 Apache-2.0 wireless-ioctl 模式，只调用 GPL 组件的公开 API | Apache-2.0 | 文件名前缀是 `k1_rtl8852bs_` 但不含任何 Realtek 派生内容：无原厂寄存器表、无原厂结构体、无原厂代码改编。它与 GPL 组件的边界只是 `k1_rtl8852bs_gpl.h` 中的公开函数与结构体声明，与 `board/k1/muse_pi_pro/src/k1_wireless.c` 的调用方式相同 |
| `chip/k1/k1_rtl8852bs_gpl.[ch]`、`chip/k1/k1_rtl8852bs_u2_nicce_fw.inc`、`chip/k1/k1_rtl8852bs_phy_reg_8852b.inc`、`chip/k1/k1_rtl8852bs_rf_radio_a_8852b.inc`、`chip/k1/k1_rtl8852bs_rf_radio_b_8852b.inc`、`chip/k1/k1_rtl8852bs_rf_headline_8852b.inc` | Realtek RTL8852BS 的 GPL 上电表、间接 CMD53 访问、HCI/DMAC、SDIO pre-init、DLE/SCC、HCI flow-control、FWDL、MAC-core、BB/RF release、默认 BB PHY CR 参数镜像、按本板 RFE/CV 选出的 RF radio A/B 参数镜像与 headline 校验表，以及完整 U2 NICCE 固件映像适配 | GPL-2.0-only | 仅在 `CONFIG_K1_RTL8852BS2_GPL_BOOTSTRAP=y` 链接；诊断功能需显式启用配置；三个 RF `.inc` 由 `tools/k1_rtl8852bs_rf_table_gen.py` 生成，不得手改；保留 Realtek 版权、SPDX、来源和改编说明 |
| `LICENSES/GPL-2.0-only.txt` | GNU GPL v2 正文 | GPL-2.0-only | 与 GPL 组件一同交付 |
| `board/k1/muse_pi_pro/` | 本项目实现 | Apache-2.0 | 每个代码文件带 SPDX |
| `tools/` 中 K1 工具 | 本项目实现 | Apache-2.0 | 每个脚本带 SPDX |
| `docs/K1_*.md` | 本项目编写 | Apache-2.0（仓库级） | 受根目录 `LICENSE` 约束 |
| manifest/工作流 | 比赛仓配置 | Apache-2.0（仓库级） | 不嵌入固件 |

根目录 `LICENSE` 是 Apache License 2.0。除上述明确隔离的 GPL-2.0-only 组件及其完整
341216-byte U2 NICCE 映像外，K1 源码没有引入厂商 SDK 头文件或不可再分发资料。

## RTL8852BS2 GPL 组件

`k1_rtl8852bs_gpl.[ch]` 是一个有意保持很小边界的 GPL-2.0-only 组件。它从
SpacemiT Linux 的 Realtek RTL8852BS 驱动改编了 SDIO MAC 上电表、CMD52 bootstrap、
power-on 状态的 32-bit 寄存器访问、SDIO pre-init、DLE/SCC page-pool/quota 初始化和
掩码读改写语义：

- 来源仓：[spacemit-com/linux-6.6](https://github.com/spacemit-com/linux-6.6)，
  分支 `k1-bl-v2.2.y`，revision `31c449aeaad8c7759bc983ca0e26946e5b6746dc`；
- 来源文件：
  `drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/mac/mac_ax/mac_8852b/pwr_seq_8852b.c`、
  `.../mac_8852b/_sdio_8852b.c`、`.../mac_8852b/init_8852b.c`、
  `.../mac_8852b/dle_8852b.c`、`.../mac_8852b/hci_fc_8852b.c`、
  `.../mac_8852b/efuse_8852b.c`、`.../mac_ax/dle.c`、
  `.../mac_ax/hci_fc.c`、`.../mac_ax/_sdio.c`、`.../mac_ax/efuse.c`、
  `.../mac_ax/fwdl.c`、`.../mac_ax/fwcmd.c`、`.../mac_ax/init.c`、
  `.../mac_ax/hw.c`、`.../mac_ax/hw.h`、
  `.../mac_ax/pwr.c`、`.../mac_ax/trxcfg.c`、`.../mac_ax/role.c`、
  `.../mac_ax/addr_cam.c`、`.../mac_ax/fwofld.c`、
  `.../hal_api_mac.c`、`.../hal_rx.c`、`.../hal_scanofld.c`、
  `.../phl_scanofld.c`、`.../mac_ax/rx_filter.c`、
  `.../mac_ax/otpkeysinfo.[ch]`、
  `.../mac_8852b/trx_desc_8852b.c`、
  `.../mac/rxdesc.h`、`.../mac/txdesc.h`、`.../mac/type.h`、
  `.../mac/mac_def.h`、`.../mac/fw_ax/inc_hdr/fwcmd_intf.h`、
  `.../mac/include/AutoGen_hw_info/g6/hw_info_ax.h`、
  `.../fw_ax/rtl8852b/hal8852b_fw.c`、
  `.../mac/hci_reg_ax.h`、`.../mac/mac_reg_ax.h`、
  `.../phy/bb/halbb_hw_cfg.c`、
  `.../phy/bb/halbb_8852b/halbb_hwimg_8852b.c`、
  `.../phy/bb/halbb_8852b/halbb_hwimg_raw_data_8852b.h`、
  `.../phy/rf/halrf_hw_cfg.c`、`.../phy/rf/halrf_interface.c`、
  `.../phy/rf/halrf_8852b/halrf_hwimg_8852b.c`、
  `.../phy/rf/halrf_8852b/halrf_hwimg_raw_data_8852b.h`、
  `.../phy/rf/halrf_8852b/halrf_reg_cfg_8852b.c`、
  `.../phy/rf/halrf_8852b/halrf_efuse_8852b.h`；
- 改编：删除 PCIe-only 的 `0x0071` 项，只保留 RTL8852B 的 SDIO `mac_pwron` 表；
  power-on table 通过现有 K1 SDIO Function 1 CMD52 API 完成；`hci_func_en()`、
  `dmac_func_pre_en_8852b()` 和 `sdio_pre_init()` 的有界对齐 32-bit 寄存器访问通过
  四次递增 CMD52 API 完成，因为这些寄存器均在 Function 1 的 17-bit 地址空间内。
  `CONFIG_K1_RTL8852BS2_DLE_SCC_DIAGNOSTIC` 额外以有界间接 CMD53 read/write 配置
  SDIO/SCC 的 WDE 126 页、PLE 688 页、15 组 quota，并等待 WDE/PLE ready bit；失败时
  会 best-effort 清除 DLE enable。`CONFIG_K1_RTL8852BS2_HCI_FC_DIAGNOSTIC` 额外按
  `hfc_init(adapter, 1, 1, 1)` 配置 8852B SDIO HCI flow-control 的六个 active channel、
  112 页公共池和 40 页 H2C pre-cost，并在错误时关闭 HCI/H2C enable 位。厂商配置中为
  `IGNORE` 的 SD reset 不执行。`CONFIG_K1_RTL8852BS2_FW_PREBOOT_DIAGNOSTIC` 额外改编
  `mac_enable_cpu(..., dlfw=1)` 和 `fwdl_phase0()`：清除 SER/FWDL 锁存状态、开启 WCPU，
  轮询 H2C-ready 后即按 `mac_disable_cpu()` 的位清除顺序回退。为保持边界可控，尚未移植
  `fwdl_precheck()` 的 DLE debug-port 查询，也没有导入 802.11
  协议栈、网络设备或蓝牙代码。`CONFIG_K1_RTL8852BS2_FW_IMAGE_LAYOUT_DIAGNOSTIC` 额外按
  `fwhdr_hdr_parser()`、`fwhdr_section_parser()` 和 `mac_get_dynamic_hdr_ax()` 解析同一 U2
  NIC image 的 80-byte static header、80-byte dynamic header 和三条 section record；它验证
  header/dynamic entry 边界、checksum 的 8-byte 扩展，以及 section offsets 是否落在原厂
  276544-byte image 内，不执行任何 SDIO 写或 WCPU 状态变化。
  `CONFIG_K1_RTL8852BS2_FW_MSS_EFUSE_DIAGNOSTIC` 额外改编
  `enable_efuse_sw_pwr_cut_8852b(..., false)`、`read_hw_efuse()` 与
  `disable_efuse_sw_pwr_cut_8852b(..., false)`：在 Wi-Fi eFuse read power gate 临时开启期间，
  只读取 `0x5ec` 和 `0x5ed` 并依 `get_mss_keypool_index()` 解码 MSS device/customer/key
  selector。该 selector 格式属于带 `MSSKPOOL` trailer 的较新 image；当前 U2 NIC image 不走此
  分支。它不触及 write-only unlock code、eFuse data programming、burn control、MSS 签名导入或
  firmware FIFO 写，最后总会尝试撤销 read power gate。
  `CONFIG_K1_RTL8852BS2_FW_MSS_LEGACY_SIGNATURE_DIAGNOSTIC` 则适配当前 U2 NIC image 的实际
  `MSSC=2` 分支：从 `array_8852b_u2_nic[0x43440..0x4383f]` 导入两份共 1024 bytes 的 trailer
  signature（SHA-256 `71632fde61d80f1230c387c354e72aba717d1cbdb864f79a7c35450d8b25ac48`），按
  `__mss_index()` 和 `otpkeysinfo.[ch]` 的两条 OTP mapping 选择 index 0 或 1，并验证 source
  offset、secure-section target offset 和长度。诊断只读取这两个 eFuse byte 并打印结果；它不会
  将 signature 复制进 secure section，也不会发送 firmware FIFO packet。
  运行期 RX helper 额外按 `rxdesc.h`、`type.h`、`trx_desc_8852b.c` 和
  `hw_info_ax.h` 解析 16/32-byte RX descriptor、`RPKT_LEN`、driver-info、
  shift、type、CRC/ICV flag 和 8-byte aggregate 对齐。它只读取已由
  `R_AX_SDIO_RX_REQ_LEN` 报告长度的固定 RX FIFO；没有启用 SDIO interrupt、
  构造 TX descriptor、注册 802.11 MAC 或 network device。
  `CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_RX_DIAGNOSTIC` 在此边界内按
  `rtw_recv.c` 的 802.11 management header/IE 规则识别 Beacon 与 Probe Response，
  并按 `fwcmd.c` 的 scan-offload reason 6 结构读取 per-channel RX count。它只保存一个
  bounded BSSID/SSID 结果，仍不提供 802.11 protocol stack、`wlan0`、关联、WPA、DHCP
  或联网功能。扫描开始前，另按 `phl_scanofld.c`、`hal_rx.c`、`hal_api_mac.c` 和
  `mac_ax/rx_filter.c` 暂存并切换 band-0 `R_AX_RX_FLTR_OPT` 和
  `R_AX_MGNT_FLTR`：关闭 CAM/Beacon 限制并把全部 management subtype 转到 host；scan-end、
  超时或 H2C 失败后总是读回并恢复原值。`R_AX_RCR`、`R_AX_PLCP_HDR_FLTR` 仅用于前后快照
  日志，不被这条诊断改写。此修改已经通过主机构建，尚待实体板 Beacon/BSS RX 验证。
  `CONFIG_K1_RTL8852BS2_FW_H2C_TX_DIAGNOSTIC` 额外改编
  `ud_fs_8852b()`、`tx_allow_fwcmd_ch()`、`tx_cmd_addr_sdio()` 和
  `txdes_proc_h2c_fwdl_8852b()`：它仅读取 Function 1 `0x1110` 的 28-byte SDIO
  TX-page 状态表，在 RAM 构造 header-only FWDL H2C envelope、24-byte descriptor 和
  FIFO address；不调用 CMD53 FIFO 写入，不携带 firmware payload。
  `CONFIG_K1_RTL8852BS2_RUNTIME_DATA_TX_DIAGNOSTIC` 额外改编
  `txdes_proc_data_8852b()`、`chk_rqd_pg_num()`、`chk_fs_enuf()`、
  `tx_cmd_addr_sdio()` 和 `ud_fs_8852b()`：它在 RAM 构造 normal-data 24-byte
  descriptor，并以同一 28-byte window 读取 ACH used-page 与 public write-page
  snapshot。当前仅开放已由本项目 HCI FC profile 配置的 ACH0--3/B0MG/B0HI；没有调用
  TX FIFO CMD53、不保留 page reservation、不配置 MACID/peer/security，也不创建
  network device。
  `CONFIG_K1_RTL8852BS2_RUNTIME_MAC_CORE_DIAGNOSTIC` 额外改编
  `trxcfg.c` 的 `mpdu_proc_init()`、`tmac_init()`、`trxptcl_init()`、
  `rmac_init()`、`cmac_com_init()`、`ptcl_init()`、`cmac_dma_init()` 与
  `spatial_reuse.c` 的 `spatial_reuse_init()` 中不依赖
  scheduler、address-CAM、role、station 或 security state 的 RTL8852B band-0
  `MAC_AX_TRX_SW_MODE` 字段，并使用 `mac_reg_ax.h` 的寄存器/bit 定义。每一个
  update 都在 CMD53 indirect access 后读回验证；它不设置 MACID/peer/key，不创建
  netdev，也不发送 802.11 frame。
  `CONFIG_K1_RTL8852BS2_RUNTIME_BB_RF_DIAGNOSTIC` 额外改编
  `mac_ax/hw.c` 的 `set_enable_bb_rf(..., 1)`：解除 BB reset、设置 ZCDC 字段、执行
  AFE-digital 的 `1 -> 0 -> 1` edge、写入并读回两路 WLAN RFC XTAL SI 值，并设置 PHY
  register cycle。该有界诊断不导入 PHY/RF parameter image、不运行 RF calibration，也不
  进行扫描、关联、netdev 注册或任何持久化写入。
  `CONFIG_K1_RTL8852BS2_RUNTIME_CONTROL_PLANE_DIAGNOSTIC` 额外改编
  `role.c` 的 `mac_fw_role_maintain()` 和 `mac_h2c_join_info()`，以及
  `fwcmd_intf.h` 的 4-byte/12-byte payload bit layout。它以公开输入结构在 RAM 构造
  `MAC/MEDIA_RPT/FWROLE_MAINTAIN` 和 `MAC/MEDIA_RPT/JOININFO`，对 RTL8852B
  固定 band-0 与 WMM 范围做校验，并将 JOININFO 的两个保留 dword 置零。该诊断不写
  SDIO FIFO、不等待 C2H、不创建 firmware role、不配置 address/BSSID CAM，也不创建
  netdev；真实发送只能由后续含 MAC 地址、BSSID、CAM 和完成确认状态的控制面执行。
  `CONFIG_K1_RTL8852BS2_RUNTIME_ADDRESS_CAM_DIAGNOSTIC` 额外改编
  `addr_cam.c` 的 `fill_addr_cam_info()` 与 `fill_bssid_cam_info()`；它以明确的
  self MAC、target MAC 和 BSSID 构造 60-byte `MAC/ADDR_CAM_UPDATE/ADDRCAM_INFO`
  payload，复用原厂 mask-dependent SMA/TMA XOR hash 与 BSSID CAM mask 规则。当前仅
  接受 RTL8852B band-0 的 no-link STA（真实 self MAC、零 target/BSSID）以及非零单播
  STA/AP create 子集，所有 security/WOL/reserved 字段为零；它不写 SDIO FIFO、不分配
  CAM index、不等待 C2H，也不创建 netdev。
  `CONFIG_K1_RTL8852BS2_RUNTIME_ROLE_CAM_DONE_ACK_DIAGNOSTIC` 进一步按 `role.c`、
  `addr_cam.c` 和 `fwcmd.c` 的既有顺序，向固定地址 F1 H2C FIFO 提交一条 no-link STA
  `FWROLE_MAINTAIN` 和一条 address/BSSID CAM H2C，并等待通用
  `MAC/FW_INFO/DONE_ACK`。回执 payload 的 category/class/function/return/sequence
  均按 `fwcmd_intf.h` 解码，且 firmware return 必须为零。该诊断只使用 eFuse self MAC，
  不创建 peer/BSSID、802.11 MAC 或 netdev，不扫描、不关联、不联网，且 role/CAM 在复位后
  消失；它不构成持久角色管理或完整 Wi-Fi 协议栈。
  `CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_CHANNEL_DIAGNOSTIC` 进一步按
  `phl_scanofld.c`、`hal_api_mac.c`、`fwofld.c` 和 `mac_def.h` 组装一条
  `MAC/FW_OFLD/ADD_SCANOFLD_CH` H2C。它只包含 channel 1、band 0、20 MHz、100 ms
  的被动条目；无 link 的原厂路径还会设置 `c2h_notify_enterCH` 与 `pause_tx_data`，但不会
  请求 probe/null/data/additional frame。诊断只检查 table 的 done-ack，不发送后续
  `SCANOFLD` start，不改 RF、不解析 beacon、不注册 Wi-Fi MAC/netdev，也不写持久介质。
  `CONFIG_K1_RTL8852BS2_RF_CONTEXT_DIAGNOSTIC` 按 `halrf_efuse_8852b.h` 的 logical
  eFuse 版图与 `mac_reg_ax.h` 的 `R_AX_SYS_CFG1`，在固件下载之前只读地报告本板 RF
  上下文：chip cut version（`0x00f1` 高 nibble）、RFE type `0x2ca`、board option
  `0x2c1`、channel plan `0x2b8`、XTAL `0x2b9`、thermal `0x2d0/0x2d1`、TSSI
  de-emphasis `0x210..0x259` 与 RX gain compensation `0x2d4..0x2dd`。未烧写的
  cell 会与原厂会替换的默认值一起打印，所以没有 RF 校准数据的板子是被报告出来而不是
  被假定。它不写 eFuse、不写寄存器、不注册 netdev，也不写任何持久介质。
  `CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC` 额外改编 `halrf_hw_cfg.c` 的
  `halrf_config_radio()`、`halrf_hwimg_8852b.c` 的 `halrf_sel_headline_8852b()` 与
  `halrf_config_8852b_radio_{a,b}_reg()`、`halrf_reg_cfg_8852b.c` 的
  `halrf_cfg_rf_radio_{a,b}_8852b()`，以及 `halrf_interface.c` 的 `halrf_wrf()`。
  原厂 `array_mp_8852b_radio{a,b}[]` 是按 {RFE type, chip CV} 选择分支的条件表，整表
  不进入固件；`tools/k1_rtl8852bs_rf_table_gen.py` 在主机上逐字复现原厂选择逻辑，只
  输出本板命中的那一条分支和一张 headline 校验表。运行时先用上面报告的真实 RFE/CV
  重放原厂 headline 选择，只有当命中行标记该 headline 恰好对应编译进来的两张镜像时才
  提交；否则拒绝，不写错误的 RF 参数。提交使用与 BB PHY CR 相同的
  `MAC/FW_OFLD/CMD_OFLD_PKT` 协议，但按原厂语义使用 RF source、radio path 与 MASKRF
  掩码；带 BIT(16) 的 RF D-die 项按 `halrf_wrf()` 改写为 `0xe000/0xf000` BB aperture
  写入。每个有界批次都必须收到 `result` 为零的 `CMD_OFLD_RSP` C2H 才继续，首个失败批次
  立即停止且不重放固件可能已经接收的批次。原厂 OUTSRC class 8/9 radio-to-FW 页上传
  有意不发送：`halrf_config_rf_parameter()`（即 `init_rf_reg` 阶段）不调用它，它也没有
  C2H 回执契约。该诊断不运行 RF calibration 收敛、不扫描、不关联、不注册 netdev，也不写
  持久介质。
  `CONFIG_K1_RTL8852BS2_FW_HEADER_PACKET_DIAGNOSTIC` 再额外引入
  `array_8852b_u2_nic` 的前 80 bytes（完整 firmware header 的静态部分），将其与
  24-byte descriptor 和 8-byte FWDL H2C header 组成唯一的 112-byte、固定地址
  CMD53 FIFO 写入。它只轮询 `R_AX_WCPU_FW_CTRL[2]` 后清理 WCPU，不携带 dynamic
  header 或任何 firmware section，也无法构成完整 firmware image。
  `CONFIG_K1_RTL8852BS2_FW_SECTION_PACKET_DIAGNOSTIC` 额外引入同一数组 offset
  `0x00a0` 起的 2020 bytes，保存在同一 GPL C 组件中；它只将该切片与一个 24-byte
  FWDL descriptor 组成 2044-byte packet，并按原厂 SDIO path 补齐为 2048-byte固定地址
  CMD53 写入。该配置在同一个 FWDL 会话中重发 static header 后
  只允许这一笔 section 写入，随后立即清理 WCPU；不携带第二 packet、checksum、完整 image、
  802.11 协议栈或蓝牙代码。历史兼容符号
  `CONFIG_K1_RTL8852BS2_FW_SECTION0_TAIL_PACKET_DIAGNOSTIC` 仅在上述首包之后追加 source
  offset `0x0884` 的固定 28-byte transport probe（与 descriptor 一起按 8 bytes 对齐为
  56-byte CMD53）；它不是 section 尾包。原厂 section zero 实际为 `0x3f420` bytes。
`CONFIG_K1_RTL8852BS2_FW_SECTION0_SECOND_PACKET_DIAGNOSTIC` 则在同一会话中追加该 offset
起完整的第二个 2020-byte packet。`CONFIG_K1_RTL8852BS2_FW_SECTION0_THIRD_PACKET_DIAGNOSTIC`
再追加 offset `0x1068..0x184b` 的第三个 2020-byte packet。
`CONFIG_K1_RTL8852BS2_FW_SECTION0_FOURTH_PACKET_DIAGNOSTIC` 再追加 offset
`0x184c..0x202f` 的第四个 2020-byte packet（SHA-256
`c9f5153472dd1c6cb0b521c21d5f06d1bc9fcf8be31ba606d72ecf97f23f9d45`）；四包后立即清理
WCPU。第四包代码已与来源逐字节核对，并已通过 RAM-only 实板验收：完整记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-fourth-packet-20260822T062136Z.log`
（SHA-256 `70e06d7ab15c7e2452b5b354309b417c32ce64d6873fb9573230d2abd8cefc30`）。所有这些有界
诊断都不检查 firmware-ready，也不引入 802.11 协议栈或蓝牙代码。

  `CONFIG_K1_RTL8852BS2_FW_FULL_DOWNLOAD_DIAGNOSTIC` 当前纳入同一 revision 的完整
  `array_8852b_u2_nicce`，以机械提取的 `k1_rtl8852bs_u2_nicce_fw.inc` 形式保留在该 GPL
  组件内。提取后映像为 341216 bytes，SHA-256 为
  `64e6f0c5744f10be980d2031b5001f83d02074598b0b92dbafaaa9febe0834d0`。下载路径先解析
  static/dynamic header、三条 section 和 NICCE legacy MSS trailer，再按原厂 H2C FIFO 协议发送三段；
  它只分配 2048-byte packet buffer，并只在当前 secure-section packet 中合入所选的 512-byte
  signature。成功条件是 `R_AX_WCPU_FW_CTRL[7:5] == 7`；失败时关闭下载/WCPU/clock 状态。
  目标始终是易失 WCPU RAM，不写 eFuse、eMMC、SPI flash 或 U-Boot environment。NICCE image
  已经通过 RAM-only 实板验收；即使 firmware-ready 成功，也不表示 Wi-Fi MAC、`netdev`、扫描、
  关联、联网或蓝牙 firmware download 已完成。

### 2026-08-24：scan-offload runtime 改用 U2 NICCE 映像

`CONFIG_K1_RTL8852BS2_FW_FULL_DOWNLOAD_DIAGNOSTIC` 当前运行的映像是同一 GPL
source revision 的 `array_8852b_u2_nicce`，机械提取为
`chip/k1/k1_rtl8852bs_u2_nicce_fw.inc`。其长度为 341216 bytes，SHA-256 为
`64e6f0c5744f10be980d2031b5001f83d02074598b0b92dbafaaa9febe0834d0`。提取工具支持
显式 array symbol 和长度，当前命令为：

```text
tools/extract_rtl8852bs_u2_nic_fw.sh hal8852b_fw.c \
  chip/k1/k1_rtl8852bs_u2_nicce_fw.inc array_8852b_u2_nicce 341216
```

选择 NICCE 是 capability table 的明确要求：同文件集的
`hal8852b_fw_cap.h` 只在 `MAC_FW_CATEGORY_NICCE` 定义
`FW_CONFIG_SCAN_OFFLOAD`，普通 `MAC_FW_CATEGORY_NIC` 没有该能力。NICCE 的下载格式仍为
`0xa0` header、三段 section、`MSSC=2` legacy trailer，section lengths 分别是
`0x4f108`、`0x3738`、`0x800`，trailer 位于 `0x530e0..0x534df`。因此现有 FWDL 传输
状态机可复用；实现从同一 NICCE array 派生 header、dynamic header、section 和所选 MSS
signature，不再保留或传输旧 NIC 的固定切片。NICCE trailer 的 SHA-256 为
`e2c25fc4e4ee85da487a4d522d397fdd7c7812ee94639bd1b8d0ec0a2572619c`。

历史段落中关于 `array_8852b_u2_nic` 的小包验证只记录当时的实测；当前可构建 profile
与 RAM-only package 以 NICCE 为准。此变更不扩大持久化权限：固件仍仅下载到易失 WCPU RAM，
不写 eMMC、SPI flash、eFuse 或 U-Boot environment。

启用 `CONFIG_K1_RTL8852BS2_GPL_BOOTSTRAP=y` 的无线 ELF 链接了该 GPL-2.0-only
组件。分发该 ELF 时必须同时提供完整对应源码和 GPL-2.0 文本；本仓的 RAM-only 打包流程
会加入 `licenses/GPL-2.0-only.txt` 与
`sources/rtl8852bs-gpl/k1_rtl8852bs_gpl.[ch]` 和
`sources/rtl8852bs-gpl/k1_rtl8852bs_u2_nicce_fw.inc`。完整工程源码仍以本仓对应 revision
为准，不能只交付二进制和局部组件副本。

## openvela/NuttX 构建输入

K1 ELF 由当前 openvela 工作区的 NuttX、apps 和工具链生成。NuttX 主体使用
Apache-2.0，并在其 `LICENSE`/`NOTICE` 中列出随树第三方材料。上板包会带：

```text
licenses/CONTEST-LICENSE
licenses/NUTTX-LICENSE
licenses/NUTTX-NOTICE  # 工作区存在时
licenses/GPL-2.0-only.txt  # 仅在 GPL wireless 配置中
sources/rtl8852bs-gpl/     # 仅在 GPL wireless 配置中
```

正式发布前仍需针对最终 defconfig 对实际链接对象做一次 SBOM/许可证扫描；当前
清单只覆盖首启 NSH 基线，不能替代最终制品合规审查。

`wireless_wlan0_scan_diag` 额外链接了 apps 树中的 `wireless/wapi`。该组件是
BSD-2-Clause（Volkan YAZICI，Gregory Nutt 适配到 NuttX），因此该 profile 显式设置
`CONFIG_ALLOW_BSD_COMPONENTS=y`，其许可证正文随 apps 树交付，本仓不复制其源码。

## K1 实板参考仓

只读参考：

```text
/home/sw/Dev/musepi-rvv-os-reference
commit 1761a1ae2801163f09ea0eefbec89c1c0fa212b1
```

| 材料 | 许可证 | 本项目使用方式 |
|---|---|---|
| 参考仓自有代码/文档 | MIT | 读取实板启动结论和硬件事实 |
| `k1-x_MUSE-Pi-Pro.dts`、`k1-x.dtsi` | GPL-2.0 OR MIT | 读取地址、IRQ、timebase 和 PLIC context 顺序 |
| vendored U-Boot | GPL-2.0 体系 | 只研究启动命令和 DT，不复制源码 |
| vendored OpenSBI | BSD-2-Clause | 只确认 S-mode/SBI handoff |

本比赛仓没有复制参考仓函数、源码文件或二进制制品。硬件地址、IRQ 编号、设备树
节点数值和启动时观测属于移植事实；引用它们时在 `K1_BOOT_INVENTORY.md` 和
`K1_PLIC_DESIGN.md` 保留了 commit 与文件路径。

## 外部资料

K1 datasheet、SpacemiT 在线文档和 openvela 赛道指南仅作为规范/事实来源，不随
仓库重新分发。链接和用途记录在 `K1_BOOT_INVENTORY.md`。

## 发布前检查

- [ ] 所有新增 `.c/.h/.S/.sh/.py/Kconfig/CMakeLists.txt/Makefile` 保留 SPDX；
- [ ] 最终 ELF 对应的 NuttX/apps commit 已记录；
- [ ] 最终 defconfig 的实际链接对象已完成许可证扫描；
- [ ] 发布包包含本仓、NuttX 和启用 GPL 组件的 LICENSE/NOTICE/对应源码；
- [ ] 没有误提交 datasheet、厂商镜像、SD 整盘或参考仓二进制；
- [ ] 文档中的参考 commit、ELF SHA256 和实板日志能够互相对应。
