# VelaROS on K1 Early Check 核心审核项

版本：2026-08-08
适用阶段：开发板、Token、量产券发放审核

## 1. 项目名称与一句话定位

**项目名称：VelaROS on K1：基于 openvela 与 Fast DDS 的端侧 ROS 2 机器人运行时**

**一句话定位：** 面向移动机器人、具身智能和硬件开发者，在进迭时空 MUSE Pi Pro
（SpacemiT K1）上把 openvela 的实时执行能力、静态 ROS 2/Fast DDS 通信、LIO
定位和导航控制整合成资源有界的端侧运行时，解决完整 Linux ROS 2 栈在资源受限
设备上的启动和运行开销、线程与动态依赖不可控，以及板内数据重复经过网络序列化
的问题。

项目不是把完整桌面版 ROS 2 原样放进开发板，也不是 micro-ROS Agent 的代理客户端。
目标是让 K1 上的 openvela 任务直接作为 ROS 2 graph 中的节点运行，同时把高频板内
数据和实时控制留在 openvela 原生数据面中。

项目交付范围包括：

- K1 的 openvela/NuttX RV64 S-mode BSP 和可复现启动链；
- VelaROS 静态 ROS 2/Fast DDS 运行时及 Linux ROS 2 互操作；
- openvela 的 uORB、syslog、KVDB、Binder 和固定共享缓冲融合；
- 从 IMU/LiDAR 输入到 LIO pose/map、导航规划和运动命令的端侧闭环；
- 面向真实传感器和底盘驱动的固定接口，为后续实板验证和产品化提供边界。

## 2. 目标开发板及选板理由

### 2.1 目标板

| 项目 | 内容 |
|---|---|
| 开发板 | 进迭时空 MUSE Pi Pro |
| SoC | SpacemiT K1，X60，8 核 RV64 RISC-V |
| 目标系统 | openvela/NuttX，首版运行在 RV64 S-mode |
| 启动链 | BootROM -> FSBL -> OpenSBI -> U-Boot -> openvela |
| 基础接口 | UART、启动存储、DRAM、GPIO/40Pin 扩展、网络接口 |
| 网络基线 | 优先 K1 Ethernet；WiFi 作为可选网络路径，当前均待实板驱动验收 |

### 2.2 选板理由

1. **符合比赛的新硬件平台适配目标。** K1/MUSE Pi Pro 当前缺少可直接交付的完整
   openvela BSP，项目可以验证从 U-Boot/OpenSBI、RV64 S-mode 入口到 NuttX、UART、
   timer、GPIO 和网络的完整移植链路。
2. **能够承载端侧机器人运行时。** K1 的 64 位多核 CPU、DRAM、网络和扩展接口，
   适合验证静态 Fast DDS、LIO/Navigation 算法以及实时控制边界；首版先用 hart 0
   建立确定性闭环，再推进多核和实板 AMP；无板 AMP 协议与状态机原型已完成。
3. **便于连接真实机器人硬件。** 40Pin、总线和网络接口可用于接入 IMU、LiDAR、
   GPIO/PWM/CAN/UART 等传感器或执行器链路，能把当前 simulator 的固定 POD 输入和
   uORB 命令替换成真实设备数据。
4. **具有后续产品扩展空间。** Linux/完整 ROS 2 可留在上位机或边缘计算节点，K1
   上的 openvela 负责确定性采集、控制和安全状态机，后续可按资源隔离条件扩展
   RPMsg/OpenAMP、NPU 或多媒体协同。

### 2.3 当前板卡状态与申请必要性

当前已完成 K1 芯片层、板级 BSP、RV64 S-mode 入口、polling UART、OpenSBI TIME、
可重复构建、ELF 验收、U-Boot 上板包，并已在 MUSE Pi Pro 实板完成 NSH、10 分钟
稳定运行、GPIO/PLIC 和 Ethernet polling 基线；WiFi、真实传感器和 Fast DDS 实板
互操作仍不能写成已通过。

