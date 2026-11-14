# 主题二 · dentry 缓存：为什么第二次打开同一个文件更快

## 你已知的

第一次 `ls /usr/bin` 可能卡一下，第二次瞬间出结果。不是“硬盘变快了”，而是
**目录项被缓存在内存里**了。

## 缓存的结构

dentry 缓存（dcache）是两张结构配合：

| 结构 | 作用 | 查找方式 |
|---|---|---|
| 哈希表 `dentry_hashtable` | 父目录 + 名字 → dentry | O(1)，按 `(parent, name)` 的哈希 |
| LRU 链表 | 没被引用的 dentry 排队等回收 | 从尾部开始丢 |

关键规则：**dentry 有引用计数 `d_lockref.count`**。

- 被路径引用（正在用、被 dentry 缓存命中并 pin 住）→ count > 0，不会被回收；
- 没人引用 → `dput()` 把它挂到 LRU 尾部，**内存紧张或手工 drop_caches 时才真正释放**。

所以 dcache 既“快”（命中不回磁盘），又“可回收”（不占死内存）。

## 快路径与慢路径

```
walk_component()
 ├─ lookup_fast()     ← RCU 模式，无锁遍历哈希表
 │                     命中：直接拿到 dentry，完全不进文件系统
 └─ lookup_slow()     ← 未命中：调 inode->i_op->lookup()
                        文件系统读目录块、构造 dentry，插进哈希表
```

`lookup_fast()` 用 RCU（读-复制-更新）：读的时候不加锁，靠序列号验证“读的过程中
父目录没变”。这就是路径解析在常见情况下能做到“几乎没有锁开销”的原因。

## 怎么看缓存规模

### `/proc/sys/fs/dentry-state`

```
nr_dentry  nr_unused  age_limit  want_pages  dummy
```

| 字段 | 含义 |
|---|---|
| `nr_dentry` | 当前 dentry 总数 |
| `nr_unused` | 其中“没人用、挂在 LRU 上”的数量 |
| `age_limit` | 内核希望保留多久（用于回收策略） |
| `want_pages` | 回收器还想再收多少页 |

### `/proc/slabinfo` 里的 `dentry` 行

```
dentry  <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> : tunables ...
```

`objsize` 就是一个 dentry 对象的大小（本内核的具体值请自己 `sed -n '/^dentry /p'
/proc/slabinfo` 看一眼，本次实验没采集到这一行）；`objperslab` 表示一页能切几个。
**缓存条目本身也要占内存**——这是 dcache 的代价，也是它必须可回收的原因。

## 手工收缩：`echo 2 > /proc/sys/vm/drop_caches`

| 值 | 清什么 |
|---|---|
| 1 | 页缓存（page cache） |
| 2 | 可回收的 slab（dentry、inode） |
| 3 | 两者都清 |

实测（主题四）：清完之后 `nr_dentry` 会掉回一个很小的基数——**证明前面涨上去的
确实是可回收缓存，不是泄漏**。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/dcache.c:2254` `__d_lookup_rcu()` | 哈希查找（RCU 版，无锁读） |
| `fs/dcache.c:900` `dput()` | 引用计数减一；到 0 进 LRU |
| `fs/dcache.c` `dentry_shrinker` / `prune_dcache_sb()` | 内存紧张时收缩 |
| `fs/drop_caches.c:52` `drop_caches_sysctl_handler()` | 手工丢缓存（打点所在） |
| `fs/dcache.c` `d_alloc()` / `d_add()` | 新建 dentry 并插入哈希 |
