# VelaROS 静态 rclcpp RAII Profile

版本：2026-08-02

## 结论

VelaROS 已提供面向 openVela 的精简 C++ 客户端 API。它以 ROS 2 Lyrical
`rclcpp 32.0.0` 为源码和接口语义基线，但不把完整桌面 `rclcpp` 搬进固件；实现
直接拥有 `rcl 10.4.4` 实体，并复用现有 VelaROS 有界 C executor。

这套 profile 是 `rclcpp/rclcpp.hpp` 的静态、源码级子集，不与完整上游
`librclcpp` 保持 ABI 兼容。它保留机器人产品节点最常用的 Topic、Service 和
Timer 编程能力，同时把容量、线程和消息类型都变成构建期可审计项。

## 已支持 API

| 类型 | 已支持能力 | 资源模型 |
|---|---|---|
| `Context` | init、Domain ID、shutdown、fini | 一个调用者拥有，无后台线程 |
| `Node` | 名称/命名空间、实体工厂、RAII fini | 默认关闭 rosout 和全局参数 |
| `QoS` | Keep Last depth、Reliable/Best Effort、Volatile/Transient Local | 直接映射 `rmw_qos_profile_t` |
| `Publisher<T>` | 静态类型发布、匹配计数 | 预生成 Fast RTPS C++ type-support |
| `Subscription<T>` | 固定函数指针加 `user_data` 回调 | 一个实体一个复用消息对象 |
| `Client<T>` | 请求发送、服务可用性、响应回调 | executor 中显式 client 槽 |
| `Service<T>` | 请求/响应回调 | executor 中显式 service 槽 |
| `WallTimer` | steady clock、cancel、reset | 不创建 timer 线程 |
| `SingleThreadedExecutor` | subscription/timer/client/service/Action | 调用者线程、单 wait set、固定容量 |

API 入口位于：

```text
middleware/velaros/include/rclcpp/rclcpp.hpp
middleware/velaros/velaros_rclcpp.cpp
```

## 示例

```cpp
rclcpp::Context context;
rclcpp::Node node(context, "controller", "/velaros");

auto publisher = node.create_publisher<std_msgs::msg::String>(
  "/velaros/status", rclcpp::QoS(4).reliable());

auto subscription = node.create_subscription<std_msgs::msg::String>(
  "/velaros/command", rclcpp::QoS(4).reliable(), on_command, &state);

auto service = node.create_service<std_srvs::srv::SetBool>(
  "/velaros/set_enabled", on_set_enabled, &state);
auto client = node.create_client<std_srvs::srv::SetBool>(
  "/velaros/set_enabled", on_response, &state);

rclcpp::ExecutorOptions limits;
limits.subscriptions = 1;
limits.timers = 1;
limits.clients = 1;
limits.services = 1;
rclcpp::executors::SingleThreadedExecutor executor(context, limits);
executor.add_subscription(*subscription);
executor.add_service(*service);
executor.add_client(*client);
executor.spin_once(std::chrono::milliseconds(100));
```

回调使用编译期函数类型和显式 `user_data`，没有引入 `std::function`、运行时插件
注册表或每实体线程。实体必须比 executor 活得久；应用按 C++ 逆序析构先销毁
executor，再销毁实体、Node 和 Context。

## 与 openVela 的融合点

1. executor 只包装现有 `velaros_executor_t`，使用一个 `rcl_wait_set`，不重复移植
   上游动态 executor 或线程池；调度仍由 openVela/NuttX task 优先级控制。
2. 本地高频板内数据仍优先走 uORB；C++ API 用于跨设备 ROS 2 Topic、Service 和
   DDS QoS，不替代 openVela 内部总线。
3. 日志、持久配置和服务管理继续复用 syslog、KVDB 和 Binder；没有引入 spdlog、
   YAML 参数栈或 ROS launch。
4. 消息和 Service 类型由锁定的 Lyrical ROSIDL 主机工具预生成，目标端全静态
   链接，不依赖 `/opt/ros` 动态库、`dlopen()` 或目标端 Python。
