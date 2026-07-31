# K1 来源与许可证清单

更新时间：2026-07-30

## 本比赛仓

| 范围 | 来源/作者方式 | 许可证 | 处理 |
|---|---|---|---|
| `chip/k1/` | 本项目实现，复用 NuttX RISC-V 公共 API 和 in-tree 驱动模式 | Apache-2.0 | 每个代码文件带 SPDX |
| `board/k1/muse_pi_pro/` | 本项目实现 | Apache-2.0 | 每个代码文件带 SPDX |
| `tools/` 中 K1 工具 | 本项目实现 | Apache-2.0 | 每个脚本带 SPDX |
| `docs/K1_*.md` | 本项目编写 | Apache-2.0（仓库级） | 受根目录 `LICENSE` 约束 |
| manifest/工作流 | 比赛仓配置 | Apache-2.0（仓库级） | 不嵌入固件 |

根目录 `LICENSE` 是 Apache License 2.0。K1 源码没有引入二进制 blob、厂商
SDK 头文件或不可再分发资料。

## openvela/NuttX 构建输入

K1 ELF 由当前 openvela 工作区的 NuttX、apps 和工具链生成。NuttX 主体使用
Apache-2.0，并在其 `LICENSE`/`NOTICE` 中列出随树第三方材料。上板包会带：

```text
licenses/CONTEST-LICENSE
licenses/NUTTX-LICENSE
licenses/NUTTX-NOTICE  # 工作区存在时
```

正式发布前仍需针对最终 defconfig 对实际链接对象做一次 SBOM/许可证扫描；当前
清单只覆盖首启 NSH 基线，不能替代最终制品合规审查。

## K1 实板参考仓

只读参考：

```text
/home/sw/Dev/musepi-rvv-os-reference
commit 1761a1ae2801163f09ea0eefbec89c1c0fa212b1
```

| 材料 | 许可证 | 本项目使用方式 |
|---|---|---|
| 参考仓自有代码/文档 | MIT | 读取实板启动结论和硬件事实 |
| `k1-x_MUSE-Pi-Pro.dts`、`k1-x.dtsi` | GPL-2.0 OR MIT | 读取地址、IRQ、timebase 和 PLIC context 顺序 |
| vendored U-Boot | GPL-2.0 体系 | 只研究启动命令和 DT，不复制源码 |
| vendored OpenSBI | BSD-2-Clause | 只确认 S-mode/SBI handoff |

本比赛仓没有复制参考仓函数、源码文件或二进制制品。硬件地址、IRQ 编号、设备树
节点数值和启动时观测属于移植事实；引用它们时在 `K1_BOOT_INVENTORY.md` 和
`K1_PLIC_DESIGN.md` 保留了 commit 与文件路径。

## 外部资料

K1 datasheet、SpacemiT 在线文档和 openvela 赛道指南仅作为规范/事实来源，不随
仓库重新分发。链接和用途记录在 `K1_BOOT_INVENTORY.md`。

## 发布前检查

- [ ] 所有新增 `.c/.h/.S/.sh/.py/Kconfig/CMakeLists.txt/Makefile` 保留 SPDX；
- [ ] 最终 ELF 对应的 NuttX/apps commit 已记录；
- [ ] 最终 defconfig 的实际链接对象已完成许可证扫描；
- [ ] 发布包包含本仓和 NuttX 的 LICENSE/NOTICE；
- [ ] 没有误提交 datasheet、厂商镜像、SD 整盘或参考仓二进制；
- [ ] 文档中的参考 commit、ELF SHA256 和实板日志能够互相对应。
