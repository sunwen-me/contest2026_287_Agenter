# Claude Handoff: K1 RTL8852BS2 Wireless Port

更新时间：2026-08-31（Asia/Shanghai）

## 目标

继续完成 SpacemiT K1 MUSE Pi Pro 上 RTL8852BS2 的 openvela/NuttX Wi-Fi 和蓝牙迁移。
真实 802.11 RX 和 2.4 GHz 全频段被动扫描已在实体板达成（运行 8 首达标，运行 12 覆盖 13 信道）。
当前目标不是再做空壳设备，而是把已验证的扫描结果接到可验收的 `wlan0`、关联和网络设备上。

## 仓库与边界

- 仓库：`/home/sw/Dev/k1-workspace/contest2026_287_Agenter`
- 板卡：SpacemiT K1 MUSE Pi Pro，板载 RTL8852BS2
- Wi-Fi：SDH1 `0xd4280800`，4-bit 1.8 V SDIO，GPIO15--20
- Bluetooth：UART2 `0xd4017100`，GPIO21 TX、GPIO22 RX、GPIO23 CTS、GPIO24 RTS
- Wi-Fi 电源：RF_PWR GPIO67、WLAN_REG_ON GPIO116、WLAN_WAKE GPIO66 输入
- Bluetooth reset：GPIO63
- 原厂参考：SpacemiT Linux 6.6，revision
  `31c449aeaad8c7759bc983ca0e26946e5b6746dc`
- 许可证：RTL8852BS 适配组件按 `GPL-2.0-only` 隔离；不要把未知来源的二进制固件或
  Realtek 专有文件加入 Apache-2.0 部分。

## 已在实体板验证

以下均为 U-Boot `loadx + go` 的 RAM-only 运行，没有 `saveenv`、eMMC/SPI/eFuse 写入：

- SDIO CMD5/CMD3/CMD7、CCCR/FBR 读取，Function 1 enable/IORDY
- RTL8852BS2 U2 NIC firmware 全量下载，WCPU runtime ready
- eFuse MAC 读取、MAC core 初始化
- BB/RF reset/AFE release
- 固件 CMD_OFLD 执行完整 1018 项 BB PHY CR 表：64 批，全部 `result=0`
- 原厂 RF Radio A/B 参数表（本板 `rfe=0x1`/`cv=0x1` 分支，A 路 0x3b3 项 0x3c 批、
  B 路 0x3a4 项 0x3b 批）经同一 CMD_OFLD 通道下发完成，随后 RF init cfg H2C 通过
- firmware H2C/C2H、地址/角色 CAM 及 2.4 GHz 1--13 passive scan-offload 控制链
- **真实空口 RX**：SDIO RX 里出现 packet type 0 的 802.11 帧，解析出 Beacon 与 BSSID
  （运行 8 首次达标，运行 12 扩到 13 个信道）
- Bluetooth UART2 H5 `SYNC -> CONFIG`，CRC 协商和 HCI Read Local Version
- **`wlan0` netdev 的扫描报告**：`SIOCSIWSCAN` 同步跑完 13 信道 sweep、`SIOCGIWSCAN` 读回，
  `wapi pscan wlan0` 从 userspace 打印出真实 BSSID ＋ 真实信道（run 7：4 个 BSS，ch8/ch8/ch1/ch1），
  25 项 `--require-*` 全链通过。**仅此而已：无 TX、不关联、无 RSSI、无 IP**
- **主动扫描**：host 把 wildcard Probe Request 存进固件 packet-offload 表、信道表带
  `tx_pkt`+`probe_req_pkt_id` 重新下发，收到真实 Probe Response（run 14 首达标）
- **open-system 认证**：向选定 AP 发单播 Authentication Request，收到它的
  Authentication Response `alg=0 seq=2 status=0`（run 20，`wireless_auth_diag`，
  27 项 `--require-*` 全过）。发送时机借固件把射频停在扫描信道上的行为，
  挂在 scan-offload dwell 里。**仅此而已：不关联、无密钥、无数据通路**
- **station join（增量 3b）**：把原厂 connect 路径的两条命令按原厂顺序发出去
  （`MEDIA_RPT/JOININFO`，然后同一 MACID 的 `MAC/ADDR_CAM_UPDATE` 带 AP 的
  target MAC/BSSID），两条 done ack 都返回 0，并要求同一次认证交换在命令之前、
  两条之间、两条之后都仍然被 AP 回答、sweep 也仍然在收 Beacon（run 25，
  `wireless_join_diag`，28 项 `--require-*` 全过，提交 `20c3191`）。
  **仍然不是关联**：无 Association Request、无 AID、无密钥、port 未使能
  （当时以为 TSF 也没同步，增量 3g 证明它其实一直跟着 AP 走）
- **可重复的管理帧交换 ＋ station join 全链（增量 3c）**：交换 armed 期间把 13 条
  scan-offload 信道表项全部填成目标信道（parked），按表项数逐条重新装填 host dwell，
  认证/关联各最多 3 次带新序列号的重传，armed 期间关掉一切轮询串口输出。效果是 join
  步骤的**三次 sweep 全部**被回答（`prejoin-rsp=0x1 joininfo-rsp=0x1 cam-rsp=0x1`），
  首次打出 `RTL8852BS2 station join complete`（run 30，`wireless_assoc_diag`，
  本 runner 的 30 条 `--require-*` 过 27 条，提交 `7c1cbe4`）。run 27 那种「同一个二进制
  一轮成一轮不成」的抖动由此消失。**关联仍然没成**：Association Request 确实发出去了
  （两个 BSS 各 3 次、46/48 字节），但一帧 Association Response 都没收到，
  `station association error=0x3d`；原因和修法见下一条（增量 3d）
- **真正的 802.11 关联完成（增量 3d）**：把 Association Request 补上该 BSS 自己的 SSID
  和一条 RSN element（id 48、长度 20：version 1、组密码回抄 BSS 公布的值、成对密码 CCMP、
  AKM PSK、RSN capabilities 0，**不含任何密钥材料**），并把目标从「只挑不带 Privacy 的
  BSS」改成打分挑「本端能说清其要求的 BSS」，run 31 一次就成了：
  `assoc target bssid=504f3be2e6d2 … cap=0x431 privacy=0x1 ssid-len=0x2 rsn=0x1
  rsn-group=0x4 rsn-ccmp=0x1 rsn-psk=0x1 rsn-tx=0x1 proven=0x1`、
  `assoc request tx … bytes=0x46 cap=0x431 rsn=0x1 rsn-group=0x4 sn=0x7 status=0x0`
  （70 字节，正是主机侧逐字节自测预期的长度），然后
  **`assoc advertised req=0x1 req-bytes=0x46 tx-status=0x0 frames=0x1 rsp-self=0x1
  rsp-target=0x1 rsp-cap=0x431 status=0x0 aid=0x1 a2=504f3be2e6d2`** —— AP 回了一帧
  Association Response，A1 等于本机 eFuse MAC、A2 等于目标 BSSID、状态码 0（成功）、
  授予 AID 1。同一次 sweep 的管理帧子类型直方图独立佐证：subtype 11（Authentication）=1、
  subtype 1（Association Response）=1。随后 `assoc info done-ack return=0x0`、
  `assoc CAM done-ack return=0x0`（两条 join 命令带着 AID 重发并被固件确认）、
  `assoc confirm bss=0x6 beacons=0xb aid=0x1`、`RTL8852BS2 station association complete`；
  本 runner 的 30 条 `--require-*` **全部通过**，退出码 0。日志
  `out/k1-serial/k1-assoc-20260830T125240Z.log`，提交 `b2d9e6f`，ELF SHA-256
  `2fe2282ddb4113892d8ac983d3e75c42f495cb368caf45e9fe4fe9b7a07b199e`。
  与 run 30 的唯一差别就是请求内容：run 30 对隐藏 SSID 的开放 VAP 发了 6 次都没人理，
  run 31 对同一台 AP 的 WPA2-PSK VAP 只发 1 次就拿到了 AID。
