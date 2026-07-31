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
- 当前裁剪明确关闭 rosout、全局参数、非空命令行参数、YAML 参数解析和
  Type Description service；这些属于后续功能增量，不应把当前结果描述成完整
  `rcl` 或完整 ROS 2 移植；
- host ROSIDL 只负责可重复代码生成，最终 ELF 和构建元数据的宿主 ROS 污染
  检查已纳入一键验收；
- K1 Ethernet 驱动尚未开始，DDS 逻辑将先在 openvela simulator 验证；
- 完整边界、依赖、Demo 和 Go/No-Go 计划见项目描述。

K1 BSP 侧：

- 已完成 K1 启动链和关键硬件参数的第一轮盘点；
- 已建立 MUSE Pi Pro 板级目录和 K1 自定义芯片层；
- 已实现不写 UART IER 的轮询 `/dev/console`；
- 已接入 OpenSBI TIME 的 24 MHz 单次定时器；
- 已加入启动阶段标记和同步异常寄存器直出日志；
- 已加入 hart、DTB 和初始 CSR handoff 日志；
- 已从参考 DTS 确认 PLIC context 1，并提供默认关闭的实现骨架；
- 已提供可重复构建、ELF 验收和 U-Boot 上板包工具；
- 已提供静态 CI、串口原始抓取和异常自动符号化；
- 已完成双轮驱动审查和来源/许可证清单；
- 已产出 RV64 S-mode NSH ELF，尚待实板启动验证。

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
docs/K1_UBOOT_BRINGUP.md     U-Boot 首启、故障定位和恢复手册
docs/K1_PLIC_DESIGN.md       PLIC context 证据、寄存器和启用门槛
docs/K1_DRIVER_REVIEW.md      59 Pattern 双轮审查与修复记录
docs/K1_DELIVERY_CHECKLIST.md 无板、上板和提交前检查清单
docs/VELAROS_VERSION_MATRIX.md ROS 2 / Fast DDS 版本与 ABI 锁定
docs/VELAROS_ROS2_MINIMAL_CLOSURE.md ROS 2 分阶段目标依赖闭包
docs/VELAROS_ROS2_SOURCE_AND_LICENSES.md ROS 2 源码与许可证
tools/build_k1.sh            可重复构建入口
tools/build_velaros_dds_sim.sh DDS simulator 干净环境构建
tools/restore_velaros_dds_sources.sh DDS 精确源码恢复与补丁应用
tools/restore_velaros_ros2_sources.sh ROS 2 精确源码恢复与校验
tools/generate_velaros_ros2_interfaces.sh ROSIDL 生成源码可重复导出
tools/check_velaros_dds_sim.py rcl/RMW、DDS 3 发 3 收和资源回收一键验收
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
