# swap：让匿名页变得可回收

上一站（`memory/`）的结论是“没有 swap 时回收空转、最后 OOM”。这一站把它反着验证：
**挂一块 64M 交换区，同样的 100M 写入能不能写完？** 能，而且 `pgsteal` 从 0 涨到 6599 页。
再在换出/换入的逐页路径上按 1/512 采样打点，和三个计数器对账；
最后把交换区搬进内存（zram），量一次压缩比。
1 份索引 + 6 篇主题文档，3 个打点（1 个 `swapon` + 2 个采样点）。
唯一改 `.config` 的是 `06`：`CONFIG_ZRAM=y`。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 内存 | 为什么空转 | [01-为什么没有swap回收就空转](01-为什么没有swap回收就空转.md) | 匿名页/tmpfs 页没有第二份备份 → 不可回收 |
| 内存 | 四个角色 | [02-swap的四个角色](02-swap的四个角色.md) | 交换区、swap_map、swap cache、rmap 各管什么 |
| 内存 | 代码路径 | [03-换出与换入的代码路径](03-换出与换入的代码路径.md) | 拿槽位 → 摘 PTE → 写设备；换入走缺页 |
| 内存 | 实测 | [04-实测-加swap前后](04-实测-加swap前后.md) | 无 swap 时 OOM，有 swap 时 100M 写完 |
| 内存 | 路径打点 | [05-换出与换入-把逐页路径打上采样点](05-换出与换入-把逐页路径打上采样点.md) | 1/512 采样：6000 次换出只剩 3 行，三个口径对上 |
| 内存 | 内存里的盘 | [06-zram-把交换区放进内存并压缩](06-zram-把交换区放进内存并压缩.md) | 换出 4.84MB 只占 112KB（72:1）；随机数据无效 |

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
| `swap_out: page#N`（每 512 次一行） | `mm/page_io.c` `__swap_writepage()` | 3 行：`#4609`/`#5121`/`#5633` |
| `swap_in: page#N`（每 512 次一行） | `mm/page_io.c` `swap_read_folio()` | 读回 60MB 时触发 |
| `mm_stat` 的 `orig/compr/mem_used` | `/sys/block/zram0/mm_stat` | `06` 用：4.96MB → 67KB → 112KB |

**换出/换入是逐页走的最热路径**（一次 60M 换出就是 1.5 万次），全打会把日志冲爆。
所以 `swap/04` 用 `/proc/vmstat` 的计数器取证；`swap/05` 补上了**按 1/512 采样**的
逐页打点：6000 次调用只留 3 行，而 `page#N` 里的计数还能反推真实调用次数。

## 自查 5 问

1. 为什么没有 swap 时 tmpfs 写满会 OOM，而不是“回收掉一部分”？（01：tmpfs 页没有磁盘备份）
2. 换出为什么必须先摘 PTE 再写盘？（03：先写盘则中间修改会丢）
3. `pswpout` 只有 39 页，能据此说“只换出了 39 页”吗？（04/`qa/01`：不能，
   全零页靠 zeromap 免写盘；看 `SwapFree` 与 `pgsteal_*`）
4. 逐页路径既然太热不能打点，怎么才能看见它？（05：按 1/512 采样，
   并用 `page#N` 反推总次数；实测 6009 次换出对应最大采样点 `#5633`）
5. zram 的 `SwapFree` 掉了 4.84MB，内存为什么只少 112KB？（06：压缩后存，
   `mm_stat` 的 `orig_data_size=4.96MB`、`compr_data_size=67KB`、`mem_used_total=112KB`；
   但随机数据压缩比≈1，换出等于没省，第一次实验就是这么把机器拖死的）

## 下一阶段

swap 这条线走完了：为什么需要（01）、四个角色（02）、代码路径（03）、加 swap 前后（04）、
逐页打点对账（05）、内存里的压缩交换区（06）。`pswpout` 那个悬案由 `qa/01` 结案
（零页 + zeromap），而 `06` 顺带把“可压缩 vs 不可压缩”量了一遍。

还没做的是 **zswap**（在交换路径上做压缩缓存，需要 `CONFIG_ZSWAP=y` + `ZPOOL`；
它和 zram 的分工是“前者压缩后仍写磁盘、后者完全不落盘”）、以及
**zram 的多流压缩与写回**（`recomp_algorithm`、`writeback`，需要 backing device）。
