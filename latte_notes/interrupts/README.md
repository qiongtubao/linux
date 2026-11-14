# 中断：硬件怎么打断内核，以及 `/proc/interrupts` 里的数字

上一站讲“日志怎么写出去”，这一站反过来：**硬件怎么主动打断 CPU**。
从 IDT 装表开始，一路跟到驱动的 handler，最后用 `/proc/interrupts` 的真实计数验收。
1 份索引 + 5 篇主题文档，配 3 个新打点与代码中文注释。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 中断 | 概念 | [01-中断是什么-IRQ与IDT](01-中断是什么-IRQ与IDT.md) | 异常/系统调用/硬件中断的区别 + 三种门 |
| 中断 | 装表 | [02-装表-idt_setup与lidt](02-装表-idt_setup与lidt.md) | 三阶段建表，`lidt` 一次生效，然后设只读 |
| 中断 | 进门 | [03-中断进门的路](03-中断进门的路.md) | 汇编桩 → `common_interrupt` → `irq_desc` → handler |
| 中断 | 谁在发 | [04-谁在发中断-request_irq实测](04-谁在发中断-request_irq实测.md) | 一次启动 9 次 `request_irq`，tick 来自本地 APIC |
| 中断 | 实测 | [05-实测-proc-interrupts](05-实测-proc-interrupts.md) | `timer 927` / `LOC 3116` / `ttyS0 6` 怎么读 |

## 一条链

```
设备（tick / UART / 键盘）
  │ 拉 IRQ 线
IO-APIC ──选向量──► CPU
  │
IDT[vector] ──► irq_entries_start 桩 ──► common_interrupt(regs)
                                            └─ irq_desc(vector)
                                                └─ handle_edge_irq / handle_level_irq
                                                    └─ action 链 → 驱动 handler
                                                        └─（返回路上）softirq 下半部
```

## 打点清单

| 打点 | 位置 | 说明 |
|---|---|---|
| `idt_setup_from_table %d`（既有） | `arch/x86/kernel/idt.c` | 每次批量写门的条数（本次共 248 次调用） |
| `idt_setup_traps`（既有） | 同上 | 32 个异常门装好 |
| `idt_setup_apic_and_irq_gates: IRQ gates ready` | 同上 | 外部中断门装好 + `lidt` + 只读 |
| `setup_APIC_timer: cpu%d uses %s` | `arch/x86/kernel/apic/apic.c` | 每 CPU 的 tick 源头 |
| `request_irq: irq=%u name=%s flags=0x%lx` | `kernel/irq/manage.c` | 谁注册了哪根线（共 9 次） |

## 两次启动

| | A：正常 | D：诊断 init |
|---|---|---|
| 用途 | 收集打点时间线 | 读 `/proc/interrupts` / `softirqs` / `stat` |
| 关键输出 | `request_irq` 9 行、`IRQ gates ready`、`cpu0 uses lapic` | `timer 927`、`LOC 3116`、`ttyS0 6` |

## 自查 3 问

1. 装好 IDT 就有中断了吗？（02：还要 APIC 配好、驱动 `request_irq`、CPU 开 `IF`）
2. 为什么内核日志输出不用中断，而串口收数据要用？（04：`printk` 可能在 NMI/关中断
   上下文里，只能轮询；收数据不知何时到来，必须中断）
3. `/proc/interrupts` 里 `LOC` 为什么不在 IRQ 号列表里？（05：它是本地 APIC 定时器
   向量，不是外部 IRQ 线，但正是 tick）

## 下一阶段

中断之后还剩两块：**下半部**（softirq / tasklet / workqueue 的分工与延迟）与
**tty 层**（`Ctrl-C` 怎么变成信号、行规程在哪）。也可以回内存看页回收。
