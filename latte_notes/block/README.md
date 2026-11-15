# 块层：一个 bio 怎么变成发给设备的请求

VFS 之下、驱动之上还有一层：**块层（block layer）**。它负责把“我要读这 2MB”
变成“给设备发若干个不超过队列上限的请求”，再负责排队、合并、完成回调。
这一站从 `blk-mq` 入手，用现成的 virtio-blk 盘实测一次冷读。
1 份索引 + 1 篇主题文档，3 个打点。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 磁盘 | 请求旅程 | [01-一次读请求的旅程-bio到virtio](01-一次读请求的旅程-bio到virtio.md) | bio 按 `max_sectors_kb` 拆成多个 request |

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

## 自查 2 问

1. 文件系统交下来的 bio 会直接发给设备吗？（01：不会，`blk_mq_submit_bio()` 会按
   `max_sectors_kb` 拆成多个 request，再交给调度器与驱动）
2. `/sys/block/vda/queue/` 下哪几个文件能解释“拆分”？（01：`max_sectors_kb`、
   `nr_requests`、`scheduler`、`rotational`）

## 下一阶段

块层还能挖：**IO 调度器对比**（`none` / `mq-deadline` / `kyber` 三个都在，
本机默认 `[mq-deadline]`，可用 `echo <名字> > /sys/block/vda/queue/scheduler` 切换，
再看 `dd` 并发读的顺序与合并效果）、**队列深度**（`nr_requests`、`blk_mq_get_tag` 的等待）、
**`blk_mq_end_request_batch`**（一次完成 32 个 tag 的批处理路径）、
**`/proc/diskstats` 各列的含义与 `iostats` 开关**。
