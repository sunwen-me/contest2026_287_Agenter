# VelaROS on K1 项目描述

版本：2026-08-08
适用阶段：项目方案审核、开发板申请、Early Check 与后续实板验收

## 一、基本信息

| 项目 | 内容 |
|---|---|
| 项目名称 | VelaROS on K1：基于 openvela 与 Fast DDS 的端侧 ROS 2 机器人运行时 |
| 所属方向 | 2026 openvela AI 硬件开发者大赛——新硬件平台适配 + AI/机器人系统创新 |
| 目标硬件 | 进迭时空 MUSE Pi Pro，SpacemiT K1（X60，8 核 RV64 RISC-V） |
| 基础系统 | openvela/NuttX，首版运行在 RV64 S-mode |
| 机器人能力 | IMU + 3D LiDAR LIO、地图、导航规划、运动控制命令边界 |
| ROS 2 基线 | ROS 2 Lyrical；`rclcpp 32.0.0`、`rcl 10.4.4`、`rmw_fastrtps_cpp 9.4.8` |
| DDS 实现 | eProsima Fast DDS 3.6.1、Fast-CDR 2.3.6，静态类型和 UDPv4/RTPS |
| openvela 定位 | K1 上的实时操作系统、BSP/驱动承载平台、板内数据平面和控制执行域 |
| VelaROS 定位 | 运行在 openvela 之上的静态 ROS 2/Fast DDS 客户端运行时和机器人应用层 |
| 当前阶段 | K1 无板 BSP 构建闭环；VelaROS/Fast DDS/LIO/Nav 已在 simulator 和主机完成验证；实板待验证 |
| 核心交付 | K1 openvela BSP、VelaROS 运行时、ROS 2 互操作、LIO/Nav 端侧链路和可复现验收工具 |

## 二、项目简介

VelaROS on K1 的目标不是单独运行一个 ROS 2 示例，而是以 SpacemiT K1 为目标硬件，
建立从启动固件、openvela 实时系统、ROS 2/DDS 通信到机器人感知和控制的完整端侧
纵向链路。项目首先为 MUSE Pi Pro 建立 openvela/NuttX RV64 S-mode BSP，使系统能够
经过原有 BootROM、FSBL、OpenSBI 和 U-Boot 启动，输出 UART 日志并进入 NSH；然后在
openvela 上静态集成 VelaROS、Fast DDS、LIO 和 Navigation，形成可被 Linux ROS 2
主机直接发现和调用的机器人运行时。

总体架构如下：

```text
K1 BootROM -> FSBL -> OpenSBI(M-mode) -> U-Boot
                                      |
                                      v
                         openvela/NuttX(S-mode, hart 0)
                                      |
        +-----------------------------+-----------------------------+
        |                             |                             |
   NuttX scheduler              uORB/KVDB/syslog              Binder/service manager
   UART/TIME/GPIO                local data/control             runtime control
        |                             |                             |
        +-----------------------------+-----------------------------+
                                      |
                   VelaROS static rcl/rmw/Fast DDS runtime
                                      |
              LIO pose/map -> Navigation -> motion command uORB
                                      |
                        future PWM/CAN/GPIO/UART driver

Linux ROS 2 Lyrical host/edge node <-> Fast DDS RTPS/UDPv4 <-> VelaROS participant
```

VelaROS 保留 ROS 2 的 Node、Topic、Publisher、Subscriber、Service、标准五通道
Action、Timer、基础 QoS 和 DDS graph 语义，但只编译经过审计的静态消息类型和有界
运行时。板内高频数据优先使用 openvela uORB，日志、配置、服务管理和大 payload
优先复用 openvela 已有能力；只有跨设备的 ROS 2 数据才经过 Fast DDS/RTPS/UDPv4。

本项目不是 micro-ROS：K1 端自身拥有 DDS DomainParticipant，直接加入 ROS 2 graph，
不使用 Micro XRCE-DDS Client，也不依赖 micro-ROS Agent 代理 ROS 2 实体。

## 三、背景与项目价值

移动机器人通常使用 Linux + ROS 2 承担感知、规划和 AI，但底层传感器、执行器和
安全控制需要更短的启动时间、更小的系统开销、明确的任务优先级和可预测的资源
上限。完整 Linux ROS 2 栈直接放入资源受限的实时系统，会带来动态类型、动态插件、
无界容器、隐藏线程、重复日志/配置/IPC 和板内数据重复序列化等问题。

K1 是国产 64 位多核 RISC-V SoC，MUSE Pi Pro 具备扩展接口和网络能力，但当前缺少
完整可复现的 openvela BSP。本项目的价值包括：

1. 建立 K1 对 openvela/NuttX RISC-V 公共架构、板级启动和基础外设的适配基线；
2. 明确 BootROM、OpenSBI、U-Boot 与 openvela 之间的 M-mode/S-mode、内存和 handoff
   边界，形成可定位的首板 bring-up 流程；
3. 让 openvela 实时任务直接参与 ROS 2 graph，减少 Linux 与实时控制域之间的私有
   协议和重复数据模型；
4. 在 K1 上验证 Fast DDS、静态 ROS 2、openvela 原生能力和 LIO/Nav 的组合方式；
5. 为后续真实 IMU/LiDAR、底盘执行器、Linux + openvela AMP 和边缘 AI 提供稳定接口。

