# VelaROS ROS 2 源码与许可证清单

版本：2026-07-31

## 锁定仓库

| 仓库 | 版本 | 精确 commit | 许可证 | 当前用途 |
|---|---:|---|---|---|
| `ros2/rcutils` | 7.1.1 | `96aea05e6ce03b851d1123e29faa1d7f8665b6bc` | Apache-2.0 | 第一批目标端 |
| `ros2/rosidl` | 5.2.1 | `024f0b9636d716b9756c25ade0b6d7b15bde18e9` | Apache-2.0 | 第一批 runtime/headers |
| `ros2/rosidl_dynamic_typesupport` | 0.4.1 | `f9ee0542b725244ba5633cb3c03d2f9cc0da0064` | Apache-2.0 | 第一批 `rmw` 依赖 |
| `ros2/rmw` | 7.10.1 | `69abaef996bdbe39488839bf230553af527a1d33` | Apache-2.0 | 第一批公共 RMW 接口 |
| `ros2/rcpputils` | 2.14.5 | `b8775ec03b9412789b34358c738b0e87c53e1b31` | Apache-2.0、BSD-3-Clause | 第二批目标端 |
| `ros2/rmw_dds_common` | 6.0.0 | `2f00e6e9208e925d61330ea92f857b2e67d595ab` | Apache-2.0 | 第二批 GraphCache 和 3 个图消息 |
| `ros2/rosidl_typesupport_fastrtps` | 3.9.5 | `fa6cab41fd21f034fc735337b4f3fe9d83829a49` | Apache-2.0 | 第二批生成 Fast RTPS C++ 类型支持 |
| `ros2/rosidl_dynamic_typesupport_fastrtps` | 0.5.1 | `de01f56ccbf80851b5460a3ca169b4ca71c58b48` | Apache-2.0 | 第二批目标端动态类型后端 |
| `ros2/rmw_fastrtps` | 9.4.8 | `198a8b57386144896e5270a2a51bc4306be33967` | Apache-2.0 | 第三批目标 RMW，完整生命周期 PASS |
| `ros2/rcl` | 10.4.4 | `da0b5e70247ef1fb0425bf35417d4e9fdf11efb9` | Apache-2.0 | 第四批最小 context/node 客户端层 |
| `ros2/rcl_interfaces` | 2.4.5 | `b90e36e4adf5ed878efa2f365a84798f782618cd` | Apache-2.0 | 已锁定，下一阶段生成消息 |
| `ros2/rcl_logging` | 3.4.1 | `904c0971bd0b3581094af853b76b56304915248e` | Apache-2.0 | 已锁定，下一阶段日志边界 |
| `ros2/libyaml_vendor` | 1.8.1 | `78a2eb793c0aefb399f0fc7e9882524868b117c1` | Apache-2.0、MIT | 已锁定，下一阶段 YAML 边界 |
| `ros2/ros2_tracing` | 8.10.2 | `24c3dea0fcee6f1fd0eb5244c6e1ea390dd479ac` | Apache-2.0 | 已锁定，下一阶段 tracing 边界 |
| `ros2/unique_identifier_msgs` | 2.8.1 | `b4779af6b3d3c45e710802a0e7f86113da281ae8` | Apache-2.0 | 已锁定，下一阶段接口依赖 |

机器可读版本和 Ubuntu Resolute binary package 版本位于
`tools/velaros-ros2-sources.lock`。

## 许可证处理

- 每个导出的源码仓保留上游 `LICENSE`、源文件版权头和 `package.xml`；
- VelaROS 的 openvela glue、恢复脚本和平台补丁使用 Apache-2.0；
- `rcpputils` 中 BSD-3-Clause 内容保持原许可证，不重新授权；
- `libyaml_vendor` 的 Apache-2.0 包装和上游 libyaml MIT 许可分别保留；
- host 工具与 target runtime 分开记录；未编入固件的生成器和测试包不计入
  target binary 依赖，但源码分发时仍保留其许可证；
- `rmw_dds_common` 的生成源码由锁定的消息定义和 Lyrical ROSIDL 生成器得到，
  只导出 71 个 C/C++ 源码/头文件，不导出 host 二进制；
- 后续实际扩展 `rcl`、接入 `rclcpp` 或新消息包时必须同步更新“当前用途”
  和最终 binary notice。

## 来源验证

恢复脚本从 ROS 2 官方 GitHub 仓下载精确 commit，以 `git archive` 导出无
`.git` 的源码树，并写入 `.velaros-source-revision`。校验命令：

```bash
tools/restore_velaros_ros2_sources.sh --check
tools/generate_velaros_ros2_interfaces.sh --check
```

本项目不复制 `/opt/ros/lyrical` 的 amd64 二进制或系统安装空间到目标固件。
一键验收还会扫描最终 ELF 和 `build.ninja`，拒绝 `/opt/ros` 及宿主 ROS
共享库痕迹。
