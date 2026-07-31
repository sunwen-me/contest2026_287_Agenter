# VelaROS on K1 项目描述

版本：2026-07-31  
适用阶段：报名方向复核 / 项目方案审核

## 一、基本信息

| 项目 | 内容 |
|---|---|
| 项目名称 | VelaROS：基于 openvela 与原生 DDS 的轻量级 ROS 2 机器人运行时 |
| 所属方向 | 2026 openvela AI 硬件开发者大赛——新硬件平台适配 + AI/机器人系统创新 |
| 目标硬件 | 进迭时空 MUSE Pi Pro，SpacemiT K1（8 核 RISC-V X60） |
| 基础系统 | openvela / NuttX，RV64 S-mode |
| ROS 2 基线 | ROS 2 Lyrical；`rclcpp 32.0.0`、`rcl 10.4.4`、`rmw_fastrtps_cpp 9.4.8` |
| DDS 实现 | eProsima Fast DDS，静态类型支持，首版只启用 UDPv4/RTPS |
| 核心交付 | K1 openvela BSP、VelaROS 裁剪运行时、DDS 网络通信、双向 Topic Demo |

## 二、项目简介

VelaROS 的目标不是只把 openvela 启动在 K1 上，而是在 openvela 上建立一个能够
直接加入 ROS 2/DDS 网络的轻量级机器人运行时。项目首先完成 SpacemiT K1 和
MUSE Pi Pro 的 openvela BSP，使系统能够以 RV64 S-mode 启动、输出 UART 日志并
进入 NSH；在此基础上裁剪并移植 ROS 2 的核心运行链路：

```text
VelaROS C++ Node API
        |
  trimmed rclcpp
        |
       rcl
        |
  rmw_fastrtps_cpp
        |
 Fast DDS / Fast-CDR
        |
 openvela POSIX + NuttX network
        |
 SpacemiT K1 BSP / Ethernet
```

VelaROS 保留 ROS 2 的 Node、Topic、Publisher、Subscriber、Timer、Executor、
静态消息类型和基础 QoS 语义，并使用 Fast DDS 完成分布式发现、序列化和 RTPS
通信。运行在 openvela 上的节点拥有本地 DDS DomainParticipant，可被 Linux
ROS 2 的 `ros2 topic list/echo/hz` 直接发现和访问。

本项目不是 micro-ROS：不使用 Micro XRCE-DDS Client，不依赖 micro-ROS Agent，
也不由 Agent 代理 ROS 2 实体。VelaROS 节点自身运行 ROS 2 的 `rcl/rmw` 链路和
DDS participant。

## 三、项目背景与价值

机器人通常使用 Linux + ROS 2 处理感知、规划和 AI，但底层电机、传感、执行器和
安全控制更需要确定性调度、快速启动和较小系统开销。传统方案往往在 MCU 上使用
专用协议或代理式客户端，Linux 与实时控制域之间存在两套编程模型。

VelaROS 希望在 openvela 的实时内核、POSIX 接口和驱动框架之上保留 ROS 2 的核心
API 与 DDS 数据模型，使实时节点和 Linux ROS 2 节点共享 Topic、消息类型、QoS
与发现机制。其价值包括：

1. 让 openvela 从“硬件操作系统”扩展为 ROS 2 兼容的实时机器人执行平台；
2. 让实时驱动节点直接进入 ROS 2 graph，减少私有桥接协议和重复数据模型；
3. 利用 DDS 的分布式发现、类型支持和 QoS，形成可复用的机器人通信底座；
4. 在国产多核 RISC-V K1 上验证 openvela、ROS 2 和 DDS 的完整纵向栈；
5. 为后续 Linux + openvela AMP、实时控制、机器人和边缘 AI 场景建立基础。

## 四、VelaROS v0.1 的技术边界

### 4.1 必须保留的 ROS 2 能力

VelaROS v0.1 只实现可验证的 ROS 2 原生垂直切片：