## 四、openvela 的系统定位

### 4.1 首版定位：K1 的实时机器人执行平台

openvela 是本项目的基础操作系统和硬件承载平台，不是 VelaROS 的一个可有可无的
依赖库，也不是 Linux ROS 2 的远程 Agent。它在 K1 上负责：

| 层次 | openvela 在本项目中的职责 |
|---|---|
| 启动与系统 | 接收 U-Boot handoff，在 RV64 S-mode 进入 NuttX，完成任务调度、时钟、异常和内存初始化 |
| BSP 与驱动 | 承载 K1 UART、timer、PLIC/GPIO、存储、网络和后续传感器/执行器驱动 |
| 实时调度 | 通过 NuttX task、pthread、work queue 和优先级机制运行传感采集、LIO、导航和控制任务 |
| 板内数据 | 通过 uORB、队列和 `poll()` 传递高频传感器状态、地图快照和运动控制命令 |
| 配置与日志 | 通过 KVDB 保存设备配置，通过 NuttX syslog 提供日志后端，不重复移植 ROS YAML/spdlog |
| 服务控制 | 通过 Binder/service manager 提供 `velarosd`/`velarosctl` 等板内控制面，不用 ROS launch 管理设备启动 |
| 跨设备边界 | 提供 socket、时间和线程基础，让 VelaROS 通过 Fast DDS 直接加入外部 ROS 2 网络 |
| 安全执行 | 在真实底盘接入后承担命令超时、急停、零速停车和执行器状态机 |

VelaROS 在这个分层中的位置是“运行在 openvela 上的 ROS 2 语义层和机器人应用层”：
它负责 ROS 2 graph、Topic/Service/Action、DDS QoS、LIO/Nav 算法和协议转换；它不
重新实现 openvela 已有的调度器、板内消息总线、日志、配置、服务管理或底层驱动。

### 4.2 首版与完整 openvela 的边界

项目目标是把 openvela 运行到 K1，并完成比赛和机器人闭环所需的平台能力，包括
启动、UART、timer、NSH、GPIO 基础路径、uORB、KVDB、syslog、Binder 和后续网络/传感器
接口；这不等于在无板阶段宣称 openvela 的所有可选模块已经在 K1 完成硬件适配。

显示、音频、摄像头、WiFi、蓝牙、NPU、SMP 和 AMP 按实际硬件资料及资源隔离条件
分阶段验证，不作为当前无板阶段的既成事实。当前 LIO 不需要摄像头或麦克风，真实
LIO 的刚性传感器依赖是 IMU 和 3D LiDAR。

### 4.3 与 Linux/云端的关系

首版 K1 可以作为独立 openvela payload 运行 VelaROS，并直接通过 Ethernet 或后续
可用的 WiFi 参加外部 ROS 2 网络。Linux 上位机或边缘节点可运行标准 ROS 2 Lyrical，
承担高层工具、复杂规划或可选感知，但不是 K1 VelaROS 的 Agent。

当前没有云端运行前提、云端推理或云端 SDK。云端若在未来产品中存在，只属于业务层
扩展，不参与 K1 的实时传感、LIO、导航和执行器闭环。

## 五、项目目标与范围

### 5.1 首版必须完成

1. 完成 K1 芯片层、MUSE Pi Pro 板级层、链接布局、NSH defconfig 和可复现构建入口；
2. 使用原有 BootROM/FSBL/OpenSBI/U-Boot 启动链，以 RV64 S-mode 进入 openvela；
3. 实板输出 early log、NuttX banner 并稳定进入 NSH，验证 UART、SBI TIME 和基础调度；
4. 在 openvela simulator 和 K1 目标闭包中保留静态 `rcl/rmw_fastrtps_cpp/Fast DDS`
   链路，完成标准 Linux ROS 2 的 Topic/Service/Action 互操作；
5. 将 openvela uORB、syslog、KVDB、Binder 与 VelaROS 运行时连接，避免重复实现板内
   数据总线、日志、配置和服务管理；
6. 将 ROS-free 的 Small Point-LIO + Navigation 核心静态链接到目标，完成固定容量
   IMU/LiDAR 输入、pose/map snapshot、规划和 `velaros_motion_command` 输出链路；
7. 拿到传感器和底盘硬件后，完成真实 IMU/LiDAR ingress、里程计、执行器和命令超时
   的硬件验收；
8. 提供构建、打包、串口采集、异常解析、ELF/Map 检查、测试记录和许可证清单。

### 5.2 条件允许时完成

1. 按实际 DTB 复核 PLIC context，并验证一个可控外部中断源；
2. 在 GPIO 稳定后增加 I2C 或 SPI 传感器接入；
3. 完成 K1 Ethernet lower-half、PHY/MDIO、DMA/cache、UDPv4 和 Fast DDS 实板互操作；
4. 单 hart 稳定后评估 SBI HSM/IPI、SMP，并将无板 AMP 原型推进到 Linux + openvela 实板跨核链路；
5. 在有明确摄像头/音频硬件和算法需求后，单独评估 MIPI/USB、I2S/DMIC、ISP/NPU
   等扩展，不把它们并入当前核心承诺。

### 5.3 本阶段不承诺

