# openvela on K1 项目描述

版本：2026-07-30
适用阶段：报名方向复核 / 项目方案审核

> 本文现在作为 VelaROS 的 K1 BSP 子计划保留。作品总体目标、原生 DDS 与裁剪版
> ROS 2 方案见 [`VELAROS_PROJECT_DESCRIPTION.md`](VELAROS_PROJECT_DESCRIPTION.md)。

## 一、基本信息

| 项目 | 内容 |
|---|---|
| 项目名称 | openvela on K1：MUSE Pi Pro RISC-V BSP 适配与实时外设验证 |
| 所属方向 | 2026 openvela AI 硬件开发者大赛——新硬件平台适配 |
| 目标硬件 | 进迭时空 MUSE Pi Pro，SpacemiT K1（8 核 RISC-V X60） |
| 目标系统 | openvela / NuttX，RV64 S-mode |
| 当前阶段 | 已完成 K1 首板启动、基础外设、显示 framebuffer handoff、USB probe、USB Host/UAS 和 eMMC 只读实板验证；PWM 波形及可写存储仍暂缓 |
| 核心交付 | K1 芯片层、MUSE Pi Pro 板级包、NSH defconfig、基础驱动、复现文档和演示 |

## 二、项目简介

本项目面向尚未完成 openvela 适配的 MUSE Pi Pro 开发板，为 SpacemiT K1
建立可复现、可维护的 openvela RISC-V BSP。项目保留开发板原有
BootROM、FSBL、OpenSBI 和 U-Boot 启动链，将 openvela/NuttX 作为 RV64
S-mode payload 装载运行，完成系统入口、内存布局、UART 控制台、SBI
时钟节拍、外部中断控制器和板级初始化的适配，最终实现稳定进入 NSH，并通过
40Pin GPIO 等基础外设完成可观察的功能演示。

项目首先保证单 hart 的最小系统链路稳定，不把 SMP、AMP、X60 AI 指令运行时、显示、音频等
高风险功能作为初赛成功的前置条件。在最小链路通过实板验证后，再根据时间和
硬件资料完整度选择扩展 PLIC 外部中断、GPIO/I2C/SPI、Ethernet、watchdog 和
eMMC 只读路径，以及 Linux + openvela AMP 原型。所有“已完成”结论均要求有源码、
构建产物或实板日志支撑。

## 三、项目背景与价值

K1 是国产 64 位多核 RISC-V SoC，MUSE Pi Pro 同时具备大容量内存、40Pin
扩展接口、网络、显示和 AI 加速能力。当前 openvela 在该平台缺少完整的芯片层和
板级支持。本项目的价值不只是让一个示例程序启动，而是补齐以下工程链路：

1. 建立 K1 对 openvela/NuttX RISC-V 公共架构能力的适配层；
2. 明确 OpenSBI、U-Boot 与 openvela 之间的权限级和启动参数边界；
3. 形成可重复构建、ELF 静态验收、上板打包、串口采集和异常定位方法；
4. 提供可复现的 BSP、驱动和文档，为后续 GPIO、总线、多核及 AIoT 应用扩展
   建立基础。

该目标与新硬件适配方向要求的“启动引导、UART 控制台、系统正常运行、defconfig、
板级初始化和必要驱动适配”直接对应。

## 四、项目目标与范围

### 4.1 初赛必须完成

1. U-Boot 能加载 K1 openvela ELF，并以 S-mode 进入系统入口；
2. 串口输出完整启动日志，openvela 稳定进入交互式 NSH；
3. SBI TIME 提供系统 tick，基础任务调度和延时功能正常；
4. 完成 K1 芯片层、MUSE Pi Pro 板级层、链接脚本和 NSH defconfig；
5. 验证一个基础外设功能，优先选择 40Pin GPIO 输出/输入，形成可重复 Demo；
6. 提供从编译、打包、U-Boot 加载到日志判定的完整复现文档；
7. 提交 AI Coding 日志、测试记录、许可证和第三方来源清单。

### 4.2 条件允许时完成