| 层级 | 首版保留内容 |
|---|---|
| Client API | Node、Publisher、Subscription、WallTimer、SingleThreadedExecutor |
| rcl | context、node、publisher、subscription、wait set、timer |
| rmw | participant、node、publisher、subscription、wait/take、graph 查询基础能力 |
| DDS | DomainParticipant、发现、DataWriter/DataReader、RTPS、CDR |
| 消息 | 预生成的静态类型支持；`builtin_interfaces`、`std_msgs`、`geometry_msgs` 精选类型 |
| QoS | Keep Last、Reliable、Best Effort、Volatile、基础 Deadline/Liveliness 状态 |
| 传输 | UDPv4；组播发现和可配置 unicast initial peers |

### 4.2 首版明确裁掉的能力

- `rclpy`、Python 解释器和 Python 消息支持；
- 在目标板上运行 `colcon`、`ament`、`rosidl` 生成器和 `ros2` CLI；
- 动态库、pluginlib、运行时插件发现和 ament resource index；
- Services、Actions、Parameters、Lifecycle、Composition；
- rosbag2、launch、RViz、tf2 全栈、导航和感知算法；
- DDS Security、TCP、IPv6、动态类型、Persistence 和 Shared Memory transport；
- 多 RMW 实现切换和对任意 ROS 2 发行版的兼容承诺。

上述功能不是永久放弃，而是避免在首版把“ROS 2 兼容运行时”扩张为完整桌面发行版。

### 4.3 与 micro-ROS 的区别

| 项目 | VelaROS v0.1 | micro-ROS 典型模式 |
|---|---|---|
| 中间件数据面 | Fast DDS / RTPS | Micro XRCE-DDS |
| 目标端 DDS Participant | 有 | 通常没有，由 Agent 代理 |
| 是否需要 Agent | 不需要 | 通常需要 |
| ROS 2 核心链路 | 裁剪 `rclcpp/rcl/rmw_fastrtps` | `rcl/rclc/rmw_microxrcedds` |
| 网络发现 | 目标端直接参与 DDS discovery | Agent 加入 DDS graph |
| 目标 | MPU/较大 RTOS 上的原生 ROS 2 子集 | MCU 资源受限客户端 |

## 五、ROS 2 裁剪与移植方案

### 5.1 依赖分层

计划按以下顺序建立可编译依赖：

1. 基础层：`rcutils`、`rcpputils`、`osrf_testing_tools_cpp` 的必要公共头；
2. 类型层：`rosidl_runtime_c/cpp`、`rosidl_typesupport_interface`；
3. 中间件接口：`rmw`、`rmw_dds_common` 的必要部分；
4. Fast DDS 适配：`rmw_fastrtps_shared_cpp`、静态
   `rmw_fastrtps_cpp`；
5. 客户端核心：`rcl`；
6. C++ API：裁剪 `rclcpp`，只保留 Node、pub/sub、timer 和单线程 executor；
7. 消息包：只引入 Demo 需要的预生成消息和 Fast RTPS type support。

不重新设计私有 RMW，也不把 DDS API 简单封装后称为 ROS 2。VelaROS 必须复用
ROS 2 标准 `rcl` 和 `rmw` 接口，Linux 端 ROS 2 能够把它识别为正常节点。

### 5.2 构建系统拆分

ROS 2 的代码生成和目标端运行分开：

```text
Ubuntu host
  -> colcon / ament / rosidl
  -> 生成 C/C++ message、introspection 和 Fast RTPS type support
  -> 导出固定版本的 generated sources

openvela build
  -> Kconfig + CMake
  -> 编译静态 ROS 2 libraries、Fast DDS、generated messages
  -> 链接进入 VelaROS 应用
```

目标板不运行 Python 代码生成器，也不解析完整 ROS 2 安装空间。package index、
类型支持映射和 RMW 选择改为构建期静态注册，避免 `dlopen()`、环境 hook 和
ament resource index。

### 5.3 POSIX 适配

移植重点包括：

- pthread、mutex、condition variable、thread-local storage；
- clock、timer、sleep 和 monotonic time；
- UDP socket、multicast membership、`select/poll` 和 socket options；
- filesystem、environment、hostname 和 network interface 枚举；
- C++20/libc++、exceptions、atomics 和 aligned allocation；
- logging、error state、allocator 和 bounded memory 行为；
- DDS 接收线程、executor 线程的优先级和栈大小。