- K1 8 核 SMP 的完整稳定性和性能优化；
- 未经实际驱动和日志验证的 K1 UART、timer、PLIC、GPIO、Ethernet 或 WiFi 运行结论；
- 完整 Nav2、tf2、RViz、rosbag2、launch、参数文件、Python 和动态插件栈；
- 摄像头视觉、麦克风语音、NPU 推理或云端 AI；
- Linux 与 openvela AMP 的产品级稳定性；
- 真实 IMU/LiDAR、底盘 PWM/CAN 和整机量产可靠性；
- 与未验证 ROS 2 发行版或其他 DDS vendor 的完全兼容。

## 六、K1 硬件与 openvela BSP 技术方案

### 6.1 目标硬件事实与待复核参数

| 项目 | 当前参考值 | 证据边界 |
|---|---:|---|
| SoC | SpacemiT K1 / X60，8 核 RV64 | 目标板资料，待实板确认具体板卡版本 |
| UART MMIO | `0xd4017000` | 参考 DTS/启动资料；需用实际 DTB 和串口日志复核 |
| UART 访问 | 32-bit，寄存器间隔 4 B（reg-shift 2） | 参考资料；S-mode IER 写入风险要求首版 polling |
| UART 参数 | 115200 8N1，参考 IRQ 42 | 波特率按 U-Boot 基线；IRQ 不作为首版必用路径 |
| PLIC | `0xe0000000`，参考 159 sources | hart 0 S-mode 暂按 context 1；实际 DTB 复核后才启用 |
| timebase | 24 MHz | 参考资料；需要 SBI 探测、tick/延时和长稳测量 |
| payload 地址 | `0x11000000` | 暂定链接/装载地址，需复核 U-Boot relocation、reserved-memory 和 OpenSBI 占用 |
| DTB staging | `0x31000000` | 暂定 staging 地址，需以实际 U-Boot/DTB 为准 |
| K1 Ethernet | 参考 EMAC `0xcac80000`，RGMII，PHY 地址 1，reset pin 110 | 只有 DTS/U-Boot 参考资料；当前没有完成的 K1 openvela EMAC 驱动 |

WiFi 不是当前 K1 VelaROS 的必选底层路径。跨设备 DDS 的首选是 K1 Ethernet；如果
后续使用 WiFi，必须另行验证无线驱动、IP/UDPv4、组播或 unicast peers、功耗和长稳，
不能把“开发板有网络能力”写成“WiFi 已经支持”。

### 6.2 启动链与权限级

首版保留厂商启动链，不替换 M-mode 固件：

```text
BootROM
  -> FSBL
  -> OpenSBI（M-mode，提供 SBI 服务）
  -> U-Boot
  -> openvela/NuttX ELF（RV64 S-mode，首版 hart 0）
  -> early entry log
  -> nx_start
  -> NSH
```

openvela 入口按 RISC-V handoff 接收 boot hart ID 和 DTB 地址，保存参数并记录初始
`sstatus`、`satp`、`stvec`、`sscratch` 等状态，显式初始化 `gp` 和栈后进入 NuttX
启动流程。OpenSBI 保持在 M-mode，openvela 不直接接管 CLINT，而通过 SBI TIME 使用
OpenSBI 提供的定时服务；SBI HSM/IPI 只在单 hart 稳定后再评估。

首版采用单 hart、flat ELF 和固定 DRAM 窗口，不同时引入复杂 Sv39 映射、SMP 和 AMP
变量。实际地址必须由首板 DTB、U-Boot memory map、reserved-memory 和 OpenSBI 占用
共同确认，不能仅凭参考项目地址作为最终平台常量。

### 6.3 UART 控制台

K1 参考串口在 S-mode 下写 UART IER 可能触发 APB 访问挂死，因此首版使用 K1 专用
polling console：

- 保留 U-Boot 已建立的串口配置，使用 32-bit MMIO 访问；
- `/dev/console` 输出轮询发送，不使用会写 IER 的通用 16550 中断路径；
- 源码和 ELF 静态检查拒绝 K1 首版 UART IER 写路径；
- 只有确认实际寄存器语义和实板行为后，才评估 UART IRQ 收发。

这不是放弃 NuttX 控制台，而是先用较小的硬件假设建立 early log、NSH 和异常定位
链路。UART IRQ 不作为首个 PLIC 验证源。

### 6.4 Timer、PLIC 与 GPIO

系统 tick 通过 NuttX RISC-V timer lower-half 和 OpenSBI TIME 设置下一次时钟事件，
timebase 暂按 24 MHz。上板验收必须同时观察 SBI extension、系统 tick、延时误差和
至少 10 分钟连续运行，不以编译通过代替 timer 证据。

PLIC 设计暂按基址 `0xe0000000`、hart 0 S-mode context 1、159 sources 建立可选骨架，
默认关闭并与 SMP 互斥。实际 DTB 与寄存器地址确认后，先选择 GPIO 或其他可控外部
IRQ 验证 claim/complete；GPIO/40Pin 输出输入作为首个可观察板级 Demo。PWM、I2C、
SPI、CAN 和电机控制不作为 UART/timer 之前的前置依赖。

### 6.5 BSP 构建和验收工具

芯片层、板级层和工具职责保持分离：

