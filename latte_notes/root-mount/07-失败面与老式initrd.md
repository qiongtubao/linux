# 主题七 · 失败面，与老式 initrd 这个历史包袱

## 两条 panic，别混

| 失败点 | 触发条件 | 报错关键字 | panic 文案 |
|---|---|---|---|
| 挂不上根 | 没有可用根设备 / 没有 fs 认得它 | `VFS: Cannot open root device` 或 `No filesystem could mount root` | `VFS: Unable to mount root fs on ...` |
| 没有 init 可跑 | 根有了，但所有 init 名字都 execve 失败 | `Failed to execute ...` | `No working init found. Try passing init= option to kernel.` |

本次实验（去掉 `/init`、又没给 `root=`）命中的是第一条：连根都没有，根本走不到
“找 init”。

## 实测日志（镜像 B，`/tmp/latte-rootB.log`）

```
[    3.570136] [latte] init_eaccess(/init) = -2
[    3.570838] check access for rdinit=/init failed: -2, ignoring
[    3.571047] [latte] prepare_namespace: enter real-root path
[    3.571284] [latte] prepare_namespace: root=(none)
[    3.789649] [latte] prepare_namespace: initrd_load
[    3.789877] [latte] initrd_load: mount_initrd=1 ROOT_DEV=0
[    3.790845] [latte] prepare_namespace: mount_root
[    3.791064] [latte] mount_root: ROOT_DEV=0
[    3.791566] [latte] mount_block_root: /dev/root
[    3.791807] [latte] mount_root_generic: ROOT_DEV=0
[    3.793621] [latte] wait_for_initramfs -> async_synchronize
[    3.794299] /dev/root: Can't open blockdev
[    3.795224] VFS: Cannot open root device "" or unknown-block(0,0): error -6
[    3.795728] Please append a correct "root=" boot option; here are the available partitions:
[    3.796244] 0b00         1048575 sr0
[    3.796463]  driver: sr
[    3.796923] List of all bdev filesystems:
[    3.797134]  ext3
[    3.797179]  ext2
[    3.797266]  ext4
[    3.797367]  vfat
[    3.797589]  msdos
[    3.797699]  iso9660
[    3.798118] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
```

三个值得记住的细节：

1. **`error -6` 是 `-ENXIO`（设备打不开）**，不是 `-EACCES`/`-EINVAL`，所以走的是
   “其它错误 → 立即 panic”，没进入“改只读再试一轮”。想让两轮重试发生，得让设备
   能打开、但超级块读不懂（典型 `-EINVAL`）。
2. **`unknown-block(0,0)`** 就是 `ROOT_DEV == 0` 被格式化后的名字：没有 `root=` 时
   `create_dev("/dev/root", 0)` 造出的节点就是 0:0，打开必然失败。
3. **`wait_for_initramfs` 又出现了**：挂根过程中会 `request_module("fs-…")` 之类的
   usermodehelper，而 umh 在 execve 前会调 `wait_for_initramfs()`——上一站《谁负责等》
   说的消费者，在这里第三次出现。

## 用“缺哪条打点”定位死点

10 条打点里，B 只出现 9 条，缺的正是 `prepare_namespace: devtmpfs + switch root`
（它在 `out:` 之后）。所以看到“打点链断在 `mount_root_generic`”，就等于看到
“死在了挂根”。这条经验比读栈回溯快得多。

## 老式 initrd：为什么这次它什么也没做

老式 initrd 和 initramfs 不是新旧版本关系，而是两种完全不同的东西：

| | 老式 initrd | initramfs |
|---|---|---|
| 形态 | **块设备镜像**（可压） | cpio 归档（可压） |
| 内核怎么用 | 读进 `/dev/ram0`，挂起来，chroot 进去跑 `/linuxrc` | 解进 rootfs，直接 execve `/init` |
| 需要 `CONFIG_BLK_DEV_RAM` | 是 | 否 |
| 本次配置 | `# CONFIG_BLK_DEV_RAM is not set` | `CONFIG_BLK_DEV_INITRD=y` |

`initrd_load()` 的三个判据：`mount_initrd`（`noinitrd` 可关）、
`rd_load_image("/initrd.image")`、`ROOT_DEV != Root_RAM0`。**没开 `BLK_DEV_RAM` 时
`rd_load_image()` 是 `return 0` 的空桩**，所以判据恒假，函数必返回 false——实测打点
`initrd_load: mount_initrd=1 ROOT_DEV=0` 之后确实没有下文。

它本来要做的活（供对照）：把 ramdisk 挂到 `/root` → chroot 进去跑 `/linuxrc` →
`linuxrc` 通过 `/proc/sys/kernel/real-root-dev` 告诉内核真根在哪 → 内核 `MS_MOVE`
把 initrd 挪到 `/old` → 挂真根 → 想把老根挪到 `/root/initrd`，失败就卸载。
一句话：**老式 initrd 把“准备根”的活交给用户态的 `linuxrc`**，initramfs 则交给
`/init`，但不需要块设备。

## 代码位置

| 位置 | 内容 |
|---|---|
| `init/do_mounts.c:246` | `-EACCES/-EINVAL` 之外的错误：立即 panic |
| `init/do_mounts.c:255` | 只读重试也失败后的 `No filesystem could mount root, tried:` |
| `init/main.c:1617` | `panic("No working init found. ...")` |
| `init/do_mounts_initrd.c:148` `initrd_load()` | 老式 initrd 入口（本次空转） |
| `init/do_mounts_initrd.c:92` `handle_initrd()` | 挂 ramdisk、跑 `/linuxrc`、再换回来 |
