# 主题二 · 装表：从 early handler 到真正的 IDT

## 你已知的

CPU 遇到中断/异常第一件事是**查表**（IDT）。表是内存里的一段数组，由内核在启动早期
装好、之后设为只读。装表分三步，一步比一步“正式”。

## 三步装表

| 阶段 | 函数 | 装了什么 | 为什么需要 |
|---|---|---|---|
| 1 | `idt_setup_early_handler()` | 32 个异常 + 早期桩 | 这时连内存管理都没全好，先有个能报错的表 |
| 2 | `idt_setup_traps()` | 32 个正式异常/陷阱门 | 从 `def_idts[]` 批量写 |
| 3 | `idt_setup_apic_and_irq_gates()` | 外部中断 + APIC 向量 + 系统向量 | 设备中断真正可用；装完 `lidt` 并设只读 |

实测日志（镜像 A）：

```
[latte] idt_setup_from_table 32
[latte] idt_setup_traps
[latte] idt_setup_apic_and_irq_gates: IRQ gates ready
```

## 表是“批量”写出来的

`idt_setup_from_table(idt, t, size, sys)` 接收一张 `struct idt_data[]`，逐条填
`gate_desc` 再 `write_idt_entry()` 写进数组：

| 表 | 内容 | sys 参数 |
|---|---|---|
| `def_idts[]` | 除零、`#PF`、`#GP`、NMI、`#BP`… | true |
| `ia32_idt[]` | 32 位兼容模式的几个特殊项 | true |
| `apic_idts[]` | APIC 错误、温度、spurious、local timer… | true |

`sym=true` 的作用：这些向量在 `system_vectors` 位图里打标记，之后
`idt_setup_apic_and_irq_gates()` 给“剩下的空位”统一填“忽略/伪中断”桩，避免漏掉
某个向量导致 CPU 收到它时跳到垃圾地址。

## 外部中断是怎么“批量”挂上去的

```c
for_each_clear_bit_from(i, system_vectors, FIRST_SYSTEM_VECTOR) {
	entry = irq_entries_start + IDT_ALIGN * (i - FIRST_EXTERNAL_VECTOR);
	set_intr_gate(i, entry);
}
```

所有外部 IRQ 共用一个“入口桩区”（`irq_entries_start`），每个向量一个 16 字节的桩，
桩里只做“把向量号压栈 + 跳公共入口”。真正的分派在 C 里做（主题三）。

## 最后两步：加载 + 只读

```c
idt_map_in_cea();          /* 映射到 CPU entry area（per-CPU 的固定地址） */
load_idt(&idt_descr);      /* 一条 lidt：从这一刻起新表生效 */
set_memory_ro((unsigned long)&idt_table, 1);   /* 改表 = 内核 bug */
```

`lidt` 之前，CPU 用的还是旧表；之后所有中断/异常都走新表。设只读是因为 IDT 被改写
等于让攻击者能劫持任意中断，属于高危。

## 一个常见误解

“装好 IDT 就有中断了”——不对。装表只是让 CPU **知道该跳哪**；还要有：
① 中断控制器配好（APIC/IO-APIC 的入口）、② 具体设备驱动 `request_irq()` 注册了
handler（主题四）、③ CPU 的 `IF` 标志开着（`sti`）。三样齐了，中断才会真的进来。

## 代码位置

| 位置 | 内容 |
|---|---|
| `arch/x86/kernel/idt.c:197` `idt_setup_from_table()` | 批量写门描述符 |
| `arch/x86/kernel/idt.c:232` `idt_setup_traps()` | 32 个异常门 |
| `arch/x86/kernel/idt.c:293` `idt_setup_apic_and_irq_gates()` | 外部中断 + `lidt` + 只读 |
| `arch/x86/include/asm/desc_defs.h` | `gate_desc` / `idt_data` 的字段 |
