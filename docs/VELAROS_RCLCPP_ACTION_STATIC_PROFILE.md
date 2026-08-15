# VelaROS 静态 rclcpp Action Profile

版本：2026-08-02

## 结论

VelaROS 已在现有 `rcl_action 10.4.4` 五通道实现上提供精简 C++ RAII API：
`rclcpp_action::Client`、`Server`、Client/Server GoalHandle，以及
`create_client()`、`create_server()`。它以 ROS 2 Lyrical 的 Action 语义为基线，
保持标准 DDS wire 互操作，但不是完整上游 `librclcpp_action` ABI。

这不是只包装一个私有 DDS 示例。每个 Action 仍使用 ROS 2 标准通道：

```text
<name>/_action/send_goal    Service
<name>/_action/cancel_goal  Service
<name>/_action/get_result   Service
<name>/_action/feedback     Topic
<name>/_action/status       Topic
```

## 已保留能力

| 能力 | VelaROS 实现 |
|---|---|
| Goal | 接受/拒绝、16-byte UUID、固定 Goal 槽 |
| Result | `SUCCEEDED`、`ABORTED`、`CANCELED`、延迟结果请求 |
| Cancel | 标准 CancelGoal 请求/响应和 `CANCELING` 状态迁移 |
| Feedback/Status | 标准 Topic 发布和 client 回调 |
| Discovery/QoS | `rcl_action -> rmw_fastrtps_cpp -> Fast DDS` |
| 生命周期 | C++ RAII 关闭 client/server、clock 和底层 DDS 实体 |
| 调度 | 与 Topic/Service/Timer 共用 VelaROS caller-owned wait set |

Client API 保留 `action_server_is_ready()`、`async_send_goal()`、
`async_cancel_goal()` 以及 Goal/Feedback/Result/Cancel 函数指针回调。Server API
保留 Goal/Cancel/Execute 回调和 `publish_feedback()`、`succeed()`、`abort()`、
`canceled()`。

## openVela 资源模型

- executor 不创建 Action worker；一次 `spin_once()` 只在调用者 task 中完成有界
  wire 分派，并给活跃 Goal 一次执行机会；NuttX task 优先级仍由产品决定。
- server 默认最多 `CONFIG_VELAROS_ACTION_MAX_GOALS=2` 个 Goal；满载时拒绝新
  Goal，不扩容。
- 当前 client 同时只允许一个活动 Goal；每个 Goal 最多保存一个等待中的 Result
  request。
- 开发配置中的 Fibonacci Result/Feedback sequence 上限由
  `CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY=32` 固定；越界立即失败。
- 产品配置关闭 `CONFIG_VELAROS_ACTION_FIBONACCI`，不编译 Fibonacci
  message functions、Fast RTPS type support 或 C++ traits。
- 回调使用函数指针与显式 `user_data`；没有 `std::function`、future/promise、
  每 Goal 线程或通用动态多线程 executor。
- Goal/Result 数据和 executor 槽容量在构建期可审计。DDS 实体初始化仍按标准
  `rcl_action` 生命周期申请，运行中的 Goal 处理不做无界扩容。

Action server 的进度钩子同时在 wire entity ready 和 executor timeout 后执行，
因此长计算应被拆成有界步骤，每次回调完成一个小步并返回。这样不会把
`spin_once()` 变成隐藏线程池，也不会阻塞同一 wait set 上的 Topic/Service。

## 静态类型白名单

开发回归白名单包含 `velaros::action::Fibonacci`，产品白名单包含固定布局的
`velaros::action::MoveRelative`。traits 在编译期绑定 Goal、Result、Feedback、
五通道生成类型、type-support 和有界 Result copy；release 只链接
`MoveRelative`。新增机器人 Action 类型时，需要：

1. 用锁定的 Lyrical ROSIDL 主机工具生成并裁剪五通道 C/Fast RTPS 源码；
2. 保留准确 TypeHash 和 CDR wire 类型，加入恢复/生成 `--check`；
3. 增加一个受审计的 traits 白名单条目，明确序列和 Goal 容量；
4. 增加 Linux ROS 2 对端成功、取消、反馈、结果和回收验收。

运行时加载任意 Action 类型被明确排除，不能通过 type name 或共享库绕过白名单。

## 有意裁剪

- 完整上游 `rclcpp_action` ABI、shared goal ownership 和任意 Action C++ 模板生态；
- future/promise、`std::function`、callback group 和动态多线程 executor；
- 每个 Action/Goal 独立线程，以及由库隐藏创建的 task；
- 无界 Goal/Result/Feedback 缓存和运行时容量增长；
- 运行时类型加载、pluginlib、Type Description graph 与 ServiceEvent；
- Action introspection、CLI/调试事件和 tracing。

裁剪项不影响选定白名单 Action 的标准 Goal/Result/Cancel/Feedback/Status wire
语义。它们主要服务桌面通用性、动态组合或调试，不是固定功能机器人节点完成高级
通信的必要条件。

## 错误状态降级修复

VelaROS 静态 Fast RTPS callbacks 不包含可选 ROS introspection type-support。
`rmw_fastrtps_shared_cpp::TypeSupport` 会尝试注册 DDS TypeObject，然后正确降级使用
静态 CDR callbacks。原实现返回前遗留“找不到 introspection”的非致命错误；创建
Action 的 14 个 DDS 实体时诊断会反复嵌套并触发 rcutils 768-byte 截断警告。

openVela 补丁现在只在这个可选降级边界调用 `rcutils_reset_error()`。真实创建、
序列化或通信错误仍原样返回；没有关闭 rcutils 警告，也没有扩大错误缓冲区。

## 验收

开发配置额外启用：

```text
CONFIG_VELAROS_RCLCPP_ACTION_SMOKE=y
CONFIG_VELAROS_ACTION_FIBONACCI=y
CONFIG_VELAROS_ACTION_MAX_GOALS=2
CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY=32
```

`velaros_rclcpp_action_smoke` 在同一个 caller-owned executor 内完成两个真实 Action
生命周期：order=6 正常成功；order=20 在收到 Feedback 后发起 Cancel 并返回
`CANCELED`。goldfish-arm64 实测结果：

- Goal callback 2 次；Feedback callback 7 次；Cancel callback 1 次；
- `SUCCEEDED` Result：PASS；`CANCELED` Result：PASS；
- 五通道 discovery/通信、RAII 回收和 `ps` 无残留：PASS；
- `rcutils` 截断或错误覆盖警告：0；
- 开发 ELF SHA256：
  `ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7`。

该结果证明静态 C++ Action API 和目标端五通道链路；C Action API 已完成 Linux
ROS 2 Lyrical 双向 client/server 成功与取消互操作。产品 `MoveRelative` 也已经
与 Linux ROS 2 Lyrical 在开发和 Release 配置中完成成功、反馈、取消、结果及
目标端正常退出验收；Release 编译图和 ELF 均不含 Fibonacci。

K1 Ethernet、实板调度抖动、峰值堆、断网恢复和 30 分钟长稳仍需拿到板卡后验收。
