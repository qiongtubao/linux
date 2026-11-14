# 主题三 · ROOT_DEV 决定挂载策略

## `ROOT_DEV` 不是普通设备号

`include/linux/root_dev.h` 给了几个伪值：`Root_NFS`、`Root_CIFS`、`Root_Generic`、
`Root_RAM0`，其余才是真实 `dev_t`。它是全局变量：`parse_root_device()` 往里写，
`do_mount_root()` 挂成功后又把它改写成实际 superblock 的设备号（`ROOT_DEV = s->s_dev`）。

## 分派表

| `ROOT_DEV` | 走哪条路 | 说明 |
|---|---|---|
| `Root_NFS` | `mount_nfs_root()` | `nfsroot=` 已解析，带 5→30 秒退避、最多 5 次重试 |
| `Root_CIFS` | `mount_cifs_root()` | 同 NFS，CIFS 版，同样有重试 |
| `Root_Generic` | `mount_root_generic()` | `root=mtd/ubi` 这类，交给通用查找 |
| `0` | 先试 `mount_nodev_root()` | 没给 `root=`：只有显式 `rootfstype=` 才试不依赖设备的 fs |
| 其它 | `mount_block_root()` | 造出 `/dev/root` 再走通用挂载 |

`case 0` 里那句 `fallthrough` 是关键：条件 `root_device_name && root_fs_names` 不成立
（没给 `root=`、也没给 `rootfstype=`）时直接掉进 `default`，也就是当块设备挂。

## `mount_root_generic()` 的重试链

1. fs 列表：给了 `rootfstype=` 就只用它（`split_fs_names()` 拆逗号），否则
   `list_bdev_fs_names()` 列出所有块设备文件系统全试一遍。
2. 逐个 `do_mount_root()`：
   - 返回 0 → 成功，跳出；
   - `-EACCES` / `-EINVAL` → 视为“这个 fs 不认这块设备”，换下一个；
   - 其它错误 → 打印 `VFS: Cannot open root device ...` + 分区表 + 可用 fs 列表，
     直接 panic。
3. 一轮全失败后，如果 `flags` 还没带 `SB_RDONLY`，加上它 `goto retry` 再来一轮
   （“也许只是脏了，只读能挂上”）。
4. 只读也全失败 → 打印分区表和试过的 fs 列表，`panic("No filesystem could mount
   root, tried: ...")`。

所以“挂不上”有两条 panic 文案：设备/超级块层面挂不上是 `Unable to mount root fs
on ...`，文件系统层面全试遍是 `Unable to mount root fs on "..." or ...`。

## 本次配置下的实际路径

没有 `root=`，所以 `ROOT_DEV == 0`、`root_device_name == NULL`：

```
case 0: 条件为假 → fallthrough
  → mount_block_root(NULL)
      → create_dev("/dev/root", 0)
      → mount_root_generic("/dev/root", NULL, root_mountflags)
          → 逐个 fs 试 → 全失败 → panic
```

这条路径的实测日志见主题六与索引里的对照表。

## 代码位置与打点

| 位置 | 打点 |
|---|---|
| `init/do_mounts.c:407` `mount_root()` 入口 | `[latte] mount_root: ROOT_DEV=%u` |
| `:392` `mount_block_root()` 转通用挂载前 | `[latte] mount_block_root: /dev/root` |
| `:194` `mount_root_generic()` 入口 | `[latte] mount_root_generic: ROOT_DEV=%u` |

`mount_root` 前的那个 `ROOT_DEV` 值就是判断走哪条分支的依据：0 表示没给 `root=`。
