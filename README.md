# VelaROS on MUSE Pi Pro (SpacemiT K1)

本项目参加 2026 openvela AI 硬件开发者大赛，目标是在 MUSE Pi Pro
（SpacemiT K1）上完成 openvela 的首次适配，并在 openvela 上移植保留原生
DDS 的裁剪版 ROS 2，形成轻量级机器人运行时 VelaROS。

审核用项目方案和范围评估见
[`docs/VELAROS_PROJECT_DESCRIPTION.md`](docs/VELAROS_PROJECT_DESCRIPTION.md)；
K1 BSP 子计划见
[`docs/K1_PROJECT_DESCRIPTION.md`](docs/K1_PROJECT_DESCRIPTION.md)。

## 当前状态

项目目前处于 K1 bring-up 和 VelaROS 可行性验证阶段。

VelaROS 侧：

- 已确定采用裁剪 `rclcpp -> rcl -> rmw_fastrtps_cpp -> Fast DDS` 的原生
  ROS 2 路线，不使用 micro-ROS/XRCE-DDS Agent；
- 已锁定 ROS 2 Lyrical、Fast DDS 3.6.1 和 Fast-CDR 2.3.6 版本组；
- 已恢复 Fast DDS 全部依赖源码，完成 openvela goldfish-arm64 固件编译；
- 已修复 libc++abi 跨任务组 TLS、DDS 线程栈、TypeObject 查询和 UDP 接收线程
  退出问题；
- 已在 simulator 使用 loopback 单播完成 DDS discovery，Publisher/Subscriber
  匹配成功，3 条消息全部发送并接收，退出后无示例任务或 DDS 线程残留；
- 已提供源码 revision lock、Fast DDS/libc++abi 补丁、不受 `/opt/ros` 污染的
  可重复构建和一键运行验收；
- DDS-only 完整验收 ELF SHA256：
  `61b99d51509012477bcf018ab89187ae1c4478d9df62d64853303e5f48d2193e`；
- 已从本机 Lyrical `package.xml` 提取真实闭包，锁定并恢复
  `rcutils 7.1.1`、`rosidl 5.2.1`、`rosidl_dynamic_typesupport 0.4.1`、
  `rmw 7.10.1`、`rcpputils 2.14.5`、`rmw_dds_common 6.0.0`、
  `rosidl_typesupport_fastrtps 3.9.5` 及后续 `rmw_fastrtps/rcl` 源码；
- 已接入 `rcutils + rosidl runtime + rmw interface` 的 openvela 构建骨架，
  并进一步目标编译 `rmw_dds_common`、其 3 个生成消息和 Fast RTPS C++ 类型
  支持；allocator/node-name、GraphCache 和 DDS 合并验收均通过；
- `rmw_fastrtps_shared_cpp 9.4.8` 与 `rmw_fastrtps_cpp 9.4.8` 已完成目标端
  静态编译和固件链接；simulator 上 context 初始化、Fast DDS participant/node
  创建、ROS graph 查询以及完整 participant 生命周期回收通过；
- 当前 RMW 集成候选 ELF SHA256：
  `186bb0de97ce925849d108c1b94686f0589b347eff57d02217a80d8a272a2f6a`；
- RMW 生命周期总验收为 **PASS**。根因是 NuttX UDP 关闭路径在接收线程仍可能
  访问 ASIO socket 时先 close、后 join 的竞态；改为停止标志、join、再 close
  后，graph listener/reader/writer、participant 和 context 均正常回收；
- `rcl 10.4.4` 最小客户端层已静态编译链接；simulator 上
  `rcl_init()`、`rcl_node_init()`、Fast DDS participant 创建、
  `rcl_node_fini()`、shutdown 和 context 回收全部通过；
- 当前 `rcl` 最小生命周期验收 ELF SHA256：
  `982e15964bbe443b779568c3cde4daafd917545fb6f784ce5750c9bdf63bf181`；
- 已接入 `rcl` publisher/subscription 与 `std_msgs/String`，完成 openvela
  simulator 和本机 ROS 2 Lyrical 的无 Agent 双向 Fast DDS 通信；guest 到 host
  与 host 到 guest 均 3 发 3 收，节点正常退出；
