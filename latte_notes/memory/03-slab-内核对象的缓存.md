# 主题三 · slab：内核自己的“对象缓存”

## 你已知的

用户态 `malloc(256)` 要 256 字节；内核也要成千上万个 256 字节左右的小对象：
`task_struct`（进程）、`inode`/`dentry`（文件）、`sk_buff`（网络包）……
直接给每个对象分配一页（4K）会浪费 94%。

## slab 的做法：页切成槽位

每种对象一个**缓存**（`struct kmem_cache`），缓存从 buddy 拿页，再把页切成等长槽位：

```
buddy 给 1 页（4K）
 └─ kmem_cache("task_struct", objsize ≈ 1.4K, objperslab=2)
     ├─ 槽位 0 → task_struct
     └─ 槽位 1 → task_struct
```

每个缓存还有 **per-CPU 的当前 slab**：分配时优先从本 CPU 的槽位拿，不抢锁——这是
slab 分配快的主要原因（另一个是回收的对象直接回到缓存，不用还给 buddy）。

## 通用档位与专用缓存

| 类型 | 例子 | 用途 |
|---|---|---|
| 通用档位 | `kmalloc-8` … `kmalloc-8k` | 驱动里 `kmalloc(size)` 落到最近的档位 |
| 专用缓存 | `task_struct`、`dentry`、`filp`、`RAWv6` | 高频固定大小对象，`kmem_cache_create()` 建的 |

`/proc/slabinfo` 的头部与两行实例（本次 128M 实测）：

```
# name   <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> : tunables … : slabdata …
p9_req_t      0      0    160   25    1 : tunables 0 0 0 : slabdata  0  0  0
RAWv6        11     11   1408   11    4 : tunables 0 0 0 : slabdata  1  1  0
```

列含义：

| 列 | 含义 |
|---|---|
| `active_objs` / `num_objs` | 已分配对象数 / 该缓存总槽位数 |
| `objsize` | 每个对象的字节数（含调试/对齐开销） |
| `objperslab` / `pagesperslab` | 一个 slab 里多少槽位 / 占几页 |
| `slabdata` 三段 | 活跃 slab 数 / 总 slab 数 / 共享可用 |

`/proc/meminfo` 里的 `Slab:` 是这些缓存合计（本次 128M 下 `5796 kB`，2G 下 `6872 kB`）。

## 自举问题：缓存自己也要缓存

`struct kmem_cache` 本身也得从 slab 分配，鸡生蛋。所以 `kmem_cache_init()` 先用两个
`__initdata` 静态结构（`boot_kmem_cache`、`boot_kmem_cache_node`）冒充缓存，等能分配了
再“搬家”到真实对象里。

实测打点（两种内存下都是 0.000 秒）：

```
[latte] kmem_cache_init: bootstrap caches ready
```

之后 `slab_state` 推到 `PARTIAL` → 再建 kmalloc 各档位 → `FULL`，`kmalloc()` 才完全可用。

## 回收时 slab 也会被收缩

`shrink_slab()` 会回收“没用到的” slab 页（例如大量 dentry 缓存），这也是内存紧张时
`pgsteal_*` 可能上涨的原因之一。slab 的可回收部分在 `/proc/meminfo` 里可见：
`SReclaimable` / `SUnreclaim`。

## 代码位置

| 位置 | 内容 |
|---|---|
| `mm/slub.c:8478` `kmem_cache_init()` | 自举（打点所在） |
| `mm/slub.c` `get_slabinfo()` | `/proc/slabinfo` 的数据源 |
| `mm/slab_common.c` `kmem_cache_create()` | 建专用缓存 |
| `mm/slub.c` `__slab_alloc` / `slab_alloc_node` | 分配快路径（per-CPU 槽位） |
