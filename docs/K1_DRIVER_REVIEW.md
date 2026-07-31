# K1 启动与基础驱动代码质量审查报告

## 基本信息

| 项目 | 内容 |
|---|---|
| 审查对象 | K1 入口、UART、SBI timer、IRQ/异常分发和板级初始化 |
| 子系统 | RISC-V arch/chip、字符设备、timer、interrupt controller |
| 审查时间 | 2026-07-30 |
| 审查方式 | 59 Pattern 双轮交叉验证（Pattern 全面检查 + 独立深层检查） |
| L1-7 激活 | P-47、P-48、P-49（异常/中断与硬件寄存器路径） |
| 设计文档 | `K1_BOOT_INVENTORY.md`、`K1_UBOOT_BRINGUP.md` |

## 质量评分总览（修复前）

| 维度 | 满分 | 得分 | Critical | High | Medium | Low |
|---|---:|---:|---:|---:|---:|---:|
| L1-1 内存安全 | 20 | 20 | 0 | 0 | 0 | 0 |
| L1-2 并发安全 | 20 | 20 | 0 | 0 | 0 | 0 |
| L1-3 资源管理 | 10 | 10 | 0 | 0 | 0 | 0 |
| L1-4 错误处理 | 10 | 10 | 0 | 0 | 0 | 0 |
| L1-5 类型与数值 | 10 | 10 | 0 | 0 | 0 | 0 |
| L1-6 输入与边界 | 15 | 15 | 0 | 0 | 0 | 0 |
| L1-7 嵌入式专项 | 15 | 15 | 0 | 0 | 0 | 0 |
| **总计** | **100** | **100** | **0** | **0** | **0** | **0** |

设计健康度：10/10（A）。

审查结论：**PASS（Pattern 评分）**。同时发现两个不属于现有 59 Pattern
精确定义、但应在实板前修复的启动可靠性问题；它们按规范列为
`LLM-UNMATCHED`，不参与上述扣分。

## Round 1：Pattern 驱动全面审查

逐项检查 P-01～P-52 和 DP-01～DP-07，未发现 Pattern 命中。重点结论：

- UART MMIO 仅在 chip/arch 层使用，不构成 DP-01；
- polling console 不写 UART IER，未发现 P-48 类 W1C/RMW 问题；
- 异常日志在 trap 上下文中不分配内存、不休眠且栈占用很小，未命中
  P-47/P-49；
- SBI timer 使用 NuttX 静态 lower-half，未引入资源生命周期；
- 用户 `read`/`write` 缓冲区和零长度路径有明确处理；
- 板级初始化不执行需要 scheduler 的阻塞总线访问。

Round 1 另记录两条体系外发现：

### DR-001：入口未初始化 `gp` [High]

- Pattern：无（LLM-UNMATCHED）
- CWE：CWE-457（类比：使用未初始化的架构寄存器）
- 文件：`chip/k1/k1_head.S`
- 描述：进入 C 代码前没有将 `gp` 设置为链接器的 `__global_pointer$`。
  当前反汇编碰巧没有依赖 `gp`，但链接松弛、代码变化或工具链升级均可能生成
  gp-relative 访问。
- 置信度：0.50（规范要求仅作 WARNING，不扣分）
- 修复方向：入口使用 `.option norelax` 初始化 `gp`，链接脚本提供
  `__global_pointer$`。

### DR-002：console 注册失败静默丢失 [Medium]

- Pattern：无（LLM-UNMATCHED）
- CWE：CWE-252
- 文件：`chip/k1/k1_console.c`
- 描述：`register_driver()` 返回值未检查；内存不足或路径冲突时系统继续启动，
  但 `/dev/console` 不存在，给首轮 bring-up 造成误导。
- 置信度：0.50（规范要求仅作 WARNING，不扣分）
- 修复方向：检查返回值，并通过不依赖 VFS/syslog 的早期 UART 路径报告。

## Round 2：独立深层审查