无法直接支持的 POSIX 功能通过小型 openvela compatibility layer 集中处理，
避免在 ROS 2 上游源码中散布平台条件宏。

### 5.4 Fast DDS 基线

当前 openvela 工作区的 `external/fastdds` 已恢复并锁定依赖源码，包含：

- Fast DDS 3.6.1、Fast-CDR 2.3.6、foonathan memory 0.7.4；
- Asio 1.34.2 和 openvela 已打包的 TinyXML2 9.0.0；
- NuttX 静态库构建、C++20、exceptions 和无 `dl.so` 配置；
- UDP/TCP/Shared Memory 等 DDS 示例的 Kconfig 入口；
- 默认关闭 Shared Memory transport 和 Security。

目前 Fast DDS、Fast-CDR 和 foonathan_memory 已完成 AArch64/openvela 静态库
编译和完整 goldfish 固件链接，源码 revision、Fast DDS/libc++abi 补丁和无 ROS
环境污染的构建入口均已纳入比赛仓。DDS HelloWorld 已完成 participant/endpoint
匹配、3 条消息发送、3 条消息接收以及退出后的 `ps` 无残留自动验收。

已解决的运行差异包括 libc++abi 跨 task-group exception TLS、Fast DDS 默认
8 KiB 线程栈、TypeObject 缺失查询异常路径，以及 NuttX 跨线程关闭 socket 时
阻塞 `recvfrom()` 不可靠唤醒。当前使用 64 KiB DDS 内部线程栈、loopback 单播
initial peers 和 100 ms `poll()` 完成确定性 simulator 验收。以上只证明
openvela simulator 的 DDS 数据面和资源回收可用，不代表 K1 实板网络已经可用。

### 5.5 版本策略

首版锁定 ROS 2 Lyrical。Lyrical 是当前开发环境 2026-07-31 已安装的稳定发行版，
其 `ros-lyrical-fastdds` 为 3.6.1，与已在 openvela 编译的 Fast DDS 版本一致。
不使用 Rolling 作为交付基线，因为其核心包和 RMW 接口持续变化。版本组冻结为：

1. `rclcpp 32.0.0`、`rcl 10.4.4`、`rmw_fastrtps_cpp 9.4.8`；
2. Fast DDS 3.6.1、Fast-CDR 2.3.6、foonathan_memory 0.7.4、Asio 1.34.2；
3. 消息生成器、生成源码和 Linux 对端统一使用 ROS 2 Lyrical；
4. 精确 Git revision、包版本、许可证和本项目补丁全部留档；
5. 不宣称与未测试发行版或其他 DDS vendor 完全互操作。

版本矩阵、ABI 边界和升级门槛见 `docs/VELAROS_VERSION_MATRIX.md`。升级必须整体
更新 ROS 2/RMW/Fast DDS 版本组并重新完成 simulator、Linux 互操作和 K1 实板
回归，不能只替换单个库。

## 六、K1 硬件底座

### 6.1 启动链

K1 首版保留厂商启动链：

```text
BootROM -> FSBL -> OpenSBI -> U-Boot
                              |
                              +-> openvela/NuttX ELF @ 0x11000000
                                      |
                                      +-> UART -> timer -> NSH -> VelaROS
```

openvela 运行在 RV64 S-mode，首版只启用 hart 0。当前已完成 S-mode 入口、
polling UART、SBI TIME、异常日志、默认关闭的 PLIC 骨架，以及可重复构建、ELF
验收和 U-Boot 上板包。所有这些结果目前只完成主机侧编译链接，仍待实板确认。

### 6.2 DDS 传输与 K1 Ethernet

DDS 直接参与网络要求 K1 上存在可用 IP 网络。参考 MUSE Pi Pro DTS 给出的
Ethernet 信息包括：

| 项目 | 参考值 |
|---|---|
| compatible | `spacemit,k1x-emac` |
| MMIO | `0xcac80000`，大小 `0x420` |
| PHY | RGMII，地址 1 |
| PHY reset pin | 110 |

