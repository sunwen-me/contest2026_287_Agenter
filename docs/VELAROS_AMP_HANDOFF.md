# VelaROS Linux/openvela AMP 无板原型交接

版本：2026-08-08

## 结论

当前已完成一个不依赖 K1 实板的 AMP 软件原型：固定小端二进制协议、CRC、单调序号、
心跳、命令超时、远端急停、openvela 侧状态机、持久 `velaros_motion_command` uORB
控制边界和 `velaros_amp_status` 状态边界均已实现。Linux 侧提供同一协议的 POSIX
socketpair 自检网关。

该原型的 transport boundary 是明确的：当前 socketpair 只用于无板确定性测试，拿到
K1 后替换为 RPMsg/OpenAMP/virtio 传输时，协议帧和状态机不变。当前不能声称 K1 的
hart、vring、跨核中断、cache、内存隔离或真实 RPMsg 已通过。

## AMP 角色分工

```text
Linux 富功能域
  ROS 2 Lyrical / Fast DDS / 相机 / NPU / 高层规划 / 存储
                         |
                 AMP gateway boundary
                         |
              fixed frame + CRC + sequence
                         |
                  RPMsg/OpenAMP (future)
                         |
openvela 实时域
  uORB -> LIO/Nav -> motion command -> PWM/CAN/GPIO/UART
```

独立运行模式仍然保留：openvela/VelaROS 可以直接通过 Ethernet 参加 ROS 2 graph；
AMP 模式下，如果 Linux 持有物理网卡，则由 Linux ROS 2 网关转发有界控制目标和状态。
没有实现 Fast DDS over RPMsg custom transport 前，不能把 RPMsg 描述成 DDS 传输。

## 固定协议

协议头和 payload 总长度固定为 92 B，使用 little-endian：

| 字段 | 大小 | 说明 |
|---|---:|---|
| magic | 4 B | `0x56414d50`，即 `VAMP` |
| version/type | 2 + 2 B | 协议版本和消息类型 |
| sequence | 4 B | 非零、严格单调递增 |
| timestamp | 8 B | 单调时间戳，单位 us |
| payload length/flags | 2 + 2 B | 固定 payload 上限 64 B |
| payload | 64 B | heartbeat、motion、status 或 emergency stop |
| CRC32 | 4 B | 对前 88 B 计算 |

当前消息类型：

- `HEARTBEAT`：周期、能力位；接收后刷新 peer 状态；
- `MOTION_COMMAND`：`linear_x_mps`、`angular_z_rps`、timeout 和 flags；
- `STATUS`：state、fault、收发序号和时间戳；
- `EMERGENCY_STOP`：远端急停原因，立即生成零速命令。

安全规则固定在 endpoint 状态机中：坏帧、CRC 错误或序号回退进入 fault；命令超过
timeout 默认 250 ms 自动输出零速；peer 超过 heartbeat timeout 默认 1000 ms 自动
停车；远端急停进入 `EMERGENCY_STOP`，不会由普通心跳自动清除。

## openvela 侧实现

代码位置：

```text
middleware/velaros/include/velaros/amp_protocol.h
middleware/velaros/velaros_amp_protocol.c
middleware/velaros/include/velaros/amp_service.h
middleware/velaros/velaros_amp_service.c
middleware/velaros/velaros_amp_smoke.c
```

`velaros_amp_service` 不拥有传输线程，也不创建隐藏 executor。传输层将收到的完整帧
交给 `velaros_amp_service_handle_frame()`；服务调用固定状态机，随后：

```text
MOTION_COMMAND / EMERGENCY_STOP
        -> velaros_motion_command uORB
        -> real future bottom driver

endpoint state/fault
        -> velaros_amp_status uORB
        -> Linux gateway / diagnostics
```

命令和状态均使用持久 uORB advertisement，晚启动的消费者可以读取最近状态。LIO/Nav
本身仍使用既有的固定 POD、pose/map snapshot 和 `velaros_motion_command` 边界，不把
点云序列化成 AMP 消息。

## 无板验证

Linux 协议网关自检：

```bash
python3 tools/velaros_amp_gateway.py --self-test
```

输出：

```text
VelaROS AMP gateway self-test: PASS command=1 heartbeat=1 status=1 timeout_stop=1
```

开发版 NuttX smoke 通过 Kconfig/CMake 构建：

```text
CONFIG_VELAROS_AMP=y
CONFIG_VELAROS_AMP_SMOKE=y
```

`velaros_amp_smoke` 会验证固定 command、heartbeat、status frame、motion uORB 映射
和 250 ms timeout-to-zero-velocity。该 smoke 不验证 K1 RPMsg、跨 hart、IRQ、DMA、
cache 或实际 Linux/openvela 分区。

## 拿到 K1 后的替换顺序

1. 复核 OpenSBI HSM、hart topology、DTB reserved-memory、vring 和 PLIC context；
2. 确定 Linux/openvela 对 CPU、RAM、IRQ、clock/reset、Ethernet 和 GPIO 的唯一所有者；
3. 用真实 RPMsg/OpenAMP 或 RPMsg virtio 替换 socketpair，先完成 echo、序号和重启恢复；
4. 将 Linux ROS 2 gateway 的 `cmd_vel`/Action 映射到 AMP frame；
5. 接入 openvela 的真实 IMU/LiDAR、LIO/Nav 和底盘执行器；
6. 测量端到端时延、丢包、队列背压、CPU、栈、功耗、温升和 30 分钟长稳。

## 不能宣称的内容

- K1 已经启动 Linux + openvela 双域；
- OpenSBI 已经成功释放专用 hart；
- RPMsg/OpenAMP vring、共享内存和跨核中断已实测；
- Linux 和 openvela 已完成 cache 一致性、内存隔离和外设隔离；
- AMP 已完成 K1 网络、真实传感器或底盘硬件闭环。
