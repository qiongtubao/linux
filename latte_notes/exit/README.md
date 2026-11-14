# 进程的结束与回收：僵尸是怎么来的

`jobs/` 站讲的是“停下再继续”，这一站讲最后一程：**退出 → 僵尸 → 被 `wait` 收掉**。
用 `SIGSTOP` 停住父进程，成功造出一个 `Z` 进程并把它收掉。
1 份索引 + 4 篇主题文档，2 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| CPU 与进程 | 退出 | [01-进程怎么结束-do_exit](01-进程怎么结束-do_exit.md) | `do_exit` 的六件事，为什么留一个“壳” |
| CPU 与进程 | 回收 | [02-僵尸与回收-为什么需要wait](02-僵尸与回收-为什么需要wait.md) | `Z` 状态、过继给 init、`wait` 家族与标志 |
| CPU 与进程 | 代码路径 | [03-代码路径-do_exit到release_task](03-代码路径-do_exit到release_task.md) | 两次事件、两个触发者；`code` 是信号号 |
| CPU 与进程 | 实测 | [04-实测-造一个僵尸再收掉它](04-实测-造一个僵尸再收掉它.md) | `SIGSTOP` 停住父进程 → 抓到 `Z` → 收掉 |

## 一条链

```
子进程：do_exit()                     ← 打点 do_exit: comm code=N
          ├─ 释放文件/内存/信号
          ├─ exit_notify()：发 SIGCHLD、孩子过继给 init
          ├─ exit_state = EXIT_ZOMBIE   （变 Z）
          └─ schedule()                 （不再被调度）

父进程：wait4() → do_wait() → wait_consider_task()
          └─ release_task()             ← 打点 release_task: comm
                （摘进程表、释放 task_struct 与内核栈）
```

## 实测一瞥

| 阶段 | 观测 |
|---|---|
| 内层 shell 起 `sleep 0.2 &` + 前台 `sleep 100`，然后 `SIGSTOP` 它 | `/proc` 扫描到 `pid=59 state=Z comm=(busybox)` |
| `SIGCONT` + `SIGKILL` 内层 shell | 打点 `do_exit: busybox code=9`；再扫描已无 `Z` |
| 全日志统计 | `do_exit` 118 次 / `release_task` 118 次，一一配对 |

## 打点清单（2 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `do_exit: %s code=%ld` | `kernel/exit.c` | 118 次（含 `code=9` 的那次） |
| `release_task: %s` | `kernel/exit.c` | 118 次（与上面配对） |

## 自查 3 问

1. 为什么僵尸几乎不占内存却必须被回收？（02：占 pid 与进程表项，数量无上限）
2. `do_exit` 的 `code` 与 shell 的 `$?` 是一回事吗？（03：不是，`code` 是信号号或退出码，
   `128+N` 是 shell 自己算的）
3. 僵尸怎么才能消失？（02：父进程 `wait`，或父进程死掉后过继给 init 由它收）

## 下阶段

至此五个领域都已成体系。还可以继续挖的点：**信号阻塞与实时信号排队**（`sigprocmask`）、
**zram/zswap**（需临时开 `CONFIG_ZRAM`）、**网络收包**（`netif_receive_skb`/NAPI，
需要给 QEMU 加网卡，是唯一还没碰过的大领域）。