```text
chip/k1/                         K1 入口、UART、timer、IRQ/PLIC
board/k1/muse_pi_pro/            板级初始化、内存布局、NSH defconfig
tools/build_k1.sh                可重复构建
tools/check_k1_elf.sh            ELF、入口、段权限和关键符号验收
tools/package_k1_bringup.sh      U-Boot 首板包
tools/capture_k1_serial.sh       串口原始日志和元数据采集
tools/decode_k1_trap.sh          scause/sepc/stval 解析和符号化
```

ELF 验收要求包括 ELF64 little-endian RISC-V、入口地址和首个 LOAD 段正确、代码段
与数据段分离、没有 RWX 段、关键 K1 符号存在且没有 UART IER 写路径。首板操作不
覆盖默认 `bootcmd`，不执行 `saveenv`，先保存原厂可恢复介质和原始串口日志。

## 七、VelaROS 与 ROS 2/Fast DDS 技术方案

### 7.1 目标端分层

VelaROS 使用标准 ROS 2 分层，但把目标端构建改为 openvela Kconfig/CMake 静态闭包：

```text
VelaROS static rclcpp subset
          -> rcl / rcl_action
          -> rmw_fastrtps_cpp / rmw_dds_common
          -> Fast DDS / Fast-CDR
          -> RTPS / UDPv4 / NuttX socket + poll
          -> K1 openvela
```

Linux ROS 2 Lyrical 主机负责标准节点、调试命令、接口生成和互操作验证；K1 端不运行
Python、`colcon`、`ament`、ROSIDL 生成器或 `ros2` CLI。目标固件只链接预生成的静态
类型、静态注册表和产品需要的 API。

### 7.2 锁定版本和保留能力

| 层级 | 锁定/保留内容 |
|---|---|
| ROS 2 | Lyrical，作为 Linux 对端和消息生成基线 |
| Client API | 静态 `rclcpp 32.0.0` 语义子集：Context、Node、QoS、Publisher、Subscription、Client、Service、WallTimer、Action Client/Server |
| `rcl` | `10.4.4`，包含 context/node、publisher/subscription、client/service、`rcl_action`、timer、wait set |
| `rmw` | `rmw_fastrtps_cpp 9.4.8`、`rmw_dds_common` 必要 GraphCache 和 request/response |
| DDS | Fast DDS 3.6.1、Fast-CDR 2.3.6、DomainParticipant、发现、DataWriter/DataReader、RTPS/CDR |
| QoS | Keep Last、Reliable、Best Effort、Volatile 和基础 Deadline/Liveliness |
| 消息 | 开发 smoke 使用 `std_msgs`、`std_srvs/SetBool`；产品保留 `geometry_msgs/Twist` 和 `velaros_interfaces/MoveRelative` |
| 传输 | UDPv4；SIMPLE PDP/EDP；支持组播发现和可配置 unicast initial peers |

### 7.3 静态裁剪和内存边界

产品 Fast DDS profile 保留标准 DDS/ROS 2 wire compatibility，编译期排除：

- TypeLookup service、DDS-RPC、Discovery Server/Client/Backup/database；
- TCP、UDPv6、动态类型、Persistence、DDS Security；
- Fast DDS 原生 SHM/DataSharing；板内普通数据使用 uORB，大 payload 使用 VelaROS 固定池；
- 运行时任意类型加载、pluginlib、动态库、Python、组件管理和 ament resource index；
- 通用动态多线程 executor、每 Goal 线程、无界 Goal/Result 缓存和未审计消息类型。

目标端 executor 是调用方拥有的 NuttX task 中的单线程分派器：复用一个 `rcl_wait_set`，
等待 subscription、client、service 和 timer 就绪后按注册顺序执行 callback，不创建
隐藏线程池、不拥有 task、不创建第二套事件循环。线程优先级、栈大小、实体槽和消息
缓冲由 Kconfig/应用明确给出。

### 7.4 openvela 原生能力复用

| 需求 | 目标实现 | 设计边界 |
|---|---|---|
| 高频板内数据 | uORB + `poll()` + 有界队列 | 传感器/控制任务不先序列化成 DDS 再自发自收 |
| 板内大 payload | 4 x 16 KiB 固定共享池、bounded lease、generation/owner descriptor、静态 ROSIDL `BufferBackend` | 同 pool 端点传 descriptor；不兼容端点回退 CDR + UDP；K1 DMA/cache/RPMsg 待实板 |
| 日志 | rcutils adapter -> NuttX syslog | 固定 384 B 栈缓冲，不引入 spdlog、日志文件轮转或线程 |
| 本地配置 | KVDB/property | 保存 Domain、participant、bridge、heartbeat 等配置；不依赖 ROS YAML |
| 服务控制 | Binder/service manager、`velarosd`/`velarosctl` | 单 task `poll()` 分派；不使用 ROS launch 或 Binder thread pool |
| AMP 边界 | 固定 92 B AMP frame、CRC/sequence、heartbeat、uORB 状态边界；RPMsg/OpenAMP 作为实板 transport | Linux/openvela socketpair 与 simulator 原型已验证；K1 hart、vring、IRQ、cache 和共享内存待实板 |

上述复用边界使 openvela 保持平台主人角色：VelaROS 只补充 ROS 2 对外语义和机器人
算法，不复制操作系统基础设施。

## 八、LIO 与 Navigation 技术方案

### 8.1 端侧数据路径

LIO/Nav 使用从 x86 工程迁移的 ROS-free 核心，不在目标端依赖 ROS 2、Nav2、rosbag
回放程序或完整 desktop navigation stack：