- **WPA2-PSK 四次握手跑通，Msg3 的 MIC 验过（run 33，提交 `b712b4e`）。**
  卡住握手的不是密码学，是接收侧的一个复位值：`R_AX_MGNT_FLTR (0xce28)`
  与 `R_AX_CTRL_FLTR (0xce2c)` 出厂就是 `0x55555555`（所以 3a–3d 的管理帧收得到），
  但 **`R_AX_DATA_FLTR (0xce30)` 复位是 `0x00000000`，每一个数据帧都被丢掉** ——
  EAPOL Msg1 是单播 QoS 数据帧，密码学写得再对也到不了主机。上游是
  `cmac_init()` 里的 `rx_fltr_init()` 一次性把三类过滤器全设成 forward-to-host，
  本移植只复刻了 `cmac_init()` 的静态子集。dwell 期间把 `0xce30` 写成
  forward-to-host、结束后恢复，握手一次就跑通：`wpa pmk ssid-len=0x2 status=0x0`、
  `msg1=1 msg2=1 msg3=1 mic=1 msg4=1`、`mic-fail=0x0 complete=0x1`、
  `msg2-bytes=0x99 msg4-bytes=0x83`（与上板前主机侧 21 条自测逐字节一致）、
  `data-self=0x2`（正好 Msg1 和 Msg3 两帧）、`deauth-self=0x0 deauth-reason=0xffff`
  （AP 不再踢我们；对比 3d 那一帧 Deauthentication，reason 15 = 握手超时，
  也就是「密码错」会长的样子）、`oversize=0 parse-err=0 crc-err=0 icv-err=0`。
  31 条 `--require-*` **全部通过**，退出码 0。日志
  `out/k1-serial/k1-wpa-20260830T145145Z.log`，镜像 `wireless_wpa_diag`，ELF SHA-256
  `3632ba044f2feeada25743a44d3ab601f4de9e4781c28477d9d83cae2cd47b4e`。
  两个可复用的结论：（a）`0xce30` **只部分可写**——写 `0x55555555` 读回
  `0x55550055`，子类型 4-7（Null / CF-Ack / CF-Poll / CF-Ack+CF-Poll，都不带 body）
  固定在 drop，所以校验只能看 Data（bit 1:0）和 QoS Data（bit 17:16），掩码
  `0x00030003`；run 32 就是因为整寄存器相等比较返 `-EIO`，一整轮 37 个下游
  判据全灭。（b）**Msg3 的 MIC 是承重证据**：AP 只可能用同一个 PMK、按同一套
  字节序推出的 PTK 才算得出它，验过一次就同时确认了口令、PBKDF2、
  key expansion、nonce/地址排序，以及 Msg2 的每一个字节。
  口令只存在仓库外的 `/home/sw/.config/k1-wifi-psk.env`（0600，`K1_WIFI_SSID` /
  `K1_WIFI_PASSPHRASE`），不进跟踪文件、不进串口日志（已逐一核对：仓库 0 命中、
  串口日志 0 命中）；但链接后的镜像 `.rodata` 里有这个串，
  **`out/k1-wpa/` 不要发布**。
- **TK 与 GTK 装进硬件，固件四条命令全部 ack（run 34，提交 `84d64e2`）。**
  安全引擎按原厂那两条读改写起来（`R_AX_SEC_ENG_CTRL 0x9d00`：`0x80002800` →
  `0x8000273f`，或上 `0x073f` 并清 `TX_PARTIAL_MODE` bit11；`R_AX_SEC_MPDU_PROC 0x9d04`：
  `0x0` → `0x3`）；Msg3 的 56 字节包装 key data 经 RFC 3394 解封得到 48 字节明文
  （`keydata-plain=0x30 unwrap=0x0 kde=0x0`），KDE 里拿到 16 字节 GTK 和 key id 1。
  两把密钥按原厂 `mac_sta_add_key()` 的顺序各发两条 H2C——**先地址 CAM
  （cat 1/class 6/func 0，与 join/assoc 同一条命令）再安全 CAM（cat 1/class 0xa/func 1，
  40 字节，`offset=0 len=0x20`）**——四条 `done-ack return=0x0`，汇总
  `sec-mode=0x2 sec-valid=0x5 tk-ent=0x0 tk-keyid=0x0 gtk-ent=0x1 gtk-keyid=0x1`。
  33 条 `--require-*` 一条没失败，日志 `out/k1-serial/k1-wpa-20260830T174332Z.log`，
  镜像 `wireless_wpa_diag`，ELF SHA-256
  `09087a0c06ad84d9e1aa262a24222c24fc604ab91c2b90bf11c8fc61ab559da8`。
  **这只说明固件接受了密钥**：发送描述符的安全字段没填、没有数据路径，
  一帧 CCMP 都没发生过。本轮 `eapol=0x4`（3e 是 `0x2`）是 AP 重传了两次 Msg1，
  因为本端仍在停驻的扫描 dwell 之间才发得出 Msg2。
- **CMAC port 0 按原厂顺序配好并使能（run 35，提交 `3aadcd8`）。**
  `mac_port_init()`（`mport.c:2010`）的 band0/port0/INFRA/`mbid_num=0` 子集，写入顺序、
  三个校验函数（`_bcn_setup_chk`/`_bcn_erly_chk`/`_tbtt_erly_chk`）连 clamp 一起复现，
  `PORT_FUNC_EN` 排最后、之后 `dly_port_us(10)` 再写 beacon early 160/setup 4/TBTT early 5。
  板上 `c400 0x1e01b → 0x1e81c`、`c404 0x00c80002 → 0x01900004`（其余 port 寄存器一位没动），
  `stat=0x3(INFRA) tsf=0x1 status=0x0`，末尾 `RTL8852BS2 port init complete`。
  同一轮还掉了 3b/3c/3d 欠的 `JOININFO` 顺序：认证前 `disconn=0x1`、关联时 `disconn=0x0`。
  34 条 `--require-*`（新增 `--require-runtime-port-init`）一条没失败，日志
  `out/k1-serial/k1-wpa-20260830T194339Z.log`，ELF SHA-256
  `d5a8f93651b35260668b03d7384812c45b453da5ab9666870f080e12f7ff84c7`。
  **顺带证伪了之前三份文档写过的「TSF 被冻住」**：`dly_port_us` 是原厂放在 `FUNC_EN` 与
  beacon early 之间的 TSF 等待，它直接读到时间在走（`c438 0x320d1d86 → 0x3211044e`），
  而 run 34 在 `FUNC_EN=0` 时 `c438` 就已经是 AP 的计时器——固件 bring-up 留着收 BSSID
  过滤和 TSF 更新使能、地址 CAM 里有 BSSID；`FUNC_EN` 管的是这个 port 要不要按 TBTT
  干活，不是计时器走不走。
  两处点名不猜：disable 流程不移植（一次 boot 只跑一个诊断、port 只初始化一次，第二次是
  幂等提前返回），`mac_wde_pkt_drop()` 正文不在手上的原厂子集里（读 `0xc560` 后记一行
  `not-ported`，板上 `c560=0x0` 本来也无事可做）。
  **握手在 port 使能之后仍然一次过**（`eapol=0x4`、`mic=0x1 mic-fail=0x0 complete=0x1`、
  `sec-valid=0x5`）：打开 port 既没修好也没弄坏「两个 dwell 之间发不出去」。
- H5 版本结果：HCI `0x0b/0x000b`，manufacturer `0x005d`，LMP subversion `0x8852`

最新已通过的扫描/PHY/FW 日志（运行 12，13 信道被动扫描）：

`/home/sw/Dev/k1-workspace/out/k1-serial/k1-run12-scan-band-pkg-20260829T063205Z.log`

对应镜像目录：

`/home/sw/Dev/k1-workspace/out/k1-run12-scan-band-pkg/`

SHA-256：`75cb8a436536e40e5ac711438fdf11cc20c1edf740a4c8bafbbde69e2ba32c0f`

实测证据：`passive scan RX data=0x52 mgmt=0x4f beacon=0x8 ... bss=0x1 channel=0x1
bssid=50:4f:3b:e2:e6:d2`，13 个 dwell 全部有帧，`crc-err=0 icv-err=0`，
`passive scan-offload done-ack return=0x0`，工具 `PASS` 且无 FAIL 行。

## 尚未完成，禁止误报