- 已接入 `rcl` Client/Service、bounded executor service 槽和裁剪后的
  `std_srvs/SetBool`；主机 Lyrical 对 `/velaros/runtime/set_bridge` 两次标准
  请求/响应均 PASS，并实际复用 openVela KVDB 更新 bridge 状态；
- 已接入 `rcl_action` 与静态 `example_interfaces/Fibonacci`，保留 SendGoal、
  GetResult、CancelGoal、Feedback、Status 五条标准通道；默认限制 2 个并发 Goal、
  序列 32 项，不引入每 Goal 线程、无界缓存或动态多线程 executor；
- simulator 与主机 Lyrical 已完成双向 Action server/client 互操作，两个方向均
  覆盖 `SUCCEEDED(4)`、`CANCELED(5)`、feedback/status 和正常资源回收；
- 已接入 steady clock、timer 和 wait set；50 ms timer 通过 `rcl_wait()` 唤醒，
  callback 恰好执行一次，wait set/timer/clock 均正常回收；
- 已实现有界、无内部线程的 VelaROS 单线程 executor，复用一个 `rcl_wait_set`；
  simulator 中 timer 发布 3 条消息，subscription callback 按序接收 3 条，
  executor 和全部 ROS 实体正常回收；
- 已实现以 Lyrical `rclcpp 32.0.0` 为语义基线的静态 C++ RAII 子集，提供
  Context、Node、QoS、Publisher/Subscription、Client/Service、WallTimer 和
  有界 SingleThreadedExecutor；simulator 上 C++ Topic 3/3、SetBool 请求/响应
  1/1、Timer 3 次和完整回收均 PASS；
- 已增加有界 `rclcpp_action` 源码级 RAII 子集，保留标准五通道、Goal 接受/拒绝、
  Feedback、成功/中止/取消和 Result；与 Topic/Service 共用 caller-owned
  executor，不包含 future、每 Goal 线程、无界缓存或动态 Action 加载；simulator
  上一次成功和一次取消均 PASS，且 rcutils 非致命 TypeObject 降级日志已清理；
- 当前静态 rclcpp + Action 开发验收 ELF SHA256：
  `ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7`；
- 已加入首个真实移动机器人产品节点：标准 `geometry_msgs/Twist /cmd_vel`
  进入持久 uORB，固定布局 `velaros_interfaces/MoveRelative` Action 支持成功、
  Feedback、Result 和取消；开发与 Release 均已和本机 ROS 2 Lyrical 互操作；
- Release 只编译产品 `MoveRelative`，开发用 Fibonacci 的 functions、Fast RTPS
  typesupport、traits 和 ELF 符号均不存在；
- 不包含完整上游 ABI、线程池、parameters、rosout、组件加载或动态类型；
- 已完成 openVela 能力复用审计：板内高频数据优先使用 uORB，日志、配置、
  服务管理和 AMP 分别复用 syslog、KVDB、Binder/service manager 与 RPMsg，
  DDS 只承担跨设备 ROS 2 graph/Topic/QoS 兼容；
- 已实现 `rcutils` 到 NuttX syslog 的薄适配；固定 384-byte 栈缓冲，不引入
  spdlog、文件日志、动态插件或日志线程；
- 已实现首批白名单 uORB ↔ ROS 2 bridge：现有 `sensor_temp` 到
  `/velaros/sensor/temperature`，以及 `/velaros/control/setpoint` 到持久化
  uORB `velaros_control_setpoint`；bridge 由调用方/executor 驱动，不创建线程、
  事件循环或运行时类型注册表；
- 已把本地运行配置接入 openvela KVDB，不移植 ROS YAML 配置栈；Domain ID、
  participant ID、bridge 开关和 heartbeat 周期使用 `persist.velaros.*` 键保存，
  talker/listener 创建 ROS context 时实际读取 Domain/participant 配置；
- 已实现 Binder 管理的 `velarosd` 控制面和 `velarosctl` 客户端，服务注册名为
  `openvela.velaros.runtime`；跨 NuttX task 完成状态查询、配置读写和干净停止，
  服务端只使用一个显式 64 KiB task 与 `poll()`，不引入线程池或 `epoll`；