```text
真实 IMU driver --------------------+
                                     v
真实 3D LiDAR driver -> uORB/POD `ImuSample`/`PointSample`
                                     |
                                     v
                         velaros_lio::Runtime
                                     |
 Small Point-LIO + SmallIVox + Super-LIO OctVox
                                     |
          pose / global map / keyframe / PGO / loop closure
                                     |
                     fixed two-slot pose/map snapshot
                                     |
                         NavigationPipeline
            fixed costmap -> bounded A* -> RPP/DWPP controller
                                     |
                     VelaRosNavigationAdapter
                                     |
                       velaros_motion_command uORB
                                     |
                     PWM/CAN/GPIO/UART 底盘驱动
```

### 8.2 LIO 组件

目标库 `velaros_lio` 当前包含：

- Small Point-LIO 前端和 SmallIVox 局部地图；
- Super-LIO OctVox 风格局部化、点云匹配和 bounded map query；
- IMU propagation、点级去畸变、外参和时间戳边界；
- keyframe manager、global map、dense pose graph/PGO；
- IMU dead reckoner、LiDAR localization、loop closure 和 map-odom correction；
- 固定容量 `velaros_lio::Runtime`，接收有界 POD 样本并发布 pose/map snapshot。

目标端不通过 ROS 传输点云。传感器驱动把数据写入固定格式和固定容量的 ingress，
LIO 直接消费；只有需要对外观察或供上位机使用的 pose/map 摘要才进入选定 ROS 2
Topic 或其他明确的接口。

### 8.3 Navigation 组件

Navigation 使用固定 costmap、bounded A*、RPP/DWPP controller 和有界碰撞弧检查，
不依赖完整 Nav2。当前配置边界包括：

```text
CONFIG_VELAROS_NAVIGATION=y
CONFIG_VELAROS_NAV_MAX_MAP_CELLS=16384
CONFIG_VELAROS_NAV_MAX_PATH_NODES=4096
```

`NavigationPipeline` 处理 goal、map version、timeout 和 emergency stop；
`VelaRosNavigationAdapter` 只发布固定布局的线速度、角速度、时间戳、250 ms timeout
和单调序号。非零 lateral velocity 被拒绝并转为零速停车，`UorbMotionCommandSink`
将命令写入持久 `velaros_motion_command` uORB。真实底盘驱动只需要消费该 uORB
边界，不需要了解 DDS、rcl 或 Action 协议。

产品 ROS 2 接口为：

- `/cmd_vel`：`geometry_msgs/msg/Twist`，使用 `linear.x` 和 `angular.z`，best effort、
  volatile、depth 4，并进行有限值和速度上限检查；
- `/velaros/move_relative`：`velaros_interfaces/action/MoveRelative`，使用固定长度
  scalar goal/result/feedback，提供标准 SendGoal、GetResult、Cancel、Feedback、Status
  五通道，不使用任意 Action 动态加载。

### 8.4 真实硬件替换点

当前 simulator 使用合成 IMU 和有界 64 点 LiDAR 平面，使用固定周期参考运动模型
验证 LIO/Nav 的数据流、命令上限、超时、急停和 uORB sink。这不代表真实传感器或真实
里程计。

拿到 K1 和传感器/底盘后，只替换以下边界：

1. K1 IMU/LiDAR 驱动通过 uORB 或固定 ingress 提供真实样本、时间戳和坐标系标定；
2. 轮速计/定位模块通过 uORB 提供真实里程计，替换参考运动模型的积分结果；
3. 底盘控制任务订阅 `velaros_motion_command`，接入 PWM/GPIO/CAN/UART 和硬件急停；
4. LIO/Nav ROS 2 API、DDS 类型、主机节点和静态资源边界保持不变。

## 九、核心功能、硬件能力与技术状态

| 功能 | 必需硬件能力 | 当前状态 |
|---|---|---|
| K1 启动、NSH 和 UART | RV64 CPU、DRAM、BootROM/FSBL/OpenSBI/U-Boot、SD/eMMC、UART | 无板构建、ELF、上板包通过；实板待验证 |
| timer、调度和 GPIO | OpenSBI TIME、24 MHz timebase、可选 PLIC、GPIO/40Pin | 代码和可选骨架完成；实测待验证 |
| VelaROS 本地运行时 | CPU/RAM、NuttX task/mutex/socket、uORB；KVDB 需要持久 Flash/eMMC | simulator 的 rcl/DDS/uORB/syslog/KVDB/Binder 通过 |
| ROS 2/Fast DDS 互操作 | K1 Ethernet 或 WiFi 的 IP/UDPv4、MAC/PHY、DMA/cache、IRQ、时钟复位 | Linux ↔ simulator 通过；K1 网络驱动和实板互操作待完成 |
| LIO pose/map | 3D LiDAR、IMU、时间同步、SPI/I2C/UART/Ethernet、CPU/RAM | 合成输入 smoke 通过；真实 driver ingress 待完成 |
| Navigation | pose/map snapshot、CPU/RAM、timer；可选轮速计/里程计 | host/library/simulator 通过；真实底盘数据待接入 |
| 运动执行 | PWM/GPIO/CAN/RS-485/UART、电机控制器、急停 | uORB command sink 通过；K1 PWM/电机消费者未验收 |
| 摄像头视觉 | MIPI/USB、CSI/ISP、连续缓冲、可选 NPU | 不属于本阶段核心功能 |
| 麦克风音频 | I2S/DMIC、codec、DMA、音频缓冲 | 不属于本阶段核心功能 |
| 云端 AI | 云服务、凭据和网络 | 当前无云端依赖或云端 SDK |

