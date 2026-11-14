# 主题一 · 岔路口：`init_eaccess("/init")` 决定走哪条路

## 决策点只有一处

`kernel_init_freeable()` 在 initcall 跑完、initramfs 解压落地之后，做一次“探路”：

```c
int ramdisk_command_access;
ramdisk_command_access = init_eaccess(ramdisk_execute_command);
if (ramdisk_command_access != 0) {
	pr_warn("check access for rdinit=%s failed: %i, ignoring\n", ...);
	ramdisk_execute_command = NULL;
	prepare_namespace();
}
```

`ramdisk_execute_command` 默认就是 `"/init"`（`init/main.c:164`），可以用 `rdinit=`
改。`init_eaccess()`（`fs/init.c`）是“仿 access(2)”的内核版：`kern_path()` 查路径
再用 `MAY_ACCESS` 判权限，返回 0 表示“这个文件存在且可访问”。

## 两种结果，两条完全不同的启动路径

| `init_eaccess` 返回值 | 含义 | 后续 |
|---|---|---|
| `0` | initramfs 里真有 `/init` | 清空分支，`ramdisk_execute_command` 保留，稍后 `kernel_init()` 直接 `kernel_execve("/init")` |
| `-ENOENT`（-2）等 | 没有可用 init | 打一句 warn、把 `ramdisk_execute_command` 置 NULL，转去 `prepare_namespace()` 挂真正的根 |

注意 `ramdisk_execute_command` 被置 NULL 的后果：后面 `kernel_init()` 里
`if (ramdisk_execute_command)` 不再成立，于是逐级退到 `init=`、`/sbin/init`、
`/etc/init`、`/bin/init`、`/bin/sh`；一个都跑不起来就 `panic("No working init
found...")`（主题六）。

## 为什么本次这一问是 0

本次用的 `bb_initramfs.cpio` 里有 `/init`（busybox 脚本），所以：

```
[latte] init_eaccess(/init) = 0        ← 探到了，直接交给它
（prepare_namespace 的任何打点都没有出现）
Run /init as init process
```

把同一个 rootfs 去掉 `/init` 重新打包后（对照实验，见索引“实测对照”）：

```
[latte] init_eaccess(/init) = -2       ← ENOENT
[latte] prepare_namespace: enter real-root path
```

**一行返回值，就是两台完全不同的启动机器。**

## 一个容易忽略的点

`init_eaccess()` 只判“存在且可访问”，不判“能不能执行”。真正的执行失败发生在
`kernel_execve()` 里，那时 `kernel_init()` 会 `pr_err("Failed to execute %s (error
%d)")` 然后继续试别的 init 名字。所以这里的 0 只保证“有东西可试”。

## 代码位置与打点

| 位置 | 打点 |
|---|---|
| `init/main.c:1701` `init_eaccess()` 调用 | `[latte] init_eaccess(%s) = %d` |
| `init/main.c:1711` 分支内、`prepare_namespace()` 之前 | `[latte] prepare_namespace: enter real-root path` |

两个打点不会同时出现：有 `/init` 就只有第一条，没有 `/init` 才两条都有。
