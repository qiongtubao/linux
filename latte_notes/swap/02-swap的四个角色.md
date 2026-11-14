# 主题二 · swap 的四个角色：交换区、swap_map、swap cache、rmap

## 你已知的

swap 分区/文件就是磁盘上的一块地方，内核把内存页挪过去。但要让“挪回去还能找到”，
需要四个东西配合。少一个，换出就是丢数据。

## 四个角色

| 角色 | 是什么 | 解决什么问题 |
|---|---|---|
| **交换区**（`swap_info_struct`） | 一个设备或文件被 `swapon` 之后占一个槽位，`swap_info[]` 数组里一项 | 记录“这块交换空间多大、类型是什么” |
| **swap_map** | 每个交换槽位一个字节的计数表 | 记录“这个槽位被几个 PTE 引用”（1=独占，>1=共享，0=空闲） |
| **swap cache** | 一个以 `swp_entry_t` 为索引的 `address_space` | 页换出后可能仍在内存里（别人还在用），靠它按 swap 地址找回 |
| **rmap**（反向映射） | 每个匿名页记录“谁映射了我” | 换出前必须把所有 PTE 摘掉，否则页被复用后老 PTE 会指错 |

### 1. 交换区

```
swap_info[0] → /dev/sda（65532 kB，priority -2）
                ├─ swap_map[]   每槽位引用计数
                └─ cluster_info 分配用的簇信息
```

`swapon` 时内核读交换区头（`swap_header`，含 `version`、`last_page`、UUID 等），
再初始化 `swap_map`。`swapoff` 时反向恢复（把还在 swap 的页读回来）。

### 2. swap_map 的计数

一个交换槽位可能被**多个** PTE 引用——最典型是 `fork()` 之后父子共享的匿名页，
或者 `KSM` 合并过的页。计数 >1 时，换出只需写一份，换入也能共享同一份 swap cache。

### 3. swap cache

页换出后，若还有其他映射在用，它不会立刻被释放，而是挂在 swap cache 里：

| 状态 | 页在哪 | PTE 指向 |
|---|---|---|
| 刚换出、还有人用 | 内存（swap cache）+ 交换区 | swap entry |
| 全部 unmapped | 只在交换区 | 无（swap_map 减到 0 才真释放） |
| 换入后 | 内存 | 正常物理地址 |

### 4. rmap：为什么匿名页需要它

文件页可以靠“文件偏移 → page cache”反查；匿名页没有文件，所以内核维护
`anon_vma` 结构，记录“这个页被哪些 VMA/PTE 映射”。换出流程必须：

```
folio_alloc_swap()     拿槽位、进 swap cache
 → try_to_unmap()      按 rmap 把所有 PTE 改成 swap entry
 → swap_writeout()     真正写设备
 → 释放物理页
```

顺序不能反：**先摘 PTE，再写盘**。如果先写盘再摘 PTE，中间进程改了内容就会丢。

## 与本次实验的对应

| 角色 | 实验里怎么体现 |
|---|---|
| 交换区 | `Adding 65532k swap on /dev/sda` + `/proc/swaps` 一行 |
| swap_map | `SwapFree` 下降（槽位被占用） |
| swap cache | `SwapCached`（仍在内存的已换出页） |
| rmap | `pgsteal_*` 上涨（真的把页摘下来换出去了） |

## 代码位置

| 位置 | 内容 |
|---|---|
| `mm/swapfile.c:3425` `SYSCALL_DEFINE2(swapon)` | 建立交换区（打点所在） |
| `mm/swapfile.c:1412` `folio_alloc_swap()` | 取槽位 + 进 swap cache |
| `mm/swap_state.c:137` `swap_cache_add_folio()` | swap cache 维护 |
| `mm/rmap.c:1866` `try_to_unmap_one()` | 摘 PTE（换出前提） |
| `mm/page_io.c:241` `swap_writeout()` | 写交换设备 |
