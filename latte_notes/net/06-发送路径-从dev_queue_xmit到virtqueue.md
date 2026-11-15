# 主题六 · 发送路径：从 `__dev_queue_xmit` 到 virtqueue

`net/05` 讲完收包最后一跳。这篇走反方向：**用户态写进 socket 的字节，怎么变成网线上的帧**，
以及一路上那些“看不见又绕不开”的中间站：路由、邻居（ARP）、qdisc、驱动队列、TX 完成中断。
全部 QEMU 实测。

## 你已知的

- 收包：网卡 → NAPI → `ip_rcv_core` → socket 队列（`net/02`、`net/04`、`net/05`）；
- `ping` 用 `SOCK_RAW + IPPROTO_ICMP`，回包是从 socket 接收队列里 `recvmsg()` 出来的（`net/05`）；
- `sk_buff` 逐层剥头靠移指针（`net/03`）。

## 内核里对应什么

发包的汇聚点只有一个：`__dev_queue_xmit()`。所有本地发出的包——用户态 `sendmsg` 的、
内核自己造的（ICMP 回包、TCP SYN、ARP）、转发的——最后都在这里排队上岗：

```
用户态 sendmsg / 内核造包
 └─ ip_send_skb / ip_output → ip_finish_output
     └─ __dev_queue_xmit(skb, sb_dev)            net/core/dev.c:4675   ← 打点
         ├─ 选发送队列（netdev_pick_tx / ndo_select_queue）
         ├─ 邻居没解析 → 先发 ARP，包暂存（这里是“多出 42 字节”的来源）
         ├─ qdisc 入队 → __qdisc_run → sch_direct_xmit
         │   └─ dev_hard_start_xmit              net/core/dev.c:3852   ← 打点
         │       └─ xmit_one → ndo_start_xmit
         │           └─ start_xmit（virtio-net） drivers/net/virtio_net.c:3376  ← 打点
         │               └─ xmit_skb → virtqueue_add_sgs
         │                   └─ virtqueue_kick_prepare/notify（敲门铃告诉 QEMU）
         └─ TX 完成中断 → NAPI → virtnet_poll_tx  drivers/net/virtio_net.c:3259 ← 打点
             └─ free_old_xmit：回收 skb、腾出 virtqueue 槽位
```

两条特殊出口，实测里都会出现：

- **ARP**：第一次给某个下一跳发包时，邻居表是空的，内核先发一个 42 字节 ARP 请求
  （`arp_send` → 同一个 `__dev_queue_xmit`），原包暂存在 `neigh` 的队列里，等应答回来再续发。
- **loopback**：`dev=lo` 时走 `dev_hard_start_xmit` → `loopback_xmit`，
  没有 virtqueue、没有 TX 中断——本机自己发的包立刻在本机收一遍。

还有一个容易搞混的点：**ICMP 回包有两个来源**。
- 目标是本机/内网设备时，回包由内核 `icmp_echo()` → `icmp_reply()` 就地造（`net/ipv4/icmp.c:407`）；
- 目标是 QEMU 用户态网络里的 `10.0.2.2` 时，回包由**宿主机上的 slirp 模拟**生成，
  随后作为外部包进入 guest 的收包路径——guest 内核**根本没有** `icmp_reply()` 这次调用。

## 实测 A：ping 外部网关（回包由 slirp 造）

`ping -c 2 10.0.2.2`，第一个包的完整序列：

```
[    5.805948] [latte] raw_sendmsg: len=64                       ← 用户态交来 64 字节 ICMP
[    5.812568] [latte] dev_queue_xmit: len=42 proto=0x0806 dev=eth0   ← 先发 ARP 请求
[    5.812901] [latte] dev_hard_start_xmit: len=42 dev=eth0
[    5.813224] [latte] virtnet_xmit: len=42 qnum=0
[    5.813797] [latte] virtnet_poll_tx: budget=64
[    5.815735] [latte] netif_receive_skb: len=50 proto=0x0806 dev=eth0 ← ARP 应答
[    5.817693] [latte] dev_queue_xmit: len=98 proto=0x0800 dev=eth0   ← 真正的 echo 请求
[    5.817995] [latte] dev_hard_start_xmit: len=98 dev=eth0
[    5.818468] [latte] virtnet_xmit: len=98 qnum=0
[    5.819815] [latte] virtnet_poll_tx: budget=64
[    5.820592] [latte] netif_receive_skb: len=84 proto=0x0800 dev=eth0 ← 回包
[    5.820959] [latte] ip_rcv_core: len=84
[    5.822805] [latte] raw_local_deliver: proto=1
[    5.823683] [latte] raw_rcv_skb: len=84
[    5.824512] [latte] sock_queue_rcv_skb: queued len=84
```

