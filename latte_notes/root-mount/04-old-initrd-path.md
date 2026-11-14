# 主题四 · 老式 initrd：`initrd_load()` 为什么这次是空转

## 先分清两个东西

| | 老式 initrd | initramfs |
|---|---|---|
| 形态 | 块设备镜像（可压） | cpio 归档（可压） |
| 内核怎么用 | 读进 `/dev/ram0`（要 `CONFIG_BLK_DEV_RAM`），挂上，chroot 进去跑 `/linuxrc` | 解进 rootfs，直接 execve `/init` |
| 本次配置 | `# CONFIG_BLK_DEV_RAM is not set` | 使用中（`CONFIG_BLK_DEV_INITRD=y`） |

所以本文件（`init/do_mounts_initrd.c`）在本次启动里**只会被调用一次、并且什么都没做**。
但它仍是理解“为什么现在的内核不需要 linuxrc”的反面教材。

## `initrd_load()` 的三个判据

```c
bool __init initrd_load(char *root_device_name)
{
	if (mount_initrd) {                        /* noinitrd 可以关掉 */
		create_dev("/dev/ram", Root_RAM0);
		if (rd_load_image("/initrd.image") && ROOT_DEV != Root_RAM0) {
			init_unlink("/initrd.image");
			handle_initrd(root_device_name);
			return true;
		}
	}
	init_unlink("/initrd.image");
	return false;
}
```

1. `mount_initrd`：默认 1，`noinitrd` 可关；
2. `rd_load_image("/initrd.image")`：把 `/initrd.image` 读成 ramdisk。**没开
   `CONFIG_BLK_DEV_RAM` 时，`init/do_mounts.h` 里它是 `return 0` 的空桩**，条件恒假；
3. `ROOT_DEV != Root_RAM0`：`root=/dev/ram` 时本来就要挂 ram0 当根，不该当 initrd 用。

`/initrd.image` 是谁放的？`populate_initrd_image()`（`init/initramfs.c`，同样只在
`CONFIG_BLK_DEV_RAM` 打开时编译）：当 initrd 内容不是合法 cpio 时，把原始镜像当作
`/initrd.image` 写进 rootfs，交给这条老路径处理。本次两者都没编进来，所以这条链
两头都是空的。

## `handle_initrd()` 做了什么（供对照）

1. `/dev/root.old` = `Root_RAM0`，把 ramdisk 挂到 rootfs 的 `/root`；
2. `init_mkdir("/old")`、`init_chdir("/old")`；
3. `call_usermodehelper("/linuxrc", UMH_WAIT_PROC)` 跑它；执行环境由
   `init_linuxrc()` 准备：`ksys_unshare(CLONE_FS|CLONE_FILES)` 复制 fs 上下文、
   chroot 到 initrd 自己的根、`ksys_setsid()`；
4. `/linuxrc` 通过 `/proc/sys/kernel/real-root-dev` 告诉内核真正的根设备；
5. 内核 `init_mount("..", ".", MS_MOVE)` 把 initrd 挪到 `/old`、`init_chroot("..")`
   回到 rootfs，再 `mount_root()` 挂真根；
6. 最后尝试把老根挪到 `/root/initrd`，`-ENOENT` 就忽略，其余错误卸载。

`pr_warn("using deprecated initrd support, will be removed soon.\n")` 就是这条路
的现状：保留兼容，不再发展。

## 结论：两者不是“新旧版本”，而是“谁负责准备根”

老式 initrd 把“准备根”的活交给用户态的 `/linuxrc`；initramfs 把同样的活交给
`/init`，但**不需要块设备**、也不需要内核先挂一个 ramdisk。机器里同时有
`CONFIG_BLK_DEV_INITRD` 和 cpio 内容时，内核优先按 initramfs 处理（上一站的
`unpack_to_rootfs()`），根本走不到这里。

## 代码位置与打点

| 位置 | 打点 |
|---|---|
| `init/do_mounts_initrd.c:150` `initrd_load()` 入口 | `[latte] initrd_load: mount_initrd=%d ROOT_DEV=%u` |

实测输出 `mount_initrd=1 ROOT_DEV=0` 之后就没有下文了——正是“判据 2 恒假”的现场。
