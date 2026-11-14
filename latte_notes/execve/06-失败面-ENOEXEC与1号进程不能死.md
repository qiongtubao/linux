# 主题六 · 失败面：`ENOEXEC`、逐级回退、1 号进程不能死

## 三条失败线

| 失败 | 长度 | 谁报错 | 后续 |
|---|---|---|---|
| 格式不认识（不是 ELF/脚本） | `-8` `ENOEXEC` | `search_binary_handler()` | `Failed to execute … (error -8)`，换下一个候选 |
| 候选文件不存在 | `-2` `ENOENT` | `alloc_bprm()` 里 `do_open_execat()` 打开失败（bprm 还没建就走人） | **静默跳过**（`try_to_run_init_process()` 只报非 ENOENT） |
| 全部候选都失败 | — | `kernel_init()` | `panic("No working init found …")` |
| 1 号进程成功启动后退出 | — | `do_exit()` | `panic("Attempted to kill init!")` |

## 实验 C：`rdinit=/bin/ls`

把 init 换成一个跑完就退出的程序：

```
Run /bin/ls as init process
[latte] kernel_execve: /bin/ls (PF_KTHREAD=0)
[latte] binfmt try: /bin/ls …… （3 次）
[latte] load_elf_binary: /bin/ls type=ET_EXEC(固定地址)
[latte] START_THREAD: ip=0x40b300 sp=0x7ffd785ef2b0
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000000
```

`execve` 完全成功（有 `START_THREAD` 就是成功），`ls` 也正常跑完；但 1 号进程一退出，
内核就失去“用户态的起点”，只能 panic。**这是唯一一个“execve 成功却依然停机”的场景**。

## 实验 D：`/init` 是没有 shebang 的纯文本

```
Run /init as init process
[latte] kernel_execve: /init (PF_KTHREAD=0)
[latte] binfmt try: /init …… （4 次，全部 -ENOEXEC）
Failed to execute /init (error -8)
Run /sbin/init as init process     ← 不存在，静默跳过
Run /etc/init as init process      ← 不存在，静默跳过
Run /bin/init as init process      ← 不存在，静默跳过
Run /bin/sh as init process        ← 这个存在
[latte] START_THREAD: ip=0x40b300 sp=0x7ffc16493e70
/bin/sh: can't open 'nokaslr': No such file or directory
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000200
```

三个值得记住的点：

1. **回退链是 `/sbin/init` → `/etc/init` → `/bin/init` → `/bin/sh`**（`init/main.c:1610`），
   不存在的候选不会打印 “Failed to execute”，只有“存在但执行失败”的才打印。
2. **`argv[1]` 是内核命令行留下的**：`/bin/sh` 被叫起来时参数是
   `["/bin/sh", "nokaslr"]`，于是 busybox 的 sh 把 `nokaslr` 当成脚本文件去打开，
   报 `can't open 'nokaslr'`，退出码 2（`0x200 >> 8`）。这就是内核命令行里多出来的词
   会变成 init 参数的原因。
3. **仍然不是 “No working init found”**：因为 `/bin/sh` 成功启动了，只是它自己退出了。
   只有**所有**候选都 execve 失败，才会走到那句 panic。

## 错误码对照

| 返回 | 含义 | 常见原因 |
|---|---|---|
| `-8` `ENOEXEC` | 没有格式处理器认识这个文件 | 文本没有 `#!`、架构不符、ELF 损坏 |
| `-2` `ENOENT` | 文件/解释器不存在 | 候选路径不存在；动态链接的 `ld.so` 不在 rootfs 里 |
| `-13` `EACCES` | 没有执行权限 | 丢了 `x` 位 |
| `-ELOOP` | 解释器改写超过 5 层 | 脚本互相 `#!` 套娃 |

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/exec.c:1670` | 每次 `binfmt try` 的地方，最后一个成功后才有 `START_THREAD` |
| `init/main.c:1441` `run_init_process()` | `Run %s as init process` + `kernel_execve` |
| `init/main.c:1455` `try_to_run_init_process()` | 只对非 `ENOENT` 打错误 |
| `init/main.c:1610` | `/sbin/init` → `/etc/init` → `/bin/init` → `/bin/sh` 回退链 |
| `init/main.c:1617` | `panic("No working init found …")` |

## 一个诊断技巧

`START_THREAD` 是“execve 真的成功了”的分界线：

- 有 `START_THREAD` → 新程序已经在跑（哪怕它下一秒就退出）；
- 只有 `binfmt try`、没有 `START_THREAD` → 格式没认下来，看 `binfmt try` 的条数：
  3 次说明它是 ELF 但后面失败了，4 次说明所有处理器都不认识。