- simulator 集成 smoke 中 syslog 2 条、uORB → ROS 3 条、ROS → uORB 3 条均
  通过，原有 DDS/RMW/rcl/executor 和 Linux Lyrical 双向 3/3 回归仍为 PASS；
- 已实现面向 openVela 的固定共享缓冲 backend：产品配置锁定 4×16 KiB payload
  池和每 slot 4 个有界 lease，uORB/ROSIDL 只传 descriptor；simulator 已验证
  同地址 payload、静态 backend、耗尽/陈旧 descriptor 拒绝和远端 CPU/CDR
  fallback。K1 DMA/cache、RPMsg/AMP 和真实相机/点云节点仍待接入；
- 当前包含 executor、syslog/uORB、KVDB/Binder 控制面与 ROS 2 Lyrical 双向
  互操作的 ELF SHA256：
  `7a44d3f0b4a08beb6e43dd756103d605d53bf729d88a4e6edfb45a5db5d7f839`
  （650175048 bytes）；删除无关 Binder 示例后，加入 VelaROS 控制面的 ELF 仍比
  上一基线减少 3783288 bytes；
- 当前包含有界 Action 的开发 ELF SHA256：
  `ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7`；
- 当前裁剪明确关闭 rosout、全局参数、非空命令行参数、YAML 参数解析、远程 ROS
  参数服务和 Type Description service；本地设备配置已由 KVDB 承担，但不应把
  当前结果描述成完整 `rcl` 或完整 ROS 2 移植；
- 已增加 `goldfish-arm64-v8a-ap-velaros` ROS-only 发布配置；完整保留 openVela
  的 UI、媒体、网络、安全库、数据库、调试和测试栈；发布配置启用 Fast DDS
  VelaROS 静态 profile，在源码级排除 TypeLookup Service、Fast DDS DDS-RPC、
  Discovery Server/Client/Backup、静态 EDP 和 Fast DDS 原生 SHM/DataSharing，并
  锁定 SIMPLE discovery + UDPv4；板内普通消息由 uORB 替代；
- profile 已通过双向 Topic、Service 和 Action 互操作，以及 DDS/RMW/rcl/executor/
  uORB/Binder/KVDB 生命周期回归；最终发布 ELF SHA256 为
  `82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`；
- 相同当前产品闭包下，删除 Fast DDS 原生 SHM/DataSharing 使 Flash 从
  23,485,960 B 降至 23,125,496 B（-360,464 B），静态 RAM 从 663,640 B 降至
  661,752 B；后续固定共享缓冲 backend 的独立体积结果见
  [`docs/VELAROS_BUFFER_BACKEND.md`](docs/VELAROS_BUFFER_BACKEND.md) 与体积文档；
- 同一发布 defconfig、`-O3` 和 openVela 基线下，静态 profile 使整机 Flash 从
  25,672,616 B 降至 23,143,224 B（-9.85%），Fast DDS 链接 Flash 从
  17,248,198 B 降至 14,769,205 B（-14.37%），静态 RAM 减少 43,936 B；完整
  排除/保留清单见
  [`docs/VELAROS_FASTDDS_STATIC_PROFILE.md`](docs/VELAROS_FASTDDS_STATIC_PROFILE.md)；
- host ROSIDL 只负责可重复代码生成，最终 ELF 和构建元数据的宿主 ROS 污染
  检查已纳入一键验收；
- K1 EMAC0/RTL8211F polling 驱动已完成实板 ARP、双向 ICMP 和最大 1472-byte
  UDPv4 payload 验证；K1 实板 Fast DDS 跨设备互操作和网络长稳仍待完成；
- 完整边界、依赖、Demo 和 Go/No-Go 计划见项目描述。

K1 BSP 侧：

- 已完成 K1 启动链和关键硬件参数的第一轮盘点；
- 已建立 MUSE Pi Pro 板级目录和 K1 自定义芯片层；
- 已实现不写 UART IER 的轮询 `/dev/console`；
- 已接入 OpenSBI TIME 的 24 MHz 单次定时器；
- 已加入启动阶段标记和同步异常寄存器直出日志；
- 已加入 hart、DTB 和初始 CSR handoff 日志；
- 已从真实 U-Boot FDT 确认 PLIC context 1，并完成 GPIO source 58 的
  Pin 22 -> Pin 33 上升沿 claim/complete 实板验证；默认配置仍关闭 PLIC；
