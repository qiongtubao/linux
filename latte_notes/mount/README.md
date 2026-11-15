# mount：一棵目录树是怎么拼出来的

上一站（`inode/`）说硬链接不能跨文件系统，因为 inode 号只在同一 superblock 内唯一。
这一站补上另一半：**不同文件系统怎么被接成一棵树**。
一次 `mount` 让 `/mnt` 的 `st_dev` 从 `0x2` 变成 `0x15`（21），`umount` 又变回来。
最后看挂载传播：shared 组里挂一次 `/a/sub`，`/b/sub` 会自己出现；
再进一个 mount namespace 看 slave 的“只收不发”。
1 份索引 + 6 篇主题文档，4 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 文件系统 | 概念 | [01-mount-一棵目录树怎么接出来](01-mount-一棵目录树怎么接出来.md) | superblock / vfsmount / 挂载点；`st_dev` 是跨挂载点的信号 |
| 文件系统 | 结构 | [02-vfsmount树与命名空间](02-vfsmount树与命名空间.md) | `struct mount` 的两个链表、命名空间隔离、mountinfo 怎么读 |
| 文件系统 | 代码路径 | [03-代码路径-mount与跨挂载点解析](03-代码路径-mount与跨挂载点解析.md) | 先建 sb 再接树；解析时 `handle_mounts()` 换根 |
| 文件系统 | 实测 | [04-实测-挂载点两侧的st_dev](04-实测-挂载点两侧的st_dev.md) | 挂载/卸载前后 state 与 mountinfo 对照 |
| 文件系统 | 传播 | [05-挂载传播-一次挂载怎么变成两份](05-挂载传播-一次挂载怎么变成两份.md) | shared 组挂一次变两份；private 一次都不传 |
| 文件系统 | 单向 | [06-slave与命名空间-只收不发的那一半](06-slave与命名空间-只收不发的那一半.md) | 上游挂载下行 ✓、下游挂载上行 ✗ |

## 一条链

```
mount -t tmpfs none /mnt
 └─ path_mount → do_new_mount（）                ← 打点
     ├─ get_fs_type("tmpfs") → fs_context
     ├─ vfs_get_tree() 造 superblock
     └─ do_add_mount → attach_recursive_mnt()     ← 接树（此刻才可见）

解析 /mnt/x
 └─ handle_mounts() → __lookup_mnt() → 换成 tmpfs 的根
```

## 实测一瞥

| 观察 | 挂载前 | 挂载后 | `umount` 后 |
|---|---|---|---|
| `/mnt` 的 `st_dev` | `0x2` | `0x15`（十进制 21） | `0x2` |
| `/mnt` 的 `st_ino` | 20 | 1 | 20 |
| mountinfo | 无 `/mnt` 行 | `24 2 0:21 / /mnt … - tmpfs none rw,size=8192k` | 行消失 |

## 打点清单（2 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `do_new_mount: %s -> %s` | `fs/namespace.c` | 5 次（内核 devtmpfs + proc/sysfs/devtmpfs/tmpfs） |
| `do_umount: flags=0x%x` | `fs/namespace.c` | 1 次（`flags=0x0`） |
| `attach_mnt: src_group=… moving=…` | `fs/namespace.c` | 挂在 shared 组里的 bind 显示 `src_group=1` |
| `propagate_mnt: dest_group=… peers=…` | `fs/pnode.c` | A 组 1 次；slave 那次 `peers=0` 但仍传播（走 master 链） |

## 自查 5 问

1. 为什么 `mount` 之后 `/mnt` 的 `st_dev` 会变？（01/04：换到了另一个 superblock）
2. 为什么要先 `vfs_kern_mount()` 再 `attach_recursive_mnt()`？（03：建 sb 可能失败，
   失败要能干净回滚，不能先动树）
3. `stat -c %D` 和 mountinfo 的设备号为什么看起来对不上？（04：`%D` 是十六进制，
   0x15 = 21 = `0:21`）
4. 为什么在 `/a/sub` 上挂载，`/b/sub` 也会出现？（05：`/a` 与 `/b` 同属一个 peer group
   （mountinfo 都标 `shared:1`），`propagate_mnt()` 给组里每份都复制一个挂载；
   设成 private 后 `propagate_mnt` 一次都不调用）
5. 子命名空间里挂的东西，为什么父命名空间看不到？（06：`MS_SLAVE` 只有 `mnt_master`
   单向链，上游的挂载会下来、下游的不会上去；实测子看到 `/up/sub`、父看不到 `/up/childmnt`）

## 下一阶段

mount 这条线走完了：概念（01）、结构（02）、代码路径（03）、`st_dev` 实测（04）、
传播（05）、slave 与命名空间（06）。**writeback** 见 `vfs/05`。

还能往下挖：**user namespace + 非特权 mount**（本内核未开 `CONFIG_USER_NS`，
开了才能测“普通用户也能 mount tmpfs”）、**mount 与 overlayfs**（容器镜像分层的实现）。
剩下的就是 **zram/zswap**（需改 `.config`）。