- **密钥装进硬件了，链路还是不能用：没有数据面。** run 34（增量 3f）里安全引擎按原厂的值
  起来了（ctrl `0x80002800` → `0x8000273f`、mpdu-proc `0x0` → `0x3`），Msg3 的 GTK 经
  RFC 3394 解封拿到（`gtk-len=0x10 gtk-id=0x1`），TK 与 GTK 按原厂顺序（先地址 CAM
  后安全 CAM）各发两条 H2C，四条 `done-ack return=0x0`。**但没有任何一帧被 CCMP 保护过**：
  发送描述符的安全字段（`sec_type`/`sec_cam_idx` 那几位）还没填，也没有数据路径去填；
  扫描统计里的 `icv-err=0`/`crc-err=0` 是「没解密过」而不是「解密都对」。
  顺带更正 3d 在这里写过的「下一步」：**四次握手本身不需要安全引擎**——EAPOL 四帧
  全是明文，增量 3e 已经在板上证明；安全引擎是握手**之后**装密钥才要的。
  3d 里那一帧 Deauthentication（reason 15 = 握手超时）在 run 33/34 都不再出现，
  `deauth-self=0x0 deauth-reason=0xffff`。
  另外：port 0 从增量 3g（run 35）起是**被使能的**（`c400=0x1e81c`、`NET_TYPE`=2(INFRA)、
  bit2 `PORT_FUNC_EN`=1），但**帧仍然从 scan-offload 停驻的 dwell 上发出去**，因为还没有
  驻留信道（`rtw8852b_set_channel_{mac,bb,rf}` 未移植）。run 35 的 `eapol=0x4`
  （和 run 34 一样）就是这个代价的直接读数：AP 重传了两次 Msg1，因为本端在两个 dwell
  之间发不出 Msg2——使能 port 没有改变这一点。
  说「完成了 WPA2-PSK 四次握手、密钥已被固件接受」是对的，
  说「Wi-Fi 通了」「能收发数据」「流量已加密」是误报。
- **认证、join、关联、四次握手、装密钥、port 使能六步都过了，仍然不等于「Wi-Fi 通了」。** run 20/run 25 收到的是 AP 的
  Authentication Response（`status=0`）；增量 3b 之后固件和 ADDR_CAM 知道本机属于这个 BSS；
  增量 3d 又拿到了 Association Response 和 AID 1；增量 3e（run 33）把四次握手做完，
  AP 用 Msg3 的 MIC 认了本端算出来的 PTK；增量 3f（run 34）把 TK 与 GTK 装进安全 CAM，
  固件四条命令全部 ack；增量 3g（run 35）把 CMAC port 0 按原厂顺序配成 INFRA 并使能。
  缺的是这六步之后的东西：发送描述符的安全字段（装好的密钥现在没有
  任何一条发送路径去引用它）、
  一个静态工作信道（`rtw8852b_set_channel_{mac,bb,rf}`，现在还是从停驻的扫描 dwell 上发包）、
  以及 BB/RF 的 DM 校准（DACK/RCK/IQK/DPK/TSSI）。
  说「能和 AP 完成 open-system 认证 ＋ 关联 ＋ WPA2-PSK 四次握手，并且把两把密钥装进
  硬件」是对的，说「握手过了就等于联网了」「流量已加密」是误报。
  （原来这里写的「硬件不会 ACK 这一帧，AP 会重传几次然后超时」是没有证据的推测，
  已删除：run 25 里每一次被回答的交换 `TX PPDU.lcck` 恰好 +2、run 24 里每一次没被回答
  的只 +1，多出来的那一个 PPDU 只能是本机发的。详见 `K1_WIRELESS_BRINGUP.md` 增量 3b。）
- **`wlan0` 这个网络接口自己仍然不做关联、WPA、DHCP 或联网。**（增量 3d 的关联在
  `wireless_assoc_diag`、增量 3e/3f 的四次握手与装密钥在 `wireless_wpa_diag`，
  都是诊断路径，
  不在 `wlan0` 的 ioctl 后面。）`wireless_wlan0_scan_diag` 里那个**只报告扫描结果**的
  `wlan0` 已经在实板通过 25 项 `--require-*` 全链（run 7，见下），但它的范围就只有扫描：
  `SIOCSIWSCAN` 同步跑一次 13 信道被动 sweep、`SIOCGIWSCAN` 读回结果——**没有 TX、
  没有数据 RX 路径、不关联、不认证、无密钥、无 RSSI（不伪造 `IWEVQUAL`）、无 IP**。
  说「Wi-Fi 能扫到 AP 了」是对的，说「Wi-Fi 通了」是误报。
  详见 `docs/K1_WIRELESS_BRINGUP.md` 的 2026-08-29（续七）～（续十）。
  这一项花了五次上板，过程本身是后续排查的参考：
  - run 3（续八）：前 24 项 `--require-*` 全通过、开机 sweep 收到真实 Beacon、
    `wlan0 scan device registered` 出现，但 `wapi` 触发的 sweep 在第 2 个信道以 `EINVAL`
    中断。根因是运行时 RX 读走 byte-mode CMD53、硬上限 512 字节（见「当前最重要的技术结论」），
    已改为按 SDIO 规范拆分读取。
  - run 4（续九）：拆分读取实板确认有效——全程没有一行 `runtime RX read error=`，
    每次 `wapi` sweep 里恰好出现一次 `CMD53 block count=0x1`（即原先返回 `EINVAL` 的那笔
    >512 字节聚合），sweep 返回 `ret=0 end=1 bss=3 data-only=1 dropped=0`。失败点换成了
    我自己的 bug：镜像没开 `CONFIG_LIBC_FLOATINGPOINT`，`wapi` 的 `%g` 打成字面量 `*float*`，
    信道列整个读不出来。（当时我还顺手把验收脚本从 2412–2472 MHz 改成判 1–13 信道号，
    这一步是改错方向的——见下面 run 6。）
  - run 5：板子在 `wapi pscan wlan0` 执行到一半自己复位（BROM `sys: 0x200` 切进一条 NuttX
    打印中间、无 trap dump、随后启动 eMMC 上的原厂 Linux），不是判据失败；上面的修复
    这一轮根本没被执行到。同类偶发复位在 run 2 的 XMODEM 阶段也出现过一次，**原样重跑即可**。
  - run 6：`*float*` 修复生效，信道列打出了真实数字——`2412`，也就是 MHz，不是信道号。
    根因是 `wapi` 自己做了 channel→MHz 换算（见「当前最重要的技术结论」），
    所以 run 4 里我改判 1–13 的那一步是把本来正确的 MHz 判据改坏了。改回同时接受两种形态，
    并拿 run 6 的真实串口字节离线回放验证（含 7 个反例：列=0、`*float*`、行数不符、5 GHz、
    非 5 MHz 栅格、全零 BSSID 都必须拒绝）。**只改主机侧脚本，镜像没动。**
  - run 7：**25 项 `--require-*` 全链通过**，末行 `PASS: K1 wireless RAM image reached NSH`
    （该行在 `tools/run_k1_wireless_smoke.py` 里位于所有 requirement 之后、`return 0` 之前，
    任何一项失败都会先抛 `XmodemError`）。`wapi` 读回 4 个 BSS：
    `a6:39:b3:66:3b:34 ch8`、`a4:39:b3:76:3b:34 ch8`（ssid `B`）、
    `56:4f:3b:e2:e6:d2 ch1`、`50:4f:3b:e2:e6:d2 ch1`（ssid `SB`），
    sweep 行 `ret=0 end=1 bss=4 data-only=1 dropped=0`，行数与 `bss=` 相等，
    `parse-err=0 crc-err=0 icv-err=0`，全程无 `runtime RX read error=`。
    日志 `out/k1-serial/k1-wlan0-scan-20260829T094725Z.log`。
    **注意**：run 7 的 sweep 聚合全部 ≤512 字节（33605 行之后没有任何 block-mode CMD53），
    所以 >512 拆分读取的实板证据来自 run 4，不是 run 7；改 RX 路径时两份证据都要看
- **主动扫描（第 26 项）已通过：run 16 拿到 `PASS: K1 wireless RAM image reached NSH`，
  26 项 `--require-*` 全链绿。** 该行位于所有 requirement 之后、`return 0` 之前，任何一项
  失败都会先抛 `XmodemError`。命令行是 `--nsh-reboot --boot-timeout 2400
  --wlan0-scan-timeout 300`，仍是 RAM-only。`wlan0 sweep ret=0 end=1 bss=3 data-only=1
  dropped=0`（`56:4f:3b:e2:e6:d2 ch1`、`50:4f:3b:e2:e6:d2 ch1`、`e0:45:6d:10:71:1f ch13`），
  全程 `bring-up failed` / `wlan0 register error=` / `runtime RX read error=` 各 0 次。
