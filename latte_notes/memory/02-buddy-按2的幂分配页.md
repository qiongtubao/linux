# 主题二 · buddy：按“2 的幂”分配页

## 你已知的

内存按页管理，x86 上一页 4K。如果每次分配都随便找几页给出去，久了会碎得没法用
（想要连续 4 页却到处只有单个空页）。buddy 系统就是为解决这个：**按 2 的幂成块管理**。

## 11 条链表

`free_area[order]`，order 0..10（MAX_ORDER-1），每条挂着“大小 = 2^order 页”的空闲块：

| order | 块大小 | 用途举例 |
|---|---|---|
| 0 | 1 页（4K） | 普通用户页、页缓存 |
| 1 | 2 页（8K） | 小对象批量 |
| 3 | 8 页（32K） | `PAGE_ALLOC_COSTLY_ORDER`，从这里开始算“高阶” |
| 9 | 512 页（2M） | 大页（THP）、DMA 大块 |
| 10 | 1024 页（4M） | 最大块 |

## 分配与释放的“分裂 / 合并”

- **分配 order=N**：先看 `free_area[N]` 有没有；没有就往上找更大的块，把它**切成两半**
  （一半给你，另一半留在 order-1），一直切到 N。
- **释放**：看你的“伙伴”（同一个大块里的另一半）是不是空闲且同 order；是就**合并**，
  继续往上合。伙伴地址可以由 `page ^ (1 << order)` 算出来。

这就是名字的来历：两块互为 buddy。

## `/proc/buddyinfo` 怎么读

```
Node 0, zone      DMA      0      0      0  ...
Node 0, zone    DMA32   1234    456     78  ...
Node 0, zone   Normal  20000  10000   5000  ...
```

每行：`Node X, zone Y` 后面 **11 个数字 = order 0..10 各自还有多少个空闲块**。
本次实测的两台机器（2G / 128M）数据见主题五。

## 碎片：为什么高阶会不够

小块分配释放很频繁，但高阶块只有等“连续的大片”被整块释放才会出现。所以长时间运行
后典型现象是：**order 0 很多，order 9/10 是 0**。此时需要：

| 手段 | 做什么 |
|---|---|
| compaction（内存压缩） | 把可移动页搬到一起，腾出连续块 |
| 回收（reclaim） | 丢掉可回收页（页缓存），让 buddy 拿到更大的块 |
| OOM killer | 实在不行，杀进程 |

## 代码位置

| 位置 | 内容 |
|---|---|
| `mm/page_alloc.c` `free_area` / `MAX_ORDER` | 11 条空闲链表 |
| `mm/page_alloc.c` `__rmqueue_smallest()` / `expand()` | 找块 + 分裂 |
| `mm/page_alloc.c` `__free_one_page()` | 释放 + 合并 buddy |
| `mm/page_alloc.c` `pageblock_order` | compaction 的移动单位 |
| `mm/vmstat.c:2289` | `/proc/buddyinfo` 的注册处 |
