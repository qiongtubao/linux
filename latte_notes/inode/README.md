# inode 与硬链接：一个文件可以有多个名字

上一站（`vfs/`）讲了 dentry 是“名字”的缓存。这一站看名字背后的本体：**inode**。
硬链接让两个名字指向同一个 inode，`ln` 之后 dentry +1 而 inode 不变——
这是三件套分层最直接的证据。1 份索引 + 4 篇主题文档，2 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 文件系统 | 本体与名字 | [01-inode-一个文件可以有多个名字](01-inode-一个文件可以有多个名字.md) | inode 存元数据，名字只是目录项；`i_nlink` 是名字数 |
| 文件系统 | 缓存 | [02-inode缓存-一个inode多个dentry](02-inode缓存-一个inode多个dentry.md) | 两套引用计数、`i_dentry` alias 链、`i_nlink=0` 但还打开着 |
| 文件系统 | 代码路径 | [03-代码路径-link与unlink](03-代码路径-link与unlink.md) | 只改计数不搬数据；inode 的四种生死状态 |
| 文件系统 | 实测 | [04-实测-硬链接让dentry加1而inode不变](04-实测-硬链接让dentry加1而inode不变.md) | 四步实验的状态表 |

## 一条链

```
ln /a /b        → vfs_link()     : i_nlink++，新 dentry 挂到 i_dentry alias 链
ln -s /a /c     → vfs_symlink()  : 新建 inode（内容是字符串 "/a"）
rm /b           → vfs_unlink()   : i_nlink--，d_delete() 让 dentry 变 negative
最后一个引用消失 → iput() → iput_final() → evict()（nlink=0 且 count=0）
```

## 实测一瞥

| 阶段 | `nr_inodes` | `nr_dentry` |
|---|---|---|
| 基线 | 652 | 622 |
| `touch /a` | 655 | 624 |
| **`ln /a /b`** | **655（不变）** | **625（+1）** |
| `ln -s /a /c` | 656（+1） | 626（+1） |
| `rm /b` | 656（不变） | 625（−1） |

## 打点清单（2 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `vfs_link: %s (i_nlink=%u)` | `fs/namei.c` | 1 次（`b`，链接前 nlink=1） |
| `vfs_unlink: %s (i_nlink=%u)` | `fs/namei.c` | 1 次（`b`，删除前 nlink=2） |

两个打点都打印**修改前**的 `i_nlink`，正好能和 `ls -l` 的链接计数对上。

## 自查 3 问

1. `ln /a /b` 之后 inode 数为什么不变？（01/04：硬链接只加名字，不复制本体）
2. 符号链接与硬链接在内核里差在哪？（01：符号链接有独立 inode，内容是路径字符串）
3. 文件已 `rm` 但空间没释放，什么情况下会发生？（03：`i_nlink=0` 但 `i_count>0`，
   还有进程打开着）

## 下一阶段

同一领域还能挖：**mount 与 vfsmount 树**（`follow_managed` 怎么跨文件系统）、
**writeback**（脏页回写与 `sync`）。换域可以做**作业控制**（`Ctrl-Z`/`fg`）或
**zram/zswap**。上一站遗留的 `nr_unused` 统计口径问题仍未解，需要读
`get_nr_dentry_unused()` 与 `d_lru_add()`。
