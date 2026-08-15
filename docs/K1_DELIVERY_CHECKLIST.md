# K1 交付检查清单

## 无板阶段（当前）

- [x] K1 启动事实、地址、UART、timer、PLIC 资料盘点；
- [x] 对照 SpacemiT 官方用户指南整理 PWR Type-C、FDL、PWR/RST、STAT 和 40Pin UART；
- [x] RV64 S-mode hart 0 入口和显式 `gp` 初始化；
- [x] 初始 hart/DTB/sstatus/satp/stvec 日志；
- [x] polling `/dev/console`，源码和 ELF 验收均禁止写 IER；
- [x] SBI TIME 24 MHz lower-half；
- [x] 同步异常寄存器直出和 `sepc` 自动符号化；
- [x] PLIC hart 0 S-mode context 1 已从 DTS 解析；
- [x] PLIC 骨架可选编译、默认关闭且与 SMP 互斥；
- [x] 静态 CI、干净构建入口、ELF 自动验收、U-Boot 上板包；
- [x] 串口原始日志和元数据自动采集；
- [x] 双轮 59 Pattern 驱动审查与修复记录；
- [x] 来源/许可证清单和发布包许可证；
- [x] 2026-08-14 十一个 K1 profile 的干净构建和 ELF SHA256 回归；证据见
  `docs/K1_HOST_BUILD_REGRESSION_20260814.md`，仅为主机侧验证；
- [x] 独立显示 framebuffer handoff、DWC3/PHY 只读探测和 USB Host profile 已接入
  构建、ELF 验收和 U-Boot 包；显示、USB probe 和 USB Host 均已有实板证据；操作
  边界见 `docs/K1_USB_DISPLAY_BRINGUP.md`；

## 拿到板后的阻塞项

- [ ] 保留并验证原厂可恢复 SD 卡；
- [ ] 使用 PWR Type-C 供电，记录 PD 电源/线缆和 STAT 指示灯状态；
- [ ] 按官方 pinout 使用 40Pin pin 6/8/10：GND/TX/RX，确认 USB-TTL 为 3.3V；
- [x] 已记录实际启动固件为 U-Boot；版本和板卡信息见
  `docs/K1_REAL_BOARD_HANDOFF.md`；
- [x] 已记录当前固件的 `bootelf -p` 异常；当前使用 wrapper + `go` 的临时启动路径，
  不再把 `bootelf -p` 作为默认操作；
- [ ] 不把正常启动时 Type-C 不枚举 USB 设备误判为故障；
- [ ] 记录板卡版本、供电、USB-UART、U-Boot 和 OpenSBI 版本；
- [ ] 只读确认 MMC 编号/分区和实际 DTB；
- [ ] 核对 payload 窗口、U-Boot relocation 和 reserved-memory；
- [x] 使用 wrapper + `go` 临时启动，不执行 `saveenv`；
- [x] 日志确认 hart、DTB、S-mode、初始 CSR；
- [x] 依次确认 early marker、NuttX banner、NSH；
- [x] 核对 SBI TIME 并连续运行 10 分钟；证据见 `docs/K1_REAL_BOARD_STABILITY_20260812.md`；
- [x] 保存 K1 Ethernet 的完整串口日志、ELF SHA256 和验收记录；证据见 `docs/K1_ETHERNET_DESIGN.md`；
- [x] 验证 EMAC0/RTL8211F 的 ARP、双向 ICMP 与最大 1472-byte IPv4 UDP payload；
- [x] K1 I2C2 polling lower-half、`/dev/i2c2` 和板载 EEPROM `0x50` 只读验收；连续两次读取 `0x54`，证据见 `docs/K1_I2C_REAL_BOARD_20260814.md`；
- [x] K1 SPI3 polling lower-half、`/dev/spi3` 与 40Pin MOSI/MISO 回环验收；仅短接 Pin 19 到 Pin 21，在 800 kHz、8-bit、模式 0 下连续两次收到 `A5 5A 3C C3`，未连接或访问 QSPI 启动 Flash，证据见 `docs/K1_SPI3_REAL_BOARD_20260814.md`；
- [ ] K1 PWM11 lower-half、`/dev/pwm11` 与 40Pin Pin 3 波形验收暂缓；当前没有示波器或逻辑分析仪，尚未声称实板通过，待具备 3.3V 兼容测量仪器后仅测量 1 kHz/50% 输出，详见 `docs/K1_PWM11_BRINGUP.md`；
- [x] K1 on-chip watchdog lower-half、`/dev/watchdog0` 与 `k1_wdt_smoke` 安全验收；U-Boot 已依次停止 `PMIC_WDT` 和 `watchdog@D4080000`，仅运行启动、三次喂狗和停止，未测试到期复位，证据见 `docs/K1_WATCHDOG_REAL_BOARD_20260814.md`；
- [x] K1 SDH2 eMMC lower-half、`/dev/mmcsd0` 实板初始化和只读块读取验收；`AppBringUp` 栈已提升到 8192，CMD1/设备注册通过，`bs=1024` 的单次读已实际走通 CMD18 多块路径并返回 0。驱动仍为只读，文件系统挂载和写入未验证，详见 `docs/K1_EMMC_BRINGUP.md`；
- [x] 已从真实 U-Boot FDT 复核 PLIC DTS，确认 `reg=0xe0000000`、`ndev=159`、hart 0 S-mode context 1 和 GPIO source 58；`CONFIG_K1_PLIC=y` 仍只在临时配置启用，证据见 `docs/K1_PLIC_REAL_BOARD_20260814.md`；
- [x] 已使用 Pin 22 -> Pin 33 的可控上升沿验证 GPIO PLIC claim/complete 与 callback；UART IRQ 继续避开 IER，证据见 `docs/K1_GPIO_BRINGUP.md`；
- [x] 显示 profile：2026-08-15 实板确认 `K1 display`、`/dev/fb0`、`800x480`、
  `32bpp`、`0x7f700000` 映射和 `FB test finished`；串口证据见
  `docs/K1_USB_DISPLAY_BRINGUP.md`。U-Boot 同轮报告 HDMI 无 HPD、MIPI 面板探测失败，
  屏幕实际色块和照片待连接并识别面板后补验；
