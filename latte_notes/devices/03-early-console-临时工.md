# 主题三 · early console：真 console 上岗前的临时工

## 你已知的

内核启动最早期最容易挂（认内存、建页表、开中断），这时候最需要日志。但“正式的
控制台”要等驱动初始化，本次实测 **0.295 秒**才 enabled。那之前的日志谁在输出？

## 答案：early console

命令行里的 `earlyprintk=serial` 会让内核在极早期注册一个“简陋但能用”的 console：

```
earlyprintk=serial  →  setup_early_printk()  →  early_console_register()
     →  注册一个 flags 带 CON_BOOT 的 console（名字 earlyser）
     →  它的 write() 直接轮询 UART 状态位、写数据寄存器
```

它不申请中断、不走 tty 层、不依赖驱动模型——只有“把字节塞进 0x3f8”这一件事，
所以 0.000 秒就能用。

## 交接：临时工什么时候下台

规则在 `register_console()` 里：**一旦有“真正的” console 注册，所有 `CON_BOOT`
的 console 就会被禁用**（除非启动参数里加了 `keep`，把它的 `CON_BOOT` 清掉）。

实测 A（`console=ttyS0 earlyprintk=serial`）：

```
[    0.000000] [latte] register_console: earlyser-1 flags=0x9      ← 临时工上岗
[    0.000000] [latte] early console registered: earlyser (keep=0)
[    0.277806] [latte] console_init: run __con_initcall section    ← 驱动初始化
[    0.281885] [latte] register_console: tty-1 flags=0x1
[    0.282244] [latte] register_console: hvc-1 flags=0x1
[    0.283040] [latte] register_console: ttyS-1 flags=0x11
[    0.295214] printk: legacy console [ttyS0] enabled
[    0.295214] printk: legacy bootconsole [earlyser0] disabled     ← 临时工下岗
```

`flags` 的含义（`include/linux/console.h:190` 起）：

| 值 | 位 | 含义 |
|---|---|---|
| `0x1` | `CON_PRINTBUFFER` | 注册时把缓冲区里已有的日志补打一遍 |
| `0x8` | `CON_BOOT` | **临时工标记**，真 console 一来就下台 |
| `0x9` | 上面两个 | earlyser |
| `0x11` | `+ CON_ANYTIME` | ttyS：在任何时候（哪怕 CPU 还没全起来）都能用 |

## 反例更能说明问题：只留 earlyprintk

去掉 `console=ttyS0`，只留 `earlyprintk=serial`，日志会在这里**戛然而止**：

```
[    0.000000] [latte] console_init: run __con_initcall section
[    0.000000] Console: colour VGA+ 80x25
[    0.000000] [latte] register_console: tty-1 flags=0x1
[    0.000000] printk: legacy console [tty0] enabled
[    0.000000] printk: legacy bootconsole [earlyser0] disabled
（之后一行都没有了，直到 QEMU 超时被杀）
```

原因链：`console=` 没写 → 首选 console 是默认的 `tty0`（VGA）→ 它是“真 console”，
一注册就把 `earlyser` 顶掉 → 之后所有日志都写进 VGA，而 QEMU 是 `-nographic`，
我们看不到 VGA。

**结论：console 名单上有谁，不等于你能在串口看到。** 名单（`register_console`）和
去向（每个 console 的 `write()`）是两件事。

## 代码位置

| 位置 | 内容 |
|---|---|
| `arch/x86/kernel/early_printk.c:400` `setup_early_printk()` | 解析 `earlyprintk=` |
| `arch/x86/kernel/early_printk.c:390` `early_console_register()` | 注册成 `CON_BOOT` |
| `arch/x86/kernel/early_printk.c:118` `early_serial_putc()` | 轮询 LSR 后写 TXR |
| `kernel/printk/printk.c:3964` `register_console()` | 真 console 上岗时撤掉 boot console |