- **根因：`R_AX_DMAC_CLK_EN (0x8404)` 少了两位，run 14 与 run 16 两次独立复现修复效果。**
  缺陷是 `R_AX_DMAC_CLK_EN (0x8404)` 少写了 `B_AX_DLE_CPUIO_CLK_EN BIT(19)` 与
  `B_AX_PKT_IN_CLK_EN BIT(20)`。原厂 `dmac_func_en_8852b()`（`init_8852b.c:1016`）写
  `0x0B1F0000`，本端口写的是 `0x1F070000`；WDE/PLE 的时钟由 `dle_scc_init()` 的
  `DLE_ENABLE_MASK=(1<<26)|(1<<23)` 补上了，所以真正的差异只有这两位。它们正好是两种帧
  的入口：固件自己的帧走 **CPU I/O**（记在 `wde-wlcpu`）、host 的帧走 **packet-in**
  （记在 `wde-hif`）——时钟关着时两种帧都被收下并记账，却永远不被推进，也不置任何错误位。
  修复后 run 14 的五个 DLE 快照全部回到
  `empty0=0x07ff079f qempty4=0x000fffff wde-hif=0x0 wde-wlcpu=0x0 ple-txpl=0x0`
  （run 8–13 是 `empty0=0x07fd039f qempty4=0x000fbfff`、`wde-wlcpu` 逐 dwell 1→7 只涨不落），
  并且拿到 `RTL8852BS2 active scan probe response complete`、`probe-rsp=0x1`、
  `rsp-self=0x1`（该帧 A1 = 本机 `84:fc:14:06:79:7b`，即**发给自己的单播 Probe Response**，
  只可能来自一次真实辐射出去的 Probe Request）。`mactx` 从 0 变 `0x19`。
  另加了 `k1_rtl8852bs_runtime_block_enable_log()`，在 TX 前打印
  `0x8400`/`0x8404`/`0xC000`/`0xC004` 的生效值（run 14 = `0xfffd0000 / 0x1f9f0000 /
  0xf000003f / 0x4000003f`），因为 `runtime_mac_function_enable()` 只回读校验 FUNC_EN。
  **该改动在公共 init 路径上**，所以每次运行都会重新走一遍另外 25 项。
- **run 14 唯一的失败是 host 侧超时**：`FAIL: timed out waiting for b'nsh>': wapi pscan wlan0`。
  `--wlan0-scan-timeout` 默认 60 s，而 `wapi pscan` 之后又打了 660 KB，115200 下光是送字符
  就要 57.3 s（日志末尾已经是 `C2H channel=0x0d reason=0x5` 扫描结束 + `report bytes=0x1c`，
  只差 `wlan0 sweep ret=0 …` 那一行）。`verify_wlan0_scan()` 在所有启动期判据之后才跑
  （`run_k1_wireless_smoke.py:1507`），所以不影响前 25 项。用 `--wlan0-scan-timeout 300` 重跑。
  诊断 profile 的 `K1 Wi-Fi SDIO:` 逐命令打印（9 MB 日志的绝大部分）曾是板上运行的时间瓶颈，
  **已在 2026-08-30 用编译期 Kconfig 关掉**（见下面「下一步优先级」与
  `docs/K1_WIRELESS_BRINGUP.md` 续十四）：整轮从 ~15 分钟降到 **70 秒**，
  `--wlan0-scan-timeout` 再也不会因为串口带宽而超时。
- **`sta_sch_init()` 确实是缺失的一步，但不是本症状的原因。** run 13 的自检行给出结论：
  `sta-sch ctrl before=0x00e40000`（BIT(0)/BIT(31) 都是 0）、`polls=0`、`init-done=1`、
  `after=0xa0e40001`、`status=0`——真的从未执行过；但 run 13 的 `post-sweep` 与 run 10
  逐字节相同。代码保留（原厂 `dmac_init()` 必要步骤），不要再把它当作 rank-1。
  已排除的假设清单见 `docs/K1_WIRELESS_BRINGUP.md`（续十一、续十二、续十三），其中
  **`preload_init` 对 8852B 是空操作**（`dle_8852b.c:450` 直接 `return MACSUCCESS`），
  `ser=0xf00000XX` 低字节是自由跑动字段、`disp-other=0x100` 是常量，两者都不是故障。
  四个使能字里 `DMAC_FUNC_EN 0xfb7d0000`、`CMAC_FUNC_EN 0xf000003f`、`CK_EN 0x4000003f`
  与原厂逐位一致，不要再核。
- **板上运行不再需要人按 RST。** 板子平时停在 `nsh>`，`--nsh-reboot`
  （`run_k1_wireless_smoke.py:526 halt_at_uboot_from_nsh()`）走 NSH `reboot` → U-Boot
  `reset` 两段恢复，全程 RAM-only、无 `saveenv`。注意 `--manual-reset` 与 `--nsh-reboot`
  是 argparse 互斥组，不能往现成 wrapper 后面追加参数，要另写一条带绝对路径的调用。
- 从原厂排除、不要照 mainline 再追的两条：`init_cctl_info`/`mac_upd_dctl_info` 在原厂
  `#if 0`、`mac_port_init()` 从不置 `B_AX_PORT_FUNC_EN`；
  **原来这里的第三条「STA 角色不发 JOININFO」写得过头了，已修正**：`role.c:595` 那个
  `self_role == MAC_AX_SELF_ROLE_AP` 判断管的是 `FWROLE_MAINTAIN` 的重发，而
  `MEDIA_RPT/JOININFO` 是 connect 路径另外一条命令，STA 一样要发。增量 3b 在板上发了它，
  done ack 返回 0（run 25）。另外 `GENERAL_PKT` 的 `probereq`
  id 在原厂 HAL 填充函数里根本不填。本端口 chinfo 编码与
  `rtw_hal_mac_scan_ofld_add_ch()` 逐字段一致，不是缺陷；mainline 的
  `num_addition_pkt`+`additional_pkt_id[]` 形式只是等价后备
- **host 侧管理帧 TX 正向对照**（`k1_rtl8852bs_runtime_mgmt_tx_probe()`）是决定性证人：
  run 13 第一次真提交，DLE 收下（`wde-hif=0x1 ple-txpl=0x1`，`empty0` 0x07fd039f→0x07dc039f）
  而 TMAC 一动不动，从而把缺陷从"扫描卸载配置"钉到"DMAC→CMAC 公共交接段"。它只挂在
  `error:` 分支、判决打印之后，所以 run 14 主动扫描一旦成功它就完全不打印。字段值取自原厂
  `txdesc.h` 的 8852B 块与 `txdes_proc_mgnt_8852b()`，其中两处继承的错误已纠正：SDIO 上
  `WD_PAGE` 必须为 **0**（只有 PCIe 变体置它），band-0 管理队列 `QSEL` 是 **0x12**
  （`RTW89_TX_QSEL_B0_MGMT`；0x10 是 B0_BCN）。完整字段表在
  `docs/K1_WIRELESS_BRINGUP.md`（续十一）。这条描述符也是关联/认证要用的同一条
- 固件自己的 scan report 依然是空的（`report dword3=0 bytes=0x1c`，`report-rx=0 report-ch=0`）；
  现有 BSS 全部是本组件从 SDIO RX 帧自己解出来的
- RMAC PPDU 计数器与实收帧数不成比例（计数器选择索引未定），不能用它判定接收机健康
- TX 正确性与 RSSI 精度相关的初始化仍缺：`set_enable_bb_rf(hal, 0)` 的 disable 半边、
  `halbb_dm_init()`、`halrf_dm_init()` 正文、五张 `init_rf_reg` store 表、halbb `phy_reg_gain`
- RF Radio A/B 参数表只为本板命中的 `rfe=0x1`/`cv=0x1` 生成；换 RFE/CV 必须用
  `tools/k1_rtl8852bs_rf_table_gen.py` 重新生成，运行时 guard 会拒绝不匹配的组合
- RF calibration/RFK 尚未移植
- 蓝牙厂商固件下载、完整 HCI 数据传输、Host 扫描和主动连接尚未作为交付验收
- 不要仅注册一个 `wlan0` 或返回假扫描结果来宣称 Wi-Fi 完成。认证、关联、四次握手、
  装密钥都过了，但那都发生在诊断 profile 里（3a/3d/3e/3f），**`wlan0` 后面仍然只有扫描**：
  密钥虽然进了硬件却没有任何发送路径引用它、没有数据收发、没有 IP，蓝牙也只到 HCI 打开。
  「Wi-Fi 能扫到 AP」「握手过了」「密钥装好了」都不等于「Wi-Fi 完成」