- [x] USB Host profile：2026-08-15 实板已启动 xHCI、启用 PLIC source 125，并完成
  USB2817 的 USB2 root port 1 与 USB3 root port 2 枚举；同一 USB-A 路径在原厂
  Linux 已实测识别 Netac XS510 UAS 盘 (`08/06/62`)，证据见
  `docs/K1_USB_DISPLAY_BRINGUP.md`；
- [x] Netac XS510 的 NuttX UAS 实板验收：`usbhost_storage` 已实现 alt 1、四条
  UAS pipe、单 stream SCSI 流程，xHCI 已实现四项线性 stream context；严格 smoke
  已确认 UAS alt 1、xHCI stream 1、异步 UAS 传输 marker 和 `/dev/sda` 注册。
  本轮 ELF SHA256 为 `f77c157a7df42b223bfdb227cbc08877788bea72cfd61a62c9cc170d15fe2cd0`，
  flat payload SHA256 为 `d1ba9a5bf0c5997444d860d0de95aac2f08ee00272c1298cd2c2d8056e33e857`，
  完整串口证据见 `out/k1-serial/k1-usb-host-20260815T125230Z.log`。仅完成枚举、
  初始 SCSI 读取和块设备注册，未挂载、未写盘；已修正 USB3 Hub 端点分配失败时的
  空端点释放，并移除会触发该失败的全量 USB 信息日志；
- [x] USB probe profile：2026-08-15 以只读方式保存 DWC3 `GSNPSID=0x5533330a`、
  `GHWPARAMS*`、PHY 和 xHCI 原始串口输出；ELF SHA256 为
  `7f283f8fd7c7ec57c753ecd1bcc59ad3d63da471e055401a0d86fda4cd5ee474`，完整日志见
  `out/k1-serial/k1-usb-probe-20260815T130353Z.log`；不把 PWR Type-C 烧录口当作
  Host 口；
- [x] NuttX USB Device/Fastboot profile、8 MiB 下载缓冲和带 manifest/哈希/串口重启
  验收的主机侧闭环工具已接入构建；`usb_fastboot_8m_fix7` 已 RAM-only 启动到 NSH，
  但正常启动 PWR Type-C 没有枚举，`fastbootd` 无法取得配置后的 ep2。因此 runtime
  USB Device/Fastboot 尚未实板验收，不得用于刷写；官方 BootROM/Titan FDL 协议仍未
  实现。边界和完整记录见 `docs/K1_FLASH_RECOVERY_WORKFLOW.md` 与
  `docs/K1_USB_DEVICE_REAL_BOARD_20260815.md`；
- [x] 2026-08-15 按住 FDL 并按 RST 后，以只读方式记录 PWR Type-C
  BootROM 下载设备 `361c:1001`、接口 `ff/42/03` 和 Fastboot `version: 0.4`；未执行
  Titan/FDL/flash/erase 或任何持久写入。写协议、镜像容器和分区布局仍待独立确认；
- [ ] 单核稳定后才设计/启用 SBI HSM + SMP。

## 提交前

- [ ] `tools/ci_k1.sh --jobs 8` 全部通过；
- [ ] 工作树只包含预期比赛改动；
- [ ] 未提交构建目录、ELF、串口设备信息或敏感环境数据；
- [ ] 实板结论与日志证据一致，未把“编译通过”表述为“上板通过”；
- [ ] README、测试记录、许可证清单和最终 SHA256 已更新；
- [ ] AI Coding 日志已按比赛规则归档；
- [ ] 用户确认提交范围后再 commit/push/PR。