当前 openvela/NuttX 中没有 K1 EMAC 驱动。首选方案是根据厂商公开寄存器资料，
复用 NuttX netdev、PHY 和 TCP/IP 框架实现 K1 EMAC lower-half，完成 DMA
descriptor、cache 维护、PHY/MDIO、IRQ、clock/reset 和 netdev 收发。

参考 U-Boot/Linux 驱动为 GPL 许可证，只能用于理解硬件行为和交叉核对，不能把
其代码直接复制到 Apache 2.0 的比赛实现。若厂商资料不足，应优先争取可兼容
授权或官方驱动支持，而不是通过 AI 猜测寄存器。

在 K1 EMAC 完成前，VelaROS 的 ROS 2/DDS 逻辑先在 openvela simulator 上通过
主机 UDP 验证。这样可将“ROS 2 裁剪问题”和“K1 网络驱动问题”分开定位。

## 七、AMP 中 openvela 的定位

### 7.1 首版关系

VelaROS v0.1 的最低闭环不要求 AMP：openvela 可作为 K1 的主 payload，通过
K1 原生 Ethernet 直接加入外部 Linux ROS 2 网络。这样能够证明 VelaROS 本身
不是依赖 Linux Agent 的代理客户端。

### 7.2 AMP 扩展

若后续实现 Linux + openvela AMP：

| 域 | 定位 | 职责 |
|---|---|---|
| Linux + 完整 ROS 2 | AI/感知/规划域 | NPU、相机、网络、存储、导航、规划和复杂 ROS 2 包 |
| openvela + VelaROS | DDS 原生实时执行域 | 传感采集、执行器、控制循环、安全状态机和实时 ROS 2 节点 |

openvela 的核心定位是“直接参加 ROS 2 graph 的实时域”，不是 Linux 的辅助固件，
也不是 micro-ROS Agent 后面的 XRCE client。Linux 和 openvela 仍使用相同 ROS 2
Topic、类型与 QoS 语义。

AMP 下物理 Ethernet 通常由 Linux 独占，openvela 不能同时直接操作同一 EMAC。
此时需要二选一：

1. Linux 为 openvela 提供 RPMsg/virtio-net，VelaROS 继续使用 UDPv4 DDS；
2. 为 Fast DDS 实现基于 RPMsg 的 K1 custom transport。

这两种方案都需要 hart、内存、cache、vring、通知 IRQ、PLIC context 和
clock/reset 的明确分区。AMP 只作为第二阶段扩展，不进入 VelaROS v0.1 的必验
范围。

## 八、演示方案

### 8.1 openvela simulator 验证

在 Linux 主机启动一个标准 ROS 2 节点和 openvela simulator：

```text
Linux ROS 2 node
  publishes /cmd_vel (geometry_msgs/Twist)
  subscribes /velaros/heartbeat
             ^
             | Fast DDS / UDPv4 / RTPS
             v
openvela VelaROS node
  subscribes /cmd_vel
  publishes /velaros/heartbeat (std_msgs/UInt32)
  runs 100 Hz timer callback
```

验收时不启动 micro-ROS Agent。Linux 端通过 `ros2 topic list` 发现 VelaROS
节点和 Topic，通过 `ros2 topic echo` 查看消息，并用 `ros2 topic hz` 检查频率。

### 8.2 K1 实板验证

K1 端按以下层级验收：

1. UART 启动并进入 NSH；
2. timer、PLIC 和 Ethernet 驱动正常；
3. 获取静态 IP 或 DHCP 地址，完成 ping/UDP 回环；
4. Fast DDS HelloWorld 与 Linux Fast DDS 通信；
5. VelaROS 节点被 Linux ROS 2 graph 直接发现；
6. `/cmd_vel` 与 `/velaros/heartbeat` 双向传输；
7. 记录 discovery 时间、消息频率、端到端延迟、丢包、内存和 CPU 占用；
8. 连续运行至少 30 分钟并保存日志、配置和 ELF SHA256。

## 九、当前进度与证据边界

截至 2026-07-31：