## 十、当前进度与证据边界

截至 2026-08-04：

| 模块 | 状态 | 证据边界 |
|---|---|---|
| K1 openvela BSP | 无板阶段完成 | RV64 S-mode 入口、polling UART、OpenSBI TIME、异常日志、ELF 检查和 U-Boot 包已形成；未完成实板启动 |
| K1 PLIC/GPIO | 可选骨架/待实板 | PLIC context 1 来自参考 DTS，默认关闭；GPIO/pinmux 和外部 IRQ 尚未实测 |
| K1 Ethernet/WiFi | 待完成 | Ethernet 只有 DTS/U-Boot 参考资料；WiFi 不在首版网络基线；没有实板网络结论 |
| Fast DDS | simulator 通过 | Fast DDS 3.6.1、Fast-CDR 2.3.6、UDPv4/RTPS、静态裁剪和资源回收已验证；未上 K1 |
| ROS 2 核心 | simulator/host 通过 | `rcl`、`rmw_fastrtps_cpp`、静态 rclcpp、Topic、Service、Action、Timer、WaitSet、executor 和 Linux Lyrical 互操作通过 |
| openvela 融合 | simulator 通过 | uORB、syslog、KVDB、Binder/service manager 和固定缓冲 backend 已完成对应门禁；K1 持久分区/时延待验收 |
| LIO | target-linked/simulator smoke 通过 | `SmallPointLioFrontend`、`SlamSystem`、pose/map snapshot 已通过合成输入；真实 IMU/LiDAR ingress 未完成 |
| Navigation | host/simulator 通过 | bounded costmap/A*/RPP/DWPP、timeout、急停、命令边界和 uORB sink 已验证；真实里程计/执行器未完成 |
| 底盘控制 | 协议边界完成 | `/cmd_vel`、`MoveRelative` 和 `velaros_motion_command` 已验证；没有 K1 PWM/CAN/GPIO 电机驱动消费者 |
| 摄像头/麦克风/NPU/云端 | 不在首版范围 | 不作为本项目当前完成度或量产依据 |

### 10.1 已有验证结果

开发版和发布版 simulator 构建均通过：

```bash
tools/build_velaros_dds_sim.sh --no-codegen --jobs 8
tools/build_velaros_release_sim.sh --no-codegen --jobs 8
```

当前开发 ELF：

```text
cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds/nuttx
SHA256: 7db77df9ba828d8b121bb320d4a925773ea1f36c2fe4d11190a2f5b272465ff2
```

当前 release ELF：

```text
cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros/nuttx
SHA256: 9287349c3ffd53f5c3b90b272c1c9466d4c7847e7b552f009a4c29371f870a65
```

已通过的关键结果包括：

- Linux ROS 2 Lyrical 与 simulator 双向 Topic、`std_srvs/SetBool` Service 和标准
  五通道 Action 互操作；
- `rcl` context/node、participant、timer、wait set、executor 创建、回调和完整回收；
- uORB 双向 bridge、rcutils -> syslog、KVDB 配置读取、Binder/service manager
  跨 task 服务发现和停止；
- 固定 4 x 16 KiB buffer pool、descriptor、lease 和非兼容端点 CDR/UDP fallback；
- LIO/Nav smoke：`frames=12`、`map_points=64`、`map_version=3`、
  `navigation_commands=12`；
- x86 LIO/Nav 主机库构建、`ctest 2/2`、`benchmark_navigation 1000/1000`，
  `planning_allocations=0`、`control_allocations=0`；
- Fast DDS 静态 profile、ROS-only trim 和 openvela baseline preservation 门禁通过。

### 10.2 未完成事项

以下事项必须在开发板和真实硬件到位后完成：

1. K1 实板 handoff、UART、timer、NSH、PLIC/GPIO、存储介质和至少 10 分钟长稳；
2. K1 Ethernet netdev、PHY/MDIO、DMA/cache、UDPv4、Fast DDS discovery 和 30 分钟网络长稳；
3. 真实 IMU/LiDAR uORB driver ingress、时间同步、坐标系标定、队列背压和 LIO 负载；
4. 真实轮速计/里程计、PWM/CAN/GPIO/UART 电机消费者、急停和运动控制闭环；
5. K1 版本的 ROM/RAM、峰值 heap、任务栈、CPU、时延、功耗、温升和异常恢复数据；
6. 生产级整机可靠性、掉电恢复、网络长稳和量产测试流程。

## 十一、Linux + openvela AMP 方案与边界

### 11.1 v0.1 的当前决策

AMP 不是 v0.1 的 K1 实板成功前置条件，但已加入无板软件原型。首版独立模式仍让
openvela 作为 K1 的 S-mode payload 运行 VelaROS，直接通过 K1 网络参加外部 ROS 2
graph；AMP 模式增加 Linux 富功能域与 openvela 实时域之间的固定协议和 uORB 服务。
这样可以先验证软件边界，再把 Linux remoteproc、hart 启停、内存隔离和跨核 cache
一致性留到开发板阶段。

### 11.2 后续 AMP 中的职责分工

