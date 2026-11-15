# 调度器：谁决定下一个跑谁

前面几站讲“进程怎么开始（`execve`）、怎么停（`jobs`）、怎么结束（`exit`）”，
这一站讲**它们怎么轮流用 CPU**。五篇：什么时候换人（阻塞 vs 抢占）、
“公平”按什么分（EEVDF 权重）、组和组之间怎么分（cgroup `cpu.weight`）、
不参与公平的实时任务（`SCHED_FIFO` 与 5% 的限流缝），
以及多核上的负载均衡（任务怎么被搬到别的核）。
1 份索引 + 5 篇主题文档，4 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| CPU 与进程 | 调度时机 | [01-什么时候换人-两种让出与抢占](01-什么时候换人-两种让出与抢占.md) | `voluntary` 自己睡，`nonvoluntary` 被人抢 |
| CPU 与进程 | 公平性 | [02-公平性-EEVDF与nice权重](02-公平性-EEVDF与nice权重.md) | nice 是权重；`slice=0.7ms`；差距 68 倍 |
| CPU 与进程 | 分组 | [03-cgroup的CPU权重-组间按份额分](03-cgroup的CPU权重-组间按份额分.md) | `cpu.weight` 100:300 → 份额 1:3；`cpu.max` 本内核没有 |
| CPU 与进程 | 实时 | [04-实时调度-SCHED_FIFO与那5%的缝](04-实时调度-SCHED_FIFO与那5%的缝.md) | RT 独占 95%，普通任务被饿 0.95s |
| CPU 与进程 | 多核 | [05-多核负载均衡-任务怎么被搬到别的核](05-多核负载均衡-任务怎么被搬到别的核.md) | 4 核 4 任务各满速；8 任务各半速；稳态不搬迁 |

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

## 打点清单（4 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `wake: comm=… pid=… state=…` | `kernel/sched/core.c` `try_to_wake_up()` | 只打用户进程（`p->mm`），探针相关 16 次 |
| `switch: prev(state=…) -> next` | `kernel/sched/core.c` `finish_task_switch()` | 只打“主动让出 + 用户进程”，探针相关 4 次 |
| `setscheduler: comm=… policy=… prio=…` | `kernel/sched/syscalls.c` `__sched_setscheduler()` | 1 次（改成 `SCHED_FIFO(1)`） |
| `migrate: comm=… pid=… 旧->新` | `kernel/sched/core.c` `set_task_cpu()` | 89 次（含内核线程）；应用只在启动瞬间搬 |

前两个都加了过滤器，而且**必须加**：不过滤时 `try_to_wake_up` 被 `rcu_preempt`
这类内核线程反复命中，2 核启动 90 秒都跑不完（详见 01 的“日志量”一节）。
`setscheduler` 只在改策略时触发，天然低频，不用过滤。

## 实测数字

| 观察点 | 数值 |
|---|---|
| `usleep(200ms)` 前后 `voluntary_ctxt_switches` | 0 → **1** |
| 同 CPU 加一个竞争者，忙等 500ms 后 `nonvoluntary_ctxt_switches` | 1 → **128** |
| nice 0 vs nice 19（绑同一 CPU，各跑 2s）的 `utime` | **196 : 3** ticks |
| 同上两进程的 `sum_exec_runtime` | **1.977 s : 0.034 s** |
| 两个 cgroup（`cpu.weight` 100 vs 300）各跑 2s 的 `utime` | **50 : 149** ticks（1:3） |
| 同上两组的 `cpu.stat` `usage_usec` | **0.511 s : 1.517 s** |
| `SCHED_FIFO(1)` vs `SCHED_OTHER` 抢同核：RT 的 `utime` | **95 ticks**（正好 95%，来自限流） |
| 同上：普通任务从被唤醒到首次运行的延迟 | **0.952 s**（被饿住） |
| 对照（两个 `SCHED_OTHER`）：`utime` | **50 : 50**，时间窗重叠 |
| 4 核 4 个忙循环（各 2s） | 每任务 `utime` **200 ticks**，四核各 +200 |
| 4 核 8 个忙循环（各 2s） | 每任务 **100 ticks**，每核仍 +200；稳态 `moves=0` |

## 自查 6 问

1. `voluntary` 和 `nonvoluntary` 的上下文切换差在哪？（01：自己调 `schedule()`
   让出 vs 被 tick/唤醒打断后强制让出）
2. `sleep 1` 的进程为什么“不占 CPU”？（01：`state` 变成可中断睡眠，
   根本不在 runqueue 里，调度器选不到它）
3. nice 0 与 nice 19 平分 2 秒会各拿多少？（02：权重 1024:15 ≈ 68:1，
   实测 1.977s : 0.034s；jiffy 粒度下是 196:3 ticks）
4. `cpu.weight=300` 会让一个组“独占三颗核”吗？（03：不会，只是竞争时占三份；
   要封顶得用 `cpu.max`，本内核没开 `CONFIG_CFS_BANDWIDTH`）
5. RT 任务的 `utime` 为什么正好是 95 ticks？（04：`sched_rt_runtime_us/periode`
   = 950000/1000000；剩下 5% 才轮到普通任务，本次普通任务被饿到 0.952 s 之后）
6. 8 个忙循环跑 4 个核，为什么每核还是 200 ticks、任务之间却没被搬来搬去？
   （05：并行度=核数，任务只是分时摊薄；负载均衡只在失衡时搬一次，稳态 `moves=0`）

## 下一阶段

调度这块主干走完了：时机（01）、权重（02）、组权重（03）、实时（04）、多核（05）。

还能挖：**唤醒抢占**（`check_preempt_wakeup`、`WAKEUP_PREEMPTION` 开关对比）、
**调度域拓扑**（`/proc/schedstat`、`-smp 8` 下的 NUMA 层级）、
**`cpu.max` 硬配额**（需重开 `CONFIG_CFS_BANDWIDTH`，见 `03`）、
**RT 限流关掉会怎样**（`echo -1 > /proc/sys/kernel/sched_rt_runtime_us`，危险，会把 shell 一起饿死）。