| 模块 | 状态 | 证据边界 |
|---|---|---|
| K1 BSP 骨架 | 已完成 | RV64 ELF 编译链接通过，未上板 |
| UART/SBI timer/early trap | 已完成基线 | 源码和 ELF 静态检查，未上板 |
| PLIC | 可选骨架 | context 来自参考 DTS，未上板 |
| K1 Ethernet | 未开始 | 只有 DTS/U-Boot 参考资料 |
| Fast DDS openvela glue | simulator 验收通过 | 版本/补丁已锁定，3 发 3 收且退出无残留；未上 K1 |
| ROS 2 核心包 | 最小 `rcl` 生命周期通过 | `rcl 10.4.4` 与 `rmw_fastrtps_shared_cpp/cpp` 已目标编译链接，context/node/graph/participant 回收 PASS；pub/sub、timer、executor、参数和 `rclcpp` 未接入 |
| VelaROS API/Demo | 未开始 | 本文仅为设计，不是完成状态 |
| AMP | 设计阶段 | 不属于 v0.1 必验项 |

## 十、实施计划与 Early Check

比赛提交截止日为 2026-09-20。以 2026-07-31 为起点还剩 51 天。

### 10.1 七天 Go/No-Go 检查

Early Check 当前结果：

1. **PASS**：已找回并锁定 Fast DDS 3.6.1、Fast-CDR 2.3.6 和 memory 0.7.4；
2. **PASS**：openvela simulator Fast DDS UDP HelloWorld 已完成可重复构建、
   3 发 3 收和干净退出；
3. **PASS**：已输出 ROS 2 Lyrical 分阶段最小依赖闭包和主机/目标边界；
4. **PASS**：`rcutils + rosidl runtime + rmw` 及第二批 `rmw_dds_common`、
   生成消息/Fast RTPS 类型支持均已目标编译，allocator/node-name、
   GraphCache smoke 与 DDS 合并验收通过；
5. **PASS**：`rmw_fastrtps_shared_cpp/cpp` 已静态编译链接，context、node、
   graph 查询、participant 和 context 完整回收通过；
6. **PASS**：`rcl 10.4.4` 最小 context/node 生命周期已静态编译并在
   simulator 完整创建、shutdown、回收；
7. **待资料/实板**：确认 K1 EMAC 寄存器、DMA、IRQ、clock/reset 和 PHY 资料；
8. **待外部条件**：明确开发板预计到达时间。

若第 2 或第 4 项在七天内没有形成可重复结果，不能向审核方承诺 K1 上完整
VelaROS，只能先提交 K1 BSP 和 openvela simulator 上的 ROS 2/DDS 技术验证。

### 10.2 主计划

| 阶段 | 预计有效工作日 | 完成条件 |
|---|---:|---|
| Fast DDS 基线恢复 | 3–5 天 | simulator UDP HelloWorld 可重复 |
| ROS 2 最小依赖裁剪 | 7–10 天 | `rcl/rmw_fastrtps` 静态链接 |
| VelaROS C++ API | 5–7 天 | Node、pub/sub、timer、executor |
| Linux ROS 2 互操作 | 3–5 天 | 无 Agent 双向 Topic |
| K1 首板 bring-up | 7–12 天 | UART、timer、NSH 稳定 |
| K1 EMAC | 8–15 天 | ping、UDP 和 Fast DDS |
| K1 VelaROS 集成 | 4–7 天 | 实板双向 Topic 与 30 分钟运行 |
| 文档/测试/视频 | 4–6 天 | 可复现交付 |

这些工作若完全顺序执行需要 41–67 个有效工作日，超过剩余窗口。因此项目属于
高风险挑战，必须复用已有 Fast DDS 胶水、严格限制 ROS 2 功能、尽早获得开发板，
并让 simulator 的 ROS 2 移植与 K1 bring-up 分阶段推进。

### 10.3 时间结论

- 只完成 K1 openvela BSP：当前时间可控；
- 完成 simulator 上原生 DDS 的 VelaROS v0.1：有条件可行；
- 完成 K1 + Ethernet + VelaROS 的完整闭环：有较高风险，但可通过七天
  Go/No-Go 和严格裁剪争取完成；
