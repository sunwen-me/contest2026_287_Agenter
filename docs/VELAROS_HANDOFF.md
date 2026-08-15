# VelaROS 项目交接文档

更新时间：2026-08-08

## 交接结论

当前项目由两条并行主线组成：

1. SpacemiT K1 / MUSE Pi Pro 的 openVela BSP；
2. 深度复用 openVela 能力、保留标准 ROS 2 DDS 互操作的精简机器人运行时
   VelaROS。

K1 主线已经完成无板阶段的启动资料盘点、RV64 S-mode BSP 骨架、UART/OpenSBI
timer、早期异常日志、可重复构建、ELF 验收和 U-Boot 上板包，但没有实板，因此
不能声称 openVela 已在 K1 启动。

VelaROS 主线已经在 goldfish-arm64 simulator 跑通 Topic、Service、Action、静态
rclcpp RAII 子集、openVela 平台融合和 Linux ROS 2 Lyrical 互操作。Fast DDS 已按
产品 profile 静态裁剪，原生 SHM/DataSharing 已删除，并由 VelaROS 固定共享缓冲
backend 提供板内大 payload 基础设施。该共享路径尚未完成 K1 DMA/cache/RPMsg
硬件验收。

截至本次交接，没有收到待接入的第三方 ROS 2 功能包源码，因此尚未对某个外部
功能包作出“已编译运行”的结论。当前具备的是接入功能包所需的静态运行时和验证
基线；是否能接入必须依据该包实际使用的 API、消息类型和依赖闭包逐包判断。

## 项目命名不变量

项目正式名称是 **VelaROS**，不是 `VelaROS ROS2` 或 `NuttxROS`。

项目自研代码统一使用：

```text
middleware/velaros/
external/velaros
CONFIG_VELAROS_*
velaros_talker
velaros_listener
velaros_bridge_service
```

`ros2` 只允许出现在确实描述 ROS 2 边界的名字中，例如：

```text
external/velaros_ros2_sources/       锁定的 ROS 2 上游源码闭包
external/velaros_ros2_generated/     ROSIDL 主机生成物
tools/restore_velaros_ros2_sources.sh
tools/generate_velaros_ros2_interfaces.sh
tools/check_velaros_ros2_host.py
docs/VELAROS_ROS2_*                  ROS 2 闭包、许可证或 host 互操作文档
```

不要重新引入 `middleware/velaros_ros2`、`external/velaros_ros2`、
`CONFIG_VELAROS_ROS2_*` 或 `velaros_ros2_*` NSH 命令。manifest 已把
`middleware/velaros` 映射到 `external/velaros`。

## 技术定位

VelaROS 不是 micro-ROS，也不依赖 XRCE-DDS Agent。跨设备链路是：

```text
VelaROS static rclcpp subset
  -> rcl
  -> rmw_fastrtps_cpp
  -> Fast DDS
  -> RTPS / UDPv4
  -> standard ROS 2 host
```

板内路径优先复用 openVela：

```text
high-rate local data     -> uORB
large local payload      -> fixed buffer pool + descriptor
logging                  -> NuttX syslog
persistent configuration -> KVDB
runtime control          -> Binder / servicemanager
AMP software boundary    -> fixed frame + uORB (socketpair/simulator now)
future AMP transport     -> RPMsg / OpenAMP / virtio on K1
cross-device ROS graph   -> Fast DDS
```

这是 VelaROS 与“在 NuttX 上直接堆叠完整 ROS 2”的核心区别。新增功能前必须先检查
openVela 是否已有等价能力，避免移植第二套日志、配置、IPC、驱动数据总线或服务
管理系统。

无板阶段已经实现一个 transport-neutral AMP 软件原型：固定 92 B frame、CRC、单调
sequence、heartbeat、命令/状态/急停和 timeout 状态机，以及 `velaros_motion_command`
和 `velaros_amp_status` uORB 边界。Linux POSIX socketpair 和 goldfish simulator
只用于协议与状态机验证，不能替代 K1 的 RPMsg/OpenAMP、reserved-memory、vring、
跨核 IRQ、cache coherency 和真实传感器/底盘硬件验收。

