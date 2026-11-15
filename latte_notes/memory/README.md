# 内存：三层记账、buddy、slab、以及内存紧张时的三级响应

前面几站一直在用内存（页表、rootfs、`__init` 段回收），这一站正面看它：
**页怎么分配、小对象怎么缓存、紧张时谁去回收、缺页时才真要页、回收先挑谁**。
7 篇主题文档，12 个打点，外加一次真的把 128M 机器写爆的实验。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 内存 | 分层 | [01-物理内存的三层记账](01-物理内存的三层记账.md) | memblock（区间）→ buddy（页）→ slab（对象） |
| 内存 | 分配 | [02-buddy-按2的幂分配页](02-buddy-按2的幂分配页.md) | 11 条 order 链表、分裂与合并、碎片 |
| 内存 | 对象缓存 | [03-slab-内核对象的缓存](03-slab-内核对象的缓存.md) | 页切槽位、per-CPU 缓存、自举问题 |
| 内存 | 回收 | [04-内存紧张时发生什么](04-内存紧张时发生什么.md) | 水位 → kswapd → 直接回收 → compaction → OOM |
| 内存 | 实测 | [05-实测-三种内存状态](05-实测-三种内存状态.md) | 2G / 128M / 写满 tmpfs 三组对照 |
| 内存 | 缺页 | [06-缺页与写时复制-malloc不分配页fork不拷页](06-缺页与写时复制-malloc不分配页fork不拷页.md) | `#PF` 才要页、`fork` 只复制页表、写才拷 |
| 内存 | 回收选择 | [07-LRU与第二次机会-访问过才活得更久](07-LRU与第二次机会-访问过才活得更久.md) | 一次访问做记号、二次访问升 active、压力下降级再丢 |

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

上面是“页从哪来”；下面是“**什么时候才来要**”（06）：

```
用户态写 → #PF(err=6 页不存在 / err=7 页只读) → do_user_addr_fault
  └─ handle_mm_fault
      ├─ 页不存在 → do_anonymous_page → alloc_pages
      └─ 页只读   → do_wp_page → wp_page_copy（引用计数 >1 才真拷）
fork → dup_mmap：只复制页表项并清可写位，一个字节都不拷
```

回收先挑谁（07）：页在 active / inactive 两条链上流转，

```
第一次访问 → 只置 PG_referenced（不动链表，页还在 inactive）
第二次访问 → lru_activate() 搬到 active（PGACTIVATE +1，发一张复活券）
内存紧张   → shrink_inactive_list 先扫 inactive；不够就把 active 降级
             （shrink_active_list）→ 下一轮没再被访问就回收
```

## 打点清单

| 打点 | 位置 | 本次观察到 |
|---|---|---|
| `kmem_cache_init: bootstrap caches ready` | `mm/slub.c` | 三组实验都是 0.000s |
| `kswapd: node%d reclaim thread ready` | `mm/vmscan.c` | 2.20~2.45s |
| `balance_pgdat: order=%d` | `mm/vmscan.c` | 只在写爆内存时出现，16 次 |
| `direct reclaim: order=%u` | `mm/page_alloc.c` | 只在写爆内存时出现，3 次 |
| `do_user_addr_fault: addr=… err=…` | `arch/x86/mm/fault.c` | 每次用户态缺页一次，`err` 说明原因 |
| `handle_mm_fault: addr=… flags=…` | `mm/memory.c` | mm 层总入口，比 #PF 略多（GUP 也会调） |
| `do_anonymous_page: addr=…` | `mm/memory.c` | 探针 3 页各 1 次 |
| `wp_page_copy: addr=…` | `mm/memory.c` | 探针 2 次（第 3 页走了复用） |
| `dup_mmap: vmas=…` | `mm/mmap.c` | fork 时 1 次（`vmas=12`） |
| `mark_accessed: pfn=… ref=… act=…` | `mm/swap.c` | 读两遍 1MB 文件：第一遍全是 `ref=0 act=0` |
| `lru_activate: pfn=… nr=…` | `mm/swap.c` | 第一遍 31 次（噪声）→ 第二遍 248 次 |
| `shrink_node: order=… target=…` | `mm/vmscan.c` | 压 200MB 时 347 次 |

**刻意不在 `__alloc_pages()` 快路径与 `shrink_folio_list()` 里打点**：前者每次分配都会走，
后者每 32 页一批，压力下上千行（本次 4MB 文件那版就打出了 17MB 日志）。
`shrink_node()` 每轮回收才一次，实测 347 次，可以打。
（在分配器里 `printk` 本身还要分配内存，容易把问题放大。）

## 两个反直觉结论（详见主题五）

1. **rootfs 是 tmpfs，上限 = RAM 的 50%**：`/proc/mounts` 里
   `rootfs / rootfs rw,size=1009404k` 就是证据。往 `/` 写文件超过一半内存就 `ENOSPC`。
2. **回收会“空转”**：写满 tmpfs 时 kswapd 被唤醒 16 次、直接回收 3 次，
   但 `pgscan_*`/`pgsteal_*` 一直是 0——没有 swap，tmpfs 页不可换出也不可丢弃，
   扫不到可回收页，最后只能 OOM。

## 自查 5 问

1. 为什么需要三层（区间/页/对象）？（01：粒度差三个数量级）
2. order 10 那一列变 0 意味着什么？（02：连续 4M 块没了，高阶分配要 compaction 或失败）
3. `pgscan` 为 0 但 OOM 了，怎么解释？（05：可回收页为 0，回收空转）
4. `fork` 之后写同一页，为什么有时拷有时不拷？（06：看引用计数；只剩自己就就地复用）
5. 同一个文件读一遍和读两遍，页在 LRU 上的位置差在哪？（07：第一遍只置引用位，
   第二遍才 `lru_activate` 进 active；但 active 也会被降级回收）

## 下一阶段

内存这条线基本走完（分配 → 缺页 → 回收 → 回收先挑谁）。
还能往下挖：**swap 换出路径**（需要交换设备或 `CONFIG_ZRAM`）、
**cgroup 内存控制**（`memory.max`、memcg LRU）。
两处已经补上了：`writeback` 见 `vfs/05`，**page cache 与 mmap** 见 `vfs/06`。
