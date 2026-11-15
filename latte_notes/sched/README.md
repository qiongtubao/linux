# 调度器：谁决定下一个跑谁

前面几站讲“进程怎么开始（`execve`）、怎么停（`jobs`）、怎么结束（`exit`）”，
这一站讲**它们怎么轮流用 CPU**。两篇：什么时候换人（阻塞 vs 抢占），
以及“公平”到底按什么分（EEVDF 的权重与切片）。
1 份索引 + 2 篇主题文档，2 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| CPU 与进程 | 调度时机 | [01-什么时候换人-两种让出与抢占](01-什么时候换人-两种让出与抢占.md) | `voluntary` 自己睡，`nonvoluntary` 被人抢 |
| CPU 与进程 | 公平性 | [02-公平性-EEVDF与nice权重](02-公平性-EEVDF与nice权重.md) | nice 是权重；`slice=0.7ms`；差距 68 倍 |

## 一条链

```
唤醒                        让出                          换人
try_to_wake_up          __schedule()                  finish_task_switch()
（进 runqueue）          ├─ 阻塞：state≠RUNNING        ├─ prev_state 记下让出原因
                         │   （自己调用 schedule）      ├─ 清 prev 的 on_cpu / mm
                         └─ 抢占：state==RUNNING       └─ 新任务继续跑
                             （tick / 唤醒时置
                              TIF_NEED_RESCHED）
                              └─ context_switch：
                                 switch_to（换栈/寄存器）
```

## 打点清单（2 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `wake: comm=… pid=… state=…` | `kernel/sched/core.c` `try_to_wake_up()` | 只打用户进程（`p->mm`），探针相关 16 次 |
| `switch: prev(state=…) -> next` | `kernel/sched/core.c` `finish_task_switch()` | 只打“主动让出 + 用户进程”，探针相关 4 次 |

两个都加了过滤器，而且**必须加**：不过滤时 `try_to_wake_up` 被 `rcu_preempt`
这类内核线程反复命中，2 核启动 90 秒都跑不完（详见 01 的“日志量”一节）。

## 实测两组数字

| 观察点 | 数值 |
|---|---|
| `usleep(200ms)` 前后 `voluntary_ctxt_switches` | 0 → **1** |
| 同 CPU 加一个竞争者，忙等 500ms 后 `nonvoluntary_ctxt_switches` | 1 → **128** |
| nice 0 vs nice 19（绑同一 CPU，各跑 2s）的 `utime` | **196 : 3** ticks |
| 同上两进程的 `sum_exec_runtime` | **1.977 s : 0.034 s** |

## 自查 3 问

1. `voluntary` 和 `nonvoluntary` 的上下文切换差在哪？（01：自己调 `schedule()`
   让出 vs 被 tick/唤醒打断后强制让出）
2. `sleep 1` 的进程为什么“不占 CPU”？（01：`state` 变成可中断睡眠，
   根本不在 runqueue 里，调度器选不到它）
3. nice 0 与 nice 19 平分 2 秒会各拿多少？（02：权重 1024:15 ≈ 68:1，
   实测 1.977s : 0.034s；jiffy 粒度下是 196:3 ticks）

## 下一阶段

调度这块还能挖：**实时调度**（`SCHED_FIFO`/`SCHED_RR` 抢占普通任务，需要 root
与 `sched_setscheduler`）、**多核负载均衡**（`-smp 4` 下 `sched_migrate_task` 打点）、
**cgroup CPU 控制**（`cpu.max`，`CONFIG_CGROUP_SCHED=y` 已开）、
**唤醒抢占**（`WAKEUP_PREEMPTION` 与 `sched_wakeup` tracepoint 的 `success` 字段）。