## 锁定版本

关键 ABI 组合：

| 组件 | 版本 |
|---|---:|
| ROS 2 | Lyrical |
| Fast DDS | 3.6.1 |
| Fast-CDR | 2.3.6 |
| rmw_fastrtps | 9.4.8 |
| rcl | 10.4.4 |
| rclcpp 语义基线 | 32.0.0 |

完整 revision、hash 和许可证见：

- `tools/velaros-ros2-sources.lock`
- `docs/VELAROS_VERSION_MATRIX.md`
- `docs/VELAROS_ROS2_SOURCE_AND_LICENSES.md`

不要因为主机安装了更新版本就自行切换目标版本。版本升级必须同时验证
`rcl/rmw/rosidl/Fast DDS/Fast-CDR` 的 ABI、生成类型和 Linux host 互操作。

## 已完成功能

### DDS 和 ROS 2 通信

- Fast DDS discovery、publisher/subscriber 和完整 participant 回收；
- simulator 与 Linux ROS 2 Lyrical 双向 Topic 3/3；
- `std_srvs/SetBool` 双向 Service；
- 标准五通道 Action：SendGoal、GetResult、CancelGoal、Feedback、Status；
- 产品 `geometry_msgs/Twist /cmd_vel` 和
  `velaros_interfaces/MoveRelative` Action；
- 成功 Goal、取消 Goal、Feedback、Result 和无任务残留验收。

### 精简运行时

- `rcl` context、node、publisher、subscription、client、service、timer、wait set；
- caller-owned、无内部线程的有界单线程 executor；
- 静态 `rclcpp` RAII Topic/Service/Timer 子集；
- 静态 `rclcpp_action` RAII 子集；
- 固定 Action 类型白名单、最多 2 个并发 Goal；
- 无运行时任意类型加载、无无界 Goal/Result 缓存、无每 Goal 线程、无通用动态
  多线程 executor。

### openVela 融合

- rcutils 日志映射到 syslog；
- 白名单 uORB 与 ROS Topic 转换；
- KVDB 保存 Domain、participant、bridge 和 heartbeat 配置；
- Binder 管理的 `velarosd` 与 `velarosctl`；
- 移动机器人 ROS 命令进入持久 uORB 控制边界；
- 固定共享缓冲池、uORB descriptor 和静态 ROSIDL BufferBackend。

### Fast DDS 产品裁剪

发布 profile 保留 SIMPLE PDP、SIMPLE EDP、UDPv4、标准 QoS 和可靠/尽力而为数据
面，排除：

- TypeLookup service；
- DDS-RPC；
- Discovery Server/Client/Backup/database；
- static EDP；
- TCP、UDPv6；
- Fast DDS 原生 SHM/DataSharing；
- 开发 HelloWorld 和诊断入口。

不要把 ROS 2 Service 与 Fast DDS DDS-RPC 混为一谈。VelaROS Service 通过标准
ROS 2 request/reply topic 语义工作，不需要 Fast DDS DDS-RPC 模块。

## 固定共享缓冲 backend

当前产品配置：

```text
CONFIG_VELAROS_BUFFER_POOL_SLOTS=4
CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE=16384
CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES=4
```

总 payload BSS 为 64 KiB。descriptor 包含 pool cookie、slot、generation、lease、
owner、length、capacity 和 flags，CDR 字段净长度 36 B。

同 pool 兼容端点传 descriptor 并映射同一 payload 地址；远端或 metadata 不兼容
端点确定性回退到 CPU buffer + CDR + UDP。generation、lease 和 owner 用于拒绝
过期 descriptor、越权释放和 double release。调用方退出异常时必须显式执行
owner reclaim；当前没有超时回收线程。

详细设计见 `docs/VELAROS_BUFFER_BACKEND.md`。

## 构建配置

开发验收配置：

```text
configs/goldfish-arm64-v8a-ap-fastdds/
```