1. 按实际 DTB 复核 PLIC context，并验证至少一个可控外部中断源；
2. 在 GPIO 稳定后增加 I2C 或 SPI 其中一项基础总线验证；
3. 单核稳定后评估 SBI HSM/IPI 和 SMP；
4. 仅在资源隔离条件满足时制作 Linux + openvela AMP 技术原型。

### 4.3 本阶段不承诺

- 8 核 SMP 的完整稳定性和性能优化；
- openvela 直接接入 K1 X60 AI 自定义指令运行时、Wi-Fi、蓝牙、显示或音频全栈；
- Linux 与 openvela AMP 的产品级稳定性；
- 未经实板日志验证的启动、时钟、中断和外设结论。

## 五、技术方案

### 5.1 启动架构

首版采用现有量产固件链，避免替换 M-mode 固件：

```text
BootROM
  -> FSBL
  -> OpenSBI（M-mode 固件与 SBI 服务）
  -> U-Boot
  -> openvela/NuttX ELF @ 0x11000000（RV64 S-mode）
  -> early log
  -> nx_start
  -> NSH
```

目标 handoff 按 RISC-V 约定接收 boot hart ID 和 DTB 地址；实际 U-Boot 是否
完整保留这两个参数需要由首板日志确认。openvela 入口会保存收到的参数，初始化
`gp` 和栈，清理 `sscratch`，记录初始 `sstatus`、`satp`、`stvec`，随后进入
NuttX 启动流程。首轮只运行 hart 0，避免在串口、timer 和中断链尚未验证时同时
引入多核变量。

当前 ELF 固定链接和装载到 `0x11000000`。该地址来自已在 K1 实板运行的参考
项目，但在比赛板上仍需结合实际 U-Boot relocation、DTB `reserved-memory`
和 OpenSBI 占用区再次核对；复核前不把它视为最终平台常量。

### 5.2 权限级与 openvela 运行模式

openvela 运行在 S-mode，OpenSBI 保持在 M-mode 并提供 TIME、HSM、IPI 等标准
服务。首版不直接接管 CLINT，也不重写厂商 FSBL/OpenSBI。这样可以缩小移植面，
并与 K1 现有 U-Boot/Linux 启动基础保持兼容。

首版使用 flat ELF 和固定 DRAM 窗口，暂不开启复杂 MMU 映射。进入 NSH 并完成
基础稳定性测试后，再依据实际 cache、MMU 和保留内存情况决定是否启用 Sv39
映射或扩展多核。

### 5.3 UART 控制台

调试串口暂按以下参数适配：

| 参数 | 当前值 |
|---|---:|
| MMIO 基址 | `0xd4017000` |
| 寄存器间隔 | 4 字节（reg-shift 2） |
| 访问宽度 | 32 bit |
| 波特率 | 115200 8N1 |
| IRQ | 42 |

参考实板记录显示，K1 在 S-mode 写 UART IER 可能导致 APB 访问挂死，因此首版不
直接使用会写 IER 的通用 16550 中断路径，而是保留 U-Boot 已建立的串口配置，
提供 K1 专用 polling console。当前源码和 ELF 检查都会拒绝 UART IER 写路径。
只有确认厂商寄存器语义和实板行为后，才评估中断式收发。

### 5.4 Timer 与中断

系统时钟采用 NuttX RISC-V 公共 timer lower-half，通过 OpenSBI TIME 设置下一次
时钟事件，当前 timebase 按 24 MHz 配置。上板后需要通过 SBI extension 探测、
系统 tick 和持续运行日志验证频率，不能仅凭编译通过认定 timer 正常。

K1 PLIC 基址暂按 `0xe0000000`、159 个 source 设计。参考 DTS 表明每个 hart
依次配置 M-mode 和 S-mode context，因此 hart 0 S-mode 对应 context 1。当前已
提供只支持 hart 0 的可选 PLIC 实现，但在默认 defconfig 中关闭。只有实际比赛
DTB 与寄存器地址复核一致后，才启用并验证 claim/complete；UART IRQ 不作为首个
中断源，以避免和 IER 风险耦合。

### 5.5 BSP 与构建集成

项目按 SoC 和板级职责拆分：

