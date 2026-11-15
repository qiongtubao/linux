# 网络收包：一次 ping 从网卡到协议栈

这是全库最后一个没碰过的领域：**网络**。给 QEMU 加一块 virtio 网卡，配 IP、ping 网关，
用 4 个打点把“中断 → NAPI 轮询 → 协议分发 → IPv4”整条链走了一遍。
1 份索引 + 4 篇主题文档。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 网络 | 网卡 | [01-网卡与netdev-从PCI到eth0](01-网卡与netdev-从PCI到eth0.md) | PCI 探测 → `net_device` → MSI-X 中断 → NAPI |
| 网络 | 收包 | [02-收包路径-从中断到协议栈](02-收包路径-从中断到协议栈.md) | 中断只“叫一声”，轮询批量收，再按协议分发 |
| 数据 | 表示 | [03-数据包的表示-skb与socket](03-数据包的表示-skb与socket.md) | `sk_buff` 四指针、逐层剥头、最后拷进 socket |
| 网络 | 实测 | [04-实测-一次ping的收包链路](04-实测-一次ping的收包链路.md) | 5 次轮询收 4 个包、2 个进 IPv4 |

## 一条链

```
MSI-X 中断（virtio0-input.0）
 └─ napi_schedule → 软中断 NET_RX_SOFTIRQ → net_rx_action
     └─ napi_poll → virtnet_poll(budget=64)          ← 打点
         └─ virtnet_receive → sk_buff → napi_gro_receive
             └─ netif_receive_skb → __netif_receive_skb_core   ← 打点
                 └─（按 skb->protocol 分发）
                     ├─ 0x0806 → ARP
                     ├─ 0x0800 → ip_list_rcv / ip_rcv → ip_rcv_core  ← 打点
                     └─ 0x86dd → IPv6
```

## 实测三组数字

| 观察点 | 数值 |
|---|---|
| `/proc/net/dev` eth0 RX | 260 B/3 包 → **504 B/6 包** |
| `/proc/interrupts` `virtio0-input.0` | **3 次** |
| 打点：`virtnet_poll` / `netif_receive_skb` / `ip_rcv_core` | **5 / 4 / 2** |

## 打点清单（4 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `virtnet_probe: ...` | `drivers/net/virtio_net.c` | 1 次 |
| `virtnet_poll: budget=%d` | 同上 | 5 次（每次 `budget=64`） |
| `netif_receive_skb: len=… proto=… dev=…` | `net/core/dev.c` | 4 次（ARP 1 + IPv4 2 + IPv6 1） |
| `ip_rcv_core: len=%u` | `net/ipv4/ip_input.c` | 2 次 |

**`ip_rcv()` 里刻意没有打点**：实测证明它一次都不触发（GRO 走了批量路径 `ip_list_rcv`），
打点放在两条路径的汇合点 `ip_rcv_core()` 才抓得到。

## 自查 3 问

1. 为什么中断 3 次却收了 6 个包？（02/04：NAPI 一次轮询批量收，`budget=64`）
2. `skb->protocol` 有什么用？（02/03：决定交给哪个协议处理函数）
3. 收包过程中数据被拷了几次？（03：进用户态前几乎不拷，只移动指针；`recvmsg()` 才拷一次）

## 下一阶段

网络还能挖：**发送路径**（`dev_queue_xmit` → `ndo_start_xmit` → virtqueue）、
**TCP 状态机**（`tcp_v4_rcv` → `tcp_rcv_established`）、**GRO/GSO 细节**。
也可以回到之前留的口子：`nr_unused` 统计口径、`pswpout` 计数偏小、zram/zswap。
