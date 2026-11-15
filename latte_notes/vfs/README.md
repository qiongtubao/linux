# VFS：一个路径怎么变成 inode，dentry 缓存又是什么

`open("/etc/passwd")` 里那串字符要经过逐段解析才能变成可读的 inode。
这一站看三件套（dentry / inode / file）、dentry 缓存为什么让第二次打开变快，
并用 `dentry-state` 与 `drop_caches` 做了一次实测；最后补上写路径：
`write()` 为什么返回了却没落盘、脏页什么时候才回写、`mmap` 的页怎么进进程，
以及 `sendfile` 的零拷贝。
1 份索引 + 7 篇主题文档，13 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 文件系统 | 路径解析 | [01-路径解析-从字符串到inode](01-路径解析-从字符串到inode.md) | 三件套 + 逐段解析 + 挂载点与符号链接 |
| 文件系统 | 缓存 | [02-dentry缓存-为什么第二次打开快](02-dentry缓存-为什么第二次打开快.md) | 哈希表 + LRU + 引用计数；`nr_unused` 才是可回收量 |
| 文件系统 | 代码路径 | [03-代码路径-从open到inode](03-代码路径-从open到inode.md) | walk → 最后一段 → `vfs_open`；创建走 `lookup_open` |
| 文件系统 | 实测 | [04-实测-dentry缓存的增长与回收](04-实测-dentry缓存的增长与回收.md) | +100 个名字 = +100 个 dentry；`drop_caches` 清不动 |
| 文件系统 | 写回 | [05-writeback-write不落盘-脏页什么时候才回写](05-writeback-write不落盘-脏页什么时候才回写.md) | `write()` 只标脏；阈值限流、`sync` 才落盘 |
| 文件系统 | 映射 | [06-mmap与page-cache-文件页怎么进进程](06-mmap与page-cache-文件页怎么进进程.md) | 零拷贝别名；预读一次 4MB；私有写才 COW |
| 文件系统 | 零拷贝 | [07-splice与sendfile-零拷贝省了什么](07-splice与sendfile-零拷贝省了什么.md) | 页直接挂成 skb frag；`read+write` 走不到这条路 |

## 一条链

```
open("/f1")
 └─ do_filp_open → path_openat
     ├─ link_path_walk → walk_component
     │    ├─ lookup_fast（查 dcache，RCU 无锁）
     │    └─ lookup_slow（问文件系统的 ->lookup）
     └─ lookup_open（最后一段：已存在 / O_CREAT 创建）
         └─ vfs_open（inode + dentry → file）
```

写路径（05），注意它和上面那条链**不在一个调用栈里**：

```
write(fd, buf, 1MB)
 └─ generic_perform_write → 拷进 page cache + 标脏 + balance_dirty_pages  ← 打点
      （到此 write() 就返回了，盘上还没动）

回写（阈值 / 定时 / sync 三条触发线）
 └─ do_writepages → ext4_writepages → submit_bio_noacct → /dev/vda   ← 打点
```

`mmap` 那条（06）：只建 vma，访问才缺页

```
mmap(fd, 4MB, MAP_SHARED)
 └─ 第一次访问 → do_fault → filemap_fault          ← 打点
     ├─ 缓存未命中 → 预读（实测一次读 4MB）
     └─ 把 page cache 那一页挂进页表（零拷贝）
msync → vfs_fsync_range(datasync=1) → do_writepages → submit_bio    ← 打点
```

## 打点与实测

| 打点 / 读数 | 位置 | 本次值 |
|---|---|---|
| `lookup_open: create %s` | `fs/namei.c:3770` | 103 次（100 次 touch + 3 次启动解包） |
| `drop_caches: value=%d` | `fs/drop_caches.c:63` | 2 次 |
| `nr_dentry` | `/proc/sys/fs/dentry-state` | 621 → 721（+100） |
| `nr_unused` | 同上 | 1 → 2（几乎不动） |
| `generic_perform_write: bytes=…` | `mm/filemap.c:4228` | 每次 1MB 写各 1 次（A/B 各 8 次） |
| `balance_dirty_pages: dirtied=…` | `mm/page-writeback.c:1811` | A 8 次；阈值调低后 78 次 |
| `do_writepages: nr_to_write=…` | `mm/page-writeback.c:2598` | B 阶段 4 次、`sync` 2 次 |
| `ext4_writepages: nr_to_write=…` | `fs/ext4/inode.c:3009` | B 阶段 4 次、`sync` 1 次 |
| `submit_bio: opf=… size=…` | `block/blk-core.c:790` | A 阶段 0 个数据 bio；B 阶段 1MB/4MB |
| `do_fault: addr=… write=…` | `mm/memory.c:5831` | 文件页缺页；`MAP_PRIVATE` 写时 `write=1` |
| `filemap_fault: addr=… idx=…` | `mm/filemap.c:3448` | 4MB 文件只 1 次（预读把整个文件读进来） |
| `vfs_fsync_range: datasync=…` | `fs/sync.c:179` | `msync` 走 `datasync=1` |
| `do_sendfile: count=…` | `fs/read_write.c` | 1 次（4MB） |
| `filemap_splice_read: len=…` | `mm/filemap.c` | 64 次（4MB ÷ 64KB 管道） |
| `tcp_splice_pages: len=…` | `net/ipv4/tcp.c` | 193 次；`read+write` 模式为 0 |

**没在 `lookup_fast()` / `lookup_slow()` 打点**：一次 `ls` 就有几百次调用，是最热路径。

## 自查 6 问

1. 为什么建 100 个文件，`nr_dentry` 正好 +100？（01/02：每个名字一个 dentry 对象）
2. `drop_caches=2` 之后 `nr_dentry` 几乎不变，怎么解释？（04：只回收无人引用的
   dentry，本次 `nr_unused` 只有 2~3）
3. `open(O_CREAT)` 与 `mknod()` 的创建路径有什么不同？（03：前者走 `lookup_open`，
   后者走 `do_mknodat` → `vfs_create`/`vfs_mknod`）
4. `dd` 写完 8MB 就返回了，为什么 `Dirty` 还有 8192 kB？（05：`write()` 只到 page cache；
   阈值没到就没人回写，`sync` 才强制落盘）
5. `mmap` 一个 4MB 文件只碰第一页，为什么内核读了 4MB？（06：预读；
   `submit_bio size=4194304`，之后每页缺页都命中缓存）
6. `sendfile` 和 `read()+write()` 差在哪？（07：`sendfile` 走
   `filemap_splice_read` + `MSG_SPLICE_PAGES`，页直接挂成 skb frag；
   `read+write` 这两处计数都是 0——因为它拷了两次）

## 下一阶段

VFS 这一支收口了：路径（01~03）、缓存（02/04）、写回（05）、映射（06）、零拷贝（07）。
还能往下挖：**writeback 的限流细节**（`dirty_ratelimit`、cgroup writeback）、
**挂载传播**（`shared`/`slave` peer group，容器里很关键）、
**`io_uring` 的 read/write**（同一批系统调用换成异步提交）。
也可以换域做 **zram/zswap**（需开 `CONFIG_ZRAM`/`CONFIG_ZSWAP`）。