```text
chip/k1/                         K1 入口、UART、timer、IRQ/PLIC
board/k1/muse_pi_pro/            板级初始化、内存布局、NSH defconfig
tools/build_k1.sh                可重复构建
tools/check_k1_elf.sh            ELF、入口、段权限和关键符号验收
tools/package_k1_bringup.sh      U-Boot 首板包
tools/capture_k1_serial.sh       串口原始日志采集
tools/decode_k1_trap.sh          scause/sepc/stval 解析和符号化
```

manifest 将两层映射到 openvela 工作区的
`vendor/spacemit/chips/k1` 和
`vendor/spacemit/boards/k1/muse_pi_pro`。构建产物需要满足 ELF64
little-endian RISC-V、入口和首个 LOAD 段地址正确、代码段与数据段分离且不存在
RWX 段。

### 5.6 实板验证与 Demo

首板不会覆盖默认 `bootcmd`，也不执行 `saveenv`。验证按以下顺序进行：

1. 保存原厂可恢复介质，记录 U-Boot、OpenSBI、板卡和供电信息；
2. 只读检查实际 DTB、DRAM、保留内存和 PLIC 节点；
3. 手工加载 ELF，先确认入口单字符和 early log；
4. 确认完整 banner、timer tick 和 NSH；
5. 执行 `help`、`ps`、`free`、延时和连续运行测试；
6. 连接 40Pin GPIO 的 LED/逻辑分析仪或回环线，演示 GPIO 输出与输入；
7. 保存原始串口日志、命令记录、ELF SHA256 和演示视频。

## 六、AMP 方案与 openvela 的定位

### 6.1 当前决策

AMP 不是初赛主线，也不是项目成功的前置条件。当前主线是让 openvela 作为
K1 上唯一的 S-mode payload 独立启动，并完成 BSP 和基础外设闭环。这样最符合
新硬件适配方向的最低验收要求，也能在没有实板验证的当前阶段控制风险。

### 6.2 若扩展 AMP，openvela 的明确定位

若后续实现 Linux + openvela AMP，两套系统的职责如下：

| 系统 | 定位 | 主要职责 |
|---|---|---|
| Linux | 富功能主系统 / AI 服务域 | 启动管理、存储、网络、显示、多媒体、X60 AI 指令运行时和 AI 推理 |
| openvela | 独立实时执行域 | 确定性传感采集、GPIO/I2C/SPI 实时控制、执行器控制、看门狗与安全状态机 |

openvela 在 AMP 中不是“为了展示而运行的第二个系统”，也不替代 Linux 的成熟
AI 推理运行时和多媒体生态；它负责 Linux 难以保证确定性的实时 I/O 与控制闭环。Linux
侧完成 AI 推理后，通过核间通信向 openvela 下发有边界的控制目标，openvela
执行实时任务并回传状态和时间戳。

### 6.3 AMP 的拟议实现边界

AMP 原型拟使用专用 hart、独立内存区和独占外设/IRQ，Linux 与 openvela 之间
通过共享内存上的 OpenAMP/RPMsg（openvela 侧可复用 Rptun/RPMsg 框架）通信。
实施前必须先确认：

1. Linux CPU 拓扑和 OpenSBI HSM 是否允许安全释放专用 hart；
2. DTB 中能否为 openvela 预留连续内存、vring 和 buffer；
3. cache 一致性、内存屏障、地址转换和通知中断机制；
4. PLIC context、IRQ、GPIO pin、clock/reset 的唯一所有者；
5. Linux remoteproc/OpenAMP 与 K1 固件链是否已有可复用支持。

以上任一条件不明确时，AMP 只保留为设计说明，不进入提交版必验功能。不能仅靠
`isolcpus` 或划出一段内存就宣称完成 AMP。

## 七、当前进度

截至 2026-08-15：

