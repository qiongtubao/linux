# 从内核线程到用户态第一个进程：一次 `execve` 的解剖

上一站讲完“找根”，这一站讲最后一步：内核找到 `/init` 之后，怎么把它跑起来。
答案是 `execve`——一次“原地换人”：`pid` 不变，“在跑什么”全换。
6 篇主题文档，配 9 个 `[latte]` 打点与代码里的中文注释。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| CPU 与进程 | 特权级 | [01-CPU两种特权级与1号进程](01-CPU两种特权级与1号进程.md) | 为什么 1 号进程能进用户态：`user_mode_thread()` 没打 `PF_KTHREAD` |
| CPU 与进程 | 换人 | [02-execve换掉了什么](02-execve换掉了什么.md) | 地址空间/文件表/信号/寄存器全换，`pid` 与 `cwd` 保留 |
| 内存 | 新栈 | [03-新栈是内核替用户摆的](03-新栈是内核替用户摆的.md) | `argc/argv/envp/auxv` 由内核压进新栈，`bprm->p` 就是初值 `RSP` |
| 文件系统 | 格式分派 | [04-一次execve要试几个格式](04-一次execve要试几个格式.md) | ELF 试 3 次、脚本试 5 次、都不是试 4 次 |
| CPU 与进程 | 进用户态 | [05-最后一跳-iretq回用户态](05-最后一跳-iretq回用户态.md) | `start_thread()` 改写 `pt_regs`，`iretq` 落地在入口 |
| CPU 与进程 | 失败面 | [06-失败面-ENOEXEC与1号进程不能死](06-失败面-ENOEXEC与1号进程不能死.md) | `-8`、回退链、init 退出即 panic |

## 一次 `execve` 的流水线

```
kernel_execve()                ← 内核态入口（1 号进程走这条）
 └─ bprm_execve()              ← 凭据、安全钩子、in_execve 标记
     └─ exec_binprm()          ← depth 循环：允许解释器改写后重来
         └─ search_binary_handler()
             ├─ misc ✗  script ✓（把 bprm->file 换成 /bin/sh）
             └─ 再一轮：misc ✗  script ✗  elf ✓
                 └─ load_elf_binary()
                     ├─ 映射 PT_LOAD 段、算 elf_entry
                     ├─ begin_new_exec()  ← 换 mm，无返回点
                     └─ START_THREAD() → start_thread()  ← 改写 RIP/RSP/CS/SS
                          ↓
                        iretq → 用户态执行 busybox 的第一条指令
```

## 三个实验（同一内核，三种 init）

| | A：脚本 `/init` | C：`rdinit=/bin/ls` | D：`/init` 无 shebang |
|---|---|---|---|
| `init_eaccess` | 0 | 0 | 0 |
| `binfmt try` 次数 | 5（ELF 第 5 次接手） | 3 | 4（全部失败） |
| `load_elf_binary` | 有（实际是 busybox） | 有 | 无 |
| `START_THREAD` | `ip=0x40b300` | `ip=0x40b300` | 最后 `/bin/sh` 才成功 |
| 结局 | busybox shell | `Attempted to kill init!` | `can't open 'nokaslr'` → 同样 panic |

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
CMD="console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1"

# A：正常（/init 是脚本）
timeout 30 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd bb_initramfs.cpio \
  -append "$CMD" -nographic -m 2G -no-reboot > /tmp/latte-execA.log 2>&1

# C：让 init 跑完就退出
timeout 30 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd bb_initramfs.cpio \
  -append "$CMD rdinit=/bin/ls" -nographic -m 2G -no-reboot > /tmp/latte-execC.log 2>&1

# D：/init 去掉 shebang，变成纯文本
rm -rf /tmp/badinit && cp -a bb_rootfs /tmp/badinit
printf 'echo hi\n' > /tmp/badinit/init && chmod +x /tmp/badinit/init
( cd /tmp/badinit && find . | cpio -o -H newc --quiet ) > /tmp/bb_badinit.cpio
timeout 30 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd /tmp/bb_badinit.cpio \
  -append "$CMD" -nographic -m 2G -no-reboot > /tmp/latte-execD.log 2>&1
```

## 打点清单（9 个）

| 打点 | 位置 | A 中出现的次数（实测） |
|---|---|---|
| `kernel_execve: %s (PF_KTHREAD=%d)` | `fs/exec.c` | 3（`/init` 1 + `/sbin/modprobe` 2） |
| `kernel_execve: strings ready` | `fs/exec.c` | 1（只有 `/init`；modprobe 在打开文件时就失败） |
| `do_execveat_common: …` | `fs/exec.c` | 4（全是用户态 execve：mount / self-exe ×2 / sh） |
| `bprm_execve: %s going to exec_binprm` | `fs/exec.c` | 5 |
| `binfmt try: %s` | `fs/exec.c` | 17（`/init` 5 + mount 3 + self-exe 6 + sh 3） |
| `begin_new_exec: %s (comm=%s)` | `fs/exec.c` | 5 |
| `load_elf_binary: %s type=%s` | `fs/binfmt_elf.c` | 5 |
| `START_THREAD: ip=… sp=…` | `fs/binfmt_elf.c` | 5 |
| `start_thread: ip=… sp=…` | `arch/x86/kernel/process_64.c` | 5 |

注意 `kernel_execve` 与 `do_execveat_common` 是**两条路**：内核态发起的 execve
（`/init`、modprobe）走前者，用户态 `execve(2)` 走后者，两者在 `bprm_execve()`
汇合。所以 5 次成功 execve = 1 次内核态 + 4 次用户态。

## 自查 3 问

1. 为什么 `kernel_execve()` 里能通过 `PF_KTHREAD` 检查？（01：1 号进程是
   `user_mode_thread()` 造的）
2. 脚本 `/init` 明明只有两行，为什么 `binfmt try` 出现 5 次？（04：第 1 轮 script
   改写解释器，第 2 轮重新分派，3 + 2）
3. `START_THREAD` 之后程序就算成功运行了吗？（06：是。它之后无论成功退出还是崩溃，
   都已经不是“execve 失败”了）

## 下一阶段

`/init` 跑起来之后就是用户态的世界：busybox 挂 `/proc`、`/sys`、`/dev`，
最后 `exec /bin/sh`。再往后可以走两条路：设备与中断（`/proc`、`/dev` 背后的机制），
或者回到内存看这些页怎么被回收。