- **已完成（2026-08-30，原优先级 1 与 3）**：
  - `K1 Wi-Fi SDIO:` 逐命令打印改成编译期开关 `CONFIG_K1_SDIO_WIFI_COMMAND_TRACE`
    （`chip/k1/Kconfig`，默认 `n`；`k1_sdio.c` 里 `trace` 在没定义时直接 `false`）。
    没有做成运行期 save/restore，理由是按字节偏移做的直方图证明这些行在整条日志里
    **均匀分布**、bring-up 阶段就占约 91%，「bring-up 成功后再永久 suppress」最多省 9%；
    而且 `k1_rtl8852bs_gpl.c` 里已有九对 `suppress(true/false)`，其中
    `scanofld_passive_wait` 与 `medium_access_log` 跨在扫描期间，收尾的 `suppress(false)`
    会把运行期开关重新打开。`#ifdef` 不会被这样清掉，运行期零成本，
    并且开着的时候 `tools/decode_rtl8852_sdio_trace.py` 照旧可用。
    失败路径（`command busy` / `command error` / `command timeout` / `data error`）
    **不受这个开关影响**，26 项判据里也没有一项去读这些行。
    实板效果（run 17）：日志 8 487 502 → 189 453 字节、SDIO 行 163 124 → 179、
    整轮 ~15 分钟 → **70 秒**，判据仍是 26/26。
  - 原厂 `dmac_init()`/`cmac_init()` 逐步对齐审计做完，唯一真缺的一步是
    `spatial_reuse_init()`，已补成 0xce48 上的一条 masked update（run 19 实板 26/26）。
    新发现的 `rst_port_info()` 是纯 host 侧 `PLTFM_MEMSET`，没有寄存器写。
    `mpdu_proc_init` 与原厂逐值相同。四个原厂写、本端口没写的寄存器
    （`R_AX_SIFS_SETTING 0xC624`、`R_AX_PTCL_FSM_MON 0xC6E8`、`R_AX_RX_TIME_MON 0xCEEC`、
    `R_AX_AGG_LEN_VHT_0 0xC618`）都已证明不适用，`R_AX_DLK_PROTECT_CTL 0xCE02`
    是 `R_AX_RCR 0xCE00` 那个 32 位字的高半字、早就写了。当时剩下唯一没审的是
    `sec_eng_init` / `sec_info_tbl_init`（本地原厂缓存里没有正文），只影响加密：
    **4-way 本身不需要它**（EAPOL 四帧都是明文，增量 3e 已在板上证明），
    要到握手之后把 TK/GTK 装进安全 CAM 才需要。**`sec_eng_init` 要用的那两个寄存器已在
    增量 3f 补上**（`R_AX_SEC_ENG_CTRL 0x9d00` 或 `0x073f` 且清 `TX_PARTIAL_MODE` bit11、
    `R_AX_SEC_MPDU_PROC 0x9d04` 或 `0x3`，各一次读改写），`sec_info_tbl_init` 仍未审。
    细节见 `docs/K1_WIRELESS_BRINGUP.md` 续十四与增量 3f。
- **已完成（2026-08-30，原优先级 1 的前半 = 增量 3a）：认证（open system）已在板上打通。**
  run 20，镜像 `wireless_auth_diag`，27 条 `--require-*` 全过、`RUN_EXIT=0`，
  基线（原 26 条）无回归。AP `50:4f:3b:e2:e6:d2`（ch1）回了
  `alg=0x0 seq=0x2 status=0x0 a2=504f3be2e6d2`——**它自己给出的成功码**。
  关键机制：**固件会把射频停在扫描信道上**。
  `k1_rtl8852bs_runtime_scanofld_next_channel_submit()`（`k1_rtl8852bs_gpl.c:9563`）
  的自带注释写明 SCANOFLD 保持当前信道直到收到 NEXT_CH——**但只在 chinfo 的 `period`
  到点之前**：原厂把 `period` 定义为 "how long to stay on this ch. unit: ms"
  （`mac_def.h:7374`，`u8`，上限 255 ms），到点固件自己就走，host 的 NEXT_CH 只能把这段
  时间缩短。所以 host 的 dwell 截止必须严格小于 `period`，否则每个信道都是一场 host 可能
  输掉的竞争（host 的计时从**读到**通知才开始）。现在是 `period` = 250 ms 配 host 侧
  dwell 截止 180 ms。「进入信道通知 → host 截止」之间射频确定停在目标信道，
  可以在里面发帧并在同一信道收回复。这是原厂状态机自己的行为，
  也是本移植目前唯一能在指定信道发送的办法（还没有
  `rtw8852b_set_channel_{mac,bb,rf}`）。实现要点：
  `mgmt_tx_build()` 加 `bool broadcast` 参数——单播管理帧必须清掉
  `K1_RTL8852BS_MGMT_TXI_BMC`，否则硬件不等 ACK 也不重传；新增精简发送核心
  `k1_rtl8852bs_runtime_mgmt_tx_frame()`（描述符与 CMD53 路径和已验证的
  `mgmt_tx_probe()` 完全相同，只去掉仪表输出，因为控制台是轮询式的，
  在 250 ms 窗口里打印十行会吃掉等回复的时间）；判据是 **A1 == 自身 MAC** 的
  host 侧 memcmp，任何寄存器都无法伪造。
  **但这不是关联**：ADDR_CAM 仍是 no-link role。
- **已完成（2026-08-30，增量 3b）：station join 的两条命令已在板上被固件接受。**
  run 25，镜像 `wireless_join_diag`，28 条 `--require-*` 全过，基线（27 条）无回归，
  提交 `20c3191`。`MEDIA_RPT/JOININFO` → 同一 MACID 的 `MAC/ADDR_CAM_UPDATE`（target
  MAC/BSSID = AP），两条 done ack 都是 0；同一次认证交换在命令之前、两条之间、两条之后
  各做一次，三次都被 AP 回答且 sweep 仍在收 Beacon。`FWROLE_MAINTAIN` 故意不重发
  （原厂也不重发）。**顺手修掉的根因**：host 自建的管理帧原来一直用序列控制字段 0，
  一次运行里所有请求呈现同一个 `<A2, 序列号, 分片号>` 三元组，正是 IEEE 802.11
  clause 10.3.2.14 重复检测的输入——AP 在 MAC 层 ACK 之后就丢掉重复帧，看起来就像
  「只回第一次然后沉默」。现在有一个 12 位本机计数器，同时写进帧和 WD BODY dword3。
  **仍然不是关联**：没有 Association Request/AID/密钥，port 0 仍
  `PORT_FUNC_EN=0`/`NET_TYPE=0`（这里原来还写了「TSF 冻结」，是错的，见增量 3g）。
- **已完成（2026-08-30，增量 3c）：管理帧交换变成可重复的，join 全链在同一次运行里跑完。**
  run 30，镜像 `wireless_assoc_diag`，本 runner 的 30 条 `--require-*` 过 27 条，
  提交 `7c1cbe4`。四个改动：(a) 交换 armed 期间把 13 条信道表项全部填成目标信道（parked）；
  (b) parked sweep 按表项数逐条重新装填 host dwell——原来「每信道一个 bit」的测试对
  parked 列表只会装填一次，整轮 sweep 因此卡死（run 28 的 `-ETIMEDOUT` 正是如此，
  而那一轮认证其实已经成功）；(c) 认证/关联各最多 3 次带新序列号的重传；
  (d) armed 期间关掉一切轮询串口输出（实测一次 witness 快照 65 行 / 6378 字节，
  115200 8N1 下约 550 ms，而一个 dwell 只有 180 ms）。外加 done-ack RX 缓冲 512 → 8192，
  这是 run 29 `-ENOSPC` 的根因。效果：`prejoin-rsp=0x1 joininfo-rsp=0x1 cam-rsp=0x1`
  ＋ `RTL8852BS2 station join complete`。**仍然不是关联**，见「尚未完成，禁止误报」第一条。
- **已完成（2026-08-30，增量 3d）：802.11 关联在板上完成，AP 授予 AID 1。**
  run 31，镜像 `wireless_assoc_diag`，本 runner 的 30 条 `--require-*` 全过、退出码 0，
  提交 `b2d9e6f`。改动就两件事：(a) Association Request 里补上目标 BSS 的 SSID 和一条
  RSN element（组密码回抄 BSS 的公布值，因为混合模式 AP 的组密码可以弱于成对密码，
  而 AP 会拿请求里的组密码和自己的比）；(b) 目标选择从「跳过一切 Privacy BSS」改成
  打分排序：公布 SSID(+4) > 开放(+2) > 已被证明会回认证(+1)，Beacon 数破平；
  本端说不清其要求的 Privacy BSS（只有 TKIP 成对密码、厂商私有 suite、只有 SAE）
  仍然跳过并返回 `-EOPNOTSUPP`。`K1_RTL8852BS_ASSOC_REQUEST_MAX_SIZE` 96 → 128。
  **关联之后的链路仍然不通**，见「尚未完成，禁止误报」第一条。
