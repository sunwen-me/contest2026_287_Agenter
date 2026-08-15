# VelaROS 固定共享缓冲 backend

版本：2026-08-02

## 结论

VelaROS 已实现面向 openVela 的第一版静态大 payload backend：payload 放在编译期
定容的本地缓冲池中，uORB 与 ROSIDL/Fast-CDR 路径只传递有界 descriptor；同一
pool 的兼容端点映射同一 payload 地址，不再复制 payload。不能共享该 pool 的
端点会确定性回退到标准 CPU buffer + CDR + RTPS/UDPv4。

这完成的是 goldfish 上的板内同地址域基础设施和端到端 smoke，不等于 K1 DMA、
cache coherency、RPMsg/AMP 或真实相机/点云产品节点已经验收。跨设备网络也仍然
必须序列化 payload，不属于零拷贝范围。

## 数据路径

```text
producer
  -> acquire fixed slot -> write -> commit
  -> retain bounded consumer lease
  -> uORB copies descriptor only
  -> consumer validates cookie/slot/generation/lease/owner
  -> map the same payload address -> release lease

ROSIDL buffer field
  -> compatible endpoint and identical pool metadata
       -> serialize 36-byte descriptor fields -> adopt the retained lease
  -> incompatible/remote endpoint
       -> to_cpu() -> normal CDR payload -> RTPS/UDPv4
```

Fast DDS 原生 SHM transport/DataSharing 没有被重新引入。`VelaBufferBackend` 通过
`rmw_fastrtps_cpp` 的编译期入口静态注册，不使用 pluginlib、动态库、运行时插件
扫描、后台回收线程或每 buffer 线程。

## 固定资源模型

产品 defconfig 当前锁定：

| Kconfig | 值 | 含义 |
|---|---:|---|
| `CONFIG_VELAROS_BUFFER_POOL_SLOTS` | 4 | 最多四个同时占用的 payload slot |
| `CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE` | 16,384 B | 每个 slot 的最大 payload |
| `CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES` | 4 | 每个 slot 最多四个有界 owner lease |

payload 总 BSS 预算固定为 65,536 B。池耗尽返回
`VELAROS_BUFFER_EXHAUSTED`，单 slot lease 耗尽返回
`VELAROS_BUFFER_LEASE_EXHAUSTED`，不会回退到无界堆分配。

descriptor 字段是 `pool_cookie + slot + generation + lease + owner + length +
capacity + flags`。CDR 字段净长度为 36 B；AArch64 C 结构体因对齐通常为 40 B，
带 uORB timestamp 的 topic record 为 48 B。descriptor 不包含虚拟地址或物理地址。

生命周期为：

```text
FREE -> WRITABLE -> COMMITTED/READ_ONLY -> one or more bounded leases -> FREE
```

`generation` 拒绝 slot 复用后的旧 descriptor，`lease + owner` 拒绝越权和重复
release。任务/端点停止时可按 owner 调用 `velaros_buffer_reclaim_owner()`；实现
没有超时 worker，调用方必须显式承担异常清理责任。统计接口提供 allocation、
retain、release、reclaim、rejection、exhaustion 和 high-water 数据。

## ROSIDL/Fast DDS 集成

ROS 2 Lyrical 源码闭包已有实验性的 `rosidl_buffer`、`rosidl_buffer_backend` 和
endpoint-aware Fast-CDR 插入点，但 NuttX 产品配置不支持 pluginlib，因此原 registry
不会发现 backend。VelaROS 在 `buffer_backend_loader.cpp` 中增加受 Kconfig 控制的
静态工厂：

- backend type：`velaros`；
- 静态实例名：`velaros/static`；
- metadata：本次启动的 pool cookie；
- descriptor typesupport：固定八字段 Fast RTPS C++ callbacks；
- 相同 metadata 的已协商 endpoint：传 descriptor；
- 未协商或 metadata 不同的 endpoint：backend 返回空 descriptor，既有 ROSIDL
  serializer 自动执行 CPU/CDR fallback。

