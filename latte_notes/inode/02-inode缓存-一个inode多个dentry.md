# 主题二 · inode 缓存：一个 inode 对应多个 dentry

## 你已知的

上一站（`vfs/`）说 dentry 是“名字”的缓存。这一站看另一层：**inode 本身也在内存里缓存**，
而且一个 inode 可能同时挂着好几个名字（硬链接）。

## 两套引用计数，别混

| 计数 | 在哪 | 含义 |
|---|---|---|
| `d_lockref.count` | `struct dentry` | 有多少**代码路径**正在用这个 dentry |
| `i_count` | `struct inode` | 有多少**代码路径**正在用这个 inode（打开文件、正在查找…） |
| `i_nlink` | `struct inode` | 文件系统层面：这个 inode 有几个**名字** |

释放规则：

```
dput()  → dentry 计数到 0 → 进 dcache 的 LRU（可回收）
iput()  → inode 计数到 0
             ├─ i_nlink > 0 → 挂进 sb->s_inodes 的“无人使用”链表，可回收
             └─ i_nlink = 0 → evict()：真正释放 inode 与数据
```

所以 **`i_nlink = 0` 但 `i_count > 0`** 是合法状态：文件已经没有任何名字，但还有进程
打开着——数据必须留着。

## 一个 inode 的多个名字：`i_dentry`

```c
struct inode {
	...
	struct list_head i_dentry;   /* alias 链：所有指向我的 dentry */
	unsigned long i_ino;
	...
};
```

硬链接后，`/a` 与 `/b` 两个 dentry 都挂在这个 inode 的 `i_dentry` 链上。
这就是“一个 inode 多个 dentry”的具体存放位置。

## 怎么看 inode 缓存

| 接口 | 内容 |
|---|---|
| `/proc/sys/fs/inode-state` | `nr_inodes`（总数）、`nr_free_inodes`（可回收量）、`preshrink` |
| `/proc/slabinfo` 的 `*inode_cache` 行 | 真正的对象数（`active_objs`/`num_objs`/`objsize`） |

注意 `nr_free_inodes` 是“可回收”的量，不是“空闲内存”——和上一站 `nr_unused` 的角色一样。

## 上一站留下的问题，方向在这里

上一站实测：建 100 个文件后 `nr_dentry` +100，但 `nr_unused` 几乎不动（1→2）。
现在能给出更准确的观察角度：

- `nr_unused` 统计的是“**没人引用**的 dentry”，而 dentry 的回收还牵连 inode：
  inode 缓存在内存里时，它的 alias dentry 会被留在缓存中以便再次查找；
- 想判断“真实占用”，看 `/proc/slabinfo` 的 `active_objs` 比看 `nr_unused` 可靠。

**这仍是推测**——要定性必须读 `get_nr_dentry_unused()` 与 `dput()→d_lru_add()` 的完整
路径（本次没有继续追）。本站的实验会用 `slabinfo` 与 `inode-state` 给出可核对的数字。

## 与前面几站的衔接

| 现象 | 上游文档 |
|---|---|
| 打开文件要文件系统的 `->lookup` | `vfs/02`（快路径/慢路径） |
| 删名字不改数据 | `inode/01`（`i_nlink` 语义） |
| slab 缓存（`*inode_cache`） | `memory/03`（slab 是什么） |

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/inode.c:1930` `iput()` | inode 引用计数减一 |
| `fs/inode.c:1874` `iput_final()` | 到 0 后的分流：evict 还是挂 unused |
| `fs/inode.c:786` `evict()` | 真正释放 |
| `fs/dcache.c:2465` `d_delete()` | 名字删除后 dentry 变 negative |
| `include/linux/fs.h` | `struct inode` 的 `i_dentry` / `i_nlink` / `i_count` |