- **已完成（2026-08-31，增量 3f）：TK 与 GTK 装进硬件，固件四条命令全部 ack。**
  run 34，镜像 `wireless_wpa_diag`，本 runner 的 33 条 `--require-*` 一条没失败，
  提交 `84d64e2`。五块：安全引擎 bring-up（`0x9d00` 或 `0x073f` 且清 bit11、
  `0x9d04` 或 `0x3`）、RFC 3394 解封 ＋ GTK KDE 解析、安全 CAM 载荷序列化
  （cat 1/class 0xa/func 1，40 字节，`offset=0 len=0x20`）、模块级密钥槽状态
  ＋ 按原厂顺序（地址 CAM 在前、安全 CAM 在后，`mac_sta_add_key()` 唯一调用者的顺序）
  发送、以及在关联完成路径上「握手完成」那行之后调用。槽位照抄原厂：mode 2 下
  单播 0–1／组播 2–4，所以 TK 用 slot 0（entry 0）、GTK 用 slot 2（entry 1），
  `sec-valid=0x5`。**这只证明固件接受了密钥，不证明任何一帧被加密**，见本节第一条。
- **已完成（2026-08-31，增量 3g）：CMAC port 0 按原厂顺序配好并使能，`JOININFO` 顺序还清。**
  run 35，镜像 `wireless_wpa_diag`，本 runner 的 34 条 `--require-*` 一条没失败，
  提交 `3aadcd8`。`mac_port_init()` 的 band0/port0/INFRA 子集共 21 步（`FUNC_EN` 最后、
  然后 `dly_port_us(10)`、然后三条被原厂校验函数管着的写入），四处按字节写
  （`0xc413` TBTT aggregate、`0xc427` DTIM、`0xca08` TSF 时间戳控制、`0xc590` 高队列窗口
  ——`0xc427` 所在 dword 的起点是 `R_AX_BCN_ERR_FLAG_P0`，dword 读改写会把它一起重写；
  本端寄存器访问在 `0x1000-0x1f00` 之外走间接 CMD52，字节访问是原生的）。
  每一次写都打「字段名 ＋ 写前 ＋ 要写 ＋ written/skip」，于是串口直接回答了固件 bring-up
  已经摆对了多少：大部分都对，真变的只有网络类型、功能使能、两个 beacon 上报使能、
  高队列窗口和它的 update 位、DTIM、beacon hold、beacon setup。
  Kconfig 符号 `K1_RTL8852BS2_RUNTIME_PORT_INIT_DIAGNOSTIC`（只有 wpa profile 打开；
  关掉时 assoc 镜像里这段代码整块不在、无新告警）。
  **失败不中断关联**：`FUNC_EN` 一旦写下去，中途放弃会留下半配置的 port 并把这一轮要收集的
  握手证据一起丢掉；代价是 `RTL8852BS2 port init complete` 只在「序列跑完 ＋ TSF 在走」
  时才打，`--require-runtime-port-init` 认这一行加 `status=0x0 tsf=0x1` 的结果行。
- **下一步优先级**：(1) `rtw8852b_set_channel_{mac,bb,rf}` 驻留信道 1 ＋ 发送描述符的
  安全字段（`sec_type`/`sec_cam_idx`）：这两件凑齐才第一次可能有「被 CCMP 保护的数据帧」。
  密钥已经在硬件里（增量 3f）、port 也已经使能（增量 3g），现在挡在「密钥装好」和
  「能收发数据」之间只剩两条——描述符不引用安全 CAM index，硬件就不会去加密；
  以及仍然是在停驻的扫描 dwell 上发帧（run 35 的 `eapol=0x4` 就是 AP 因此重传了两次
  Msg1，使能 port 并没有改变它）。3b/3c/3d 欠的 `JOININFO` 顺序已在 3g 还清
  （认证前 `disconn=0x1`、关联时 `disconn=0x0`）。
  (2) `rtw_hal_bb_dm_init` / `rtw_hal_rf_dm_init`（DACK/RCK/IQK/DPK/TSSI），
  发送正确性与 RSSI 精度要靠它；同一批还有 `set_enable_bb_rf(hal, 0)` 的 disable 半边、
  `halbb_dm_init()`/`halrf_dm_init()` 正文、五张 `init_rf_reg` store 表、halbb `phy_reg_gain`。
  (3) 真正的 STA 链路最终还是要把 `rtw8852b_set_channel_{mac,bb,rf}` 移植进来，
  让信道控制不再依赖扫描卸载状态机——也就是 (1) 的前半条。

## 当前最重要的技术结论

原厂 `hal_start_8852b()` 顺序为：

1. MAC 初始化和默认 RX filter
2. eFuse process
3. BB/RF release
4. `halbb_init_reg()`：默认 BB PHY CR 表
5. `halrf_init_reg()`：RF Radio A/B 参数表
6. BB/RF dynamic management、PPDU status 和 calibration

项目已完成前 5 项的有界版本，普通 packet type 0 RX 已经出现（见上）。第 6 项的
dynamic management/calibration 仍缺，因此 TX 与 RSSI 精度还不可信。
新增的几条硬结论（读日志/写发送路径之前先看）：

- **Association Request 沉默的原因几乎总在请求内容里，不在发送路径里。** run 30 发了 6 次
  没有一帧回应，run 31 只发 1 次就拿到 AID，两者的发送路径完全一样。两处必须满足：
  IEEE 802.11 11.3.5.3 要求请求带该 BSS 自己的 SSID（认证请求不带 SSID，
  所以隐藏 SSID 的 BSS 会「认证能过、关联静默」）；clause 12.6.3 要求对带 Privacy 的 BSS
  必须给出密码套件，否则请求在本移植的任何一行代码被检验之前就已经被拒。
  推论：调试关联先打印/校验请求字节，不要先去动 MAC 或 PHY。
- **RSN element 的组密码必须回抄 BSS 公布的值，不能固定写 CCMP。** 混合模式 BSS 会合法地
  公布一个比成对密码更弱的组密码，而 AP 会把请求里的组密码和自己的比。run 31 的目标
  `rsn-group=0x4`（CCMP），但同一次普查里 `789682af9f60` 就是 `rsn-group=0x2`（TKIP）。
- **host 自建的管理帧必须带真序列号。** 帧里的序列控制字段就是上空气的那个；描述符里
  能让硬件代填的 `AX_TXD_HW_SSN_SEL`/`AX_TXD_EN_HWSEQ_MODE` 在本移植里都是 0。所有请求
  共用一个号码 = 触发 802.11 clause 10.3.2.14 重复检测，AP 会 ACK 然后丢掉，症状是
  「第一次有回复、后面全沉默」。这曾经让增量 3b 连续两轮报 `error=0x3d`（ENODATA）。
- **串口日志里 C2H 行的位置是 host 读到它的时间，不是固件产生它的时间。** 不要用一行
  C2H 出现在哪两行之间来推断固件事件的先后。
- **`pause_tx_data` 不会挡住管理帧**：原厂定义是 "whether disable tx (except manage
  pkt) after sending probe req"（`mac_def.h:7398`）。
- **`done_ack_wait()` 会抽 RX FIFO**，所以任何 H2C 提交都不能放在扫描 RX 循环里，只能放
  在两轮 sweep 之间；`k1_rtl8852bs_runtime_mgmt_tx_frame()` 可以从 RX 循环里调。
- **这一版 scan-offload 固件从不自己换信道。** 每一次 sweep 的每一个 enter 通知都跟在本
  host 的 next-channel 命令之后，`fw-next=0x0`、`firmware_advanced_channels` 一直是空的，
  `mac_ax_scanofld_chinfo.period` 描述的固件侧自动推进在本移植上从未被观察到
  （180 ms 的 host dwell 总是先到）。**host 是 sweep 唯一的节拍器**：host 一停下发，
  固件就停在当前信道上无限等下去——run 28 的 `-ETIMEDOUT` 就是这么来的，而那一轮
  认证其实已经成功（同一份日志里有 `rsp-self=0x1 status=0x0 a2=504f3be2e6d2`）。
  提交 `d0cc279` 的说明里有一条相反的推测，以本条为准。
- **成功的交换会被没结束的 sweep 掩盖。** 报错的 errno 来自 sweep 的等待循环，不代表
  空口上那一步失败了；判定认证/关联成功与否只看 `rsp-self`/`status`/`frames` 这些
  逐帧计数器，不要看外层 `wait error=`。
- **parked 信道有副作用**：parked sweep 结束后射频停在目标信道上，后面做 H2C 时 SDIO
  RX FIFO 仍在被那个 AP 的 Beacon/数据帧填满，所以 done-ack 的 RX 缓冲要按聚合帧尺寸
  给（本组件 512 → 8192）。`rx_read()` 对太小的缓冲返回 `-ENOSPC` 且**不消费** FIFO
  （`if (request_length > buffer_size) { *transfer_length = request_length; return -ENOSPC; }`），
  公布的长度一直挂着，于是第一次失败之后每一次都以同样方式失败（run 29 的 `-28`）。
