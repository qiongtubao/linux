# 主题三 · 数据包的表示：`sk_buff` 与 socket

## 你已知的

网卡收到的是“一串字节”。内核要在这些字节前面加/减各层头部，还要能零拷贝地
在上面各层之间传递。`struct sk_buff`（简称 skb）就是干这个的。

## 四个指针，一块缓冲

```
 head          data                                   tail        end
  ↓              ↓                                       ↓          ↓
 ┌──────────────┬────────────┬──────────────────────────┬──────────┐
 │ 预留头部空间  │ 已填的头部  │        载荷数据           │  空闲    │
 └──────────────┴────────────┴──────────────────────────┴──────────┘
```

| 指针 | 含义 |
|---|---|
| `head` | 缓冲起点（分配时定下，之后不动） |
| `data` | 当前协议层的起点：加头时往前移，剥头时往后移 |
| `tail` | 数据结尾（发送时往里追加用） |
| `end` | 缓冲终点 |

另外还有三个“记录位置”的指针：`mac_header` / `network_header` / `transport_header`，
分别记住以太网头、IP 头、TCP/UDP 头在缓冲里的偏移——**不必复制数据**，各层只管动
`data` 和这些偏移。

## 收包时是“剥洋葱”

```
以太网帧          [eth][ip][icmp][data]
   ↓ 驱动填好 skb，data 指向 eth 头
netif_receive_skb 之后：skb_pull(eth 头)  → data 指向 ip 头
ip_rcv 之后：      skb_pull/记录           → 交给传输层
icmp/tcp/udp       → 取载荷
```

这就是为什么“协议栈”能高效分层：**同一块内存，只改指针**。

## skb 的其它关键字段

| 字段 | 作用 |
|---|---|
| `dev` | 从哪个网卡收的（或要发到哪个） |
| `protocol` | 以太网类型：`0x0800`=IPv4、`0x0806`=ARP |
| `len` / `data_len` | 总长度 / 非线性部分长度（分片、GSO） |
| `cb` | 各层私用的控制块（TCP/IP 各用各的） |
| `users` | 引用计数：多个“观察者”（如抓包）共享同一个 skb |

## 到了 socket 这一段

```
协议层找到匹配的 socket
 └─ 把 skb 挂进 sock->sk_receive_queue
     └─ 唤醒等在这个 socket 上的进程
         └─ recvmsg()：把载荷从 skb 拷进用户缓冲区（这一步是拷贝）
```

**收包流程里唯一的用户态拷贝就在 `recvmsg()`**，前面全是“移动指针 + 挂队列”。

## 三种 socket

| 类型 | 谁用 | ping 用哪种 |
|---|---|---|
| `SOCK_RAW`（`IPPROTO_ICMP`） | `ping`、抓包、自定义协议 | ✅ |
| `SOCK_DGRAM`（UDP） | DNS、DHCP、音视频 | — |
| `SOCK_STREAM`（TCP） | HTTP、SSH | — |

所以 `ping` 的 ICMP 回包最终是交给一个 **raw socket**，而不是“内核自己吃掉”。

## 与前几站的衔接

| 现象 | 上游文档 |
|---|---|
| 每个包都打一条 `netif_receive_skb` | 主题二（打点） |
| `/proc/net/dev` 的 RX/TX 计数 | 主题一（`net_device.stats`） |
| `virtio0-input.0` 的中断计数 | `interrupts/` 站（`/proc/interrupts`） |
| skb 从 slab 分配 | `memory/03`（slab 对象缓存） |

## 代码位置

| 位置 | 内容 |
|---|---|
| `include/linux/skbuff.h:889` `struct sk_buff` | 四个指针与各层 header 偏移 |
| `net/core/skbuff.c` `alloc_skb()` / `skb_pull()` | 分配与剥头 |
| `net/ipv4/icmp.c` `icmp_rcv()` | ICMP 处理（ping 的回包） |
| `net/core/sock.c` `sock_queue_rcv_skb()` | 把 skb 挂进 socket 接收队列 |
| `net/socket.c` `sock_recvmsg()` | 最后拷贝到用户态 |