开发板是完成下列关键验收的必要条件：

- 确认实际 DTB、DRAM、保留内存、U-Boot relocation 和 handoff 参数；
- 保持 UART、SBI TIME、连续运行、PLIC/GPIO 和基础 NSH 回归；
- 已完成 K1 Ethernet 的 UDPv4 基线，仍需完成 Fast DDS 实板互操作和网络长稳；
- 接入真实 IMU/LiDAR 和底盘执行器，完成 LIO/Nav 的硬件闭环。

## 3. 核心功能及所需硬件能力

下表中的“已验证”均明确指无板构建、goldfish/openvela simulator 或主机验证；
“待实板”不等同于代码不存在，而是尚未取得 K1 硬件证据。

| 核心功能 | 当前能力与边界 | 所需硬件能力 |
|---|---|---|
| K1 启动与 openvela/NSH | 已用 wrapper + U-Boot `go` 在实板进入 NSH；持久启动/恢复集成仍待完成 | K1 RV64 CPU、DRAM、启动介质、UART；不需要摄像头、WiFi、麦克风或 PWM |
| UART 控制台、系统 timer 与基础调度 | polling `/dev/console`、SBI TIME 24 MHz、UART 和 10 分钟稳定运行已通过；UART IRQ 未实现 | K1 UART、OpenSBI TIME、timebase、PLIC/GPIO；不需要摄像头、麦克风或网络 |
| VelaROS 端侧运行时 | `rcl/rmw/Fast DDS` 静态链路、单线程 executor、Topic/Service/Action/Timer、openvela 原生融合已在 simulator 验证 | CPU、RAM、NuttX task/mutex/socket 能力；KVDB 需要 Flash/eMMC 等持久存储；Binder、syslog、uORB 主要使用板内 CPU/RAM；不需要摄像头、麦克风或 PWM |
| ROS 2/Fast DDS 跨设备通信 | Linux ROS 2 Lyrical 与 simulator 已完成互操作；K1 Ethernet UDPv4 已通过，K1 Fast DDS 实板互操作尚未验收 | K1 Ethernet 的 IP/UDPv4、MAC/PHY、DMA/cache、IRQ、时钟复位和网络驱动；不需要摄像头、麦克风或 PWM |
| Linux/openvela AMP 软件边界 | 固定 92 B little-endian frame、CRC/sequence、heartbeat、motion/status/急停和 timeout 状态机已在 Linux socketpair 与 openvela simulator 验证；K1 跨核链路尚未验收 | Linux/openvela 两个执行域、预留共享内存、RPMsg/OpenAMP/virtio、vring、跨核通知 IRQ 和 cache coherency；当前不需要摄像头、麦克风或 PWM |
| LIO pose/map | Small Point-LIO、SmallIVox、Super-LIO OctVox、keyframe、PGO、loop closure、global map 和 map-odom correction 已接入；当前输入为合成 IMU + 64 点 LiDAR | 真实 3D LiDAR、IMU、统一时间戳/同步、SPI/I2C/UART/Ethernet 等传感器入口、CPU/RAM；当前算法不需要摄像头、麦克风、WiFi 或 NPU |
| 地图与导航规划 | 固定 costmap、bounded A*、RPP/DWPP controller、NavigationPipeline 和 LIO snapshot 连接已在 simulator 验证 | LIO pose/map、CPU/RAM、单调 timer；真实机器人可选轮速计/里程计；算法本身不需要摄像头、麦克风或 WiFi，但执行器需要 PWM/GPIO/CAN/UART 等接口 |
| `/cmd_vel` 与 `MoveRelative` 控制入口 | 标准 `geometry_msgs/Twist`、有界 `MoveRelative` Action 进入持久 `velaros_motion_command` uORB；真实底盘消费者尚未接入 | 外部上位机需要 Ethernet 或 WiFi；板内需要 uORB；真实执行需要 PWM/GPIO、CAN、RS-485/UART 或电机控制器；当前没有已验收的 K1 PWM/电机驱动 |
| GPIO/基础板级 Demo | GPIO 输出及 Pin 22 -> Pin 33 GPIO/PLIC 上升沿已实板通过；level/wakeup/debounce 仍未实现 | GPIO 控制器、pinmux、40Pin、回环线；不需要摄像头、WiFi、麦克风或 PWM |
| 摄像头视觉 | **不属于本次 Early Check 的已完成核心功能，不作现阶段承诺** | 后续如扩展，需要 MIPI/USB 摄像头、CSI/ISP、连续图像缓冲，必要时使用 NPU；当前没有对应驱动、算法和验收数据 |
| 麦克风/音频 | **不属于本次 Early Check 的已完成核心功能，不作现阶段承诺** | 后续如扩展，需要 I2S/DMIC、音频 codec、DMA 和音频缓冲；当前没有对应功能或云端语音服务承诺 |

