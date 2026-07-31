# K1 交付检查清单

## 无板阶段（当前）

- [x] K1 启动事实、地址、UART、timer、PLIC 资料盘点；
- [x] RV64 S-mode hart 0 入口和显式 `gp` 初始化；
- [x] 初始 hart/DTB/sstatus/satp/stvec 日志；
- [x] polling `/dev/console`，源码和 ELF 验收均禁止写 IER；
- [x] SBI TIME 24 MHz lower-half；
- [x] 同步异常寄存器直出和 `sepc` 自动符号化；
- [x] PLIC hart 0 S-mode context 1 已从 DTS 解析；
- [x] PLIC 骨架可选编译、默认关闭且与 SMP 互斥；
- [x] 静态 CI、干净构建入口、ELF 自动验收、U-Boot 上板包；
- [x] 串口原始日志和元数据自动采集；
- [x] 双轮 59 Pattern 驱动审查与修复记录；
- [x] 来源/许可证清单和发布包许可证；
- [x] 最终干净构建、ELF/包 SHA256 回归；

## 拿到板后的阻塞项

- [ ] 保留并验证原厂可恢复 SD 卡；
- [ ] 记录板卡版本、供电、USB-UART、U-Boot 和 OpenSBI 版本；
- [ ] 只读确认 MMC 编号/分区和实际 DTB；
- [ ] 核对 payload 窗口、U-Boot relocation 和 reserved-memory；
- [ ] 手工 `bootelf -p`，不执行 `saveenv`；
- [ ] 日志确认 hart、DTB、S-mode、初始 CSR；
- [ ] 依次确认 early marker、NuttX banner、NSH；
- [ ] 核对 SBI TIME 并连续运行 10 分钟；
- [ ] 保存完整串口日志、JSON 元数据、ELF SHA256 和验收记录；
- [ ] 复核实际 PLIC DTS 后，才在临时配置启用 `CONFIG_K1_PLIC=y`；
- [ ] 使用可控外部 IRQ 验证 claim/complete；UART IRQ 需继续避开 IER；
- [ ] 单核稳定后才设计/启用 SBI HSM + SMP。

## 提交前

- [ ] `tools/ci_k1.sh --jobs 8` 全部通过；
- [ ] 工作树只包含预期比赛改动；
- [ ] 未提交构建目录、ELF、串口设备信息或敏感环境数据；
- [ ] 实板结论与日志证据一致，未把“编译通过”表述为“上板通过”；
- [ ] README、测试记录、许可证清单和最终 SHA256 已更新；
- [ ] AI Coding 日志已按比赛规则归档；
- [ ] 用户确认提交范围后再 commit/push/PR。