- 已提供可重复构建、ELF 验收和 U-Boot 上板包工具；
- 已提供静态 CI、串口原始抓取和异常自动符号化；
- 已完成双轮驱动审查和来源/许可证清单；
- 已使用 wrapper + U-Boot `go` 在 MUSE Pi Pro 实板启动 RV64 S-mode NSH；
  `bootelf -p` 不是当前固件上的可用路径。

当前构建通过只证明芯片层、板级层和 NSH 已完成编译链接闭环，不代表 openvela
已经在 K1 实板上启动。

## 第一阶段目标

第一阶段只追求一条可重复验证的最小链路：

```text
BootROM -> FSBL -> OpenSBI -> U-Boot -> openvela/NuttX (S-mode)
                                                |
                                                +-> UART banner -> NSH
```

验收条件：

1. U-Boot 能从 SD 卡加载 openvela/NuttX 镜像和 MUSE Pi Pro DTB；
2. payload 在 S-mode 运行，保留 OpenSBI 服务；
3. 串口输出 NuttX 启动日志；
4. 进入交互式 NSH；
5. 构建、写卡和启动步骤可由文档复现。

## 目录结构

```text
board/k1/muse_pi_pro/       MUSE Pi Pro 板级支持包
chip/k1/                    K1 RISC-V 自定义芯片层
docs/K1_BOOT_INVENTORY.md   启动资料、硬件参数、风险和验证清单
docs/K1_MUSE_PI_PRO_OFFICIAL_HARDWARE.md 官方硬件接口、Type-C、UART 和首板操作
docs/K1_UBOOT_BRINGUP.md     U-Boot 首启、故障定位和恢复手册
docs/K1_PLIC_DESIGN.md       PLIC context 证据、寄存器和启用门槛
docs/K1_DRIVER_REVIEW.md      59 Pattern 双轮审查与修复记录
docs/K1_DELIVERY_CHECKLIST.md 无板、上板和提交前检查清单
docs/CONTEST_MILESTONE_20260816.md 本次比赛里程碑的构建、验收与边界证据
docs/K1_HOST_BUILD_REGRESSION_20260814.md 十一个 K1 profile 的主机构建回归证据
docs/K1_WATCHDOG_BRINGUP.md   K1 watchdog 寄存器依据、安全 smoke 与实板步骤
docs/VELAROS_VERSION_MATRIX.md ROS 2 / Fast DDS 版本与 ABI 锁定
docs/VELAROS_ROS2_MINIMAL_CLOSURE.md ROS 2 分阶段目标依赖闭包
docs/VELAROS_ROS2_SOURCE_AND_LICENSES.md ROS 2 源码与许可证
docs/VELAROS_OPENVELA_INTEGRATION.md ROS 2 与 openVela 能力复用边界
docs/VELAROS_COMMUNICATION_PROFILE.md 精简高级通信保留/裁剪与发布配置
docs/VELAROS_FASTDDS_STATIC_PROFILE.md Fast DDS 产品静态 profile、体积和验收
docs/VELAROS_BUFFER_BACKEND.md openVela 固定池、uORB descriptor 与 ROSIDL backend
docs/VELAROS_HANDOFF.md        VelaROS/K1 当前状态、复现入口、边界和下一阶段交接
docs/VELAROS_RCLCPP_STATIC_PROFILE.md 静态 rclcpp RAII API、裁剪边界和验收
docs/VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md 静态 C++ Action 五通道、资源边界和验收
docs/VELAROS_ROBOT_PRODUCT_PROFILE.md 移动机器人 Topic/Action、uORB 边界和产品验收
docs/VELAROS_SIZE_BASELINE.md ELF/Map 体积基线与优化优先级
tools/build_k1.sh            可重复构建入口
tools/build_velaros_dds_sim.sh DDS simulator 干净环境构建
tools/build_velaros_release_sim.sh VelaROS 精简通信发布构建
tools/check_velaros_release_config.sh 发布能力存在性和诊断排除检查
tools/check_velaros_fastdds_profile.sh Fast DDS 静态 profile 编译图门禁
tools/restore_velaros_dds_sources.sh DDS 精确源码恢复与补丁应用
tools/restore_velaros_ros2_sources.sh ROS 2 精确源码恢复与校验
tools/generate_velaros_ros2_interfaces.sh ROSIDL 生成源码可重复导出
tools/generate_velaros_action.sh 开发 Fibonacci/产品 MoveRelative Action 生成与校验
tools/check_velaros_dds_sim.py rcl/RMW、DDS 3 发 3 收和资源回收一键验收
tools/check_velaros_ros2_host.py simulator 与 ROS 2 Lyrical 双向一键验收
tools/ci_k1.sh               静态/完整一键回归
tools/check_k1_elf.sh        ELF、Kconfig 和 UART 安全验收
tools/package_k1_bringup.sh  U-Boot 上板包生成器
tools/capture_k1_serial.sh    原始串口日志与元数据采集
tools/decode_k1_trap.sh       异常寄存器解析和 sepc 符号化
logs/                       AI Coding 日志
contest2026_287_Agenter.xml repo manifest
```

