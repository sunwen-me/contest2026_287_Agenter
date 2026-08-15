# VelaROS ROS-only 裁剪发布配置

该配置用于验证“保留完整 openVela 平台基线，只裁 ROS 2/DDS 开发闭包”的发布
策略。它与 `goldfish-arm64-v8a-ap-fastdds` 开发配置使用相同的 openVela 能力，
包括 LVGL、音视频、QuickJS、curl、mbedTLS、FreeType、JPEG/PNG、libuv、
UnQLite、TCP/DNS、调试符号、KASAN 和测试框架。它同时保留：

- ROS 2 Lyrical `rcl` / `rmw_fastrtps_cpp` / Fast DDS 数据链路；
- Topic、Client/Service、有界 Action、QoS、ROS Graph/Discovery、有界 executor，
  以及静态 `rclcpp` Topic/Service/Timer/Action RAII SDK；
- Fast DDS UDPv4 单播与组播能力；
- openVela syslog、KVDB、uORB 和 Binder 控制面融合；
- 标准 `std_srvs/SetBool` 运行时配置服务；
- 静态 `example_interfaces/Fibonacci` 五通道类型与 `rcl_action`；互通命令仅在
  开发配置中启用。

静态 rclcpp SDK 不包含完整上游 ABI、动态 executor、线程池、parameters、rosout
或组件加载；接口和 simulator 验收见
[`../../docs/VELAROS_RCLCPP_STATIC_PROFILE.md`](../../docs/VELAROS_RCLCPP_STATIC_PROFILE.md)
与
[`../../docs/VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md`](../../docs/VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md)。

发布配置启用 `CONFIG_FASTDDS_VELAROS_STATIC_PROFILE`，在源码级排除 TypeLookup
Service、Fast DDS 自带 DDS-RPC、Discovery Server/Client/Backup 数据库和静态
EDP，并把产品路径锁定为 SIMPLE discovery + UDPv4。ROS 2 Service 不依赖被删除
的 Fast DDS RPC API，仍通过标准 RMW Request/Reply 工作。详细裁剪项、保留边界、
Map 差值和互通结果见
[`../../docs/VELAROS_FASTDDS_STATIC_PROFILE.md`](../../docs/VELAROS_FASTDDS_STATIC_PROFILE.md)。

发布配置还关闭 Fast DDS Statistics、内部开发调试和旧日志宏，并且不编入 DDS
HelloWorld、VelaROS smoke 命令和 ROS Topic/Service/Action 互通演示。目标端 ROS
闭包也不引入 ROS CLI、rclpy/Python 消息支持、colcon、目标端 rosidl 生成器、
YAML 参数文件、rosout、service event introspection 或 tracing。

Fast DDS 的运行时 IDL 外部预处理在 NuttX 适配中明确返回不支持，因此 DDS 不依赖
`popen()`；但 openVela 自身的 `CONFIG_SYSTEM_POPEN` 仍然保留，供平台其他模块
使用。TCP/DNS 同样作为 openVela 平台能力保留，VelaROS v0.1 的 DDS 产品链路只
使用 UDPv4。KVDB 继续复用 openVela 的 UnQLite 后端和 `/data/persist.db`，不另造
ROS 配置后端。

构建：

```bash
tools/build_velaros_release_sim.sh --clean --jobs 8
```

当前 goldfish ELF SHA256、ROS-only Map 差值和运行验收结果见
[`../../docs/VELAROS_SIZE_BASELINE.md`](../../docs/VELAROS_SIZE_BASELINE.md)。

该配置是 goldfish 的 ROS-only 产品闭包和体积基线，不代替 K1 板级 defconfig。
K1 上只需把网络设备和 KVDB 持久化路径绑定到板级资源，不应借 ROS 裁剪名义删除
openVela 的平台能力。
