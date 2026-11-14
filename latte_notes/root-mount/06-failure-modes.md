# 主题六 · 失败面：挂不上根，与没有 init 可跑

## 两条独立的失败线

| 失败点 | 触发条件 | 报错 | 结局 |
|---|---|---|---|
| 挂不上根 | 没有可用根设备/文件系统 | `VFS: Cannot open root device ...` 或 `No filesystem could mount root` | `panic("VFS: Unable to mount root fs on ...")` |
| 没有 init 可跑 | 所有 init 名字都 execve 失败 | `Failed to execute ...` | `panic("No working init found...")` |

第一次实测（去掉 `/init`、又没有 `root=`）命中的是第一条：`prepare_namespace()`
在挂根这一步就死了，根本没机会走到“找 init”。

## 实测：挂根失败的全过程（真实日志，`/tmp/latte-rootB.log`）

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

三个可以学到的细节：

1. **`error -6` 是 `-ENXIO`**，不是 `-EACCES`/`-EINVAL`，所以走的是
   `mount_root_generic()` 里“其它错误”那一支：**立即 panic**，没有进入
   “改只读再试一轮”的 `retry:`。想让两轮重试发生，得让设备能打开但超级块读不懂
   （典型是 `-EINVAL`）。
2. **`unknown-block(0,0)`** 就是 `ROOT_DEV == 0` 被 `scnprintf` 格式化后的名字：
   没有 `root=` 时 `create_dev("/dev/root", 0)` 造出的节点就是 0:0，打开必然失败。
3. **`wait_for_initramfs` 打点夹在挂载尝试中间**：挂根时会 `request_module("fs-…")`
   之类的 usermodehelper，而 umh 在 execve 之前会 `wait_for_initramfs()`——上一站
   主题五说的“三个消费者”在这里又出现了一次。

## 用“缺哪个打点”定位死点

本次 10 个打点里，B 运行只出现了 9 个：**缺 `prepare_namespace: devtmpfs + switch
root`**。因为它在 `out:` 之后，而 `mount_root_generic()` panic 了，函数没有返回。
所以看到“打点链在 `mount_root_generic` 断掉”，就等于看到“死在挂根”。

## 第二条失败线长什么样（供对照）

如果根挂上了、里面却没有可用 init：`ramdisk_execute_command` 被置 NULL，于是
`kernel_init()` 依次试 `init=`、`CONFIG_DEFAULT_INIT`、`/sbin/init`、`/etc/init`、
`/bin/init`、`/bin/sh`，全失败才：

```
Kernel panic - not syncing: No working init found.  Try passing init= option to kernel.
```

注意这两条 panic 的区别：前者是“没有根文件系统”，后者是“有根但没有可执行 init”。

## 代码位置

| 位置 | 内容 |
|---|---|
| `init/do_mounts.c:246` | `-EACCES/-EINVAL` 之后那条立即 panic |
| `init/do_mounts.c:255` | 只读重试也失败后的 `No filesystem could mount root, tried:` |
| `init/main.c:1617` | `panic("No working init found. ...")` |