本项目当前的硬件刚性依赖是 **K1 + UART/启动介质 + CPU/RAM**，真实 LIO 闭环增加
**IMU + 3D LiDAR**，跨设备 ROS 2 增加 **Ethernet 或 WiFi**，真实运动执行增加
**PWM/GPIO/CAN/UART 等底盘接口**。摄像头、麦克风和 NPU 不属于当前核心交付，
不能用它们已经存在的假设替代真实 LiDAR、IMU、网络和执行器验收。

## 4. 各功能技术方案

### 4.1 总体部署方式

核心计算和实时控制全部规划在 K1/openvela 端侧完成。当前没有云端依赖、云端推理
或云端 SDK；标准 ROS 2 节点可以运行在 Linux 上位机或边缘计算节点，用于调试、
高层规划和跨设备通信。Linux 主机不是 VelaROS 的 Agent，K1 端的 Fast DDS
participant 直接加入 ROS 2 graph。

总体链路为：

```text
真实 IMU/LiDAR 驱动
        |
        v
openvela uORB / 固定 POD ingress
        |
        v
Small Point-LIO -> map/keyframe/PGO -> fixed costmap -> bounded A*/RPP/DWPP
        |                                                   |
        +---------------- pose/map snapshot ----------------+
                                                            v
                                                    uORB motion command
                                                            |
                                                            v
                                                    PWM/CAN/GPIO/UART 底盘

Linux ROS 2 Lyrical <-> UDPv4/RTPS Fast DDS <-> VelaROS static rcl/rmw
```

### 4.2 分功能方案

