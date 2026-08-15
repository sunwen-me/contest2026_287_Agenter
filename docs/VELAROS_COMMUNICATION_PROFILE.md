# VelaROS 精简高级通信剖面

> 移动机器人产品级 Topic/Action、uORB 控制边界与验收见
> [VELAROS_ROBOT_PRODUCT_PROFILE.md](VELAROS_ROBOT_PRODUCT_PROFILE.md)。

版本：2026-08-02

## 定位

VelaROS 不是“把桌面 ROS 2 全包搬到 NuttX”，也不是只在 NuttX 上运行一个 DDS
示例。它保留能让 openVela 节点直接进入 ROS 2 graph 的标准通信语义，并把板内
实时数据、日志、配置和服务管理映射到 openVela 已有设施。

```text
Linux ROS 2 Lyrical
  Topic + Service + Action + QoS + Discovery
                |
          DDS / RTPS UDPv4
                |
 rcl -> rmw_fastrtps_cpp -> Fast DDS
                |
 bounded VelaROS executor (one caller-owned task)
       |             |             |
     uORB          KVDB          syslog/Binder
```

“高级”指标准 ROS 2 Topic、Service 与 Action、DDS 分布式发现、ROS graph、QoS 和跨设备
无 Agent 通信；“精简”指静态类型、白名单消息、有界 executor、固定资源上限和
openVela 原生能力复用，而不是减少到私有 UDP 协议。

Fast DDS 产品层的编译期能力边界、准确排除项和体积结果见
[`VELAROS_FASTDDS_STATIC_PROFILE.md`](VELAROS_FASTDDS_STATIC_PROFILE.md)。
静态 C++ Action API、容量模型和裁剪边界见
[`VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md`](VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md)。

## 发布闭包

| 类别 | 保留 | 实现边界 |
|---|---|---|
| Topic | Publisher/Subscription、wait/take | `rcl` + `rmw_fastrtps_cpp` 标准路径 |
| Service | Client/Service request-response | 标准 DDS Request/Reply；首个类型为 `std_srvs/SetBool` |
| Action | Goal/Result/Cancel service + Feedback/Status topic | 标准五通道；产品静态类型为 `velaros_interfaces/MoveRelative` |
| QoS | Reliable/Best Effort、Keep Last、Volatile 等基础策略 | 复用 RMW/Fast DDS，不另造策略层 |
| Discovery/Graph | participant、endpoint discovery、ROS graph cache | Fast DDS + `rmw_dds_common` |
| 调度 | subscription、timer、client、service、Action client/server | 一个可复用 wait set；调用方拥有 task 和优先级 |
| 板内实时数据 | uORB ↔ ROS Topic 白名单桥 | 不让板内数据无条件经 DDS 自发自收 |
| 板内大消息 | 固定共享缓冲池 + uORB/ROSIDL descriptor | 核心链路已在 simulator 验证；真实产品类型、K1 DMA/cache 与 AMP 仍待接入 |
| 配置 | KVDB | Domain、participant、bridge、heartbeat；不移植 YAML 参数文件栈 |
| 日志 | NuttX syslog | 固定 384-byte 缓冲；无日志线程和第二套后端 |
| 管理 | Binder/service manager | 单 task `poll()` 控制面；不使用 ROS launch |
| 传输 | UDPv4、组播发现、确定性 unicast peers | NuttX socket + poll；不依赖 Linux epoll；产品不编译 Fast DDS 原生 SHM/DataSharing |

当前 `std_srvs/SetBool` 生成闭包为 15 个 C/C++ 文件。目标端删除
ServiceEventInfo、`SetBool_Event`、service introspection 和 type-description
graph，只保留请求、响应、Fast-CDR C/C++ 序列化与准确的 Lyrical TypeHash。
禁用 introspection 返回 `RCL_RET_OK`，尝试启用则明确返回
`RCL_RET_UNSUPPORTED`。

Action 生成目录同时支持开发回归用 Fibonacci 与产品用 MoveRelative；编译白名单
由 Kconfig 再收紧。开发配置保留 Fibonacci 并将序列限制为 32 项，发布配置关闭
`CONFIG_VELAROS_ACTION_FIBONACCI`，不编译其 message functions、Fast RTPS
type support 或 C++ traits，只链接固定布局 `MoveRelative`。两者均使用标准
`SendGoal`、`GetResult`、`CancelGoal`、`Feedback` 和 `Status` 通道，Goal 槽默认
上限为 2，达到上限时拒绝新 Goal，不扩容缓存。

## ROS-only 明确裁剪

发布固件中的 ROS 2/DDS 产品闭包不包含：

