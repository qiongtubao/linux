# 主题三 · 内存里的根：ramfs 与 initramfs

## 你已知的

内存（RAM）很快，但断电就没；磁盘（或 SSD）慢，但东西留着。所以“系统文件”通常
住在磁盘上，开机后再读进内存用。

Linux 启动时有个反常识的点：**`/` 一开始是内存里的文件系统，和磁盘没关系。**

## 内存里怎么会有“文件系统”

内核手里有一份“页缓存”——所有文件内容在内存里的那份副本。ramfs 就是只借用它：

| 普通文件系统（ext4） | ramfs |
|---|---|
| 目录/文件的信息存在磁盘上的 inode | 存在内存里 |
| 文件内容存在磁盘块 | 存在页缓存里 |
| 断电后还在 | 断电就没了 |
| 需要块设备 | **不需要块设备** |

内核自己启动时创建的第一个文件系统叫 **rootfs**，后端就是 ramfs（开了 tmpfs 时
也可能选 tmpfs）。它自动挂在 `/` 上——所以从内核还只有几百 KB 可用的时候起，
`/` 就已经存在了。

## initramfs 是把“一套目录树”打包塞进 rootfs

initramfs 是一个 cpio 归档（可以再 gzip/xz 压一层）。内核在 rootfs_initcall 阶段把
它解压进 rootfs（上一站 `latte_notes/initramfs-rootfs.md`）：目录用 mkdir、文件用
open+write、设备用 mknod，全部发生在内存里。解压完，rootfs 里就有了 `/bin/busybox`、
`/init` 这些条目。

于是“内存里有一套能跑的文件系统”这件事从口号变成了事实。

## 为什么启动早期一定要用内存根

因为**磁盘根经常一时半会儿拿不到**：

- 磁盘驱动还没加载（驱动在 initcall 里，可能晚于挂根的需求）；
- 根在一个软 RAID / LVM / 加密卷上，得先组起来、解密；
- 根在网络另一头（NFS），网卡还没配 IP；
- 根在 USB 上，设备还没枚举完。

这些活儿都是“需要用户态程序才能干”的：所以先给一个内存里的极小系统，让它跑
`/init` 去把真正的根准备好；真根就绪后，再切过去（主题六的换根）。

## 两条路的分工

| | 内存根（rootfs） | 磁盘根 |
|---|---|---|
| 什么时候有 | 内核启动就自动有 | 要等设备、驱动、参数都到位 |
| 谁创建 | 内核 `init_rootfs()` | `mount_root()` 挂到 `/root` |
| 里面有什么 | initramfs 解出来的东西（`/init`、busybox…） | 完整发行版（`/sbin/init`、`/etc`…） |
| 本次结果 | 有 `/init` → 直接用它 | 没有 `root=` → 挂不上 → panic |

`init_rootfs()` 的选择逻辑：既没给 `root=`、也没给 `rootfstype=` 时用 tmpfs，否则
退回 ramfs。两者都能当 rootfs，区别是 tmpfs 支持 swap 与大小上限。

## 内存不是白给的：解压完要还

initrd 那 2M 是 bootloader 放进 RAM、由 `memblock_reserve()` 记过账的保留区；解压
完之后 `free_initrd_mem()` → `free_reserved_area()` 把它还给 buddy（上一站实测
`Freeing initrd memory: 2148K`）。而 initramfs 解出来的内容仍在 rootfs 里占着内存，
这是它“快”的代价。

## 代码位置

| 位置 | 内容 |
|---|---|
| `init/do_mounts.c:557` `init_rootfs()` | 选 rootfs 的后端（ramfs / tmpfs） |
| `init/initramfs.c` | 解压 cpio 进 rootfs（上一站） |
| `init/initramfs.c:725` `free_initrd_mem()` | 归还 initrd 占的内存 |
