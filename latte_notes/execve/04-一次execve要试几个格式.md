# 主题四 · 一次 `execve` 要试几个“文件格式”

## 你已知的

可执行文件不止一种：编译出来的二进制（ELF）、`#!/bin/sh` 开头的脚本、Python/Perl
文件……Windows 靠**扩展名**决定谁来跑，Linux 靠**文件头的几个字节**（magic）。
认格式这件事，内核里叫 `binfmt`（binary format）。

## 内核里的机制

`fs/exec.c` 维护一张格式处理器链表，每个处理器提供一个 `load_binary()`：

```
[misc] → [script] → [elf] → [compat_elf]
```

- 注册顺序 = 链接顺序（`fs/Makefile:36-39`）；
- 内核按顺序挨个调用，谁认识谁就接手；
- 返回 `-ENOEXEC` 就是“我不认识，换下一个”；
- 全都不认识 → `execve` 返回 `-8`（`ENOEXEC`）。

## 三种文件，三种真实次数（本次实测）

| 文件 | `binfmt try` 次数 | 过程 |
|---|---|---|
| `/bin/ls`（ELF 静态） | **3** | misc 不认识 → script 不认识 → **elf 接手** |
| `/init`（`#!/bin/sh` 脚本） | **5** | 第 1 轮：misc✗、script✓（改写成 `/bin/sh`）；第 2 轮：misc✗、script✗、**elf 接手** |
| `/init` 去掉 shebang 的文本 | **4** | 4 个处理器全部返回 `-ENOEXEC` → `execve` 失败 `-8` |

## 脚本为什么会让次数翻倍

`binfmt_script` 不自己执行脚本，它只做两件事：

```c
retval = bprm_change_interp(i_name, bprm);   /* 记录解释器路径 */
file = open_exec(i_name);                    /* 打开 /bin/sh */
bprm->interpreter = file;
return 0;                                     /* 注意：返回"成功" */
```

外层 `exec_binprm()` 看到 `bprm->interpreter` 非空，就把 `bprm->file` 换成解释器，
**再来一轮**格式分派：

```c
exec = bprm->file;
bprm->file = bprm->interpreter;
bprm->interpreter = NULL;
```

这就是 `exec_binprm()` 里那个 `depth` 循环存在的理由（`fs/exec.c:1714` 起，最多 5 层防环）。
也解释了日志里 5 条 `binfmt try: /init`：**文件名一直是 `/init`，但第 5 次认的是
`/bin/sh` 的内容**——`bprm->filename` 不会因为改写而更新。

## 认下来之后：入口地址从哪来

ELF 处理器读 ELF 头，两种情形：

| 情形 | 入口 | 本次是否出现 |
|---|---|---|
| 静态链接（`PT_INTERP` 缺失） | `elf_entry = e_entry`（ELF 头里的字段） | ✅ busybox 是静态的 |
| 动态链接（有 `PT_INTERP`） | `elf_entry = 解释器加载位置 + 解释器 e_entry`，由 `ld.so` 再去加载主程序 | 本 rootfs 里没有 `ld.so` |

实测 `load_elf_binary: /init type=ET_EXEC`：`ET_EXEC` 表示固定地址（非 PIE），
所以四次 execve（`/init`、`/bin/mount`、`/proc/self/exe`、`/bin/sh`）的入口
**都是同一个 `0x40b300`**——它们是同一个 busybox。如果换成一个动态链接的程序，
这里会变成 `ld.so` 的入口，然后由 `ld.so` 把控制权交给 `main` 之前的启动代码。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/exec.c:1670` | `[latte] binfmt try:` 打点所在处（挨个试处理器） |
| `fs/exec.c:1688` `exec_binprm()` | `depth` 循环：处理解释器改写 |
| `fs/binfmt_script.c` `load_script()` | 脚本改写成 `[解释器, 脚本, …]` |
| `fs/binfmt_elf.c:836` `load_elf_binary()` | 校验、映射、算 `elf_entry` |
