# tty 与信号：`Ctrl-C` 是怎么变成一个信号的

上一站（`devices/`）看到 initramfs 的 shell 打印 `can't access tty`，这一站把它做成实验：
**同一句 `Ctrl-C`，为什么有时能杀进程、有时完全没反应？**
4 篇主题文档，3 个有界打点，三次启动做对照。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 设备 | 终端是什么 | [01-终端是什么-tty与行规程](01-终端是什么-tty与行规程.md) | UART → tty → 行规程：回显、行缓冲、控制字符 |
| 设备 | 一个字符的旅程 | [02-一个字符的旅程](02-一个字符的旅程.md) | 中断读字节 → flip buffer → `n_tty` → `read()` |
| CPU 与进程 | 控制终端 | [03-控制终端与会话](03-控制终端与会话.md) | 会话/pgrp/ctty 三层；`tty_get_pgrp()` 为空就没人收信号 |
| 设备 | 实测 | [04-实测-Ctrl-C的两种命运](04-实测-Ctrl-C的两种命运.md) | `pgrp=none` vs `pgrp=set`，`rc=143` vs `rc=130` |

## 一条链

```
UART 收到 0x03
 └─ IRQ 4 → serial8250_interrupt() → flip buffer
     └─ n_tty_receive_buf_common()        ← 打点 n_tty_rx
         └─ n_tty_receive_char_special()
             └─ isig(SIGINT)              ← 打点 isig
                 └─ __isig()              ← 打点 __isig（pgrp=set/none）
                     └─ kill_pgrp()       ← 真正投递
```

## 三次启动的对照

| 场景 | `tty_get_pgrp()` | `isig` 是否触发 | 结局 |
|---|---|---|---|
| init 直接 `cat`（无 ctty） | `none` | 是 | 只被 `timeout` 杀 → `rc=143` |
| ctty 建在 `/dev/console` | `none` | 是 | 收不到信号 |
| ctty 建在 `/dev/ttyS0` | `set` | 是 | `Ctrl-C` 生效 → `rc=130` |

## 打点清单（3 个）

| 打点 | 位置 | 频率 |
|---|---|---|
| `n_tty_rx: %zu bytes` | `drivers/tty/n_tty.c` | 每批输入一次（本次几次） |
| `isig: sig=%d` | 同上 | 每个控制字符一次 |
| `__isig: sig=%d pgrp=%s` | 同上 | 每个控制字符一次，直接暴露“有没有前台进程组” |

**刻意不在 `serial8250_interrupt()` 里打点**：那是每次按键/每个字节都走的中断上下文，
不适合 `printk`。

## 自查 3 问

1. `isig` 触发了但进程没死，问题在哪？（03/04：`tty_get_pgrp()` 为空 → 没有前台进程组）
2. 为什么把 ctty 建在 `/dev/console` 不管用？（03：字符从 `/dev/ttyS0` 进，两个
   `tty_struct` 各记各的 pgrp）
3. `rc=130` 与 `rc=143` 分别代表什么？（04：128+2 SIGINT；128+15 SIGTERM）

## 下一阶段

两条都补上了：**作业控制**（`Ctrl-Z`、`fg/bg`，需要完整 job control）见 `jobs/01`~`04`，
**信号排队与投递**（`sigprocmask`、标准信号 vs 实时信号、sigframe）见 `jobs/05`。
还没做的：**signalfd/pselect**（把信号当 fd 读）、**行规程的 `termios` 细节**
（`ONLCR`/`ICANON`/`VMIN`）。