| 功能 | 部署位置 | 技术方案、框架与 SDK | 当前验证/后续动作 |
|---|---|---|---|
| K1 BSP 与启动 | K1 端侧 | openvela/NuttX BSP；U-Boot 负责装载；OpenSBI 提供 M-mode SBI；首版 RV64 S-mode、单 hart 0；K1 专用 polling UART 和 SBI TIME | 无板构建、ELF 和上板包已通过；拿板后验证 handoff、UART、timer、NSH、GPIO 和持续运行 |
| ROS 2 核心 API | K1 端侧 | 静态 `rcl` 10.4.4、`rmw_fastrtps_cpp` 9.4.8、静态 `rclcpp` 32.0.0 语义子集；caller-owned 有界单线程 executor 和一个 wait set；不引入动态插件、Python、colcon 或隐藏线程池 | simulator 和 Linux ROS 2 Lyrical 互操作已通过；K1 实板网络后复测 |
| DDS 数据面 | K1 端侧 + Linux/边缘主机 | eProsima Fast DDS 3.6.1、Fast-CDR 2.3.6、RTPS/UDPv4；保留 SIMPLE PDP/EDP、Reliable/Best Effort 和基础 QoS；裁掉 TypeLookup、DDS-RPC、TCP/IPv6、Discovery Server、原生 SHM/DataSharing | simulator Topic/Service/Action 通过；K1 Ethernet/WiFi 驱动、时延、丢包和长稳待测 |
| Linux/openvela AMP | Linux 富功能域 + openvela 实时域 | Linux 侧网关将有界控制目标编码为固定 92 B little-endian frame；openvela 侧 `velaros_amp_service` 校验 CRC/sequence、处理 heartbeat/timeout/急停并写入持久 uORB；当前 socketpair 仅用于无板验证，拿板后替换为 RPMsg/OpenAMP/virtio | Linux/socketpair 与 simulator AMP smoke 通过；K1 hart、reserved-memory、vring、IRQ、cache、真实传感器和底盘闭环待实板 |
| ROSIDL 类型与构建 | 主机生成、K1 静态链接 | Linux ROS 2 Lyrical/rosidl 生成固定消息和 Fast RTPS type support；K1 只编译白名单静态类型，不运行 Python 生成器、ament/colcon 或 `ros2` CLI | 产品使用 `geometry_msgs/Twist`、`velaros_interfaces/MoveRelative`；后续按传感器和控制接口扩展白名单 |
| 板内高频数据 | K1 端侧 | 复用 openvela uORB；传感器和控制任务通过有界 POD Topic 交换，ROS Topic 只在需要跨设备时使用，不把板内高频数据绕一圈 DDS | uORB/ROS bridge 和运动命令 sink simulator 通过；真实 sensor uORB driver 待接入 |
| 板内大 payload | K1 端侧 | 固定 4 x 16 KiB shared buffer pool、bounded lease、generation/owner descriptor 和静态 ROSIDL `BufferBackend`；不恢复 Fast DDS 原生 SHM；不兼容端点回退 CDR + UDP | simulator 固定缓冲验证通过；K1 DMA、cache coherency、真实点云/相机 payload 和 RPMsg 尚未验收 |
| 配置、日志与服务控制 | K1 端侧 | 配置使用 openvela KVDB；日志使用 NuttX syslog/rcutils adapter；服务发现和控制使用 Binder/service manager；不移植 ROS YAML 参数栈、rosout 或 ROS launch | KVDB、syslog、Binder 融合已通过 simulator；拿板后验证存储介质和异常恢复 |
| LIO 处理链 | K1 端侧 | 迁移 x86 工程的 ROS-free 核心，使用 Small Point-LIO、SmallIVox、Super-LIO OctVox、Eigen scalar backend、keyframe manager、global map、PGO、IMU dead reckoner、loop closure 和 map-odom correction；输入是固定容量 `ImuSample/PointSample` | LIO smoke 通过；当前用合成 IMU 和有界 64 点 LiDAR；后续实现真实 IMU/LiDAR uORB ingress、时间同步和资源/实时性测量 |
| Navigation | K1 端侧 | LIO 发布 bounded pose/map snapshot；NavigationPipeline 使用 fixed costmap、bounded A*、RPP/DWPP controller；`VelaRosNavigationAdapter` 输出 `velaros_motion_command` uORB；不依赖完整 Nav2、tf2 或动态插件 | 导航库、1000 次 benchmark 和 12 次 LIO/Nav smoke 通过；后续接真实里程计、底盘驱动和 PWM/CAN/GPIO 控制 |
| 外部机器人控制 | K1 端侧与 Linux/边缘主机 | 主机用标准 ROS 2 Lyrical；K1 用 Fast DDS 接收 `/cmd_vel` 和 `MoveRelative`，静态转换器写入持久 `velaros_motion_command`；Action 控制权、取消和零速停车由端侧状态机保证 | 产品 Topic/Action 和 uORB sink 已通过；真实电机控制器和急停链路待实板 |
| 摄像头、麦克风和云端 AI | 不在本次核心部署 | 当前不引入摄像头 SDK、音频 SDK、NPU SDK、云端推理 SDK 或云端服务依赖；未来扩展需另立硬件、算法、带宽和隐私边界 | 不纳入本次 Early Check 的完成度或量产承诺 |

