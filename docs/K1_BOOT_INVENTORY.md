# K1 启动资料盘点

更新时间：2026-07-30

## 1. 目标与边界

本文件记录 MUSE Pi Pro（SpacemiT K1）openvela bring-up 的事实依据、暂定
决策和待验证项。目标是避免把参考项目的“可运行结论”误当成 openvela 已验证
结果。

第一阶段只覆盖：

- U-Boot/OpenSBI 到 NuttX 的 handoff；
- RV64 S-mode 入口；
- DRAM 中的固定地址 flat build；
- 轮询 UART 控制台；
- SBI timer；
- 单 hart NSH，随后再启用 SMP。

存储、网络、显示、音频和 NPU 不属于第一阶段。

## 2. 资料来源

| 编号 | 来源 | 用途 | 可信度 |
|---|---|---|---|
| S1 | [openvela K1 适配指引](https://github.com/open-vela/vendor_SpacemiT/blob/dev-ai-contest-2026/boards/k1/muse_pi_pro/README_zh-cn.md) | 官方赛道边界和上游目录 | 官方 |
| S2 | [K1 Datasheet](https://cdn-resource.spacemit.com/file/chip/K1/K1_datasheet_en.pdf) | SoC 寄存器和内存图 | 芯片厂商 |
| S3 | [SpacemiT K1 文档](https://github.com/spacemit-com/docs-chip/blob/main/en/key_stone/k1/k1_docs/k1_ds.md) | 可检索芯片资料 | 芯片厂商 |
| S4 | `/home/sw/Dev/musepi-rvv-os-reference`，commit `1761a1ae2801163f09ea0eefbec89c1c0fa212b1` | 已在 K1 实板运行的 S-mode OS 参考 | 实板参考 |
| S5 | 参考仓 `docs/K1_BOOT_CHAIN.md` | 实板探测、启动链、镜像布局 | 实板参考 |
| S6 | 参考仓 `platform.h`、`uart.c`、`start.S`、U-Boot env | 地址、UART 风险和 handoff 行为 | 实现证据 |
| S7 | 当前 openvela/NuttX RISC-V common code | SBI、S-mode、timer、IPI 可复用能力 | 当前基线 |

参考仓根代码采用 MIT 许可证，vendored U-Boot/OpenSBI 有各自许可证。本项目
当前仅摘录硬件事实和验证结论，没有复制参考仓源码。

## 3. 已确认的实板事实

以下内容来自 S4-S6 的 2026-05-17 实板只读探测和后续启动验证。

### 3.1 CPU 与运行模式

| 项目 | 值 |
|---|---|
| CPU | SpacemiT X60，8 hart |
| 架构 | RV64 little-endian |
| DT ISA | `rv64imafdcv` |
| MMU | Sv39 |
| DT compatible | `spacemit,k1-x` |
| SBI | specification v1.0 |
| 量产链路 | OpenSBI -> U-Boot -> Linux |
| openvela 目标模式 | S-mode |

结论：openvela 应复用 OpenSBI 的 TIME、IPI 和 HSM 服务。第一版不应尝试替换
FSBL/OpenSBI，也不应从 M-mode 起步。

### 3.2 DRAM 与装载地址

设备树暴露：

- `0x00000000` 起的 2 GiB DRAM；
- `0x100000000` 起的 6 GiB DRAM。

已观察的低地址保留区包括：

- `0x00000000 - 0x0007ffff`：M-mode 保留；
- `0x00100000 - 0x006fffff`：remoteproc/vring/buffer；
- `0x2ff40000` 附近：显示保留区；
- `0x58000000 - 0x6fffffff`：Linux CMA。

参考系统已验证：

- payload/link address：`0x11000000`；
- 暂用窗口：128 MiB；
- DTB staging：`0x31000000`。

`0x11000000` 对 openvela 仍是“高可信暂定值”，必须用比赛使用的 U-Boot、DTB
和 OpenSBI reserved-memory 再核对一次。

### 3.3 UART

主控制台：

| 项目 | 值 |
|---|---|
| base | `0xd4017000` |
| size | `0x10000` |
| IRQ | 42 |
| compatible | `spacemit,pxa-uart` |
| baud | 115200 8N1 |
| register shift | 2 |
| register I/O width | 32 bit |

关键风险：

- U-Boot 已初始化该 UART；
- 参考实板测试记录：S-mode 写 IER 会导致 APB 总线挂死；
- 参考实现仅保留 U-Boot 配置、复位 FIFO，并以轮询方式接收；
- NuttX 通用 `uart_16550.c` 即使抑制初始配置，部分路径仍会写 IER。

因此第一版需要 K1 专用的安全 polling console，或为通用 16550 lower-half 增加
真正禁止 IER 写入的能力。未解决前不能启用中断式 RX。

### 3.4 中断和时钟

| 模块 | 参数 |
|---|---|
| PLIC base | `0xe0000000` |
| PLIC size | `0x04000000` |
| PLIC sources | 159 |
| CLINT base | `0xe4000000` |
| CLINT size | `0x00010000` |
| timebase | 24 MHz |
| timer mode | SBI TIME；硬件报告 SSTC 可用 |

Linux 日志报告 8 个 handler、16 个 PLIC contexts。参考 U-Boot DTS 的
`interrupts-extended` 已确认每个 hart 按 M-mode(11)、S-mode(9) 排列，因此
hart 0 S-mode 是 context 1。推导和默认关闭的实现边界见
`K1_PLIC_DESIGN.md`；拿到板后仍须用实际 U-Boot DTB 复核。

## 4. 启动链设计

目标链路：

```text
BootROM
  -> FSBL
  -> OpenSBI
  -> U-Boot
  -> load NuttX payload @ 0x11000000
  -> load k1-x_MUSE-Pi-Pro.dtb @ 0x31000000
  -> enter NuttX in S-mode
```

首版策略：

1. 保留厂商 FSBL/OpenSBI/U-Boot；
2. 在现有可启动 SD 卡上增加独立 openvela 启动项；
3. 先手工执行 U-Boot 命令，不立即覆盖默认 `bootcmd`；
4. flat NuttX 固定链接到 `0x11000000`；
5. 主 hart 单核运行并打印 banner；
6. timer 和 UART 稳定后再通过 SBI HSM 启动其他 hart。

参考仓的专用 U-Boot 使用 `bootelf -p` 和 `bootmusepi` 两条路径。openvela 首轮
优先验证 ELF handoff，确认段装载和入口后再固定 raw binary 流程。

## 5. U-Boot handoff 待确认

RISC-V 常见约定是：

- `a0`：boot hart ID；
- `a1`：DTB 地址。

参考仓确认 `a0` 可作为 hart ID 使用，但其主入口没有依赖 `a1` 保存 DTB。对
openvela 必须在首轮实板日志中记录并验证：

- 实际 privilege mode；
- `a0` 和 `a1`；
- `sstatus`、`satp`、`stvec` 初始值；
- OpenSBI HSM/TIME/IPI extension 可用性；
- cache 和 MMU 状态；
- U-Boot 是否已停止两个 watchdog。

## 6. openvela/NuttX 复用与缺口

当前 NuttX 已有：

- RISC-V S-mode trap 公共代码；
- SBI TIME、IPI、HSM、RFENCE、SRST 和 PMU 封装；
- RV64、SMP、Sv39 和 vector 上下文能力；
- PLIC 和 machine timer 的多种 SoC 参考实现。

当前已补齐：

- `vendor/spacemit/chips/k1` 自定义芯片层；
- hart 0 S-mode 入口、IRQ 编号和异常分发；
- 不写 IER 的轮询 `/dev/console`；
- 通过 SBI TIME 接线的 24 MHz timer lower-half；
- 固定链接到 `0x11000000` 的 RV64 NSH ELF 构建闭环。

当前仍缺少：

- K1 PLIC S-mode context 的实际比赛固件复核和外部 IRQ 上板验证；
- K1 timer 和轮询控制台的首次实板验证；
- K1 cache/MMU 初始化策略；
- 最终安全 SD 布局确认（当前已有非破坏性 U-Boot 上板包）。

按比赛规则，通用 NuttX 架构改动应提交到公共 NuttX 的
`dev-ai-contest-2026` 分支；板级、vendor 驱动和复现文档保留在本比赛仓并最终
上游到 `vendor_SpacemiT`。

## 7. 分阶段验证清单

### M0：资料与骨架

- [x] 记录参考仓 commit 和许可证；
- [x] 记录 CPU、DRAM、UART、PLIC、CLINT 和 timebase；
- [x] 建立 `board/k1/muse_pi_pro`；
- [x] 更新 manifest 映射；
- [x] 建立最小 NSH defconfig 骨架。

### M1：最早期串口

- [x] K1 芯片层进入编译；
- [x] ELF LOAD 地址确认从 `0x11000000` 开始；
- [x] 启动阶段标记和同步异常寄存器日志进入 ELF；
- [x] 自动验证 ELF LOAD 段、关键符号、Kconfig 和 UART IER 安全约束；
- [ ] U-Boot 手工加载 ELF；
- [ ] NuttX 入口打印单字符；
- [ ] polling UART 打印完整 banner；
- [x] 静态验证 K1 console 的唯一 MMIO 写目标为 UART THR，不存在 IER 写路径；

### M2：timer 与 NSH

- [ ] SBI TIME extension 探测；
- [ ] 24 MHz tick 校准；
- [ ] 连续运行 10 分钟无 tick 漂移或 trap；
- [ ] 进入 NSH；
- [ ] `help`、`ps`、`free` 可运行。

### M3：中断与 SMP

- [x] 从参考 DTS 确认 PLIC context 1，并保持默认关闭；
- [ ] 从实际比赛 DTB 复核 PLIC context；
- [ ] 外部中断 claim/complete；
- [ ] SBI HSM 拉起次级 hart；
- [ ] 8 hart 压力运行；
- [ ] SBI IPI 调度验证。

## 8. 首轮上板需采集的证据

1. 完整 UART 日志，从 BootROM/FSBL 开始；
2. U-Boot `version`、`bdinfo`、`printenv`；
3. `fdt addr`、`fdt print /cpus`、`fdt print /soc/serial@d4017000`；
4. `fdt print /soc/interrupt-controller@e0000000`；
5. payload 文件大小、加载地址和入口地址；
6. 第一次 exception 的 `scause`、`sepc`、`stval`；
7. 成功进入 NSH 后的版本、内存和任务列表。

以上日志应和对应 AI Coding session 一起归档到比赛仓。

## 9. 2026-07-30 构建基线

构建命令：

```bash
contest2026_287_Agenter/tools/build_k1.sh --clean --package --jobs 8
```

结果：

- 从空的目标 CMake 目录开始配置，1097 个编译/链接目标和 `System.map`
  生成全部通过；
- 产物：`cmake_out/muse_pi_pro_nsh/nuttx`；
- 上板包：`out/k1-bringup/`；
- 目标：ELF64 little-endian RISC-V，soft-float ABI；
- 入口和首个 LOAD 段：`0x11000000`；
- LOAD 段权限已拆分为 RX 与 RW，不存在 RWX 段；
- 当前验收产物 SHA256：
  `7e47941ecd558123fb16c3e22bb2170b5fbf80b548c2a5cd082c00bf7d1a34ee`；
- 当前只启用 SBI supervisor timer，中断式 UART 和 PLIC 外部中断保持关闭。

同一源码和默认 defconfig 的两次干净构建得到相同 SHA256。另以临时
`CONFIG_K1_PLIC=y` 配置完成 1098 个目标的编译链接，确认 PLIC 骨架进入 ELF；
随后已恢复默认关闭配置并重新生成上述最终产物。

此基线的下一验收门是实板 U-Boot `bootelf -p`，不是继续扩展外设。
