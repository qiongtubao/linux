# swap：让匿名页变得可回收

上一站（`memory/`）的结论是“没有 swap 时回收空转、最后 OOM”。这一站把它反着验证：
**挂一块 64M 交换区，同样的 100M 写入能不能写完？** 能，而且 `pgsteal` 从 0 涨到 6599 页。
1 份索引 + 4 篇主题文档，1 个打点（swapon），不改 `.config`。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 内存 | 为什么空转 | [01-为什么没有swap回收就空转](01-为什么没有swap回收就空转.md) | 匿名页/tmpfs 页没有第二份备份 → 不可回收 |
| 内存 | 四个角色 | [02-swap的四个角色](02-swap的四个角色.md) | 交换区、swap_map、swap cache、rmap 各管什么 |
| 内存 | 代码路径 | [03-换出与换入的代码路径](03-换出与换入的代码路径.md) | 拿槽位 → 摘 PTE → 写设备；换入走缺页 |
| 内存 | 实测 | [04-实测-加swap前后](04-实测-加swap前后.md) | 无 swap 时 OOM，有 swap 时 100M 写完 |

## 一条链

```
内存紧张 → shrink_folio_list()
  ├─ folio_alloc_swap()   拿交换槽位、进 swap cache
  ├─ try_to_unmap()       按 rmap 把所有 PTE 改成 swap entry
  ├─ swap_writeout()      写交换设备
  └─ 释放物理页
进程再访问 → #PF → do_swap_page() → 读回 → 重建 PTE
```

## 打点与关键读数

| 打点 / 读数 | 位置 | 本次值 |
|---|---|---|
| `swapon: activating swap area` | `mm/swapfile.c` | 1 次，4.43s |
| `Adding 65532k swap on /dev/sda` | `mm/swapfile.c`（内核原文） | 交换区生效 |
| `SwapFree` | `/proc/meminfo` | 65532 → 39144 kB |
| `pgsteal_kswapd` + `pgsteal_direct` | `/proc/vmstat` | 0 → 4535 + 2064 = 6599 页 |
| `pswpout` | `/proc/vmstat` | 39（与实际换出量不符，见主题四） |

**只加了 1 个新打点**：换出/换入是逐页走的最热路径（一次 60M 换出就是 1.5 万次），
在那里 `printk` 会把日志冲爆，也会干扰回收时序。所以用 `/proc/vmstat` 的计数器取证。

## 自查 3 问

1. 为什么没有 swap 时 tmpfs 写满会 OOM，而不是“回收掉一部分”？（01：tmpfs 页没有磁盘备份）
2. 换出为什么必须先摘 PTE 再写盘？（03：先写盘则中间修改会丢）
3. `pswpout` 只有 39 页，能据此说“只换出了 39 页”吗？（04：不能，看 `SwapFree` 与
   `pgsteal_*`；批量换出时该计数偏小）

## 下一阶段

swap 之外还剩：**zram/zswap**（把交换区放在内存里压缩，需要 `CONFIG_ZRAM`，本内核未开）、
**换出路径的细节**（`swap_writepage` → `swap_read_folio`，需要交换设备才能实测）。
两处已经补上了：**页面回收的 LRU 细节**（active/inactive 双链表、第二次机会）见 `memory/07`，
**VFS 路径查找**（dentry 缓存、`lookup_fast`）见 `vfs/01`~`04`。
