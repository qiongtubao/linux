# 块层：一个 bio 怎么变成发给设备的请求

VFS 之下、驱动之上还有一层：**块层（block layer）**。它负责把“我要读这 2MB”
变成“给设备发若干个不超过队列上限的请求”，再负责排队、合并、完成回调。
这一站从 `blk-mq` 入手：先实测一次冷读看清 bio→request 的拆分，
再去试三个 IO 调度器——第二篇是个诚实的负结果（这套虚拟机上测不出差别，以及为什么）。
1 份索引 + 2 篇主题文档，3 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 磁盘 | 请求旅程 | [01-一次读请求的旅程-bio到virtio](01-一次读请求的旅程-bio到virtio.md) | bio 按 `max_sectors_kb` 拆成多个 request |
| 磁盘 | 调度器 | [02-IO调度器-为什么虚拟机上测不出差别](02-IO调度器-为什么虚拟机上测不出差别.md) | 队列不堆就没得排；限速器又接管了节奏 |

## 一条链

```
VFS / page cache（vfs/05、vfs/06）
 └─ submit_bio()                     block/bio.c            ← 打点（vfs/05 已有）
     └─ blk_mq_submit_bio()          block/blk-mq.c:3120    ← 打点
         ├─ 按 max_sectors_kb 拆成若干 request（可能 1→N）
         ├─ 调度器排队（本机默认 mq-deadline）
         └─ blk_mq_start_request()   block/blk-mq.c:1350    ← 打点
             └─ virtio_blk 的 queue_rq → virtqueue
                 └─ blk_mq_end_request()  block/blk-mq.c:1158 ← 打点
```

## 实测一组数字（详见主题一）

| 阶段 | bio | `blk_mq_submit_bio` | `start_rq` | 请求尺寸 |
|---|---|---|---|---|
| A `max_sectors_kb=4096` | 3（2×2MB + 4KB） | 3 | 3 | 2×2MB + 1×4KB（**不拆**） |
| B `max_sectors_kb=128` | 3（同上） | **33** | **33** | **32×128KB** + 1×4KB |

同一个 2MB 的 bio，在不同 `max_sectors_kb` 下变成 1 个或 16 个请求。

## 自查 3 问

1. 文件系统交下来的 bio 会直接发给设备吗？（01：不会，`blk_mq_submit_bio()` 会按
   `max_sectors_kb` 拆成多个 request，再交给调度器与驱动）
2. `/sys/block/vda/queue/` 下哪几个文件能解释“拆分”？（01：`max_sectors_kb`、
   `nr_requests`、`scheduler`、`rotational`）
3. 三个调度器在本机为什么看不出差别？（02：不限速时 `inflight` 恒为 0，队列没有可排的东西；
   限速后节奏由限速器决定，且重复测量的波动（−28%~+15%）大于效应本身）

## 下一阶段

块层还能挖：**用 tracepoint 重做调度器对比**（`block:block_rq_insert` / `block_rq_issue`
直接给内核时间戳，不再靠 `dmesg` 切窗口；再配 `fio --iodepth=32` 固定队列深度）、
**队列深度与 tag 等待**（`nr_requests`、`blk_mq_get_tag` 的 sleep 路径）、
**`blk_mq_end_request_batch`**（一次完成 32 个 tag）、
**`/proc/diskstats` 各列与 `iostats` 开关**、**`dm-delay` 造真实延迟**（需 `CONFIG_DM_DELAY`）。