| 系统 | 定位 | 主要职责 |
|---|---|---|
| Linux | 富功能主系统/AI 服务域 | 复杂 ROS 2 包、相机、多媒体、存储、显示、NPU、云接入和高层感知/规划 |
| openvela | 独立实时执行域 | IMU/LiDAR 采集、GPIO/I2C/SPI、底盘控制、看门狗、急停、安全状态机和实时 ROS 2 节点 |
| RPMsg/OpenAMP | 跨域控制与数据边界 | 传递有界控制目标、状态和必要 payload；不默认让每个 hart 各自运行完整 DDS |

AMP 下 openvela 不是“展示用的第二个系统”，也不是 Linux 的普通后台进程。它承担
Linux 难以保证确定性的实时 I/O 和安全控制，Linux 完成 openvela 不适合承载的复杂
AI/多媒体任务。

### 11.3 无板 AMP 软件原型

当前已实现固定 92 B little-endian AMP frame、CRC32、严格递增 sequence、heartbeat、
motion command、status、emergency stop、命令超时和 peer timeout 状态机。openvela
侧通过 `velaros_amp_service` 将接收的控制目标写入持久 `velaros_motion_command`
uORB，并将状态和故障发布到 `velaros_amp_status` uORB；Linux 侧自检网关使用
POSIX socketpair 模拟传输。

该实现是 transport-neutral 的。RPMsg/OpenAMP 只需要替换 frame 的收发层，不能把
socketpair smoke 写成 K1 跨核验收。详细协议、代码位置、无板结果和实板替换清单见
[`VELAROS_AMP_HANDOFF.md`](VELAROS_AMP_HANDOFF.md)。

### 11.4 AMP 实现前置条件

必须先确认 Linux CPU topology、OpenSBI HSM、专用 hart、DTB reserved-memory、vring、
cache coherency、通知 IRQ、PLIC context、GPIO/clock/reset 唯一所有者以及
remoteproc/OpenAMP 支持。若物理 Ethernet 由 Linux 独占，openvela 不能同时直接
操作同一 EMAC，需要 Linux 提供 RPMsg/virtio-net，或另行设计经过验证的 custom
transport。以上条件未确认前，不得写成 K1 AMP 跨核硬件已完成；当前只能表述为
“无板 AMP 软件原型已完成，K1 transport 和跨核验收待实板”。

## 十二、实施计划与 Go/No-Go 门禁

### 12.1 阶段计划

| 阶段 | 主要工作 | 退出条件 |
|---|---|---|
| 无板收口 | 锁定源码、构建、ELF、打包、日志、simulator、LIO/Nav smoke | 所有无板结果可复现，未验证项明确列出 |
| 首板 bring-up | 只读核对 DTB/内存，手工加载 ELF，获取 early log | UART、入口、hart/DTB、NSH banner 可解释 |
| 最小系统 | 验证 SBI TIME、调度、连续运行和基础 NSH 命令 | timer 通过，单核稳定运行至少 10 分钟 |
| 基础外设/网络 | GPIO/PLIC 选择性验证；实现并验收 Ethernet 或确定 WiFi 路径 | ping/UDPv4，Fast DDS discovery 和 Topic 互操作 |
| 真实机器人链路 | 接入 IMU/LiDAR、里程计和底盘控制 | LIO pose/map、Navigation 和命令执行形成硬件闭环 |
| 稳定与交付 | 资源、功耗、温升、长稳、视频、文档和日志 | 证据与结论一致，第三方可复现 |

### 12.2 门禁

1. **G0 软件门禁：** simulator、host LIO/Nav、静态 profile、构建和回收检查通过；
2. **G1 BSP 门禁：** K1 实板进入 NSH，UART/timer/内存/handoff 证据完整；
3. **G2 网络门禁：** K1 netdev、UDPv4、DDS participant、Topic/Service/Action 和长稳通过；
4. **G3 传感器门禁：** 真实 IMU/LiDAR 时间戳、uORB ingress、LIO pose/map 和负载通过；
5. **G4 执行器门禁：** 里程计、PWM/CAN/GPIO/UART、急停、命令超时和零速停车通过；
6. **G5 产品门禁：** ROM/RAM/heap/stack/CPU/温升/功耗/异常恢复和整机长稳有记录。

没有 G1 证据时，不能把 simulator 结果写成 K1 启动通过；没有 G3/G4 证据时，不能
把合成 LIO/Nav smoke 写成真实机器人闭环；没有 G5 证据时，不能写成量产就绪。

## 十三、AI 辅助开发与技术风险复核

AI 可用于依赖图分析、代码骨架、移植补丁草案、测试生成、编译错误归类和审查提示，
但不作为硬件事实来源。以下信息必须由厂商资料、实际 DTB、上游源码、ELF 或实板
日志支撑：

1. UART MMIO、寄存器宽度、IER 副作用、IRQ 和 pinmux；
2. payload/DTB 地址、DRAM、reserved-memory、U-Boot relocation 和 OpenSBI 占用；
3. timebase、SBI extension、PLIC source/context、claim/complete 行为；
4. Ethernet EMAC 寄存器、DMA descriptor、cache maintenance、PHY/MDIO、IRQ 和 clock/reset；
5. IMU/LiDAR 总线、时间同步、坐标系、队列背压、传感器任务所有权；
6. PWM/CAN/GPIO/UART 执行器、电机控制器、急停和故障恢复语义。