| 状态 | 内容 | 证据边界 |
|---|---|---|
| 已完成 | K1 芯片层、MUSE Pi Pro 板级骨架、NSH defconfig | 已进入实际构建 |
| 已完成 | RV64 S-mode hart 0 入口、`gp` 初始化、handoff/CSR 日志 | 源码和 ELF 可检查 |
| 已完成 | 不写 IER 的 polling `/dev/console` | 源码和 ELF 静态验收 |
| 已完成 | OpenSBI TIME 24 MHz timer 和单核 10 分钟稳定运行 | MUSE Pi Pro 实板日志 |
| 已完成 | PLIC context 1、GPIO source 58 和 GPIO 边沿 IRQ | FDT 只读核对及 Pin 22 -> Pin 33 实板日志 |
| 已完成 | EMAC0/RTL8211F polling 网络、I2C2、SPI3、watchdog、SDH2 eMMC 只读路径 | 对应实板日志；eMMC 已验证 CMD18 多块读取，PWM 波形除外 |
| 已完成 | 可重复构建、ELF 验收、U-Boot 包、串口采集和 trap 解析 | 主机侧工具已验证 |
| 已完成 | 干净构建 1097 个目标通过 | 只证明编译链接闭环 |
| 已完成 | U-Boot framebuffer handoff、DWC3 Host/xHCI、USB2817 双根端口、Netac XS510 UAS 和 `/dev/sda` 注册 | RAM-only 实板日志；不等同于原生 DPU/面板全栈、USB Device/FDL 或磁盘写入 |
| 已完成 | DWC3/PHY register probe 原始输出 | 只读 RAM-only 实板日志；不等同于 USB Device/FDL |
| 待实板验证 | 已识别面板的视觉色块 | `display_fb` 仍依赖 U-Boot handoff，需连接并识别面板 |
| 待完成 | PWM11 波形、持久烧录/恢复闭环、K1 Fast DDS 实板互操作 | 需要测量仪器或进一步软件集成 |
| 可选 | SMP、AMP、X60 AI 指令运行时和复杂外设 | 尚未实现，不作为承诺 |

当前基线 ELF 为 RV64 little-endian RISC-V，入口为 `0x11000000`，RX/RW
LOAD 段分离且无 RWX 段。当前产物 SHA256 为
`7e47941ecd558123fb16c3e22bb2170b5fbf80b548c2a5cd082c00bf7d1a34ee`。
这些结果不能替代实板启动证据。

## 八、实施计划

比赛作品提交截止日为 2026-09-20。以 2026-07-30 为起点还剩 52 天，计划采用
“主线优先、扩展设门”的方式推进。

| 阶段 | 建议时间 | 交付和退出条件 |
|---|---|---|
| 无板收口 | 现在至开发板到达 | 构建、打包、日志、异常定位和首板检查表保持可用 |
| 首板启动 | 到板后 1–5 天 | 确认 DTB/内存；获取 early log，解决入口和 UART 问题 |
| 最小系统 | 到板后 6–10 天 | timer 正常、进入 NSH、连续运行至少 10 分钟 |
| 基础外设 | 到板后 11–18 天 | PLIC 按需启用；完成 GPIO Demo 和日志 |
| 稳定与交付 | 到板后 19–28 天 | 回归、文档、测试记录、视频、AI 日志和提交材料 |
| 扩展功能 | 仅使用剩余缓冲 | I2C/SPI 或 AMP 可行性原型，不影响主线 |

范围控制节点：

- 若 2026-08-10 前到板，主线有合理缓冲，可在 NSH 稳定后考虑一项扩展；
- 若 2026-08-11 至 08-15 到板，主线仍可行，但扩展项必须等最小系统通过；
- 若 2026-08-20 仍未到板，冻结 AMP/SMP/X60 AI 指令运行时，只做最小系统和一个基础 Demo；
- 若 2026-08-31 仍未到板，实板移植进入高风险状态，应立即与组委会确认板卡；
- 只有在 2026-08-25 前完成单核 NSH、timer 和 GPIO/PLIC 基线，且 AMP 所需
  资源分区证据完整，才允许投入不超过 5 个工作日做 AMP 可行性原型。

## 九、Early Check：可行性与 AI 生成细节复核

### 9.1 时间是否来得及

结论：**初赛主线有条件可行，前提是开发板最晚在 8 月中旬左右到达且首板没有
长期阻塞；产品级 AMP 来不及作为硬承诺。**

