# 内存管理闭环：memblock → buddy → slab → vmalloc

承接 `6cb9b35`（中断初始化）与 `eabe460`（setup_arch 打点），本文讲清 `setup_arch()`
建骨架、`mm_core_init()` 让页和对象真可用、`page_alloc_init_late()` 收尾这三段的因果。

## 一句话模型

四层，每层都在补上一层的短板：

| 层 | 名字 | 管什么 | 特点 |
|---|---|---|---|
| 1 | memblock（启动期记账本） | 纯物理区间记账：谁占了哪段 | 只增不减的链表，不产生 `struct page` |
| 2 | buddy（伙伴系统） | 4K 页的货架，按 order 分档 | 只能整页给/整页收，反复分配释放会碎 |
| 3 | slab（对象缓存） | 把页切成固定大小的格子 | 小对象不浪费整页，还带复用 |
| 4 | vmalloc（虚拟连续地址） | 物理不连续、虚拟连续 | 用来做大块映射，不适合高频小分配 |

调用链（本阶段地图）：

```
setup_arch()
 └─ paging_init() → zone_sizes_init() → free_area_init()   ← 建 zone / struct page 骨架（还没有空闲页）
start_kernel()
 ├─ trap_init()
 └─ mm_core_init()
     ├─ build_all_zonelists()     ← zone 优先级表
     ├─ memblock_free_all()       ← 空闲页进入 buddy ★
     ├─ mem_init()                ← x86: after_bootmem = 1
     ├─ kmem_cache_init()         ← slab 自举 ★
     └─ vmalloc_init()            ← vmalloc 地址空间 ★
kernel_init_freeable()
 └─ page_alloc_init_late()        ← 打印 "Memory: …"、memblock_discard()
```

## 启动时序表

| 阶段 | 函数 | 文件:行 | 一句话 |
|---|---|---|---|
| setup_arch | `free_area_init` | `mm/mm_init.c:1832` | 建 zone 和全部 `struct page`，页全标 reserved |
| mm_core_init | `memblock_free_all` | `mm/memblock.c:2364` | 把可分配内存一次性交给 buddy |
| mm_core_init | `free_low_memory_core_early` | `mm/memblock.c:2306` | 遍历非 reserved 区间，逐段释放 |
| mm_core_init | `__free_memory_core` | `mm/memblock.c:2233` | 物理区间 → 页帧区间，向内对齐 |
| mm_core_init | `memblock_free_pages` | `mm/mm_init.c:2517` | memblock 释放页的统一入口 |
| mm_core_init | `__free_pages_core` | `mm/page_alloc.c:1588` | 清 Reserved、refcount 归零，页变可分配 |
| mm_core_init | `__free_one_page` | `mm/page_alloc.c:945` | buddy 合并核心，伙伴空闲就升 order |
| mm_core_init | `mem_init` | `arch/x86/mm/init_64.c:1460` | x86 架构收尾：`after_bootmem = 1` |
| mm_core_init | `kmem_cache_init` | `mm/slub.c:8478` | slab 自举的入口 |
| mm_core_init | `bootstrap` | `mm/slub.c:8438` | 静态 boot 结构搬家成真对象 |
| mm_core_init | `create_kmalloc_caches` | `mm/slab_common.c:966` | 建 kmalloc-8…8k，把 `slab_state` 推到 UP |
| mm_core_init | `vmalloc_init` | `mm/vmalloc.c:5265` | vmalloc 地址区间变成可分配 |
| mm_core_init | `mm_cache_init` | `kernel/fork.c:2962` | 建 `struct mm_struct` 的专用缓存 |
| kernel_init_freeable | `page_alloc_init_late` | `mm/mm_init.c:2334` | 补 deferred 页、打印 `Memory:`、丢 memblock |

## 六个概念（小白版）

**zone 与 node**：node 是 NUMA 的物理分组（一台机器可能只有 1 个），zone 是同一
node 内按“能怎么用”划的段（DMA/DMA32/Normal）。分配时先按 node 就近再按 zonelist 退。

**PFN 与 `struct page`**：物理内存按 4K 切页，页帧号（PFN）就是页的编号，
`struct page` 是它的元数据本子（refcount、flags、所属 zone 等）。
`free_area_init()` 干的就是把这个本子逐页初始化出来。

**order 与伙伴合并**：order 表示 2^order 个连续页，`MAX_PAGE_ORDER` 在 4K 页上是
10（即 4M）。释放时若“伙伴页”（PFN 异或 1<<order 那页）也空闲，就合并成 order+1，
所以反复分配释放不会把大块内存永久切碎。