- **Association Request 必须带该 BSS 自己的 SSID**（IEEE 802.11 11.3.5.3），
  Authentication Request 不带。所以对隐藏 SSID 的 BSS 会出现「认证有回复、关联全静默」，
  这不是发送路径的问题。
- **TX 仪表日志前缀是 `TX state` 与 `TX PPDU`**（旧日志里的旧前缀已改名，不要当成缺失）。

不要继续花时间调整已经排除的因素：

- SDIO CMD53 byte mode 的计数字段只有 9 位（0 表示 512），所以**一次 byte-mode CMD53 最多
  512 字节**。设备在 `SDIO_RX_REQ_LEN`(0x1108) 公布的接收聚合长度经常超过 512，
  必须像 Linux SDIO core 那样拆：512 对齐部分走 block-mode CMD53
  （BLOCK_SIZE/BLOCK_COUNT ＋ `MMCSD_MULTIBLOCK`），余数走一个 byte-mode CMD53，
  地址保持不变。本组件的 `k1_sdio_wifi_rxfifo_read()` 就是这条路径，只接受读方向、
  function 1、`increment == false`、地址 `0x1f00`。这类失败与流量相关：小帧 sweep 会通过，
  遇到大 Beacon 或被 NIC 聚合的两帧才断，所以不要按「偶发」处理
- **驱动报信道号，`wapi` 打 MHz，两者都对，不要去「修」任何一侧。**
  `nuttx/include/nuttx/wireless/wireless.h` 写明 `struct iw_freq` 是
  `0-1000 = channel, > 1000 = frequency in Hz`；本组件和 in-tree 的 `bcmf_driver.c` 一样报
  `u.freq.e = 0; u.freq.m = ctl_ch`。但 `apps/wireless/wapi/src/wireless.c` 的
  `wapi_scan_event()` 在 `SIOCGIWFREQ` 分支里自己换算了：
  `e == 0` 且 `m` 在 1–13 时 `info->freq = 2407 + 5 * m`（14 → 2484，36–165 → `5000 + 5 * m`），
  之后才 `%g` 打印。所以 `wapi pscan` 那一列实测是 2412–2472。验收脚本两种形态都收，
  并折算回信道号；`(MHz - 2407) % 5 == 0` 保证不是任意数
- 这个换算顺带保住了严格性：`wapi` 只换算它认得的信道，信道没解出来（`m = 0`）时那一列留在 0，
  两种形态都判不过，所以「注册了 `wlan0` 但信道是假的」不可能混过验收
- `wapi` 用 `%g` 打这一列（值是 double），所以任何要从 userspace 读回信道的 profile 都必须带
  `CONFIG_LIBC_FLOATINGPOINT=y`，否则 NuttX 打出字面量 `*float*`
  （`lib_libvsprintf.c` 在关掉浮点 printf 时对 `%e..%g` 就是这个行为）。
  `pow()` 本身由 `CONFIG_LIBM_TOOLCHAIN` 提供，只有 printf 这一侧要开。
  代价：text +5074 字节（链入 `__dtoa_engine`）
- CMD5 R4 bit 定义已修正：I/O-ready 是 bit 31，不是 bit 27
- SDH1 APMU 时钟地址、mux、gate、reset 已验证
- GPIO20 的 DS2/DS3 两种候选已验证
- Linux SDHCI force-clock/override-clock 位已验证无效
- BT reset 时序和 REG_ON 10 ms 延迟已验证不是根因
- SDR104 retune、High-Speed 回退和同会话重放都未解决 PHY CR direct CMD53 CRC

PHY CR direct CMD53 在第 `0x32d` 项、地址 `0x0024` 出现 Data CRC；改用原厂对应的
firmware I/O-offload 后，1018 项全部得到 C2H `result=0`。成功合同是 C2H result，
不要在 WCPU runtime 后用宿主 indirect readback 验证 PHY CR；该读回曾得到
`0xdeadbeef`，这不是 CMD_OFLD 失败。

## 建议 Claude 的下一步

A/B/C 三条已执行完（见「已在实体板验证」），保留在此作为方法论与回归参考。
D 的第 5 步（`wlan0` 只报告扫描结果）**已在实板通过 25 项全链**，可复现镜像：

```bash
tools/build_k1.sh --clean \
  --config board/k1/muse_pi_pro/configs/wireless_wlan0_scan_diag \
  --build-dir cmake_out/k1-wlan0-scan \
  --package --package-dir out/k1-wlan0-scan --jobs 8
```

镜像 `out/k1-wlan0-scan/`，SHA-256
`a73d324becdec6ff23eba4016dc44009ded46eaf5dbd348c6a717ad2c2872ad5`，
text 710768 / data 9568 / bss 24416（含 CMD53 RX 拆分读取修复 ＋ `CONFIG_LIBC_FLOATINGPOINT`）。
验收命令是 `docs/K1_WIRELESS_BRINGUP.md`（续七）里那条完整 `--require-*` 链加
`--require-wlan0-scan`，需要用户按 RST；run 7 已通过（续十）。

上面这段「第一块砖是 TX」已经完成：Probe Request 真发出去并收到 Probe Response
（run 14）、open-system 认证收到 AP 的成功回复（run 20 / 增量 3a）、station join 的两条
命令被固件接受（run 25 / 增量 3b）、join 全链 ＋ 可重复的管理帧交换（run 30 / 增量 3c）、
Association Response ＋ AID 1（run 31 / 增量 3d）、WPA2-PSK 四次握手且 Msg3 的 MIC
验过（run 33 / 增量 3e）、TK 与 GTK 装进安全 CAM 且固件四条命令全部 ack
（run 34 / 增量 3f）、CMAC port 0 按原厂顺序配成 INFRA 并使能（run 35 / 增量 3g）。
**当前实际下一步是数据面：`rtw8852b_set_channel_{mac,bb,rf}` 驻留信道 1 ＋ 发送描述符的
安全字段**——密钥已经在硬件里、port 也已经使能了，但没有任何一条发送路径去引用它的安全
CAM index，所以仍然一帧 CCMP 都没有；而且发帧仍然发生在停驻的扫描 dwell 上（run 35 的
`eapol=0x4` 就是 AP 因此重传了两次 Msg1，使能 port 没有改变它）。做法与欠账见
「尚未完成，禁止误报」末尾那条优先级 (1)。复现 3e/3f/3g 镜像：

```bash
tools/build_k1_wpa.sh             # profile board/k1/muse_pi_pro/configs/wireless_wpa_diag
```

板上验收是 34 条 `--require-*`（30 条关联链 ＋ 握手链 ＋ `--require-runtime-wpa-keys`
＋ `--require-runtime-port-init`），run 35 一条没失败、以
`PASS: K1 wireless RAM image reached NSH` 收尾，日志
`out/k1-serial/k1-wpa-20260830T194339Z.log`（run 34 是其中 33 条，日志
`out/k1-serial/k1-wpa-20260830T174332Z.log`；run 33 是 31 条，日志
`out/k1-serial/k1-wpa-20260830T145145Z.log`）。跟踪的 profile defconfig **不含**
口令：脚本从 `${K1_WIFI_PSK_ENV:-~/.config/k1-wifi-psk.env}`（0600，仓库外）读出
`K1_WIFI_PASSPHRASE`/`K1_WIFI_SSID`，生成一份 `include` 该 profile 的
`cmake_out/k1-wpa-config/defconfig`（0600，仓库外）再编译；`--no-key` 可以不带口令构建。
**链接后的镜像 `.rodata` 里会有这个串，`out/k1-wpa/` 不要发布**。

复现 3d 镜像：

```bash
tools/build_k1_assoc.sh           # profile board/k1/muse_pi_pro/configs/wireless_assoc_diag
```

板上验收在既有 28 条 `--require-*` 之后追加 `--require-runtime-assoc-response` 与
`--require-runtime-assoc`（共 30 条）。run 31（提交 `b2d9e6f`）**30 条全过、退出码 0**，
日志 `out/k1-serial/k1-assoc-20260830T125240Z.log`；上一轮 run 30 过 27 条，
没过的三条正是 Association Response、关联本身、以及依赖它们的 bring-up 成功。

