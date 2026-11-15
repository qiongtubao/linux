# 主题三 · `nr_unused` 为什么几乎不动：证伪了一个假设，锁定了一个新疑点

## 问题回顾

`vfs/04` 与 `inode/04` 两次实测：建 100 个文件后 `nr_dentry` +100，但 `nr_unused`
只从 1 涨到 3；`echo 2 > drop_caches` 也清不掉。

当时列了三种可能，其中第一种是“**统计口径有问题**（读到的数是过期快照）”。
这一站用两个实验把它**证伪**，并把问题缩小到“新建的 dentry 为什么一直有引用”。

## 实验 A：先证明计数器本身是好的

```sh
cat /proc/sys/fs/dentry-state      # 基线
ls -R / > /dev/null                # 制造大量 dentry
cat /proc/sys/fs/dentry-state
echo 2 > /proc/sys/vm/drop_caches  # 手工回收
cat /proc/sys/fs/dentry-state
```

| 阶段 | `nr_dentry` | `nr_unused` |
|---|---|---|
| 基线 | 621 | 1 |
| `ls -R /` 之后 | **19423** | **18804** |
| `drop_caches=2` 之后 | **2715** | **1712** |

两件事同时被证明：

1. **`nr_unused` 会正常增长**（1 → 18804），读数不是过期快照；
   `proc_nr_dentry()`（`fs/dcache.c:191`）在每次读之前都会汇总 per-CPU 计数。
2. **`drop_caches=2` 会正常回收**（19423 → 2715，收回一万七千多个 dentry）。

**结论：统计口径没有问题。**

## 实验 B：异常只出现在“新建文件”这个场景

```sh
touch /f1 … /f100      # 一次调用建 100 个文件
cat /proc/sys/fs/dentry-state   # 立刻读
sleep 2
cat /proc/sys/fs/dentry-state   # 等 2 秒再读
ls -l / > /dev/null             # 让它们被"再查一次"
cat /proc/sys/fs/dentry-state
cat /f1 > /dev/null             # 重新打开再关闭一个
cat /proc/sys/fs/dentry-state
```

| 阶段 | `nr_dentry` | `nr_unused` |
|---|---|---|
| 基线 | 621 | 1 |
| `touch` 100 个文件后 | 721（+100） | 2 |
| 等 2 秒 | 721 | 2 |
| `ls -l /` 之后 | 721 | 2 |
| 重新打开 `/f1` 再关闭 | 721 | 2 |

**这 100 个 dentry 既不进 LRU（`nr_unused` 不涨），也没被销毁（`nr_dentry` 不掉）。**

## 现在的结论

| 假设 | 状态 |
|---|---|
| ① 统计口径/读数是过期快照 | ❌ **已证伪**（实验 A） |
| ② 它们走了另一条入 LRU 的路径 | 未验证 |
| ③ 有其他代码持有这些 dentry 的引用 | **最可能**（既不进 LRU 也不销毁 = 引用计数未归零） |

代码上本应发生的（`fs/dcache.c`）：

```c
fast_dput() → 计数减到 0 → retain_dentry(dentry, false)   /* 无锁时 punt */
            → 拿锁 → retain_dentry(dentry, true)
                → 若不在 LRU 上：d_lru_add()  → nr_dentry_unused++   ← 应发生却没发生
```

**这是本次能给出的最强结论**：不是计数器的问题，而是“这些 dentry 在 `dput` 之后没有
走到 `d_lru_add()`”。剩下的问题变成“谁还持有它们的引用”。

## 下一步的具体方法（配置已具备，未执行）

本内核已开 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`、`CONFIG_FTRACE=y`，
可以完全从用户态用 tracefs 观察，不必改代码：

```sh
mount -t tracefs none /sys/kernel/tracing
echo 'p:dp dput' >> /sys/kernel/tracing/kprobe_events        # 探 dput
echo 'p:rd retain_dentry' >> /sys/kernel/tracing/kprobe_events
echo 1 > /sys/kernel/tracing/events/kprobes/enable
# 然后跑 touch /x1 …，再读 trace 与 trace_pipe
cat /sys/kernel/tracing/trace | tail -40
```

注意 `d_lru_add()` 与 `retain_dentry()` 都是 `static`/`static inline`，可能被内联而
探不到；`dput()` 是全局符号，一定可以。**这一步留给后续**。

## 可操作的结论（这条是确定的）

- 看 dcache 真实占用用 `/proc/slabinfo` 的 `dentry` 行（`active_objs`/`num_objs`），
  不要用 `nr_unused`；
- `drop_caches` 不是“清空缓存”，只回收“无人引用、在 LRU 上”的那部分（实验 A 证实
  它能收回 16700 个，也证实它清不掉“仍被引用”的 100 个）。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/dcache.c:899` `dput()` | 引用计数减一（kprobe 的合适探点） |
| `fs/dcache.c:812` `fast_dput()` | 无锁快路径 |
| `fs/dcache.c:744` `retain_dentry()` | 决定保留/入 LRU/销毁 |
| `fs/dcache.c:489` `d_lru_add()` | `nr_dentry_unused` 加一 |
| `fs/dcache.c:191` `proc_nr_dentry()` | `/proc/sys/fs/dentry-state` 读数刷新 |
