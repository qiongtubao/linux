# 主题四 · 岔路：内存根里有 `/init`，还是没有

## 你已知的

内核初始化到最后一步，得找一个**用户态程序**来当 1 号进程——之后所有用户态的东西
都由它派生。问题是：这个程序从哪个盘（或哪个内存文件系统）上找？

候选只有两个来源：

| 来源 | 里面约定的 init 路径 | 谁来跑 |
|---|---|---|
| 内存根（rootfs） | `/init` | 有就直接 `execve`，不用挂任何磁盘 |
| 真正的根（磁盘/网络） | `/sbin/init`、`/etc/init`… | 得先挂根（主题五）再来找 |

## 判据只有一行

内核在 initcall 跑完、initramfs 解压之后，问自己一句：

```c
int ramdisk_command_access;
ramdisk_command_access = init_eaccess(ramdisk_execute_command);
if (ramdisk_command_access != 0) {
	pr_warn("check access for rdinit=%s failed: %i, ignoring\n", ...);
	ramdisk_execute_command = NULL;
	prepare_namespace();          /* 去挂真正的根 */
}
```

- `ramdisk_execute_command` 默认就是 `"/init"`，可用 `rdinit=` 改；
- `init_eaccess()` 是“仿 `access(2)`”的内核版本：`kern_path()` 查路径 + `MAY_ACCESS`
  判权限，返回 0 表示“这文件在，能访问”。

一行返回值，就是两台完全不同的启动机器：

| 返回值 | 含义 | 后续动作 |
|---|---|---|
| `0` | 内存根里真有 `/init` | 什么都不做，稍后 `kernel_init()` 直接 `execve("/init")` |
| `-ENOENT`（-2）等 | 没有 | `prepare_namespace()` 挂真根（主题五） |

## 实测对照（同一内核，两个 initramfs）

- 镜像 A：`bb_initramfs.cpio`，自带 `/init`
- 镜像 B：同一份 rootfs 去掉 `init` 重新打包

| 观察点 | A | B |
|---|---|---|
| `init_eaccess(/init)` | `= 0` | `= -2` |
| `check access ... ignoring` | 无 | 有 |
| `prepare_namespace()` | 未调用（0 条打点） | 调用（9 条打点） |
| 结局 | `Run /init as init process` → busybox shell | `VFS: Cannot open root device ""` → panic |

```
A: [    3.793793] [latte] init_eaccess(/init) = 0
   [    4.162512] Run /init as init process

B: [    3.570136] [latte] init_eaccess(/init) = -2
   [    3.570838] check access for rdinit=/init failed: -2, ignoring
   [    3.571047] [latte] prepare_namespace: enter real-root path
```

## 两个容易踩的细节

1. **`0` 只代表“存在且可访问”，不代表“能执行”**。真正 execve 失败发生在
   `kernel_init()` 里，那时会打印 `Failed to execute ...` 并继续试别的名字
   （`init=`、`/sbin/init`、`/etc/init`、`/bin/init`、`/bin/sh`）。
2. **`ramdisk_execute_command` 被置 NULL 之后，后面所有的 `if
   (ramdisk_execute_command)` 都失效**。所以 B 的启动不可能再回头跑 `/init`，
   哪怕挂根成功后根里恰好也有 `/init` —— 它只会按 `/sbin/init` 那一串去找。

## 代码位置与打点

| 位置 | 打点 |
|---|---|
| `init/main.c:1701` `init_eaccess()` 调用 | `[latte] init_eaccess(%s) = %d` |
| `init/main.c:1711` 分支内、`prepare_namespace()` 之前 | `[latte] prepare_namespace: enter real-root path` |

两条打点不会同时出现：有 `/init` 只有第一条，没有才两条都有。