TX 正确性依赖的初始化缺口
（`rtw_hal_bb_dm_init`/`rtw_hal_rf_dm_init` 那一批）见本节前面的清单
（`set_enable_bb_rf(hal, 0)` disable 半边、`halbb_dm_init()`、`halrf_dm_init()` 正文、
五张 `init_rf_reg` store 表、halbb `phy_reg_gain`），不要在补齐之前把发不出去归因于 MAC 层。

如果 `--require-wlan0-scan` 回归了，排查顺序（这三条各自都真实发生过一次）：

1. 串口里有没有 `runtime RX read error=<errno> length=<长度>`。run 4 已经证明没有这行，
   512 上限确实是 run 3 的主因；若它重新出现，`length` 直接指出是另一个 `EINVAL` 源
   （例如 R5 的 `OUT_OF_RANGE`），不要回头怀疑扫描逻辑。
2. `wapi` 输出里有没有 `*float*`。有则镜像丢了 `CONFIG_LIBC_FLOATINGPOINT`
   （`nuttx/libs/libc/stream/lib_libvsprintf.c` 在关掉浮点 printf 时对 `%e..%g` 就打这个
   字面量），脚本会直接指名这个 Kconfig 符号。
3. 日志尾部是不是 BROM 的 ` sys: 0x200` ＋ `try sd...`。是则板子偶发复位、和判据无关，
   原样重跑即可；`assert/PANIC/mcause/EPC` 一个都搜不到可以佐证不是软件陷入。
   重跑时按一次 RST 后到结果出来前（约 3 分钟）不要再碰板子或线缆——XMODEM 只占前 40 秒，
   之后 NuttX 还要跑完两轮 `wapi pscan`。
4. `--require-runtime-scanofld-active` 单独回归、并且串口里是
   `active scan probe response error=0x000000000000003d`（`ENODATA`）＋
   `bring-up failed: -61`，**第一步是用同一个二进制原样重跑**（不重新编译，70 秒一轮）。
   主动扫描要求 dwell 期间 AP 真的回一帧 Probe Response，这一项本来就有环境抖动：
   run 16 听到 3 个 AP、run 17 是 2 个、run 18 一个都没回、run 19 又是 3 个。
   判断依据是 `passive scan TX notify pre-tx=0xd post-tx=0xd fw-txfail=0`
   （Probe Request 在 13 个信道上都发了）加 `rsp-self=0x0 rsp-other!=0x0`
   （空中有 Probe Response，只是发给别的 STA）。`rsp-self`/`rsp-other` 是 host 侧
   按 A1 逐字节比较分出来的，没有寄存器能把发给自己的单播算成别人的，所以这个组合
   只可能是「发出去了没人回」，不要因此回头怀疑 MAC/RX 配置。完整证据见
   `docs/K1_WIRELESS_BRINGUP.md` 续十四最后一节。

该 profile 的父 profile 必须是 `wireless_fw_runtime_scan_rf_readback_diag`——
`wireless_fw_runtime_scanofld_rx_diag` 虽然名字里有 rx，却不打开 BB/RF release、
RF context、PHY CR（含 fwofld 重放）、BB reset、RF radio A/B（A-die pad 使能在这条路径里）
和 High-Speed-only SDIO，用它做父会得到一个聋接收链、`bss=0`、`wlan0` 不注册。
改 defconfig 后 `tools/build_k1.sh` 必须加 `--clean`，否则不会重新生成 `.config`。

### A. 先取得真实 RFE/CV 上下文（已完成：`rfe=0x1` `cv=0x1`，来源 eFuse）

从原厂 Linux 启动日志、RTL8852BS eFuse decode 或驱动运行时日志确认：

- `rfe_type`
- chip `CV`
- package/type 选择
- RF eFuse 校准数据是否已由原厂驱动写入

不能假定 RFE 或 CV。原厂 RF 表的条件块会按 RFE/CV 选择分支。

### B. 解析而不是盲写原始 RF 表（已完成：`tools/k1_rtl8852bs_rf_table_gen.py`）

原厂文件：

`drivers/net/wireless/realtek/rtl8852bs/phl/hal_g6/phy/rf/halrf_8852b/halrf_hwimg_raw_data_8852b.h`

其中：

- `array_mp_8852b_radioa[]` 从约第 144 行开始
- `array_mp_8852b_radiob[]` 从约第 7368 行开始
- 文件约 3 MB、近 4 万行，包含 `IF/ELSE IF/ELSE/END` 条件记录

原厂选择逻辑：

`halrf_hwimg_8852b.c:halrf_config_8852b_radio_a_reg()`，B 路径同理。

先做一个 GPL-only 的 host 解析/生成工具，输入明确的 RFE/CV，输出命中的连续 RF
寄存器序列和统计摘要；不要把整张带条件的表直接加入固件。

### C. 扩展已有 CMD_OFLD RF 写入（已完成：RF radio A/B offload 全部通过）

已有通用框架在：

`chip/k1/k1_rtl8852bs_gpl.c`

PHY CR 已使用 `source=BB` 的 16-byte `fwcmd_cmd_ofld`。RF 参数需要复用同一批处理和
C2H result 等待机制，但使用原厂定义的 `source=RF`、RF path A/B、mask/value 语义。
每批失败立即停，不要对可能已被固件接收的事务盲目重放。

### D. 实板验收顺序

1. 构建并通过 `tools/check_k1_sources.sh`、ELF 检查
2. 使用已有 `tools/run_k1_wireless_smoke.py` 的 RAM-only 流程
3. RF A/B 参数 offload 全部等待 C2H 成功
4. 再跑已有 1--13 scan-offload
5. 只有 SDIO RX 中出现 packet type 0 且解析出 Beacon/Probe Response/BSSID，才继续
   `wlan0`/关联/认证/网络层（该门槛已于运行 8 达成，运行 9/10/12 复现）

板上操作需要用户在监听器打开后按 RST。`--nsh-reboot` 只做非持久的两阶段恢复；不能
依赖 `adb reboot` 自动停在 U-Boot。

## 关键文件

- 驱动：`chip/k1/k1_rtl8852bs_gpl.c`
- 头文件：`chip/k1/k1_rtl8852bs_gpl.h`
- SDIO：`chip/k1/k1_sdio.c`、`chip/k1/k1_sdio.h`
- 板级入口：`board/k1/muse_pi_pro/src/k1_wireless.c`
- `wlan0` 扫描 netdev（Apache-2.0，只调 GPL 组件公开 API）：
  `chip/k1/k1_rtl8852bs_netdev.c`、`chip/k1/k1_rtl8852bs_netdev.h`
- `wlan0` profile：`board/k1/muse_pi_pro/configs/wireless_wlan0_scan_diag/defconfig`
- 认证/join/关联/WPA profile 与构建脚本：
  `board/k1/muse_pi_pro/configs/wireless_auth_diag/`＋`tools/build_k1_auth.sh`、
  `board/k1/muse_pi_pro/configs/wireless_join_diag/`＋`tools/build_k1_join.sh`、
  `board/k1/muse_pi_pro/configs/wireless_assoc_diag/`＋`tools/build_k1_assoc.sh`、
  `board/k1/muse_pi_pro/configs/wireless_wpa_diag/`＋`tools/build_k1_wpa.sh`
- 测试 AP 的凭据（**仓库外**，0600）：`/home/sw/.config/k1-wifi-psk.env`，
  变量 `K1_WIFI_SSID` / `K1_WIFI_PASSPHRASE`。`tools/build_k1_wpa.sh` 从这里读出来，
  写进同样在仓库外的 `cmake_out/k1-wpa-config/defconfig`（0600）；
  **不要把口令写进跟踪文件、不要打到串口、不要发布 `out/k1-wpa/`**
  （链接后的 `.rodata` 里有这个串）
- PHY CR：`chip/k1/k1_rtl8852bs_phy_reg_8852b.inc`
- Wi-Fi 固件：`chip/k1/k1_rtl8852bs_u2_nic_fw.inc`、`chip/k1/k1_rtl8852bs_u2_nicce_fw.inc`
- 蓝牙 H5：`chip/k1/k1_bt_uart.c`
- 实板长记录：`docs/K1_WIRELESS_BRINGUP.md`
- 来源/许可：`docs/K1_SOURCE_AND_LICENSES.md`
- RAM 加载工具：`tools/run_k1_wireless_smoke.py`

## 工作区注意事项

工作区本来就有大量未提交的迁移改动和诊断 profile。不要 `git reset`、`git checkout` 或
清理无关文件。`/tmp` 中的大型 K1/Linux 临时源码树已清理；继续参考原厂时使用单文件
下载或临时目录，完成后删除临时文件，不要再次克隆整棵 Linux 树。
