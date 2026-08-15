# VelaROS 移动机器人产品通信剖面

## 1. 定位

这一剖面把当前工程从“能运行 ROS 2/DDS 示例”推进到可复用的机器人产品节点。首个产品边界面向差速或平面移动底盘，保留完整 ROS 2 主机可识别的 Topic 与 Action 语义，同时把目标端实现限制在固定类型、固定容量和单执行线程内。

它不是 micro-ROS，也不是把 Linux 版 rclcpp 原样搬入 NuttX：主机仍使用标准 ROS 2 Lyrical `rclpy/rclcpp`，openVela 侧使用 VelaROS 静态 rclcpp 子集、Fast DDS UDPv4 和 openVela uORB。

## 2. 对外 ROS 2 接口

### 2.1 `/cmd_vel`

- 类型：标准 `geometry_msgs/msg/Twist`
- 目标端实际生成闭包：`geometry_msgs/Vector3`、`geometry_msgs/Twist`
- QoS：目标端 best-effort、volatile、depth 4
- 当前移动底盘使用字段：`linear.x`、`angular.z`
- 输入限制：`|linear.x| <= 2.0 m/s`，`|angular.z| <= 4.0 rad/s`，所有分量必须为有限数
- 用途：短周期遥控、上层控制器速度输出和安全停车

没有把整个 `geometry_msgs` 包编进固件；宿主机生成器只导出上述两个类型的 C/C++ 与 Fast RTPS 源码。

### 2.2 `/velaros/move_relative`

- 类型：`velaros_interfaces/action/MoveRelative`
- 用途：带反馈、结果和取消语义的相对位移
- Action 五通道：SendGoal、GetResult、CancelGoal、Feedback、Status

Goal：

```text
float32 distance_m
float32 yaw_rad
float32 max_linear_speed_mps
float32 max_angular_speed_rps
```

Result：

```text
int8 status
float32 traveled_m
float32 turned_rad
```

Feedback：

```text
float32 traveled_m
float32 turned_rad
float32 remaining_distance_m
float32 remaining_yaw_rad
```

所有字段均为固定线长标量。目标端不需要 string、无界 sequence、任意 Action 类型加载或结果堆缓存。

## 3. openVela 深度融合

Topic 和 Action 不直接各自操作设备。两条 ROS 入口经过同一个静态转换器，发布到持久 uORB Topic：

```text
ROS 2 /cmd_vel -----------+
                          +--> velaros_motion_command --> 底盘驱动/控制任务
MoveRelative Action ------+
```

`velaros_motion_command` 包含单调时钟时间戳、线速度、角速度、250 ms 失效时间和单调递增序号。持久 uORB 状态允许晚启动的驱动读取最新命令；真实 K1 底盘驱动可以消费同一 Topic，不需要感知 DDS、rcl 或 Action 协议。

控制权规则是确定的：执行 `MoveRelative` 时 Action 持有控制权，外部 `/cmd_vel` 不会覆盖运动过程；成功、取消或执行错误都会先向 uORB 发布零速度，再释放控制权。当前产品节点只允许一个物理运动 Action 并发，协议层仍保留配置化的固定 Goal 槽用于生命周期管理。

## 4. 静态裁剪

目标固件保留了高级通信能力，但删除以下运行时通用性成本：

- 不生成 `geometry_msgs` 的 Pose、Transform、Wrench 等未使用类型；
- release 不编译开发回归用 Fibonacci functions、Fast RTPS type support 或 traits；
- 不允许运行时加载任意 Topic/Action 类型；
- 删除 Action Service Event 与目标端类型描述反射；
- 不使用 future、shared goal ownership、每 Goal 线程或动态多线程 executor；
- 目标端 Result/Feedback 没有无界容器；
- Topic 与 Action 由同一个 caller-owned wait set 驱动；
- 不复制一套 ROS 参数、日志或设备总线，继续复用 openVela KVDB、syslog 和 uORB。

这类裁剪只作用于 VelaROS/ROS 2 闭包，不删除 LVGL、音频、curl、mbedTLS 等 openVela 平台能力。

## 5. 模拟器与 K1 的边界

在没有 K1 板卡和底盘硬件时，产品节点用 50 ms 固定控制周期积分命令，作为确定性的参考运动模型，用来验收 Goal、Feedback、Result、Cancel、控制权仲裁和 uORB 数据通路。它不冒充真实里程计。

拿到板卡后的替换点只有运动进度来源和底盘执行器：

1. K1 驱动/控制任务订阅 `velaros_motion_command`；
2. 轮速计或定位模块通过 uORB 输出真实里程计；
3. `MoveRelative` 的 traveled/turned 从参考积分切换为真实里程计差值；
4. ROS 2 接口、DDS 类型、主机节点和上层调用方式保持不变。

## 6. 可重复生成与验收

```bash
./tools/generate_velaros_std_msgs.sh --check
./tools/generate_velaros_action.sh --check
./tools/build_velaros_host_interfaces.sh --check
./tools/build_velaros_dds_sim.sh
./tools/check_velaros_ros2_host.py --robot-only
./tools/build_velaros_release_sim.sh
./tools/check_velaros_ros2_host.py \
  --robot-only \
  --output ../cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros
```

验收脚本在模拟器内使用
`velaros_robot_node --qemu-interop --cmd-vel 1 --goals 2`。其中
`--qemu-interop` 只选择 QEMU 用户网络所需的确定性单播和 UDP 端口重定向；
K1 部署不带该开关，使用板端网络剖面。这个选择已经进入精简
`rclcpp::ContextOptions`，不依赖目标任务恰好继承某组环境变量。

机器人专用验收要求：

- 本机 ROS 2 Lyrical 与目标端完成类型匹配；
- 一条 `Twist` 真正进入 openVela uORB；
- 一个 `MoveRelative` Goal 成功并收到反馈、Result；
- 第二个 Goal 在反馈后取消并返回 canceled；
- 目标端完成后正常退出，不残留产品节点任务。

## 7. 当前能力边界

这是首个可交付的移动底盘通信剖面，不等于已经包含 Nav2、TF 全闭包、地图、感知或任意机器人接口。后续接口应按产品需要逐个加入白名单，并继续遵守固定布局、静态容量、复用 openVela 基础设施和可测量资源预算四项约束。

## 8. 2026-08-02 验收结果

- 开发配置完整 ROS 2/DDS 回归：PASS；
- 开发配置产品 Topic/Action 跨机验收：PASS；
- Release 配置产品 Topic/Action 跨机验收：PASS；
- `/cmd_vel` 1 条、成功 Goal 1 个、取消 Goal 1 个、Feedback 12 条、
  uORB 发布 15 条，结束后产品节点任务无残留；
- Release 编译图中 Fibonacci functions/Fast RTPS type support 对象为 0，
  ELF 中 Fibonacci/`velaros_fibonacci` 符号为 0；
- 开发 ELF SHA256：
  `ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7`；
- Release ELF SHA256：
  `82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`；
- Release Map 装载 Flash：23,152,776 B（22.08 MiB）；静态 RAM：
  728,376 B（711.30 KiB）。

尺寸是包含完整 openVela 平台模块的产品固件绝对值。由于这一版本同时加入产品
节点、`geometry_msgs/Twist`、`MoveRelative` 和 uORB 数据通路，不能把它与旧
固件的总差值全部归因于删除 Fibonacci；Fibonacci 的裁剪效果以编译图和 ELF
符号确实不存在作为验收证据。
