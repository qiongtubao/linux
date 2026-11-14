# 主题五 · 换根三步：rootfs 怎么把 `/` 让出来

## 三步分别解决什么

```c
out:
	devtmpfs_mount();
	init_mount(".", "/", NULL, MS_MOVE, NULL);
	init_chroot(".");
```

| 语句 | 作用 |
|---|---|
| `devtmpfs_mount()` | 以相对路径 `dev` 挂 devtmpfs；此刻 cwd 已经在 `/root`（新根），于是落在**新根的 `/dev`** 上 |
| `init_mount(".", "/", NULL, MS_MOVE, NULL)` | 把 cwd 上的挂载（新根，此前挂在 `/root`）整体移到 `/`，覆盖原来那个 rootfs |
| `init_chroot(".")` | 进程的根切到新根，旧 rootfs 从命名空间里彻底消失 |

三句用的是 `init_*()` 而不是 syscall：init 阶段没有用户地址空间、也不该占 fd
（`fs/init.c` 开头那句 “mimic syscalls, but don't use the user address space or
file descriptors”）。

## 为什么必须“先挂 /root，再挪到 /”

`do_mount_root()` 把文件系统挂在 `/root`：

```c
ret = init_mount(name, "/root", fs, flags, data_page);
if (ret)
	goto out;
init_chdir("/root");
```

好处是失败时上一条根还完整可用——能打印分区表、能把 `unknown-block(0,0)`
这种信息报清楚（主题六）。挂成功之后才整体挪到 `/`，一步到位，不存在“挂一半”的
中间状态。

## 这三步在 initramfs 场景里根本不跑

有 `/init` 时 `prepare_namespace()` 不会被调用（主题一），rootfs 自始至终就是
`/`，`/init` 直接在它上面 execve。所以“换根”是**给块设备根（或 NFS 根）准备的
机制**，不是 initramfs 的一部分。

反过来看很关键：`mount_root_generic()` 一旦 panic，`out:` 就永远不会到达。实测里
`[latte] prepare_namespace: devtmpfs + switch root` 这条打点缺失，等于直接告诉我们
“死在了挂根这一步，没走到换根”。

## 与“根文件系统本身”的区别

| 概念 | 谁创建 | 何时 |
|---|---|---|
| rootfs（ramfs/tmpfs） | `init_rootfs()` + 内核早期 | 启动最早期就是 `/` |
| 真根（ext4/nfs/…） | `mount_root()` 挂到 `/root` | initcall 之后、换根之前 |
| 换根 | `MS_MOVE` + `init_chroot()` | 真根挂好之后 |

initramfs 的作用就是“让前两者之间的空档里，有一个能跑的 `/init`”。

## 代码位置

| 位置 | 内容 |
|---|---|
| `init/do_mounts.c:530` | `out:` 之后的换根三步 |
| `fs/init.c` | `init_mount()` / `init_chroot()` 等仿 syscall 实现 |
| `drivers/base/devtmpfs.c` | `devtmpfs_mount()`：`mount_dev`/`thread` 就绪才真的挂 |
