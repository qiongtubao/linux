# 内存：三层记账、buddy、slab、以及内存紧张时的三级响应

前面几站一直在用内存（页表、rootfs、`__init` 段回收），这一站正面看它：
**页怎么分配、小对象怎么缓存、紧张时谁去回收**。5 篇主题文档，4 个打点，
外加一次真的把 128M 机器写爆的实验。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 内存 | 分层 | [01-物理内存的三层记账](01-物理内存的三层记账.md) | memblock（区间）→ buddy（页）→ slab（对象） |
| 内存 | 分配 | [02-buddy-按2的幂分配页](02-buddy-按2的幂分配页.md) | 11 条 order 链表、分裂与合并、碎片 |
| 内存 | 对象缓存 | [03-slab-内核对象的缓存](03-slab-内核对象的缓存.md) | 页切槽位、per-CPU 缓存、自举问题 |
| 内存 | 回收 | [04-内存紧张时发生什么](04-内存紧张时发生什么.md) | 水位 → kswapd → 直接回收 → compaction → OOM |
| 内存 | 实测 | [05-实测-三种内存状态](05-实测-三种内存状态.md) | 2G / 128M / 写满 tmpfs 三组对照 |

## 一条链

```
memblock（启动早期区间记账）
  └─ memblock_free_all() ──► buddy free_area[0..10]
       ├─ alloc_pages() 快路径：直接摘块
       │    └─ 失败 → __alloc_pages_slowpath()
       │         ├─ 唤醒 kswapd ──► balance_pgdat() ×N
       │         ├─ 直接回收 ──► __perform_reclaim → try_to_free_pages()
       │         ├─ compaction
       │         └─ OOM killer
       └─ kmem_cache ──► slab：页切成对象槽位（kmalloc-* / task_struct / dentry）
```

## 打点清单

| 打点 | 位置 | 本次观察到 |
|---|---|---|
| `kmem_cache_init: bootstrap caches ready` | `mm/slub.c` | 三组实验都是 0.000s |
| `kswapd: node%d reclaim thread ready` | `mm/vmscan.c` | 2.20~2.45s |
| `balance_pgdat: order=%d` | `mm/vmscan.c` | 只在写爆内存时出现，16 次 |
| `direct reclaim: order=%u` | `mm/page_alloc.c` | 只在写爆内存时出现，3 次 |

**刻意不在 `__alloc_pages()` 快路径与 `shrink_node()` 里打点**：前者每次分配都会走，
后者在回收循环里被反复调用，打了会把日志冲爆（而且在分配器里 `printk` 本身就要分配
内存，容易把问题放大）。

## 两个反直觉结论（详见主题五）

1. **rootfs 是 tmpfs，上限 = RAM 的 50%**：`/proc/mounts` 里
   `rootfs / rootfs rw,size=1009404k` 就是证据。往 `/` 写文件超过一半内存就 `ENOSPC`。
2. **回收会“空转”**：写满 tmpfs 时 kswapd 被唤醒 16 次、直接回收 3 次，
   但 `pgscan_*`/`pgsteal_*` 一直是 0——没有 swap，tmpfs 页不可换出也不可丢弃，
   扫不到可回收页，最后只能 OOM。

## 自查 3 问

1. 为什么需要三层（区间/页/对象）？（01：粒度差三个数量级）
2. order 10 那一列变 0 意味着什么？（02：连续 4M 块没了，高阶分配要 compaction 或失败）
3. `pgscan` 为 0 但 OOM 了，怎么解释？（05：可回收页为 0，回收空转）

## 下一阶段

还剩两条可挖：**swap 与匿名页回收**（换出路径，本机没有交换设备，需要造一个 swap
文件或 zram）、**页面错误与写时复制**（`#PF` → `do_anonymous_page` / `wp_page_copy`，
与中断那站能接上）。
