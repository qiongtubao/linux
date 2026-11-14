# 主题五 · 最后一跳：`iretq` 回到用户态

## 你已知的

CPU 在内核态和用户态之间切换，靠的是一对指令：进门用 `syscall`/中断，出门用
`iretq`（或 `sysret`）。`iretq` 会一次性恢复：`CS:RIP`（从哪继续执行）、`RSP`
（用哪个栈）、`RFLAGS`（中断开关等）、`SS`（用哪个栈段）。所以“进用户态”在 CPU
层面就是：**把寄存器改好，然后执行 `iretq`**。

## 内核里对应什么

每个进程都有一份 `pt_regs`（寄存器快照，内核栈上）。`execve` 的最后一步就是改写它：

```c
void start_thread(struct pt_regs *regs, unsigned long new_ip, unsigned long new_sp)
{
	start_thread_common(regs, new_ip, new_sp, __USER_CS, __USER_DS, 0);
}
```

`start_thread_common()` 里真正改的四个字段：

| 字段 | 改成 | 含义 |
|---|---|---|
| `regs->ip` | 程序入口（本次 `0x40b300`） | 落地执行的第一条指令 |
| `regs->sp` | 新栈顶（`bprm->p`） | 栈上已经摆好 `argc/argv/envp/auxv` |
| `regs->csx` | `__USER_CS` | 切到用户代码段（特权级 3） |
| `regs->ssx` | `__USER_DS` | 切到用户栈段 |
| `regs->flags` | `X86_EFLAGS_IF` 等 | 开中断、清掉内核态标志 |

## 1 号进程的特殊之处

普通程序回到用户态，是**系统调用返回**：`syscall` 进来 → 内核干活 → 改 `rax` →
`sysret`/`iretq` 回去。1 号进程不是：它是 `kernel_init()` 这个内核 C 函数里调
`execve`，然后函数一路返回，最后走到内核线程的返回路径（`ret_from_fork` →
`syscall_exit_to_user_mode`），同样是 `iretq`——**只是这次 `pt_regs` 已经被
`start_thread()` 改写成了 busybox 的入口**。

所以“1 号进程从内核态进入用户态”这件事，表面上是 `return 0`，实际上是一次
寄存器全量替换 + `iretq`。`kernel_init()` 里 `if (!ret) return 0;` 那行之所以
看起来“就这么结束了”，原因在此。

## 实测证据

镜像 A 启动 1 号进程时：

```
[latte] START_THREAD: ip=0x40b300 sp=0x7fff4ade2b70
[latte] start_thread: ip=0x40b300 sp=0x7fff4ade2b70
```

两行紧挨着：`START_THREAD` 是宏（`fs/binfmt_elf.c:1389`），它最终调用
`start_thread()`（`arch/x86/kernel/process_64.c:587`）。同一时刻，日志里还没有
任何用户态输出——直到 100ms 后才出现 `=== Mounting essential filesystems ===`。

| 时刻 | 事件 |
|---|---|
| 4.166s | `start_thread: ip=0x40b300 sp=0x7fff…`（寄存器已改写，紧接着 `iretq`） |
| ~4.23s | busybox 的 sh 跑起来，打印 `=== Mounting essential filesystems ===` |
| 4.236s | `do_execveat_common`：脚本里的 `mount -t proc …` 开始执行 |

从 `start_thread` 到第一行用户态输出约 70ms，就是 busybox 静态启动的开销
（静态链接，没有 `ld.so` 参与）。

## 和“换根”那一步的区别

| 动作 | 改的是什么 | 谁在做 |
|---|---|---|
| `start_thread()` | 寄存器（`RIP/RSP/CS/SS`） | `execve` 路径 |
| `MS_MOVE` + `init_chroot()` | 目录树的根 | `prepare_namespace()`（上一站） |

前者让“同一个进程开始跑新程序”，后者让“同一个 `/` 变成另一个文件系统”。
两者互不相干，别混。

## 代码位置

| 位置 | 内容 |
|---|---|
| `arch/x86/kernel/process_64.c:587` `start_thread()` | 改写 `pt_regs` 的入口 |
| `arch/x86/kernel/process_64.c:531` `start_thread_common()` | 四个字段 + `RFLAGS` |
| `fs/binfmt_elf.c:1389` `START_THREAD()` 调用点 | 入口与栈顶来源 |
| `fs/exec.c:1714` 起 `exec_binprm()` | 成功后返回到这里，交给内核返回路径 |