包含 smoke、Topic/Service/Action 互操作命令和共享缓冲测试。

产品发布配置：

```text
configs/goldfish-arm64-v8a-ap-velaros/
```

关闭开发 smoke、Fibonacci 和通用互操作命令，保留产品 MoveRelative、
openVela UI/媒体/网络/安全库/数据库/调试测试基线。

构建脚本会清除继承的 ROS/ament/colcon 环境变量。不要直接依赖
`source /opt/ros/lyrical` 后的 target CMake 发现结果；主机 ROS 只用于接口生成和
host 互操作。

## 可重复恢复与构建

从仓库根目录执行：

```bash
OPENVELA_ROOT=/home/sw/Dev/k1-workspace \
  tools/restore_velaros_dds_sources.sh --check
OPENVELA_ROOT=/home/sw/Dev/k1-workspace \
  tools/restore_velaros_ros2_sources.sh --check

tools/build_velaros_dds_sim.sh --jobs 8
tools/build_velaros_release_sim.sh --jobs 8
```

需要重建锁定外部源码时使用恢复脚本的 `--replace`，不要直接在临时恢复目录中
做不可复现修改。VelaROS 对上游源码的修改必须同步固化到 `tools/patches/`。

## 第三方 ROS 2 功能包接入规则

### 结论边界

不能把任意 ROS 2 包直接放到目标端运行 `colcon build`。目标端采用 openVela
Kconfig/CMake 静态编译，主机 ROS 2/ament/colcon 只负责依赖分析、ROSIDL 代码
生成和 Linux 互操作验证。

以下源码包最有可能直接适配：

- 纯算法、控制或协议节点；
- 只使用当前 Topic、Service、Action、Timer 和有界 single-threaded executor；
- 消息和内存上限可在编译期确定；
- 不依赖 Linux 进程、动态加载、桌面图形或大型第三方运行时。

以下功能通常需要改写或由 openVela 能力替代：

- ROS parameters/持久配置：优先接 KVDB；
- ROS logging/rosout：优先接 syslog；
- 设备内高频传感数据：优先接 uORB；
- components、pluginlib、class_loader：改为 Kconfig 静态选择和链接；
- 通用多线程 executor、callback group 和无界 future：改为有界静态 executor；
- 本地大 payload：评估 VelaROS fixed buffer，而不是恢复 Fast DDS 原生 SHM；
- AMP 跨核数据：规划 RPMsg/OpenAMP descriptor 路径。

以下包不能承诺原样落地：

- 依赖完整 rclcpp API 或运行时加载任意类型/插件的包；
- 使用 `epoll`、`fork`、`dlopen` 等 Linux 专用语义的包；
- RViz、完整 Nav2/MoveIt、PCL/OpenCV 大闭包或仅提供 Linux 二进制的包。

### 新包接入步骤

收到源码路径或 Git 地址后按以下顺序执行，不要先盲目改代码：

1. 读取 `package.xml`、`CMakeLists.txt` 和源文件，锁定许可证、ROS 2 版本、直接及
   传递依赖；
2. 列出实际使用的 rcl/rclcpp API、Topic/Service/Action 类型、QoS、线程、动态
   内存和 Linux API；
3. 给出 `Go / Conditional Go / No-Go` early check，并把每项功能标为保留、用
   openVela 替代、静态化或裁剪；
4. 在 Lyrical 锁定版本上生成 ROSIDL C/C++ 与 Fast RTPS typesupport，禁止混用
   主机最新发行版生成物；
5. 把目标代码接入 `middleware/velaros` 或独立产品模块的 Kconfig/CMake，静态
   链接到固件，不把 ament/colcon 带到目标端；
6. 先做 Linux host 单元/互操作，再做 goldfish simulator Topic/Service/Action
   端到端验证；
7. 输出 Flash、静态 RAM、峰值 heap、线程数/栈、消息上限、退出回收和被裁剪
   能力报告；拿到 K1 后再补实板性能和稳定性结论。

