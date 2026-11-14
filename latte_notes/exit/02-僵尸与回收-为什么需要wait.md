# 主题二 · 僵尸与回收：为什么每个进程都要被 `wait`

## 你已知的

`ps` 里偶尔能看到 `defunct` / `Z` 的进程，kill 也杀不掉。那个就是僵尸：**已经死了，
但没人给它收尸**。

## 僵尸是什么

| 部分 | 状态 |
|---|---|
| 代码、数据、页表、文件 | 已释放（`do_exit` 里做完了） |
| `task_struct`（进程表项）、pid、退出码 | **还留着** |

所以僵尸几乎不占内存（就一个结构体），但**占 pid**，而且没有上限的话会耗尽 pid 空间。

## 谁负责收尸

```
子进程退出 → 变 Z → 发 SIGCHLD 给父进程
父进程调用 wait()/waitpid()
 └─ 取到退出码与状态
     └─ release_task()：摘掉进程表项、释放 task_struct 与内核栈
```

父进程不 `wait` 会怎样：

| 情况 | 结果 |
|---|---|
| 父进程还活着但不 wait | 僵尸一直挂着（`ps` 显示 `Z`） |
| 父进程退出了 | 僵尸被**过继给 init（1 号）**，由 init 收尸 |
| 父进程自己就是 init 的 zombie？ | init 会 reap 所有被过继的孩子 |

## `wait` 家族与常用标志

| 调用 | 作用 |
|---|---|
| `wait(&status)` | 等任意一个子进程结束 |
| `waitpid(pid, &status, 0)` | 等指定子进程（或 `-pgrp`、`-1`） |
| `WNOHANG` | 不阻塞，立即返回（轮询用） |
| `WUNTRACED` | 子进程“停止”也算状态变化（作业控制用，见 `jobs/` 站） |

退出码从 `status` 里解出来：`WIFEXITED(s)` / `WEXITSTATUS(s)` /
`WIFSIGNALED(s)` / `WTERMSIG(s)`。shell 的 `$?` 和 `128+信号` 那套就是这么来的
（`jobs/04` 里 `rc=130` = 128+SIGINT）。

## 本次实验怎么“造”一个僵尸

正常情况很难看到僵尸：shell 会及时 reap。所以实验里用了一步技巧：

1. 启动一个内层 shell，它先起一个 0.2 秒就退出的后台子进程，再前台 `sleep 100`；
2. 用 `SIGSTOP` **把内层 shell 停住**（`jobs/` 站验证过 SIGSTOP 一定生效）；
3. 内层 shell 停了就不能 `wait`，它那个已退出的子进程于是留在 `Z`；
4. 恢复 + `SIGKILL` 内层 shell → 僵尸被过继给 init → 立刻被收掉。

这样一来，“有僵尸 / 没僵尸”完全可控，`/proc/*/stat` 第三列就能读到 `Z`。

## 与前面几站的衔接

| 现象 | 上游文档 |
|---|---|
| `Z` 是状态字母之一 | `jobs/01`（状态表） |
| `SIGSTOP` 停住父进程 | `jobs/03`（`do_signal_stop`） |
| 过继给 1 号进程 | `init-process.md`（1 号进程是什么） |

## 代码位置

| 位置 | 内容 |
|---|---|
| `kernel/exit.c:901` `do_exit()` | 退出 |
| `kernel/exit.c:738` `exit_notify()` | `SIGCHLD` 与过继 |
| `kernel/exit.c:1710` `do_wait()` | `wait4(2)` 的实体 |
| `kernel/exit.c:245` `release_task()` | 收尸 |
| `include/linux/sched.h` | `struct task_struct`（`exit_code`、`exit_state`） |