第二轮按启动时序、异常时序、资源对称性、边界和错误传播重新阅读代码，不引用
Round 1 结论。独立发现：

- 进入首个 C 函数前的架构寄存器初始化不完整（对应 DR-001）；
- VFS console 注册的错误没有传播或可观察记录（对应 DR-002）；
- 早期异常日志若 UART MMIO 本身触发 access fault 可能递归异常。该场景依赖
  U-Boot 串口 handoff 失效，当前只能在实板通过最早期字符验证，保留为低置信度
  风险，不改为无日志模式。

资源对称性：

| 资源/动作 | 释放/完成 | 结论 |
|---|---|---|
| 静态 SBI timer lower-half | 进程生命周期内常驻 | 对称，不需要释放 |
| `/dev/console` 注册 | 系统启动期常驻 | 对称，不需要注销 |
| trap/IRQ handler attachment | 系统启动期常驻 | 对称，不需要 detach |
| UART 配置 | 继承 U-Boot，不获取所有权 | 不执行恢复是有意设计 |

并发结论：当前为单 hart、polling UART、PLIC 关闭的初始配置，没有共享可变驱动
状态；写入按字符串行发生。多调用者输出可能交织，但不造成 MMIO RMW 或内存破坏。

## 交叉验证与裁决

| 最终 ID | 来源 | R1 | R2 | Pattern | 置信度 | 裁决 |
|---|---|---|---|---|---:|---|
| DR-F001 | R1 + R2 | DR-001 | 同位置 | LLM-UNMATCHED | 0.50 | WARNING，实板前修复 |
| DR-F002 | R1 + R2 | DR-002 | 同位置 | LLM-UNMATCHED | 0.50 | WARNING，实板前修复 |

两轮均确认的事实仍按规范保留 0.50：置信度上限来自“无现有 Pattern 映射”，而
不是问题不存在。工程处置优先级仍为上板前修复。

## 需求一致性

| 需求项 | 状态 | 备注 |
|---|---|---|
| RV64 S-mode、hart 0 首启 | 符合 | 非 boot hart 停在 `wfi` |
| 固定入口 `0x11000000` | 符合 | 由链接脚本和 ELF 脚本共同验证 |
| 保留 U-Boot UART 且不写 IER | 符合 | Kconfig + 源码 + ELF 验收 |
| SBI TIME 24 MHz | 符合 | 使用 RISC-V common mtimer/SBI 路径 |
| 同步异常早期日志 | 符合 | 输出 scause/sepc/stval/sstatus/satp/sp |
| PLIC 外部中断初始关闭 | 符合 | 尚未启用 S-mode context |
| SMP 初始关闭 | 符合 | defconfig 与 ELF 验收共同限制 |

## 修复状态

- DR-F001：已修复；入口显式初始化 `gp`，链接脚本提供
  `__global_pointer$`。
- DR-F002：已修复；注册失败时由 polling UART 打印固定错误消息。
- 额外加固：安装 trap vector 前清零 `sscratch`；早期日志增加 hart、DTB、
  初始 `sstatus`、`satp` 和 `stvec`，用于验证 U-Boot/OpenSBI handoff。

最终结论仍为 **PASS**，上述修复需由干净构建、ELF 符号检查和实板日志完成
最终闭环。

## PLIC 骨架追加复核

PLIC 骨架加入后按同一清单复核：

- P-13：enable 位 RMW 包在 critical section 内；
- P-47/P-49：external trap 路径不分配、不休眠、无大栈对象；
- P-48：claim/complete 使用规定的直接读/写语义，不做 W1C read-modify-write；
- DP-01/DP-02/DP-05：PLIC MMIO 保持在 K1 chip/arch 层；
- context、source 上限和默认关闭策略均与设计文档一致。

临时 `CONFIG_K1_PLIC=y` 的 1098 目标构建已通过，随后恢复默认关闭配置。追加
复核未产生新的扣分项；运行时正确性仍需实际 DTB 复核和外部 IRQ 实板测试。
