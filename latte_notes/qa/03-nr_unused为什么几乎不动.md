# 主题三 · `nr_unused` 为什么几乎不动：有结论，也有没查清的

## 问题回顾

`vfs/04` 与 `inode/04` 两次实测都是同一个现象：建 100 个文件后 `nr_dentry` +100，
但 `nr_unused` 只从 1 涨到 3；`echo 2 > drop_caches` 也清不掉它们。

## 代码说“本应发生什么”

```c
/* fs/dcache.c:899 */
void dput(struct dentry *dentry)
{
	if (likely(fast_dput(dentry)))   /* 计数减一；到 0 时判断要不要保留 */
		return;
	...
}

/* fs/dcache.c:744 retain_dentry() 的结尾 */
if (unlikely(!(d_flags & DCACHE_LRU_LIST))) {
	if (!locked)
		return false;            /* 无锁时不敢动，交给带锁的再判一次 */
	d_lru_add(dentry);           /* ← 这里 this_cpu_inc(nr_dentry_unused) */
}
```

也就是说：**只要 dentry 的引用计数真的归零、且它还在哈希表里，就应该被挂进 LRU 并
让 `nr_unused` 加一**。而 `nr_unused` 是 per-CPU 累加、读 `/proc/sys/fs/dentry-state`
时先汇总（`proc_nr_dentry()`，`fs/dcache.c:192`）再输出，**不是过期快照**。

## 新实验（本次补做）

| 动作 | `nr_dentry` | `nr_unused` | `nr_negative` |
|---|---|---|---|
| 基线 | 621 | 1 | 0 |
| `stat /nope1`（确实报了 ENOENT） | 621 | 2 | 0 |
| 100 次 `stat /nopeN` | 621 | 2 | 0 |
| 100 次 `[ -e /missingN ]` | 621 | 2 | 0 |
| 100 次 `touch /fN` | **721** | 3 | 0 |
| `drop_caches=2` 之后 | 723 | 3 | 0 |

两条新事实：

1. **负 dentry 不驻留**：100 次不存在的路径查找（确认真的发生了：`stat` 打印了
   `No such file or directory`）之后 `nr_dentry` 一点没涨、`nr_negative` 保持 0。
   所以“用不存在的路径来制造可回收 dentry”这个思路在本 rootfs（tmpfs）上行不通。
2. **正向 dentry 驻留但不计入 `nr_unused`**：建 100 个文件后 +100，之后 `drop_caches`
   也清不掉。

## 现状：仍未定性

按上面的代码，第 2 条不该发生——`dput` 到 0 就该 `d_lru_add()`。可能的解释有三类：

| 猜测 | 需要怎么验证 |
|---|---|
| 这些 dentry 的引用计数没归零（有别处持有引用） | 给 `retain_dentry()` 的两个分支打点，看进入哪个 |
| 走了另一条入 LRU 的路径（本版本已无 `__dput_to_list`） | 读本版本 `lock_for_kill()` / `__dentry_kill()` 全路径 |
| tmpfs 的 dentry 操作让它们被直接销毁/不缓存 | 查 `simple_lookup()` 与 `shmem` 的 `dentry_operations` |

本次**没有**继续加打点：boot 期间进出 LRU 的 dentry 数以百计，逐条 `printk` 会把日志冲爆。
更合适的工具是 kprobe/`drgn` 之类的动态观测（不在本系列“打点 + 实测”的框架内）。

## 可操作的结论（这条是确定的）

- **看 dcache 真实占用用 `/proc/slabinfo` 的 `dentry` 行**（`active_objs`/`num_objs`），
  不要用 `nr_unused`；
- `drop_caches` 不是“清空缓存”，只回收“无人引用、且在 LRU 上”的那部分。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/dcache.c:899` `dput()` | 引用计数减一 |
| `fs/dcache.c:812` `fast_dput()` | 无锁快路径 |
| `fs/dcache.c:744` `retain_dentry()` | 决定保留 / 放 LRU / 销毁 |
| `fs/dcache.c:489` `d_lru_add()` | `nr_dentry_unused` 加一 |
| `fs/dcache.c:191` `proc_nr_dentry()` | `/proc/sys/fs/dentry-state` 的读数刷新 |
| `fs/inode.c` / `fs/libfs.c` | inode 与 dentry 的绑定、`simple_lookup()` |