**reserved / refcount 1→0**：`__init_single_page()` 把每页 refcount 设成 1，意思是
“已被占用”；`__free_pages_core()` 清掉 `PageReserved` 并归零 refcount，这一页才算空闲。
`free_area_init()` 之后不能分配，就是因为 refcount 还都是 1。

**slab 与 kmalloc 档位**：slab 把页切成同样大小的对象反复复用，`kmalloc()` 按请求
大小挑最近的档位（8/16/…/8K）。`slab_state` 到 UP 时 `kmalloc()` 才敢用。

**vmalloc 与直接映射的区别**：直接映射是“物理地址 + 固定偏移”，虚拟连续则物理
一定连续；vmalloc 区用页表拼出虚拟连续、物理可散落的大块地址，代价是多一层查表。

## 代码走读

**`free_area_init()`**：算出各 zone 的 PFN 上下界，为每个 node 建 `pg_data_t` /
zone / usemap，再逐页初始化 `struct page`（全标 reserved）。只搭骨架，无空闲页。

**`mm_core_init()`**：总调度。① 建 zone 优先级表和 CPU 热插拔回调；② 早期调试
设施（page_ext、加固开关、KFENCE、KMSAN、栈仓库、KHO）；③ `memblock_free_all()`
把空闲页交给 buddy；④ `mem_init()` 架构收尾；⑤ `kmem_cache_init()` + `vmalloc_init()`。

**`mem_init()`（x86_64）**：置 `after_bootmem = 1` 宣告 bootmem 阶段结束，所以它
必须排在 `memblock_free_all()` 之后。

**`memblock_free_all()`**：先释放 memmap 自己占的页、清零 managed 计数，再由
`free_low_memory_core_early()` 遍历可分配区间交给 buddy，最后累进 `totalram_pages`。

**`__free_pages_memory()`**：按 start 的对齐程度决定一次放多大的块
（`__ffs(start)`），块不够就降 order，再把这块交给 `memblock_free_pages()`。

**`__free_pages_core()`**：页真正变可分配的最后一步——清 `PageReserved`、
refcount 归零、累加 zone 的 managed_pages，然后绕过 PCP 挂到 buddy 链表尾。

**`__free_one_page()`**：buddy 的合并核心。找伙伴页，空闲且迁移类型可合就
`order++` 继续合，合不动了才挂到 `zone->free_area[order]` 的链表上。

**`kmem_cache_init()` + `bootstrap()`**：slab 要管缓存、缓存自己也占内存 → 鸡生蛋。
先用两个 `__initdata` 静态结构顶替，`kmem_cache_node` 可用后设 `slab_state =
PARTIAL`，再建 `kmem_cache`，`bootstrap()` 把内容搬进真对象并修正 `slab_cache`。

**`create_kmalloc_caches()`**：按 `KMALLOC_NORMAL/CGROUP/RECLAIM` 三类建档位，
非 2 的幂的 96/192 单独建，其余从 64B 建到 8K，结束置 `slab_state = UP`。

**`vmalloc_init()`**：建 `vmap_area` 缓存（依赖 slab 已 UP）、初始化每 CPU vmap 队列、
接管 `vmlist` 里的早期映射、`vmap_init_free_space()` 登记空闲段并置 `vmap_initialized`。

**`mm_cache_init()`**：建 `struct mm_struct` 缓存，含动态 `mm_cpumask`/`mm_cid`，
`SLAB_ACCOUNT` 参与 memcg 记账，usercopy 范围限定在 `saved_auxv`。

**`page_alloc_init_late()`**：补完 deferred 页、`mem_init_print_info()` 打印
`Memory: ... available`、建 buffer_head 哈希、`memblock_discard()` 丢记账本、
打散各 node 空闲链表。

## 三个关键问答

**为什么 `free_area_init()` 之后还不能分配页？**
它只建了 zone 和 `struct page` 骨架，每页 refcount 还是 1、`PageReserved` 还在，
buddy 的 `free_area` 链表全空。要等 `memblock_free_all()` 走 `__free_pages_core()`
清零 refcount 并挂链表，页才真能被拿走。

**为什么 `kmem_cache_init()` 要“鸡生蛋”？**
管理 slab 的结构（`kmem_cache`、`kmem_cache_node`）本身也要从 slab 分配。
硬件上没法自举，所以先用两个 `__initdata` 静态变量冒充缓存对象，
等能分配了再 `bootstrap()` 把内容搬进真对象并修正指针。

