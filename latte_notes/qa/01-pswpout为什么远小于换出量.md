# 主题一 · 为什么 `pswpout` 远小于实际换出量：全零页与 zeromap

## 问题回顾

`swap/04` 实测：写满 tmpfs 时 `SwapFree` 掉了 26388 kB（约 6597 页），
但 `pswpout` 只有 **39**。当时如实记为“未解”。

## 代码给的答案

`swap_writeout()`（`mm/page_io.c:241`）里有一条捷径：

```c
if (is_folio_zero_filled(folio)) {
	swap_zeromap_folio_set(folio);   /* 只记一个"这页是零"的标记 */
	goto out_unlock;                  /* 不写盘，也不调 count_swpout_vm_event() */
}
...
__swap_writepage(folio, swap_plug);   /* 真正写盘，里面才计 pswpout */
```

计数函数 `count_swpout_vm_event()` 只在**真正写盘**的路径上被调用。

而 `swap/04` 那次压力测试用的是：

```sh
dd if=/dev/zero of=/mnt/big bs=1M count=100
```

**写进去的全是零页** → 内核用 zeromap 记“这些槽位是零” → 跳过 IO → 不计 `pswpout`。
但交换槽位照样被占用了，所以 `SwapFree` 照样下降。**两个数字本来就不该相等。**

## 验证实验：把零换成随机数

同样 128M + 64M swap，只把 `dd` 的源换成 `/dev/urandom`：

| | 全零（`swap/04`） | 非零（本次） |
|---|---|---|
| `dd` 结果 | 100+0 records out | 100+0 records out |
| `SwapFree` | 65532 → 39144（用掉 26388 kB） | 65532 → **34384**（用掉 31148 kB） |
| `SwapCached` | 28 kB | 4128 kB |
| **`pswpout`** | **39** | **7811** |
| `pgsteal_kswapd` + `pgsteal_direct` | 6599 | 6790 |

`pswpout` 7811 页 × 4K ≈ 31244 kB，与 `SwapFree` 掉的 31148 kB（7787 页）**基本吻合**
（差的二十几页与在换出/换入之间来回的页有关）。

## 结论

- `pswpout` 不是“换出了多少页”的可靠指标，它只统计**真正产生 IO 的页**；
- 全零页走 zeromap 优化，不占 IO 但占槽位；
- 判断“换了多少”应看 `SwapFree` 的差值与 `pgsteal_*`，与 `swap/README` 里给的建议一致，
  现在有代码与对照实验双重支撑。

## 顺带一个副产品

`zeromap` 也解释了另一个现象：**全零页换出后再读回，不会触发磁盘 IO**（swap-in 时
直接从 zeromap 知道“这页是零”）。这就是“零页优化”在 swap 上的体现。

## 代码位置

| 位置 | 内容 |
|---|---|
| `mm/page_io.c:241` `swap_writeout()` | 零页检测与分流 |
| `mm/page_io.c:296` `count_swpout_vm_event()` | 唯一计 `pswpout` 的地方 |
| `mm/page_io.c:174` `is_folio_zero_filled()` | 判定是否全零 |
| `mm/page_io.c:204` `swap_zeromap_folio_set()` | 记“零页”标记 |