三个长度是同一包的三个视角：

| 打点 | len | 含义 |
|---|---|---|
| `raw_sendmsg` | 64 | 用户态交下来的 ICMP 部分（8 头 + 56 数据） |
| `dev_queue_xmit` / `dev_hard_start_xmit` / `virtnet_xmit` | 98 | 加上 20 字节 IP 头 + 14 字节以太头 |
| `ip_rcv_core`（收包侧） | 84 | 收到时以太头已被剥掉（98−14） |

**注意 84 这个数在 `net/04` 出现过**：那时它是收包打点的 len，这里它是发送路径
“加完 IP 头、还没加以太头”的长度。同一份 skb，收发两侧打点量的不是同一段。

第二个 ping 没有 ARP（邻居已缓存），`raw_sendmsg → dev_queue_xmit(98)` 直接连着出。

## 实测 B：ping 自己（回包由内核 `icmp_reply` 造）

`ping -c 1 10.0.2.15`（本机地址，走 `lo`）：

```
[    4.295757] [latte] raw_sendmsg: len=64
[    4.296510] [latte] dev_queue_xmit: len=98 proto=0x0800 dev=lo    ← 没有 ARP、没有 virtio
[    4.296842] [latte] dev_hard_start_xmit: len=98 dev=lo
[    4.297676] [latte] netif_receive_skb: len=84 proto=0x0800 dev=lo
[    4.297999] [latte] ip_rcv_core: len=84
[    4.298372] [latte] raw_local_deliver: proto=1
[    4.298600] [latte] raw_rcv_skb: len=84
[    4.298760] [latte] sock_queue_rcv_skb: queued len=84
[    4.298950] [latte] icmp_rcv: type=8 code=0 len=56                ← 自己收到 ECHO
[    4.299199] [latte] icmp_reply: type=0 code=0 len=56              ← 内核就地造回包
[    4.300877] [latte] dev_queue_xmit: len=98 proto=0x0800 dev=lo    ← 回包再走一遍发送链
[    4.301119] [latte] dev_hard_start_xmit: len=98 dev=lo
[    4.301669] [latte] netif_receive_skb: len=84 proto=0x0800 dev=lo
[    4.301911] [latte] ip_rcv_core: len=84
[    4.302031] [latte] raw_local_deliver: proto=1
[    4.302162] [latte] raw_rcv_skb: len=84
[    4.302440] [latte] sock_queue_rcv_skb: queued len=84
[    4.302655] [latte] icmp_rcv: type=0 code=0 len=56                ← 回包到 socket
[    4.302824] [latte] ping_rcv: len=56
```

一次自 ping 在日志里是**两个来回**：请求被自己收一遍（`type=8`），内核造出回包
（`icmp_reply: type=0`），回包又被自己收一遍（`type=0`）。注意 `dev=lo`
时 `virtnet_xmit` / `virtnet_poll_tx` 一次都不出现——驱动换了，汇聚点没换。

## 实测 C：三个计数器把两条回包来源分开

`/proc/net/snmp` 的 `Icmp:` 行（值取自 `icmpmsg_statistics` 按 type 索引 + 0x100 位）：

| 时刻 | `OutEchos` | `OutEchoReps` | `InEchoReps` |
|---|---|---|---|
| ping 前 | 0 | 0 | 0 |
| ping 网关（slirp 答）后 | **1** | **0** | 1 |
| 再 ping 自己（内核答）后 | 2 | **1** | 2 |

差分一目了然：

- `OutEchos` 只跟“本地发出的 echo **请求**”走，两次 ping 各 +1——不管是用户态 raw
  socket 发的还是内核发的。它由 `icmp_out_count()`（`net/ipv4/icmp.c:343`）在
  `__ip_make_skb()`（`net/ipv4/ip_output.c:1496`）里统一计数，所以**用户态发的包也算**。
- `OutEchoReps` 只有 `icmp_reply()` 真跑过才 +1（ping 自己那次）。
  网关那次为 0，正是“回包不是 guest 内核造的”的铁证。

## 实测 D：TX 计数与打点对账

同一次外部 ping（`/proc/net/dev` 的 eth0 TX 列，前 → 后）：

```
    eth0:       0  ...        90       1  ...      ← 前（开机时的 1 个 IPv6 包）
    eth0:     260  ...       504       6  ...      ← 后
```

前值那 1 个包是开机时的 IPv6 邻居发现，**重跑时也可能没有（0/0）**——所以只看差值：
**+414 字节** = 42（ARP）+ 98（echo 请求 1）+ 98（echo 请求 2）+ 86 + 90（两个 IPv6 ND 包），
与 `dev_queue_xmit` 打点逐条对得上；