**为什么 `Memory:` 出现在 `page_alloc_init_late()` 而不是 `mm_core_init()`？**
总量/可用量在 deferred 页（开了 `CONFIG_DEFERRED_STRUCT_PAGE_INIT` 时）补齐
之前不准，那些页由 `page_alloc_init_late()` 里的 kthread 补完，所以打印得更晚。

## QEMU 实测

构建 `make -j16 bzImage` 得到 `bzImage is ready (#9)`；日志 `/tmp/latte-mm.log` 共 648 行。

`[latte]` 共 264 行，其中 245 行是完全相同的 `idt_setup_from_table 1`（每条表项打
一次），只滤掉这 245 行后剩 19 行原文：

    tr -d '\r' < /tmp/latte-mm.log | grep -n "\[latte\]" \
        | grep -v "idt_setup_from_table 1$"

```
56:[    0.000000] [latte] start_kernel -> setup_arch
58:[    0.000000] [latte] idt_setup_from_table 2
75:[    0.000000] [latte] setup_arch -> e820__end_of_ram_pfn
80:[    0.000000] [latte] setup_arch -> e820__memblock_setup
81:[    0.000000] [latte] setup_arch -> init_mem_mapping
99:[    0.000000] [latte] setup_arch -> initmem_init
103:[    0.000000] [latte] setup_arch -> x86_init.paging.pagetable_init
147:[    0.000000] [latte] main -> trap_init
148:[    0.000000] [latte] trap_init
149:[    0.000000] [latte] idt_setup_from_table 19
150:[    0.000000] [latte] idt_setup_traps
152:[    0.000000] [latte] main -> mm_core_init
157:[    0.000000] [latte] mm_core_init -> memblock_free_all
158:[    0.000000] [latte] memblock_free_all: 504690 pages -> buddy
159:[    0.000000] [latte] mm_core_init -> mem_init
160:[    0.000000] [latte] mm_core_init -> kmem_cache_init
162:[    0.000000] [latte] mm_core_init -> vmalloc_init
175:[    0.000000] [latte] idt_setup_from_table 12
423:[    0.938300] [latte] kernel_init_freeable -> page_alloc_init_late
```

`SLUB:` 与 `Memory:` 原文：

```
161:[    0.000000] SLUB: HWalign=64, Order=0-3, MinObjects=0, CPUs=1, Nodes=1
424:[    0.938629] Memory: 2015128K/2096632K available (18425K kernel code, 2896K rwdata, 7324K rodata, 2856K init, 672K bss, 77820K reserved, 0K cma-reserved)
```

四条时序都对上了：

- `Zone ranges:`(104)、`Early memory node ranges`(109)、`Initmem setup node 0`(112)
  全在 `main -> mm_core_init`(152) 之前 —— 骨架确实来自 `setup_arch`。
- `SLUB:`(161) 夹在 `kmem_cache_init`(160) 与 `vmalloc_init`(162) 之间。
- `Memory:`(424) 紧跟 `page_alloc_init_late`(423)。
- 638 行 `=== Mounting essential filesystems ===`，已进用户态 init。

`504690` 页 ≈ 1.92G，比 `-m 2G` 的 524288 页少的是内核镜像、页表、initrd 等
保留区；这个数非 0 就说明 buddy 真的拿到页了。


## 自查 5 问

1. order 最大是多少？—— `MAX_PAGE_ORDER`，4K 页上是 10，即一次最大 4M。
2. `slab_state` 四个值？—— DOWN 无功能 / PARTIAL 仅 kmem_cache_node / UP 可
   kmalloc 但附加功能未就绪 / FULL 全部就绪。
3. memblock → buddy 的链？—— `memblock_free_all()` → `free_low_memory_core_early()`
   → `__free_memory_core()` → `__free_pages_memory()` → `__free_pages_core()`。
4. 为什么要合并且合并到 order 更大？—— 不合并反复分配释放会把连续大块切成碎页，
   之后要 4M 大页就再也拿不到。
5. `memblock` 何时彻底消失？—— `page_alloc_init_late()` 的 `memblock_discard()`，
   那之后 `memblock` 的内存也还给 buddy。

## 下一阶段

`page_alloc_init_late()` 之后是 `do_basic_setup()` 的 initcall 阶段和 `free_initmem()`
（把 `__init` 段还给 buddy）；下一步可深入 `rest_init → 1 号进程（kernel_init）`。