- ROS CLI、rclpy、Python、colcon、ament、目标端 rosidl 生成器；
- DDS HelloWorld、VelaROS smoke/互通演示和迁移诊断命令；
- rosout、LTTng tracing、Fast DDS Statistics、内部开发日志和旧日志宏；
- service event introspection、Type Description service、运行时类型插件；
- YAML 参数文件、ROS launch、pluginlib、ament resource index、动态库加载；
- Lifecycle、Composition、rosbag2、Security、Fast DDS 原生 SHM transport 和
  DataSharing 实现。

LVGL、音视频、QuickJS、curl、mbedTLS、FreeType、JPEG/PNG、libuv、UnQLite、
TCP/DNS、gtest/ostest、KASAN、allsyms、回溯和调试符号均属于 openVela 平台
基线，发布配置继续保留。Fast DDS 不调用运行时 IDL 外部预处理和 `popen()`，并不
意味着关闭 openVela 的 `CONFIG_SYSTEM_POPEN`；VelaROS DDS v0.1 只使用 UDPv4，
也不意味着删除平台 TCP/DNS 能力。

Action 保留标准 wire 语义，但裁掉运行时加载任意 Action 类型、ServiceEvent/类型
描述、自增长 Goal/Result 缓存、每 Goal 一个线程以及通用动态多线程 executor。
新增 Action 必须在 host 侧按锁定 IDL 生成、审计资源上限后静态进入白名单。

## 两种配置

| 配置 | 用途 | 是否包含测试命令 |
|---|---|---|
| `goldfish-arm64-v8a-ap-fastdds` | 通用 Fast DDS 开发和回归，静态 profile 默认关闭 | 是：DDS、RMW、rcl、Topic/Service/Action、融合 smoke |
| `goldfish-arm64-v8a-ap-velaros` | 完整 openVela 基线上的 ROS-only 产品闭包，静态 profile 开启 | 否：保留 MoveRelative 产品节点，移除 Fibonacci 与 ROS/DDS 测试命令 |

两种配置都使用 `-O3` 并保留相同 openVela 基线。发布配置关闭 Fast DDS
Statistics/`INTERNAL_DEBUG` 和 ROS/DDS 测试入口，并额外启用 Fast DDS VelaROS
静态 profile。profile 的 before/after Map 使用同一发布 defconfig 单独采集，
不会把测试入口差异或平台 UI、媒体、网络模块误算成 DDS 优化。开发配置可通过
`CONFIG_FASTDDS_DEVELOPER_DIAGNOSTICS=y` 临时恢复内部诊断，诊断能力不会成为
产品 ABI 或运行时依赖。

## 已验证和未验证

开发配置已在 goldfish simulator 与本机 ROS 2 Lyrical 完成：

- guest → host `std_msgs/String`：3/3；
- host → guest `std_msgs/String`：3/3；
- host → guest `std_srvs/SetBool`：2/2 请求响应；
- host Action client → guest server：SUCCEEDED + CANCELED；
- guest Action client → host server：SUCCEEDED + CANCELED；
- Action 两向均收到 Feedback/Status，且 Goal/Result/Cancel 三个 service 生效；
- SetBool 实际写入 openVela KVDB bridge 状态；
- host `geometry_msgs/Twist` → guest uORB，产品 MoveRelative 成功 + 取消；
- 所有目标任务退出后 `ps` 无残留；
- profile-enabled 完整回归 ELF SHA256：
  `5338a437b6fc0a3bbe9482c01d188f30ebede044e230790eded0016217179895`。

ROS-only 发布配置保留标准 Action 库和 `MoveRelative` 产品类型，但不包含
Fibonacci 或互通示例；静态审计
要求 `FASTDDS_STATISTICS` 关闭、Statistics backend 对象数为 0，并检查 36 个
静态 profile 后端未进入编译图，同时验证上述 openVela 平台能力全部仍在。发布
当前 Release ELF SHA256 为
`82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`，且已完成
产品 `Twist`/`MoveRelative` 运行验收。Map 绝对值、同 defconfig 历史差值和验收见
[`VELAROS_FASTDDS_STATIC_PROFILE.md`](VELAROS_FASTDDS_STATIC_PROFILE.md)。K1 实板
Ethernet、时延、丢包、CPU、峰值堆和长稳数据仍未验证，不能用 simulator 结果
替代。

## 后续通信增强顺序

1. 把 uORB 温度/控制验证类型换成 Demo 所需的语义化消息白名单；
2. 固定 openVela 缓冲池、有界 uORB descriptor 和静态 `VelaBufferBackend` 已完成；
   下一步把真实相机/点云白名单类型接入，并在 K1 上实现 DMA/cache coherency；
3. 测量不同 QoS 下的延迟、丢包、history 深度和峰值堆，形成资源预算；
4. 静态 `rclcpp` Topic/Service/Timer/Action RAII 已完成；按真实需求增加下一批
   机器人 Action 白名单，不移植上游线程池；
5. K1 Ethernet 可用后复跑 discovery、Topic、Service、Action、QoS 和 30 分钟长稳。
