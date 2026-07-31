# K1 PLIC 离线设计记录

## 结论

K1 的 hart 0 S-mode PLIC context 为 **context 1**。对应寄存器：

| 寄存器组 | 地址 |
|---|---:|
| priority source N | `0xe0000000 + 4 * N` |
| hart 0 S-mode enable | `0xe0002080` |
| hart 0 S-mode threshold | `0xe0201000` |
| hart 0 S-mode claim/complete | `0xe0201004` |

实现位于 `chip/k1/k1_plic.c` 和 `chip/k1/hardware/k1_plic.h`，通过
`CONFIG_K1_PLIC` 控制，**默认关闭**。初始 NSH defconfig 不启用它，因此不会改变
当前 polling UART + SBI timer 的首板基线。

2026-07-30 已使用临时 `CONFIG_K1_PLIC=y` 配置完成编译链接，ELF 中包含
`k1_plic_initialize/enable_irq/disable_irq/claim/complete`；验证后恢复默认配置。
这只证明代码路径可编译，不代表 PLIC 已在实板工作。

## 证据链

参考仓：

```text
/home/sw/Dev/musepi-rvv-os-reference
commit 1761a1ae2801163f09ea0eefbec89c1c0fa212b1
```

其 U-Boot DTS：

```text
third_party/u-boot-k1/arch/riscv/dts/k1-x_MUSE-Pi-Pro.dts
  -> includes k1-x.dtsi
```

`k1-x.dtsi` 的 PLIC 节点为：

```dts
interrupt-controller@e0000000 {
  compatible = "riscv,plic0";
  interrupts-extended = <
    &cpu0_intc 11 &cpu0_intc 9
    &cpu1_intc 11 &cpu1_intc 9
    ...
    &cpu7_intc 11 &cpu7_intc 9
  >;
  reg = <0x0 0xe0000000 0x0 0x04000000>;
  riscv,max-priority = <7>;
  riscv,ndev = <159>;
};
```

`11` 是 machine external interrupt，`9` 是 supervisor external interrupt。
PLIC context 按 `interrupts-extended` 顺序编号，因此 hart 0 M-mode 为 context 0，
hart 0 S-mode 为 context 1。标准 PLIC context stride 推导出上述 enable、
threshold 和 claim/complete 地址。

## 默认关闭的原因

首板阶段必须先单独验证：

1. U-Boot 能进入 `0x11000000` 的 RV64 S-mode ELF；
2. polling UART 能稳定输出且不写 IER；
3. SBI TIME 能进入 NSH 并连续运行；
4. 实际 U-Boot DTB 的 PLIC 节点仍与参考 DTS 一致。

满足后才在临时测试配置中启用 `CONFIG_K1_PLIC=y`。首次外部中断只建议接入一个
低风险、可控源；UART IRQ 42 在“禁止写 IER”的约束解除前不能作为首个 PLIC
验证源。

## 实现边界

- 只支持 hart 0 S-mode；
- `depends on !SMP`，不声称支持多 hart context；
- 初始化时清空 context 1 的 enable 位并将所有 source priority 置零；
- `up_enable_irq()` 按 source 设置 priority=1 和 enable bit；
- external trap 循环 claim、调用 NuttX IRQ 分发、再 complete；
- 不触碰 UART IER，不接管 OpenSBI/CLINT；
- 实板验证前不进入比赛默认 defconfig。

## 上板复核命令

在 U-Boot 中加载实际 DTB 后执行：

```text
fdt addr ${fdt_addr_r}
fdt print /soc/interrupt-controller@e0000000
```

若节点路径不同，先用 `fdt list /` 和 `fdt list /soc` 定位。只有
`interrupts-extended`、`reg`、`riscv,ndev` 均与本文一致，才允许启用该 Kconfig。
