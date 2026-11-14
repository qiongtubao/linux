# 主题二 · 一次 `printk` 的旅程：从环形缓冲区到串口

## 你已知的

用户态 `printf` 最终写文件描述符；内核里没有 libc、也没有可用的 `write(2)`，
它有自己的输出函数 `printk()`。本系列笔记里每一条 `[latte]` 日志，都走这条链。

## 四站

```
printk("...")
 └─ vprintk_store()            ← ① 写进环形缓冲区（ring buffer），立刻返回
     └─ console_flush_all()    ← ② 拿 console_lock，把未输出的记录一条条推出去
         └─ console_emit_next_record()   ← ③ 对每个已注册的 console 调 write()
             └─ serial8250_console_write() → 等 THRE → outb(TXR)   ← ④ 落到硬件
```

| 站 | 做什么 | 为什么这么设计 |
|---|---|---|
| ① 写入 ring buffer | 格式化后拷进固定缓冲区，分配序号 | `printk` 可能在中断、NMI、panic 里被调用，同步写设备随时会卡死 |
| ② flush | 持 `console_lock`，逐条推进 | 保证多个 CPU 同时 printk 时输出不交错 |
| ③ 分发给每个 console | 有几个 console 就写几份 | 同时有串口、VGA、earlycon、网络 console 时都要看到 |
| ④ 驱动写硬件 | 轮询 UART 状态位，再写数据寄存器 | 控制台输出发生在“中断已关”的上下文里，不能等中断 |

## 为什么是“轮询”而不是“中断”

串口发送用中断是为了**不阻塞**：有数据要发时，把数据交给 UART，发完让 UART 打断你。
但 `printk` 的语境正相反——它经常在“中断关着、甚至 NMI 里”被执行，此时等中断就是
死锁。所以 console 输出走轮询：

```c
while ((serial_in(port, LSR) & XMTRDY) == 0 && --timeout)
	cpu_relax();
serial_out(port, TXR, ch);
```

`LSR` 的 `THRE` 位表示“发送保持寄存器空”，空了才能写下一个字符；`timeout` 是防硬件
坏掉时死等。

## 你在日志里看到的顺序

镜像 A（`console=ttyS0 earlyprintk=serial`）里，控制台是“先 earlycon，后 ttyS0”：

```
[latte] early console registered: earlyser (keep=0)   ← earlycon 先上
[latte] console_init: run __con_initcall section      ← 之后跑驱动初始化
[latte] register_console: ttyS0 flags=…               ← 真正的串口 console
```

之后所有内核日志都从 `ttyS0` 出去；`earlyser` 作为 `CON_BOOT` 临时工被撤掉
（主题三）。用户态的 `=== Mounting essential filesystems ===` 走的是另一条路：
`/dev/console` → tty 层 → 同一个 UART（主题五）。

## 代码位置

| 位置 | 内容 |
|---|---|
| `kernel/printk/printk.c:2219` `vprintk_store()` | 写 ring buffer |
| `kernel/printk/printk.c:3169` `console_flush_all()` | 把未输出的记录推给各 console |
| `kernel/printk/printk.c:3964` `register_console()` | 控制台注册（谁在名单上） |
| `drivers/tty/serial/8250/8250_port.c:3292` `serial8250_console_write()` | 串口写入口 |
| `drivers/tty/serial/8250/8250_port.c:3196` `serial8250_console_wait_putchar()` | 轮询 THRE |