### 4.3 端侧与云端边界

| 部署域 | 本项目承担的内容 |
|---|---|
| K1/openvela 端侧 | BSP、实时调度、uORB、LIO、地图快照、导航、控制命令、Fast DDS participant 和确定性资源边界 |
| Linux 上位机/边缘节点 | 标准 ROS 2 Lyrical 节点、调试工具、上层规划或可选的复杂感知；通过标准 ROS 2 Topic/Service/Action 接入 |
| 云端 | 当前无必需功能、无云端推理、无云端 SDK；云端若存在，只能是未来业务层扩展，不是本项目的运行前提 |

## 当前证据与审核边界

已形成的主机侧证据：

- K1 BSP 无板构建、RV64 ELF 检查和 U-Boot 上板包闭环；
- goldfish/openvela simulator 的开发和 release Fast DDS 构建通过；
- Linux ROS 2 Lyrical 与 VelaROS 双向 Topic、Service、Action 互操作通过；
- LIO/Nav smoke：`frames=12`、`map_points=64`、`map_version=3`、
  `navigation_commands=12`；
- x86 LIO/Nav 核心构建、单元测试和 1000 次导航 benchmark 通过，规划和控制
  benchmark 无动态分配；
- Fast DDS 静态 profile、ROS-only 裁剪和 openvela baseline preservation 门禁通过。

仍必须在开发板到位后完成：

- K1 实板启动、UART、timer、PLIC/GPIO、SD/eMMC 和长稳；
- K1 Ethernet 或 WiFi 的 netdev、DMA/cache、UDPv4 和 Fast DDS 实板互操作；
- 真实 IMU/LiDAR 驱动 ingress、时间同步、点云负载和 LIO 实时性；
- 真实底盘里程计、PWM/CAN/GPIO/UART 电机消费者、急停和运动闭环；
- 生产级功耗、温升、异常恢复、网络长稳和整机可靠性。

## 对开发板、Token 和量产券审核的建议口径

- **开发板：建议发放。** 当前没有 K1 实板，BSP、网络、真实传感器和执行器的关键
  验收均无法仅靠 simulator 完成；开发板是把现有软件闭环推进为硬件闭环的必要条件。
- **Token：建议支持。** 后续工作包含 K1 网络驱动、真实 IMU/LiDAR ingress、LIO/Nav
  资源测量和底盘控制适配，需要持续进行代码实现、构建、测试和故障定位。
- **量产券：建议按条件进入后续硬件验证/小批试制阶段，不将本 Early Check 直接表述
  为量产就绪。** 当前已经具备静态依赖、固定内存边界、标准 ROS 2 互操作和端侧
  产品接口，但实板启动、网络、传感器、PWM/底盘、功耗温升和长稳仍是量产前置条件。

因此，本项目当前适合评估为 **“软件方案 Conditional Go，申请开发板后继续实板
验证”**：已有足够的代码和 simulator/host 证据支持资源发放，但不把无板阶段结果
冒充为 K1 实板或整机量产验收。

## 相关资料

- [`VELAROS_PROJECT_DESCRIPTION.md`](VELAROS_PROJECT_DESCRIPTION.md)
- [`VELAROS_NAVIGATION_HANDOFF.md`](VELAROS_NAVIGATION_HANDOFF.md)
- [`VELAROS_ROBOT_PRODUCT_PROFILE.md`](VELAROS_ROBOT_PRODUCT_PROFILE.md)
- [`VELAROS_COMMUNICATION_PROFILE.md`](VELAROS_COMMUNICATION_PROFILE.md)
- [`K1_PROJECT_DESCRIPTION.md`](K1_PROJECT_DESCRIPTION.md)
- [`K1_DELIVERY_CHECKLIST.md`](K1_DELIVERY_CHECKLIST.md)
