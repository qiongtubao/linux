# 主题三 · 代码路径：从 `do_exit` 到 `release_task`

## 你已知的

子进程退出后变成僵尸，父进程 `wait` 之后僵尸才消失。中间是两个函数、两个触发者：
**子进程触发 `do_exit`，父进程触发 `release_task`**。

## 第一段：子进程自己走（`do_exit`）

```
do_exit(code)                          kernel/exit.c:901（打点）
 ├─ synchronize_group_exit()           多线程组一起退出
 ├─ exit_mm()                          释放地址空间（页表、页）
 ├─ exit_files() / exit_fs() / exit_sem()
 ├─ exit_shm() / exit_tasks_rcu_start()
 ├─ exit_notify(tsk, group_dead)       kernel/exit.c:738
 │   ├─ forget_original_parent()       孩子过继给 init
 │   ├─ 给父进程发 SIGCHLD
 │   └─ 决定 autoreap（父进程不要了就直接收）
 ├─ tsk->exit_state = EXIT_ZOMBIE       ← 变 Z
 └─ schedule()                         再也不回来
```

`do_exit()` 是 `__noreturn`：它不会返回，最后由调度器切到别人。

## 第二段：父进程来收（`wait4` → `release_task`）

```
wait4(pid, &status, options, rusage)
 └─ do_wait(&wo)                       kernel/exit.c:1710
     ├─ 扫子进程：wait_consider_task()
     │   ├─ 已退出（Z）→ 取 exit_code → release_task()   kernel/exit.c:245（打点）
     │   ├─ 已停止且 WUNTRACED → 报告停止（jobs/ 站）
     │   └─ 都没变化 → WNOHANG 就返回 0，否则睡在 wait_chldexit 上
     └─ release_task()：从 pid 哈希/进程表摘掉、释放 task_struct 与内核栈
```

## 两个函数的分工，一张表看清

| | `do_exit()` | `release_task()` |
|---|---|---|
| 谁调用 | 退出者自己 | 父进程（`wait` 路径）或过继后的 init |
| 释放什么 | 文件、内存、信号、定时器 | `task_struct`、内核栈、pid |
| 之后进程状态 | `Z`（还在进程表里） | 完全消失 |
| 打点 | `do_exit: comm code=N` | `release_task: comm` |

## 打点怎么读

```
[latte] do_exit: busybox code=0        ← 正常退出
[latte] release_task: busybox          ← 1ms 后被父进程收掉
[latte] do_exit: busybox code=9        ← 被 SIGKILL 杀掉（code = 信号号）
```

- `code=0`：正常退出码 0；
- `code=9`：被 9 号信号杀死（`do_exit` 收到的是信号号，不是 128+9——那是 shell 的算法）；
- **两行之间的时间差**就是“僵尸存活时间”。正常情况 1ms，被停住的父进程可能几秒。

## 为什么僵尸杀不掉

`kill` 只能给进程投递信号，而僵尸已经不再被调度、也没有 `mm`——信号处理逻辑直接跳过它。
唯一的出路是父进程 `wait`（或父进程死掉、过继给 init 被收）。

## 代码位置

| 位置 | 内容 |
|---|---|
| `kernel/exit.c:901` `do_exit()` | 退出（打点） |
| `kernel/exit.c:738` `exit_notify()` | 通知/过继 |
| `kernel/exit.c:1710` `do_wait()` | `wait4(2)` 实体 |
| `kernel/exit.c:1442` `wait_consider_task()` | 逐个判断子进程状态 |
| `kernel/exit.c:245` `release_task()` | 收尸（打点） |
| `include/linux/sched.h` | `EXIT_ZOMBIE` / `exit_state` |