5. executor 的 subscription、timer、client、service、Action client/server 容量由产品节点显式给出；
   超出容量立即返回错误，不在运行中扩容。

## 静态类型与 Service hash 修复

当前 C++ 白名单包含 `std_msgs/String`、`std_msgs/Float64`、
`geometry_msgs/Vector3/Twist`、`std_srvs/SetBool` 和产品
`velaros_interfaces/MoveRelative`。生成器保留 Request、Response、CDR 回调和 ROS 2 TypeHash，
裁掉 ServiceEvent 与运行时 Type Description graph。

初次 C++ Service 验收发现，裁剪器把 C++ type-support handle 的
`get_type_hash_func` 一并置空；`rmw_fastrtps_cpp` 在创建 Service DataReader/
DataWriter QoS 时必须调用该函数，因而触发空函数指针异常。现在 C/C++ handle
共同引用锁定 `std_srvs 5.9.2` 的静态 SetBool/Request/Response hash，只删除
description/source 回调。这样既保留 wire compatibility 和 QoS 类型一致性检查，
也不重新引入目标端类型描述图。

## 明确未包含

- 完整上游 `rclcpp` ABI、components 和 class loader；
- 动态多线程 executor、callback group、每实体或每 Goal 线程；
- parameters/parameter events、YAML、rosout 和远程日志配置；
- intra-process manager、loaned message 和面向 openVela 的零拷贝 DDS 通路；
- GenericPublisher/GenericSubscription、运行时加载任意消息或 Action 类型；
- 完整上游 `rclcpp_action` ABI、future/shared goal ownership 和任意 Action 动态
  类型；VelaROS 已提供五通道、有界、caller-thread 的静态 C++ RAII 子集，详细
  边界见
  [`VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md`](VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md)。

这些是首版 profile 的范围，不等于 ROS 2 协议不支持。Topic、SetBool Service 和
有界 Fibonacci Action 的标准 DDS wire 行为已经由现有 C/rcl/RMW 链路验证。

## 构建与验收

开发配置：

```text
CONFIG_VELAROS_RCLCPP=y
CONFIG_VELAROS_RCLCPP_SMOKE=y
```

ROS-only 发布配置保留 `CONFIG_VELAROS_RCLCPP=y`，关闭 smoke 命令。验证：

```bash
tools/restore_velaros_ros2_sources.sh --check
tools/generate_velaros_std_msgs.sh --check
tools/generate_velaros_std_srvs.sh --check
tools/build_velaros_dds_sim.sh
tools/check_velaros_dds_sim.py --timeout 150
tools/build_velaros_release_sim.sh --jobs 12
```

goldfish-arm64 干净验收结果：

- C++ Context/Node/Publisher/Subscription/Service/Client/Timer 初始化：PASS；
- Topic 3 次发布、3 次 subscription callback：PASS；
- SetBool Service 1 次请求、1 次服务回调、1 次响应回调：PASS；
- Fibonacci Action 成功与取消各 1 次、Feedback 7 次、五通道回收：PASS；
- 一个 caller-owned executor 完成四类实体分派：PASS；
- executor、实体、Node、participant 和 Context 回收：PASS；
- `ps` 无 Publisher、Subscriber、DDSHelloWorldExample 或 `velarosd` 残留：PASS；
- host ROS 环境污染检查：PASS；
- 开发 ELF SHA256：
  `ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7`。
- ROS-only 发布配置构建和静态 profile 门禁：PASS；发布 ELF SHA256：
  `82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`。
- Release `Twist` -> uORB、`MoveRelative` 成功/取消和任务回收：PASS；
  Fibonacci 编译对象与 ELF 符号为 0。

上述结论来自 openVela goldfish simulator。K1 Ethernet、实板时延/CPU/峰值堆、
30 分钟长稳和异常掉线恢复仍需拿到板卡后重新验收。