manifest 将芯片层和板级目录分别映射到：

```text
vendor/spacemit/chips/k1
vendor/spacemit/boards/k1/muse_pi_pro
```

获奖后的目标上游仓为
[`open-vela/vendor_SpacemiT`](https://github.com/open-vela/vendor_SpacemiT)。

## 已确认的移植约束

- K1 为 RV64，多核系统，量产启动链已经提供 OpenSBI；
- openvela/NuttX 应运行在 S-mode，不应重新接管 M-mode；
- 参考仓在实板上验证过 `0x11000000` 作为 payload staging 地址；
- 调试串口为 `0xd4017000`，寄存器间隔 4 字节，32 位 MMIO；
- 参考仓记录 K1 在 S-mode 写 UART IER 会导致总线挂死，不能直接假设通用
  16550 驱动安全；
- PLIC、定时器和 SMP 应优先复用 NuttX 已有的 SBI/S-mode 公共能力，再补充
  K1 SoC 层。

完整依据和待验证项见
[`docs/K1_BOOT_INVENTORY.md`](docs/K1_BOOT_INVENTORY.md)。

## 构建入口

推荐从比赛仓目录执行：

```bash
tools/build_k1.sh --clean --package --jobs 8
```

脚本会临时建立 manifest 对应的 vendor 映射、使用工作区内置的
`riscv-none-elf-gcc`、把 ccache 指向可写目录，并在退出时清理临时映射。

若 manifest linkfile 已映射到 vendor 目录，也可以从 openvela 工作区根目录直接
执行底层构建命令：

```bash
./build.sh vendor/spacemit/boards/k1/muse_pi_pro/configs/nsh --cmake -j8
```

2026-07-30 已完成干净构建验证，成功标志为：

```text
#### build completed successfully
```

ELF 位于 `cmake_out/muse_pi_pro_nsh/nuttx`，上板包位于
`out/k1-bringup/`。无板阶段使用 `tools/ci_k1.sh --jobs 8` 做完整回归；拿到板后
由 U-Boot 手工加载该 ELF，采集入口/handoff、异常寄存器、timer 和 NSH 日志。

DDS simulator 可使用一个命令完成启动、3 发 3 收和退出资源检查：

```bash
tools/check_velaros_dds_sim.py --build
```

## AI 辅助开发

本工作区已接入 openvela 官方 AI Skills，驱动开发阶段将使用：

- `nuttx-driver-development`：需求、设计、实现和验证；
- `driver-code-reviewer`：提交前驱动代码审查；
- `openvela-build`：配置、构建和错误定位；
- `contest-log-collector`：比赛 AI Coding 日志归集。

日志工具只写入本地 `logs/`，不会自动 commit 或 push。

## 参考项目

启动盘点使用了本机参考仓
`/home/sw/Dev/musepi-rvv-os-reference`（上游
[`KaranocaVe/musepi-rvv-os`](https://github.com/KaranocaVe/musepi-rvv-os)）。
该仓以 MIT 为主并包含独立许可的第三方 U-Boot/OpenSBI 代码。本项目当前只引用
其文档化的实板参数和验证结论，没有复制其源代码。