- 再叠加产品级 AMP、SMP、NPU、导航或完整 ROS 2：当前期限内不现实。

开发板应尽量在 2026-08-10 前到达。若 2026-08-15 仍未到板，应冻结 AMP、
SMP 和第二种消息类型；若 2026-08-20 仍未到板，K1 EMAC + VelaROS 实板闭环
进入明显高风险。

## 十一、AI 辅助开发复核原则

AI 可用于依赖图分析、移植补丁草案、编译错误归类、测试生成和代码审查，但不得
把以下内容直接当作完成结论：

1. ROS 2 包能在 Linux 构建，不代表能在 openvela 静态链接；
2. Fast DDS 有 CMake 胶水，不代表依赖源码和 ABI 已匹配；
3. DDS HelloWorld 能通信，不代表 ROS 2 graph/type support 已兼容；
4. simulator 能运行，不代表 K1 的 cache、DMA、IRQ 和 Ethernet 正常；
5. 参考 GPL 驱动能运行，不代表其代码可以复制到比赛仓；
6. AI 推导的寄存器、依赖删减和线程模型必须经上游源码、厂商资料和运行日志验证。

每个里程碑必须保存：锁定的 commit、defconfig、构建命令、ELF/库大小、
测试命令、原始日志和失败记录。没有证据的功能写为“计划”或“待验证”，不写成
“已经迁移完成”。

## 十二、预期交付与验收

### 12.1 代码交付

1. K1 芯片层、MUSE Pi Pro 板级包和 NSH/VelaROS defconfig；
2. K1 UART、timer、PLIC 和 Ethernet 必要驱动；
3. VelaROS 裁剪后的 ROS 2 核心包与 openvela compatibility layer；
4. Fast DDS/Fast-CDR 固定版本构建集成；
5. host-side rosidl 代码生成和 target-side 静态构建脚本；
6. VelaROS Publisher/Subscriber/Timer/Executor Demo；
7. 自动构建、ELF/size 检查、互操作测试和日志采集工具。

### 12.2 核心验收

- openvela 在 K1 实板稳定进入 NSH；
- Fast DDS 在 openvela 上通过 UDPv4 进行 RTPS 通信；
- VelaROS 节点无需 Agent 即可被标准 Linux ROS 2 发现；
- Linux 与 VelaROS 可以双向收发标准 ROS 2 Topic；
- QoS、消息类型和 Topic 名称与选定 ROS 2 基线一致；
- 文档能够让第三方重复构建并复现实验；
- 明确报告 ROM/RAM、线程、栈、发现时间、频率和延迟数据。

## 十三、项目创新点

1. **不是单纯 K1 BSP**：在国产 RISC-V 上建立从硬件到 ROS 2 graph 的完整纵向栈；
2. **不是 micro-ROS**：openvela 节点原生持有 DDS participant，无 XRCE Agent；
3. **保留 ROS 2 标准分层**：复用 `rcl/rmw_fastrtps/Fast DDS`，避免私有消息代理；
4. **面向实时机器人的裁剪**：围绕 pub/sub、timer、executor 和确定性 QoS，
   删除桌面工具与动态机制；
5. **可演进到 AMP**：openvela/VelaROS 定位为 ROS 2 原生实时域，Linux 保留
   AI、感知与规划能力；
6. **证据驱动和可上游**：版本、许可证、构建、ABI、实板日志和性能数据均可追踪。

## 十四、参考依据

- ROS 2 internal interfaces：`rcl` 位于 client library 与 `rmw` 之间，
  `rmw_fastrtps_cpp` 连接 Fast DDS；
- ROS 2 middleware vendors：Fast DDS 是 ROS 2 支持的 DDS/RMW 实现；
- openvela 当前源码：`external/fastdds/CMakeLists.txt` 和 `Kconfig`；
- MUSE Pi Pro 参考 DTS：`spacemit,k1x-emac`、RGMII 和 PHY 参数；
- K1 BSP 子计划：`docs/K1_PROJECT_DESCRIPTION.md`；
- K1 启动证据与待验证项：`docs/K1_BOOT_INVENTORY.md`。
