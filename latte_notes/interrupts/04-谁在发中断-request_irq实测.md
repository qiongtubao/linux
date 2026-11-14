# 主题四 · 谁在发中断：一次启动里的 9 次 `request_irq`

## 你已知的

中断不是抽象的：每条 IRQ 线后面都挂着一个具体设备（定时器、键盘、串口、硬盘）。
驱动要收到中断，必须先**注册**：`request_irq(irq, handler, flags, name, dev_id)`。

## 实测：本次启动注册了什么

打点 `[latte] request_irq: irq=%u name=%s flags=0x%lx` 的完整输出（镜像 A）：

| IRQ | name | flags | 设备 | 干什么活 |
|---|---|---|---|---|
| 0 | `timer` | `0x215a00` | 定时器（PIT/HPET） | 系统 tick：更新 jiffies、调度 |
| 1 | `i8042` | `0x200080` | PS/2 键盘 | 按键、鼠标 |
| 4 | `ttyS0` | `0x200080` | 串口 | 收到字节、发送完成、modem 状态 |
| 8 | `rtc0` | `0x200000` | 实时钟 | 定时唤醒 |
| 9 | `acpi` | `0x2080` | ACPI | 电源事件 |
| 12 | `i8042` ×2 | `0x200080` | PS/2 鼠标 | 同一根线上的两个 handler |
| 14 | `ata_piix` | `0x80` | IDE 控制器 | 磁盘读写完成 |
| 15 | `ata_piix` | `0x80` | IDE 控制器 | 同上 |

几个能直接读出来的信息：

- **`0x80` 就是 `IRQF_SHARED`**：`ata_piix` 的两条线都带这个标志，表示这条 IRQ 上
  允许挂多个 handler（老 IDE 风格，两条通道共用控制器）；
- **IRQ 12 注册了两次**（都带 `0x80`）：PS/2 的键盘与鼠标两个 handler 共享这根线，
  中断来了两个都会被调用，各自判断“是不是我”；
- **注册时间有先后**：`timer` 在 0.000s、`acpi` 1.11s、`ata_piix` 2.50s、
  `i8042` 3.24s、`ttyS0` 3.45s——驱动什么时候 probe 完，什么时候才有中断可用。

## 定时器：tick 从哪来

```
setup_APIC_timer()            ← 每个 CPU 注册本地 APIC 定时器（打点：cpu0 uses lapic）
  → clockevents_register_device()
      → tick 框架按周期/单次触发
          → LOCAL_TIMER_VECTOR 中断
              → tick_periodic() → 更新 jiffies、触发调度、跑定时器
```

本次 `setup_APIC_timer: cpu0 uses lapic` 出现在 0.890s，之后 `/proc/interrupts`
里的 `LOC:` 计数一路涨到 3116（主题五）——这就是“系统在走”的证据。

## 串口：收数据靠中断，发数据靠轮询

有意思的不对称：

| 方向 | 机制 | 为什么 |
|---|---|---|
| 收（RX） | **中断** | 数据什么时候来不知道，不能干等 |
| 发（TX，console 输出） | **轮询** | `printk` 可能在中断关着/NMI 里跑，等不了中断 |

所以 `ttyS0` 的 IRQ 4 主要用来“收到字节就叫我”；内核日志的输出走的是
`serial8250_console_write()` 里的轮询路径（见 `devices/02`）。

## 共享中断与 `dev_id`

`request_irq()` 的最后一个参数 `dev_id` 是“这条 action 的身份”。共享时内核依次调用
每个 handler，handler 判断“是不是我的设备”“然后返回 `IRQ_HANDLED` 或
`IRQ_NONE`。`/proc/interrupts` 里的计数就是按 action 记的。

## 代码位置

| 位置 | 内容 |
|---|---|
| `kernel/irq/manage.c:2070` `request_threaded_irq()` | 注册入口（打点所在） |
| `arch/x86/kernel/time.c:52` | `request_irq(0, timer_interrupt, …, "timer", …)` |
| `drivers/tty/serial/8250/8250_core.c:170` | `request_irq(port.irq, serial8250_interrupt, …, port.name, …)` |
| `arch/x86/kernel/apic/apic.c:573` `setup_APIC_timer()` | 每个 CPU 的 tick 源头 |
| `include/linux/interrupt.h:74` | `IRQF_SHARED` / `IRQF_TIMER` 等标志 |