无板阶段已经完成了大部分可离线完成的工作。预计到板后，单核启动、UART、
timer、NSH 和 GPIO Demo 需要 12–18 个有效工作日，文档、回归和视频需要
5–8 个工作日，仍可保留故障缓冲。AMP 会额外引入 hart 启停、内存隔离、
cache 一致性、PLIC/IRQ 归属、remoteproc 和双系统调试，预计至少再需要
2–4 周，且高度依赖 K1 固件与 Linux 支持，因此只适合作为可选原型。

### 9.2 当前最可能出错的技术假设

| 假设 | 当前依据 | 必须怎样验证 |
|---|---|---|
| `0x11000000` 可安全装载 | 参考项目实板记录 | 实际 U-Boot、DTB reserved-memory 和 OpenSBI 区域 |
| UART 基址和寄存器宽度正确 | 厂商/参考 DTS | U-Boot DTB 和首板只读/输出测试 |
| 禁止写 IER 能避免 APB 挂死 | 参考项目实板记录 | 保持 polling，观察连续串口运行 |
| timebase 为 24 MHz | DTS/参考资料 | SBI 探测、tick/延时测量和连续运行 |
| PLIC hart 0 S-mode 是 context 1 | 参考 DTS 顺序推导 | 实际比赛 DTB 和可控 IRQ claim/complete |
| U-Boot 按预期传递 hart/DTB | RISC-V 约定与参考入口 | early log 直接记录 `a0/a1` 和 CSR |
| AMP 可由专用 hart + 共享内存实现 | 架构设计 | Linux/OpenSBI HSM、remoteproc、cache 和 IRQ 实测 |

### 9.3 AI 辅助内容的处理原则

AI 用于资料检索、代码骨架、测试脚本和审查提示，但不作为硬件事实来源。对 AI
生成或补全的技术细节执行以下规则：

1. 地址、位定义、时钟和 IRQ 必须能追溯到厂商文档、实际 DTB 或可信实板日志；
2. “能编译”“ELF 符合格式”“参考项目能运行”与“本项目实板能运行”分开表述；
3. 所有驱动代码需要人工逐行检查 MMIO 宽度、寄存器副作用、并发和错误路径；
4. 每次功能启用必须保留原始串口日志、配置、ELF SHA256 和复现命令；
5. 无法在截止前完成双重证据闭环的功能，从必选目标降为可选设计，不写成成果。

当前代码已完成静态检查、驱动模式审查和首板行为验证。仍未完成的项目明确限于
PWM11 波形、持久烧录/恢复闭环、Fast DDS 实板互操作，以及未具备可靠寄存器依据
的复杂外设；这些不写成已完成成果。

## 十、预期成果与验收标准

最终交付包括：

1. K1 芯片级适配代码和 MUSE Pi Pro 板级支持包；
2. 可直接构建的 NSH defconfig、链接脚本和 manifest 映射；
3. polling UART、SBI timer、按条件启用的 PLIC 和至少一个基础外设；
4. 可重复构建、ELF 验收、U-Boot 打包、日志采集和异常解析工具；
5. 启动/移植指南、测试记录、来源与许可证清单；
6. 从 U-Boot 加载到 NSH 和 GPIO Demo 的完整演示视频；
7. 与代码对应的 AI Coding 日志和人工复核记录。

主线验收以实板证据为准：

- U-Boot 可重复加载同一 ELF；
- 串口完整显示 openvela/NuttX 启动过程；
- 系统稳定进入 NSH；
- timer、任务调度和基础命令正常；
- GPIO Demo 可重复；
- 复现者能够按文档从干净工作区得到一致产物并完成上板。

## 十一、项目特点

1. **国产多核 RISC-V 的首次 openvela 适配**：覆盖从 S-mode 入口到板级
   defconfig 的完整链路；
2. **面向真实 K1 风险设计**：针对 UART IER、OpenSBI handoff、PLIC context
   和保留内存等问题设置显式保护和验证门槛；
3. **证据驱动的移植过程**：构建、ELF、日志、异常寄存器和 SHA256 均可追踪；
4. **可演进的系统定位**：先完成独立 openvela BSP，再将 openvela 作为确定性
   实时域接入 Linux + AI 的 AMP 架构；
5. **AI 辅助但不依赖 AI 结论**：AI 生成内容必须经过资料、代码审查和实板日志
   三层校验，保证最终成果可复现、可维护、可上游。
