# 进程状态与作业控制：`Ctrl-Z`、`kill -TSTP` 与“孤儿进程组”

上一站（`tty/`）讲清楚 `Ctrl-C` 走的是行规程。这一站看它的兄弟：**停止与继续**。
同一句 `kill -TSTP`，在没有控制终端时被内核**丢弃**，有控制终端时进程立刻变成 `T`。
1 份索引 + 4 篇主题文档，1 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| CPU 与进程 | 状态 | [01-进程状态与停止信号](01-进程状态与停止信号.md) | R/S/D/T/Z 字母表；SIGSTOP 不可捕获；孤儿组丢弃停止信号 |
| CPU 与进程 | 作业控制 | [02-作业控制是什么](02-作业控制是什么.md) | 进程组 + 会话 + 终端前台组；`fg`/`bg` 的实质 |
| CPU 与进程 | 代码路径 | [03-代码路径-停止与继续](03-代码路径-停止与继续.md) | `get_signal` → 孤儿检查 → `do_signal_stop` → 让出 CPU |
| CPU 与进程 | 实测 | [04-实测-SIGTSTP的两种命运](04-实测-SIGTSTP的两种命运.md) | 两种环境下的状态与打点对照 |

## 一条链

```
Ctrl-Z / kill -TSTP
 └─ 行规程 isig() → SIGTSTP            （tty/01、tty/02）
     └─ get_signal()                    kernel/signal.c:2808
         ├─ 孤儿进程组且非 SIGSTOP → 丢弃
         └─ do_signal_stop()           ← 打点 signr=19/20
             └─ schedule：状态变 T
                 └─ kill -CONT → 清标记、唤醒 → 状态回 S
```

## 两次启动的对照

| | A：无控制终端 | B：`setsid` + ctty=`ttyS0` |
|---|---|---|
| `set -m` | 失败（`can't access tty`） | `rc=0` |
| `kill -STOP` | `state=T`，打点 `signr=19` | —— |
| `kill -TSTP` | **被丢弃**，`state` 仍 `S` | `state=T`，打点 `signr=20` |
| `kill -CONT` | 回到 `S` | 回到 `S` |

## 打点清单（1 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `do_signal_stop: signr=%d comm=%s` | `kernel/signal.c` | A：1 次（`signr=19`）；B：1 次（`signr=20`） |

## 自查 3 问

1. `kill -TSTP` 完全没反应，最可能的原因是什么？（01/04：孤儿进程组，内核按 POSIX 丢弃）
2. `SIGSTOP` 和 `SIGTSTP` 差在哪？（01：前者不可捕获/忽略，也不受孤儿规则限制）
3. `fg` 到底做了哪两件事？（02：`tcsetpgrp` 设前台组 + `SIGCONT`）

## 下一阶段

同类可选：**`wait4` 与僵尸**（`WUNTRACED`/`WNOHANG`，把 `Z` 状态走完）、
**信号队列与阻塞**（`sigprocmask`、实时信号排队）。换域可以做 **zram/zswap**
（需临时开 `CONFIG_ZRAM`）或 **网络收包路径**（`netif_receive_skb`，本内核无网卡，
需要加 QEMU 网卡）。
