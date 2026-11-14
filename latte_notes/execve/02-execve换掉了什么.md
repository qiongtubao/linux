# 主题二 · `execve` 到底换掉了进程的什么

## 你已知的

Windows 里“打开一个 exe”会创建**新进程**，旧进程还在。Linux 的 `execve` 不是这样：
它是**原地换人**——不创建新进程，而是把当前进程“掏空”，换成另一个程序。证据就是
`pid` 不变：`sh` 里执行 `exec ls`，跑的还是同一个 pid。

## 换了什么、留下什么

| 被换掉 | 被保留 |
|---|---|
| 地址空间：代码段、数据段、堆、栈（`mm` 整体换新） | `pid`、父子关系 |
| 页表、已映射的文件 | 进程的 `nice` 值、会话、控制终端 |
| 信号处理函数（恢复默认）、待处理信号 | 未 `FD_CLOEXEC` 的文件描述符 |
| 凭据（setuid 程序会把 euid 变成文件所有者） | 当前工作目录、`umask` |
| CPU 寄存器（`RIP` 变成新入口、`RSP` 变成新栈顶） | 资源限制（`rlimit`） |

一句话：**“进程是谁”不变，“进程在跑什么”全换**。

## 换 `mm` 的那一刻，就是“无返回点”

`begin_new_exec()` 里这一行真正执行切换：

```c
retval = exec_mmap(bprm->mm);     /* current->mm 换成新程序的空地址空间 */
```

在此之前，旧程序还完整地活着，失败可以原样返回（用户态拿到 `-ENOEXEC` 继续跑老程序）；
之后旧地址空间已被丢弃，任何错误都只能杀掉这个进程：

```c
/*
 * Ensure all future errors are fatal.
 */
bprm->point_of_no_return = true;
```

所以顺序是“**先把新程序准备好（新地址空间 + 已经摆好的参数栈），再一次性切换**”：
`bprm->mm` 在很早的 `bprm_mm_init()` 里就建好了，参数也在里面摆好了（主题三）。

## 六个打点对应六个阶段

镜像 A 启动 1 号进程时的真实顺序：

```
[latte] kernel_execve: /init (PF_KTHREAD=0)        ← 入口：内核态发起的 execve
[latte] kernel_execve: strings ready, argc=2 envc=2 ← argv/envp 已压进新栈
[latte] bprm_execve: /init going to exec_binprm     ← 凭据/安全检查通过
[latte] binfmt try: /init ……                        ← 按格式挑处理器（主题四）
[latte] load_elf_binary: /init type=ET_EXEC         ← 映射段、算入口
[latte] begin_new_exec: /init (comm=swapper/0)      ← 换 mm（注意 comm 还是旧的）
[latte] START_THREAD: ip=0x40b300 sp=0x7fff…        ← 改写寄存器（主题五）
[latte] start_thread: ip=0x40b300 sp=0x7fff…
```

`begin_new_exec` 那行 `comm=swapper/0` 很有意思：它说明**打点这一刻进程还叫旧名字**
（1 号进程从 idle 手上继承了 `comm`），换成 busybox 是后面才发生的事。

同一份日志里还有用户态自己发起的 execve（`/bin/mount`、`/proc/self/exe`、`/bin/sh`），
它们比内核这次多一行 `do_execveat_common`：因为 `kernel_execve()` 直接跳到
`bprm_execve()`，而用户态的 `execve(2)` 要先经过 `do_execveat_common()` 处理用户指针。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/exec.c:1874` `kernel_execve()` | 内核态入口（1 号进程走这条） |
| `fs/exec.c:1788` `do_execveat_common()` | 用户态入口，处理 argv/envp 用户指针 |
| `fs/exec.c:1734` `bprm_execve()` | 凭据、安全检查、`in_execve` 标记 |
| `fs/exec.c:1103` `begin_new_exec()` | `exec_mmap()` 换地址空间 + 无返回点 |
