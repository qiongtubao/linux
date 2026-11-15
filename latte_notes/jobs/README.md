# 进程状态与作业控制：`Ctrl-Z`、`kill -TSTP` 与“孤儿进程组”

上一站（`tty/`）讲清楚 `Ctrl-C` 走的是行规程。这一站看它的兄弟：**停止与继续**。
同一句 `kill -TSTP`，在没有控制终端时被内核**丢弃**，有控制终端时进程立刻变成 `T`。
最后补上**信号投递**：发信号与投递是两条链，handler 的现场是内核在你栈上摆的。
1 份索引 + 5 篇主题文档，5 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| CPU 与进程 | 状态 | [01-进程状态与停止信号](01-进程状态与停止信号.md) | R/S/D/T/Z 字母表；SIGSTOP 不可捕获；孤儿组丢弃停止信号 |
| CPU 与进程 | 作业控制 | [02-作业控制是什么](02-作业控制是什么.md) | 进程组 + 会话 + 终端前台组；`fg`/`bg` 的实质 |
| CPU 与进程 | 代码路径 | [03-代码路径-停止与继续](03-代码路径-停止与继续.md) | `get_signal` → 孤儿检查 → `do_signal_stop` → 让出 CPU |
| CPU 与进程 | 实测 | [04-实测-SIGTSTP的两种命运](04-实测-SIGTSTP的两种命运.md) | 两种环境下的状态与打点对照 |
| CPU 与进程 | 投递 | [05-信号投递-内核怎么把信号摆到用户栈上](05-信号投递-内核怎么把信号摆到用户栈上.md) | 标准信号发 2 投 1、实时信号发 2 投 2；frame 在用户栈 |

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

投递那一段（05）单独拎出来看，它发生在**每次回用户态之前**：

```
kill(pid, SIG)
 └─ __send_signal_locked            标准信号已 pending → 短路；实时信号入队   ← 打点
      （中间隔着：进程什么时候回用户态）

exit_to_user_mode_loop  _TIF_SIGPENDING
 └─ arch_do_signal_or_restart → get_signal → handle_signal   ← 打点
     └─ setup_rt_frame → x64_setup_rt_frame                  ← 打点
         └─ 用户栈上出现 rt_sigframe（ucontext + sigcontext + 掩码 + pretcode）
```

## 两次启动的对照

| | A：无控制终端 | B：`setsid` + ctty=`ttyS0` |
|---|---|---|
| `set -m` | 失败（`can't access tty`） | `rc=0` |
| `kill -STOP` | `state=T`，打点 `signr=19` | —— |
| `kill -TSTP` | **被丢弃**，`state` 仍 `S` | `state=T`，打点 `signr=20` |
| `kill -CONT` | 回到 `S` | 回到 `S` |

## 打点清单（5 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `do_signal_stop: signr=%d comm=%s` | `kernel/signal.c` | A：1 次（`signr=19`）；B：1 次（`signr=20`） |
| `send_signal: sig=… shared=…` | `kernel/signal.c` | 标准 2 次 + 实时 2 次（发几次记几次） |
| `handle_signal: sig=…` | `arch/x86/kernel/signal.c` | 标准 **1** 次、实时 **2** 次 |
| `setup_rt_frame: sig=…` | `arch/x86/kernel/signal.c` | 同上 |
| `setup_sigcontext: sig=… sp=…` | `arch/x86/kernel/signal_64.c` | 同上，`sp` 两次相同 |

## 自查 4 问

1. `kill -TSTP` 完全没反应，最可能的原因是什么？（01/04：孤儿进程组，内核按 POSIX 丢弃）
2. `SIGSTOP` 和 `SIGTSTP` 差在哪？（01：前者不可捕获/忽略，也不受孤儿规则限制）
3. `fg` 到底做了哪两件事？（02：`tcsetpgrp` 设前台组 + `SIGCONT`）
4. 连续 `kill` 两次 `SIGUSR1`，handler 为什么只跑一次？（05：标准信号只有一位 pending，
   第二次在 `legacy_queue()` 被短路；实时信号才排队）

## 下一阶段

同类还剩：**`wait4` 与僵尸**（`WUNTRACED`/`WNOHANG`，把 `Z` 状态走完）、
**`signalfd`/`pselect` 与信号阻塞**（把信号当文件描述符读，和 `05` 接得上）。
**信号队列与阻塞**已经补上了，见 `05`；**网络收包路径**也已补完（`net/01`~`06`）。
换域可以做 **zram/zswap**（需临时开 `CONFIG_ZRAM`）或 **调度器**（`sched_switch`，需 `-smp 2`）。