- 打点计数：`raw_sendmsg` 2、`dev_queue_xmit` 10、`dev_hard_start_xmit` 10、
  `virtnet_xmit` 10、`virtnet_poll_tx` **12**、`icmp_reply` **0**。
- `virtnet_poll_tx` 比 `virtnet_xmit` 多 2 次：TX 完成是 NAPI 轮询，
  **不是“一个包一次回调”**，空闲时也会被轮一次。

## 四个结论

1. **发包只有一个汇聚点**：用户态 `sendmsg` 和内核造包走同一个 `__dev_queue_xmit()`，
   区别只在前面怎么把 skb 造出来。所以给一处打点就能看到所有出站流量。
2. **第一次给新下一跳发包会多出一个 ARP**：42 字节先出去，原包在邻居表里等；
   实测里 `dev_queue_xmit(42)` 就出现在 `raw_sendmsg(64)` 之后、真正的
   `dev_queue_xmit(98)` 之前。这也是“ping 第一包慢、后面快”的一个原因（不止是 RTT）。
3. **“回包由谁造”能用 `OutEchoReps` 判定**：内核答 → +1；外部/slirp 答 → 0。
   只做内网 ping 实验的人很容易把这两条路径混为一条。
4. **TX 完成是 NAPI 轮询而非每包一次中断**：`virtnet_poll_tx(budget=64)` 在
   `virtqueue_notify()` 之后由 TX 中断触发，回收 `free_old_xmit`；次数与包数不必相等。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/net6 && cp -a bb_rootfs /tmp/net6
cat > /tmp/net6/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
dev() { $B awk '/eth0/{print "  "$0; exit}' /proc/net/dev; }
snmp() { $B awk '/^Icmp:/{n++; if(n==2){print "  OutEchos="$25" OutEchoReps="$26" InEchoReps="$11; exit}}' /proc/net/snmp; }
$B ip link set lo up
$B ip link set eth0 up
$B ip addr add 10.0.2.15/24 dev eth0
echo "=== dev before ==="; dev
echo "=== snmp before ==="; snmp
echo "=== ping gateway (slirp/宿主机答) ==="
$B ping -c 2 -W 2 10.0.2.2
echo "=== snmp mid ==="; snmp
echo "=== ping self (内核答) ==="
$B ping -c 1 -W 2 10.0.2.15
echo "=== dev after ==="; dev
echo "=== snmp after ==="; snmp
echo "=== net6 done ==="
exec /bin/sh
EOF
chmod +x /tmp/net6/init
( cd /tmp/net6 && find . | cpio -o -H newc --quiet ) > /tmp/bb_net6.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_net6.cpio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-net6.log 2>&1 || true

tr -d '\r' < /tmp/latte-net6.log | grep -nE \
  "\[latte\] (raw_sendmsg|icmp_reply|dev_queue_xmit|dev_hard_start_xmit|virtnet_xmit|virtnet_poll_tx|icmp_rcv)"
tr -d '\r' < /tmp/latte-net6.log | grep -E "OutEchos=|eth0:"
```

## 代码位置

| 位置 | 函数 | 做什么 | 本次打点 |
|---|---|---|---|
| `net/ipv4/raw.c:489` | `raw_sendmsg()` | 用户态 raw socket 发包入口 | `len=64` ×2 |
| `net/ipv4/icmp.c:407` | `icmp_reply()` | 内核收到 ECHO 后就地造回包 | 自 ping 时才出现 |
| `net/ipv4/icmp.c:343` | `icmp_out_count()` | 本地发出的 ICMP 包计数（含用户态） | 只读，解释 `OutEchos` |
| `net/ipv4/ip_output.c:1496` | `__ip_make_skb()` | 调用上一行的位置 | 只读 |
| `net/core/dev.c:4675` | `__dev_queue_xmit()` | 所有发包的汇聚点：选队列 → qdisc | `len=42/98/86/90` |
| `net/core/dev.c:3852` | `dev_hard_start_xmit()` | qdisc 交出 skb，调 `ndo_start_xmit` | 与上一条同数 |
| `drivers/net/virtio_net.c:3376` | `start_xmit()` | virtio 的 `ndo_start_xmit`：入环 + kick | `qnum=0` |
| `drivers/net/virtio_net.c:3259` | `virtnet_poll_tx()` | TX 完成 NAPI：回收 skb、腾槽位 | 12 次（> 发包数是正常的） |

## 一句话收尾

收包那条链的终点是 `sk_receive_queue`（`net/05`），发包这条链的起点是
`sendmsg`、终点是 `virtqueue_notify()`——两者在 `__dev_queue_xmit()` 处分叉，
在同一块 virtio 网卡上闭环。