外部 ROS 2 主机不会拥有 guest pool cookie，因此仍收到标准 ROS 消息 payload，
不会收到只能在板内解释的私有裸指针。对 `rmw_fastrtps 9.4.8` 的静态入口修改由
`tools/patches/rmw-fastrtps-9.4.8-velaros-static-buffer.patch` 单独固化，并已通过
`tools/restore_velaros_ros2_sources.sh --check`。

## uORB 集成

`velaros_buffer_transfer` 是固定布局 uORB topic：

```c
struct velaros_buffer_transfer_s {
  uint64_t timestamp;
  velaros_buffer_descriptor_t descriptor;
};
```

producer 在发布前为 consumer 创建 lease，uORB 只复制 topic record；consumer
通过 descriptor 映射 pool。这个 topic 是 backend 的验证与接入原语，不是运行时
加载任意 ROS 类型的通用桥。实际相机、激光或点云节点仍需按产品白名单定义消息
语义、owner 生命周期和溢出策略。

## 已完成验收

`velaros_buffer_backend_smoke` 在 goldfish-arm64 上验证：

- 4 个 slot 全部占用后，第 5 次 acquire 有界失败；
- double release、release 后 map，以及 reset 后 slot 重用时的旧 descriptor 被拒绝；
- uORB 收发前后映射地址相同，128 B pattern 完整；
- 静态 ROSIDL backend 被 RMW loader 发现；
- 兼容 endpoint 经 descriptor 序列化/反序列化后映射同一地址；
- 非兼容 endpoint 回落到 `cpu` backend，payload CDR 内容一致；
- 测试结束 active slot 和 lease 均为 0。

完整 simulator 回归还覆盖 RMW/rcl/rclcpp、Topic、Service、Action、Binder/KVDB、
普通 uORB bridge、DDS HelloWorld 3 SENT/3 RECEIVED 和任务清理，结果全部 PASS。

## 体积效果

与加入 backend 前的同一 release 产品闭包相比：

| 指标 | backend 前 | backend 后 | 增量 |
|---|---:|---:|---:|
| Flash | 23,125,496 B | 23,152,808 B | +27,312 B（+26.67 KiB） |
| 静态 RAM | 661,752 B | 728,376 B | +66,624 B（+65.06 KiB） |

其中 65,536 B 是配置明确要求的 4×16 KiB payload BSS。代码、slot/lease 元数据、
mutex 和 backend 状态合计增加约 26.67 KiB Flash 与 1.06 KiB 额外静态 RAM。
上表对应命名纠正前的 backend 测量快照
`747199e229e1ad8b6a9a2c0626a473602caa85bc12f2e080f3bd3f654eee91f4`。仅纠正模块、
Kconfig 和 NSH 命名后，当前 release ELF SHA256 为
`82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`，Flash 为
23,152,776 B（只减少 32 B `.rodata`），静态 RAM 不变。

若把“删除 Fast DDS 原生 SHM/DataSharing”和“加入 VelaROS backend”作为完整替代
一起看，Flash 仍比删除前少 333,152 B（325.34 KiB）；代价是为可预测的 payload
容量显式增加 64,736 B 静态 RAM。详细 section 口径见
[`VELAROS_SIZE_BASELINE.md`](VELAROS_SIZE_BASELINE.md)。

## 尚未完成的硬件与产品项

- pool 目前是 64-byte 对齐的普通静态 BSS，不是 K1 DMA heap/保留内存；
- 尚未根据 K1 cache 属性实现 clean/invalidate 或 ownership barrier；
- uORB/RPMsg 跨核传 descriptor 与共享地址转换尚未实测；
- 真实 `sensor_msgs/Image`、点云或自定义 bounded 大消息尚未接入该 buffer field；
- DDS descriptor 在序列化后若因发送失败而无人 adopt，需要端点生命周期调用
  owner reclaim；当前没有后台超时回收；
- 没有 K1 上的延迟、吞吐、CPU、峰值 heap、cache miss 和 30 分钟长稳结果。

因此当前准确表述是：“板内固定池/uORB/ROSIDL backend 核心链路已在 simulator
完成”，不是“面向所有设备和所有 ROS 消息的零拷贝已经完成”。