功能包接入的完成标准不是“编译通过”，而是目标固件可重复构建、节点在 simulator
正常启动和退出、与 Linux ROS 2 Lyrical 完成约定通信、资源有界，并且没有重复
移植 openVela 已有的日志、配置、IPC 或驱动数据能力。

## 验收入口

开发 simulator 全回归：

```bash
python3 tools/check_velaros_dds_sim.py --timeout 150
```

Linux ROS 2 Lyrical 双向通信：

```bash
python3 tools/check_velaros_ros2_host.py
```

只跑真实机器人产品节点：

```bash
python3 tools/check_velaros_ros2_host.py --robot-only
```

对 release ELF 跑产品节点：

```bash
python3 tools/check_velaros_ros2_host.py --robot-only --output \
  /home/sw/Dev/k1-workspace/cmake_out/\
contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros
```

release 静态门禁：

```bash
tools/check_velaros_release_config.sh \
  /home/sw/Dev/k1-workspace/cmake_out/\
contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros
```

源码和命名残留检查：

```bash
rg -n 'CONFIG_VELAROS_ROS2_|middleware/velaros_ros2|\
external/velaros_ros2([/"`]|$)|velaros_ros2_' \
  . --glob '!logs/**'
```

预期无输出。`velaros_ros2_sources`、`velaros_ros2_generated` 和明确的 ROS 2
host/闭包工具不属于错误残留。

## 当前构建基线

最近一次全量回归时间为 2026-08-04。以下两个文件在本次交接时重新计算过
SHA256，和文档记录一致。

发布 ELF：

```text
/home/sw/Dev/k1-workspace/cmake_out/
  contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros/nuttx
SHA256: 9287349c3ffd53f5c3b90b272c1c9466d4c7847e7b552f009a4c29371f870a65
```

开发 ELF：

```text
/home/sw/Dev/k1-workspace/cmake_out/
  contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds/nuttx
SHA256: 7db77df9ba828d8b121bb320d4a925773ea1f36c2fe4d11190a2f5b272465ff2
```

共享 backend 相对加入前的同一 release 闭包增加约 26.67 KiB Flash 和
65.06 KiB 静态 RAM，其中 64 KiB 是显式 payload 池。把删除 Fast DDS 原生 SHM
和加入 VelaROS backend 合并计算，Flash 仍净减少约 325.34 KiB。精确口径见
`docs/VELAROS_SIZE_BASELINE.md`。

日志路径：

```text
cmake_out/velaros-dds-sim-build.log
cmake_out/velaros-release-sim-build.log
cmake_out/velaros-dds-sim-runtime.log
cmake_out/velaros-ros2-host-runtime.log
cmake_out/velaros-ros2-host-node.log
```

模拟器主动关闭后，外层 emulator launcher 偶尔打印 segmentation fault/139。
只有在全部 guest 断言、任务残留检查和 host 验收已通过后出现时，才按 launcher
退出问题处理；不能用它掩盖 guest 节点或 DDS 生命周期失败。

## LIO 与导航迁移状态

已将 x86_lio_slam 默认 ROS-free core 的完整 LIO + Navigation 路径接入
`middleware/velaros`。`velaros_lio` 现在同时编译 Small Point-LIO、SmallIVox、
Super-LIO OctVox localizer、global map、keyframe manager、dense pose graph、
PGO、IMU dead reckoner、loop closure、map-odom correction 和导航 snapshot
bridge；导航库包含固定容量 costmap、有界 A*、RPP/DWPP 控制器、双槽 snapshot、
`NavigationPipeline`、VelaROS POD adapter 和真实持久
`velaros_motion_command` uORB sink。目标构建选择 scalar Small Point-LIO；x86
AVX2/RVV、ROS bag replay、实验性 Batch LIO 和 block PCG 仍属于不适合该目标
profile 的可选路径。

`velaros_lio::Runtime` 通过固定容量 `ImuSample`/`PointSample` ingress 调用真实
`SmallPointLioFrontend -> SlamSystem -> GlobalMap`，把有效 pose、非空全局点云和
单调 map version 发布到同一个 `NavigationSnapshotPublisher`；导航 pipeline
随后从该 snapshot 读取并经 `UorbMotionCommandSink` 发布运动命令。开发 simulator
defconfig 打开 `CONFIG_VELAROS_NAVIGATION`、`CONFIG_VELAROS_LIO` 及 smoke；release
defconfig 链接导航和 LIO 库但关闭 smoke。

当前已验证：源工程两组测试通过，导航 benchmark 1000/1000 成功且规划/控制均
为零次动态分配；开发 simulator 的 LIO smoke 使用合成 IMU + 64 点 LiDAR 输入，
验证 12 帧真实 LIO pose、非空 global map、map version、NavigationPipeline 和
真实 uORB motion-command 发布：

```text
VelaROS LIO smoke: PASS frames=12 map_points=64 map_version=3 navigation_commands=12
```

独立导航 smoke 仍使用有界参考 pose 来覆盖命令限幅、时间戳/超时/序号、陈旧
snapshot 停车、急停和侧向速度拒绝；它不替代上面的 LIO 链路测试。当前尚未完成
的是把具体 K1 IMU/LiDAR 驱动接入这些 POD ingress（仓库目前没有可复用的真实
传感器 uORB topic/driver）、K1 工具链/底盘消费者和实板运行时验证；因此 simulator
结果不能表述为真实传感器或 K1 硬件验收。

完整导航迁移记录见 [`VELAROS_NAVIGATION_HANDOFF.md`](VELAROS_NAVIGATION_HANDOFF.md)。

## K1 当前状态

已经完成：

- K1 启动资料和参考仓盘点；
- MUSE Pi Pro board/chip 骨架；
- S-mode 轮询 UART；
- OpenSBI TIME 24 MHz timer；
- 启动阶段、hart、DTB、CSR 和同步异常日志；
- PLIC context 1 的证据与默认关闭骨架；
- 可重复构建、ELF 验收、U-Boot 包、串口采集和异常符号化；
- RV64 S-mode NSH ELF。

尚未完成：

- 实板 BootROM/FSBL/OpenSBI/U-Boot/openVela 启动闭环；
- UART、timer、PLIC 实测；
- K1 Ethernet PHY/MAC/DMA/IRQ/clock/reset；
- K1 网络上的 ROS 2 Topic/Service/Action；
- KVDB 真实持久分区与掉电恢复；
- init 自启动和长时间稳定性。

上板时先追求 UART banner 和 NSH，不要在基础 IRQ、timer、网络尚未稳定前同时
调试完整 VelaROS。

## 下一阶段优先级

### 已完成：命名后的最终回归

- 开发和 release 配置均从干净输出目录构建成功；
- `check_velaros_dds_sim.py` 全回归 PASS；
- 新 `velaros_talker`、`velaros_listener`、`velaros_bridge_service` 已完成 Linux
  Lyrical Topic 3/3、Service 2/2 和双向 Action 验收；
- release `Twist`/`MoveRelative` 产品验收 PASS；
- 文档、当前 ELF hash 和 release section 大小已更新。

### P0：拿板后的 K1 最小启动闭环

1. U-Boot 加载 ELF/bin 与正确 DTB；
2. 捕获第一条 UART 和 early trap；
3. 验证 OpenSBI timer tick；
4. 进入 NSH；
5. 再启用 PLIC 和 Ethernet。

### P1：K1 网络与 ROS 2 互操作

1. PHY link、DHCP/static IP、ping；
2. UDP 单播和 MTU；
3. Fast DDS discovery；
4. Topic 3/3；
5. Service 和 MoveRelative Action；
6. 资源、线程栈、退出和 30 分钟稳定性。

### P2：真实大 payload 产品节点

选择一个 bounded 产品消息，例如相机帧块或激光点云块：

1. 定义静态 ROSIDL buffer 字段和最大尺寸；
2. 接入 VelaROS 固定池与 uORB descriptor；
3. 实现 K1 DMA 内存、cache clean/invalidate 和 ownership barrier；
4. 如跨核，接入 RPMsg descriptor 和地址转换；
5. 测量 zero-copy/local 与 CDR/remote 两条路径的延迟、吞吐、CPU、峰值 heap；
6. 做丢包、发送失败、任务崩溃和 owner reclaim 压力测试。

## 禁止过度声明

当前可以表述为：

> VelaROS 已在 openVela simulator 完成裁剪 ROS 2/DDS 高级通信、openVela
> 原生能力融合和固定共享缓冲核心链路，并与 Linux ROS 2 Lyrical 互操作。

当前不能表述为：

- 完整 ROS 2 或完整上游 rclcpp 已迁移；
- 任意 ROS 类型可运行时加载；
- K1 已启动 openVela；
- K1 Ethernet 已完成；
- 所有消息或跨设备 DDS 已零拷贝；
- K1 DMA/cache/AMP 共享内存已经验收；
- VelaROS 已在实板达到产品稳定性。

## Git 工作区状态

参赛仓库路径：

```text
/home/sw/Dev/k1-workspace/contest2026_287_Agenter
```

当前分支为 `feat/k1`，HEAD 为：

```text
fd40c62 feat: add K1 BSP and VelaROS DDS baseline
```

本轮 VelaROS 扩展、产品 profile、命名重构、LIO/导航迁移和文档仍在未提交工作区
中；`git status --short` 当前包含大量修改、删除和未跟踪文件。大量
`middleware/velaros_ros2/*` 的删除与 `middleware/velaros/*` 的新增是同一次命名
迁移，不要只恢复删除项，也不要用 `git reset --hard`、`git checkout --` 或清理
未跟踪文件破坏这些改动。提交前应
先完整审核 diff，并把 rename、生成物策略和补丁恢复链路一起纳入提交。

## 新会话接续指令

可把下面内容直接交给下一会话：

```text
继续 /home/sw/Dev/k1-workspace/contest2026_287_Agenter 的 VelaROS 工作。
先完整阅读 docs/VELAROS_HANDOFF.md，并核对 git status，保护当前所有未提交改动。
项目是 VelaROS，不是 micro-ROS 或 NuttxROS；ROS 2 基线为 Lyrical，Fast DDS 为
3.6.1。不要 source 主机 ROS 环境污染 target 构建，不要恢复 Fast DDS 原生 SHM，
也不要重复移植 openVela 已有的 uORB/syslog/KVDB/Binder 能力。

如果我提供一个 ROS 2 功能包，先检查 package.xml、CMakeLists.txt、源码 API、
ROSIDL 类型、QoS、线程/内存和完整依赖闭包，给出 Go/Conditional Go/No-Go；
然后按 VelaROS 静态 profile 接入 Kconfig/CMake，运行 simulator 与 Linux ROS 2
Lyrical 互操作验收，并报告裁剪项和资源成本。不要声称任意 ROS 2 包可以原样
colcon 上板。

如果没有收到功能包，则先复跑文档中的恢复检查和 simulator 基线；没有 K1 实板
时不要声称 K1 已启动，也不要用 simulator 结果替代 K1 Ethernet/DMA/cache 验收。
```

## 接手者首先阅读

1. `README.md`
2. `docs/VELAROS_PROJECT_DESCRIPTION.md`
3. `docs/VELAROS_OPENVELA_INTEGRATION.md`
4. `docs/VELAROS_COMMUNICATION_PROFILE.md`
5. `docs/VELAROS_FASTDDS_STATIC_PROFILE.md`
6. `docs/VELAROS_BUFFER_BACKEND.md`
7. `docs/VELAROS_ROBOT_PRODUCT_PROFILE.md`
8. `docs/K1_DELIVERY_CHECKLIST.md`
9. `docs/VELAROS_NAVIGATION_HANDOFF.md`

接手后先运行恢复检查和现有 simulator 验收，确认基线未退化，再开始 K1 或新产品
消息工作。