参考 Linux/U-Boot 驱动只用于理解硬件行为，必须核对许可证和可复制范围，不能把
GPL 代码直接复制到比赛实现。每项新增功能都要记录新增线程、栈、堆、socket、
持久化空间、关闭方式和 simulator/实板验证方法。

## 十四、预期交付与验收标准

### 14.1 代码和文档交付

1. K1 芯片层、MUSE Pi Pro 板级包、链接脚本、manifest 和 NSH/VelaROS defconfig；
2. K1 polling UART、SBI TIME、按条件启用的 PLIC/GPIO 及后续网络/传感器驱动；
3. 静态 VelaROS `rcl/rmw/Fast DDS`、消息生成闭包、openvela compatibility layer；
4. uORB、syslog、KVDB、Binder 和固定 buffer backend 的融合实现；
5. ROS-free LIO/Nav 库、固定输入、pose/map snapshot 和 motion command adapter；
6. 构建、ELF/Map/size、串口采集、trap 解析、互操作和资源验收工具；
7. 启动指南、硬件接线、测试记录、来源/许可证清单、原始日志、SHA256 和演示材料。

### 14.2 核心验收

- U-Boot 可重复加载 K1 openvela ELF，openvela 以 S-mode 启动并进入 NSH；
- UART、SBI TIME、调度、GPIO/PLIC 和基础存储按实板日志验收；
- K1 Ethernet 或确定的 WiFi 路径完成 UDPv4 和 Fast DDS discovery；
- K1 VelaROS 节点无需 Agent 被标准 Linux ROS 2 Lyrical 发现，并完成 Topic/Service/
  Action 互操作；
- 真实 IMU/LiDAR 进入固定 ingress，LIO 生成有效 pose/map，Navigation 生成有界命令；
- 真实里程计和底盘消费者完成命令超时、急停、零速停车和运动闭环；
- 有明确的 ROM/RAM、线程/栈、heap、CPU、时延、功耗、温升和长稳数据；
- 第三方能够按文档从干净工作区复现构建、验收和失败定位。

## 十五、当前结论与资源申请依据

当前项目适合评估为 **“软件方案 Conditional Go，申请开发板后继续实板验证”**：

- **开发板：必要。** K1 尚无实板，启动、网络、真实传感器和底盘执行器无法由
  simulator 替代；开发板用于完成 G1-G4 硬件门禁。
- **Token：必要。** 后续需要实现 K1 网络、传感器 ingress、底盘驱动，开展构建、
  调试、资源测量、异常复现和长稳回归。
- **量产券：建议条件化。** 当前已经具备静态依赖、固定资源边界、标准 ROS 2 互操作
  和产品接口，但必须在实板网络、真实传感器、PWM/底盘、功耗温升和整机可靠性通过
  后，才能进入量产/小批试制结论。

## 十六、项目创新点

1. **以 K1 为目标的 openvela 纵向适配。** 不止验证应用启动，而是覆盖启动链、
   RV64 S-mode、UART、SBI TIME、PLIC/GPIO、BSP、构建和首板诊断边界。
2. **openvela 作为实时机器人执行域。** openvela 负责调度、驱动、板内数据、日志、
   配置、服务和安全控制，VelaROS 只在其上提供 ROS 2/Fast DDS 与机器人算法语义。
3. **原生 ROS 2/DDS 互操作。** K1 端直接拥有 DDS participant，通过标准 RTPS/UDPv4
   加入 Linux ROS 2 graph，不依赖 micro-ROS Agent 或私有桥接协议。
4. **板内能力复用和资源有界。** uORB、KVDB、syslog、Binder、固定共享池和静态消息
   类型替代重复的 ROS/Linux 基础设施，LIO/Nav 的容量、线程和控制边界可审计。
5. **LIO 到控制的端侧闭环。** 将 ROS-free Small Point-LIO、Super-LIO OctVox、PGO、
   costmap、bounded A*、RPP/DWPP 和 uORB motion command 连接到同一 K1 运行时。
6. **证据驱动、可复现、可上游。** 每个硬件结论按源码、配置、ELF、原始日志和实板
   记录分层，明确区分 simulator PASS、目标链接和 K1 实板验收。

## 相关文档

- [`VELAROS_EARLY_CHECK.md`](VELAROS_EARLY_CHECK.md)
- [`VELAROS_HANDOFF.md`](VELAROS_HANDOFF.md)
- [`VELAROS_OPENVELA_INTEGRATION.md`](VELAROS_OPENVELA_INTEGRATION.md)
- [`VELAROS_NAVIGATION_HANDOFF.md`](VELAROS_NAVIGATION_HANDOFF.md)
- [`VELAROS_ROBOT_PRODUCT_PROFILE.md`](VELAROS_ROBOT_PRODUCT_PROFILE.md)
- [`VELAROS_COMMUNICATION_PROFILE.md`](VELAROS_COMMUNICATION_PROFILE.md)
- [`VELAROS_AMP_HANDOFF.md`](VELAROS_AMP_HANDOFF.md)
- [`K1_PROJECT_DESCRIPTION.md`](K1_PROJECT_DESCRIPTION.md)
- [`K1_DELIVERY_CHECKLIST.md`](K1_DELIVERY_CHECKLIST.md)
