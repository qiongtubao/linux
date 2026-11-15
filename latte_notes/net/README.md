# 网络收发：一次 ping 从网卡到 socket，再发出去

这是全库最后一个没碰过的领域：**网络**。给 QEMU 加一块 virtio 网卡，配 IP、ping 网关，
用 4 个打点把“中断 → NAPI 轮询 → 协议分发 → IPv4”整条链走了一遍，
再用 5 个打点把“IPv4 → ICMP → socket 接收队列”这一跳补完，
最后用 6 个打点把反方向“sendmsg → virtqueue”走通。
1 份索引 + 6 篇主题文档。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 网络 | 网卡 | [01-网卡与netdev-从PCI到eth0](01-网卡与netdev-从PCI到eth0.md) | PCI 探测 → `net_device` → MSI-X 中断 → NAPI |
| 网络 | 收包 | [02-收包路径-从中断到协议栈](02-收包路径-从中断到协议栈.md) | 中断只“叫一声”，轮询批量收，再按协议分发 |
| 数据 | 表示 | [03-数据包的表示-skb与socket](03-数据包的表示-skb与socket.md) | `sk_buff` 四指针、逐层剥头、最后拷进 socket |
| 网络 | 实测 | [04-实测-一次ping的收包链路](04-实测-一次ping的收包链路.md) | 5 次轮询收 4 个包、2 个进 IPv4 |
| 网络 | 最后一跳 | [05-ICMP到socket最后一跳](05-ICMP到socket最后一跳.md) | raw 先拿克隆、`icmp_rcv` 后看，都汇到 socket 队列 |
| 网络 | 发送 | [06-发送路径-从dev_queue_xmit到virtqueue](06-发送路径-从dev_queue_xmit到virtqueue.md) | 先 ARP 再发帧，TX 完成靠 NAPI 轮询 |

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
                     │   └─ ip_protocol_deliver_rcu
                     │       ├─ raw_local_deliver → raw_rcv_skb
                     │       │    └─ sock_queue_rcv_skb → sk_receive_queue  ← 打点
                     │       └─ icmp_rcv → ping_rcv                        ← 打点
                     └─ 0x86dd → IPv6
```

反方向（发包）只有一个汇聚点：

```
用户态 sendmsg / 内核造包
 └─ raw_sendmsg / icmp_reply / ip_output            ← 打点
     └─ __dev_queue_xmit                            ← 打点
         ├─ 邻居未解析 → 先发 ARP（42 字节）
         └─ dev_hard_start_xmit                     ← 打点
             └─ start_xmit（virtio）→ virtqueue_notify  ← 打点
                 └─ TX 中断 → virtnet_poll_tx       ← 打点
```

## 实测三组数字

| 观察点 | 数值 |
|---|---|
| `/proc/net/dev` eth0 RX | 260 B/3 包 → **504 B/6 包** |
| `/proc/interrupts` `virtio0-input.0` | **3 次** |
| 打点：`virtnet_poll` / `netif_receive_skb` / `ip_rcv_core` | **5 / 4 / 2** |

## 打点清单（15 个）

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `virtnet_probe: ...` | `drivers/net/virtio_net.c` | 1 次 |
| `virtnet_poll: budget=%d` | 同上 | 5 次（每次 `budget=64`） |
| `netif_receive_skb: len=… proto=… dev=…` | `net/core/dev.c` | 4 次（ARP 1 + IPv4 2 + IPv6 1） |
| `ip_rcv_core: len=%u` | `net/ipv4/ip_input.c` | 2 次 |
| `raw_local_deliver: proto=%d` | `net/ipv4/raw.c` | 2 次（`proto=1`=ICMP） |
| `raw_rcv_skb: len=%u` | 同上 | 2 次（`len=84`，含 IP 头） |
| `icmp_rcv: type=… code=… len=…` | `net/ipv4/icmp.c` | 2 次（`type=0`=ECHOREPLY） |
| `ping_rcv: len=%u` | `net/ipv4/ping.c` | 2 次（走到但 `ping_lookup` 未命中） |
| `sock_queue_rcv_skb: queued len=%u` | `net/core/sock.c` | 2 次（真入队，在 `spin_unlock` 之后） |
| `raw_sendmsg: len=%zu` | `net/ipv4/raw.c` | 2 次（`len=64`=用户态交来的 ICMP） |
| `icmp_reply: type=… len=…` | `net/ipv4/icmp.c` | 自 ping 1 次；ping 网关 0 次 |
| `dev_queue_xmit: len=… proto=… dev=…` | `net/core/dev.c` | 10 次（ARP 42、echo 98、IPv6 86/90） |
| `dev_hard_start_xmit: len=… dev=…` | 同上 | 10 次（与上一条同数） |
| `virtnet_xmit: len=… qnum=…` | `drivers/net/virtio_net.c` | 10 次（`dev=lo` 时 0 次） |
| `virtnet_poll_tx: budget=%d` | 同上 | 12 次（比发包多，NAPI 会空轮询） |

**`ip_rcv()` 里刻意没有打点**：实测证明它一次都不触发（GRO 走了批量路径 `ip_list_rcv`），
打点放在两条路径的汇合点 `ip_rcv_core()` 才抓得到。

## 自查 5 问

1. 为什么中断 3 次却收了 6 个包？（02/04：NAPI 一次轮询批量收，`budget=64`）
2. `skb->protocol` 有什么用？（02/03：决定交给哪个协议处理函数）
3. 收包过程中数据被拷了几次？（03：进用户态前几乎不拷，只移动指针；`recvmsg()` 才拷一次）
4. raw 与 ping socket 两条路径怎么区分？（05：看 `raw_local_deliver`/`raw_rcv_skb`
   打点有没有出现；`ping_table` 与 raw 哈希表是两张表，`ping_rcv` 再查一次才命中）
5. 同一个 ping，回包是谁造的？（06：`OutEchoReps` 只有内核 `icmp_reply()` 才 +1；
   ping `10.0.2.2` 时是宿主机上的 slirp 答的，guest 内核没参与）

## 下一阶段

网络还能挖：**TCP 状态机**（`tcp_v4_rcv` → `tcp_rcv_established`，需要 slirp `hostfwd`
加一个 guest 内 `httpd`）、**GRO/GSO 细节**（为什么 4 个包能成 1 个 list）。
也可以回到之前留的口子：`nr_unused` 统计口径、`pswpout` 计数偏小、zram/zswap。
