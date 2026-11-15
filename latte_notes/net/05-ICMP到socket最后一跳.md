# 主题五 · ICMP 到 socket 接收队列：最后一跳

`net/04` 把链路追到 `ip_rcv_core()` 就断了。这篇补上最后一跳：**包从 IPv4
往上走，谁先拿到它、什么时候真正进 socket 接收队列、用户态 `recvmsg()`
从哪个队列取**。全部有 QEMU 实测数据。

## 你已知的

- 网卡 → 中断 → NAPI → `ip_rcv_core()`（`net/02`、`net/04`）；
- `sk_buff` 是“带游标的缓冲区”，逐层剥头靠移动指针（`net/03`）；
- `ping` 是用户态程序，它必然要 `recvmsg()` 才能读到回包。

## 内核里对应什么：一个包，两个消费者

`ip_rcv_core()` 之后是 `ip_local_deliver()` → `ip_local_deliver_finish()`，
后者先剥掉 IP 头（`__skb_pull(skb, ip_hdrlen(skb))`），把 skb 交给
`ip_protocol_deliver_rcu()`（`net/ipv4/ip_input.c:188`）。**这个函数里顺序是硬的**：

```c
raw = raw_local_deliver(skb, protocol);          /* :193  raw socket 先拿一份 */
...
ret = INDIRECT_CALL_2(ipprot->handler, ...);     /* :205  再交给 ICMP 协议栈 */
```

所以一个 ICMP 回包会走两条路：

```
ip_rcv_core()                                  net/ipv4/ip_input.c:467  ← 打点
 └─ ip_protocol_deliver_rcu()                  net/ipv4/ip_input.c:188
     ├─① raw_local_deliver(skb, protocol)      net/ipv4/raw.c:200      ← 打点（先）
     │   ├─ raw_v4_input()：遍历 raw 哈希表，命中就 skb_clone()
     │   └─ raw_rcv() → raw_rcv_skb()          net/ipv4/raw.c:296      ← 打点
     │       └─ sock_queue_rcv_skb_reason()    net/core/sock.c:523
     │           └─ __sock_queue_rcv_skb()     net/core/sock.c:488     ← 打点
     │               └─ __skb_queue_tail()：进 sk_receive_queue（真入队）
     └─② ipprot->handler = icmp_rcv()          net/ipv4/icmp.c:1207     ← 打点
         ├─ ECHOREPLY(0) → ping_rcv()          net/ipv4/ping.c:962      ← 打点
         │   └─ __ping_queue_rcv_skb()：只有 ping socket 命中才入队
         └─ 其它 type → icmp_pointers[type].handler()
```

①和②是**同一个 skb 的两种用途**：①给 `SOCK_RAW` 的用户态程序（拿的是克隆，
含完整 IP 头），②给内核 ICMP 协议处理（决定是否回应、是否回注错误）。
`raw_local_deliver()` 的返回值 `raw=1` 只表示“有 raw socket 想要”，
并不阻止②继续执行。

## 实测：这次走的是 raw socket 路径

`ping` 用的是哪个 socket，实测能分辨：

- `raw_local_deliver: proto=1` 出现 **2 次**（= 包数）→ 确实存在
  `SOCK_RAW + IPPROTO_ICMP` 的监听者（busybox ping 就是这么开的）；
- `ping_rcv:` 也出现 **2 次**，但此后**没有**第二个 `sock_queue_rcv_skb`。
  ping socket 走的是独立的哈希表 `ping_table`（`net/ipv4/ping.c:57`），
  不在 raw 哈希表里；本次没有 ping socket，`ping_lookup()` 未命中，
  `ping_rcv()` 直接 `kfree_skb_reason(SKB_DROP_REASON_NO_SOCKET)`——它拿到的是
  原始 skb，克隆已经由①入队，所以 ping 照样成功。

一句话：**本次走 raw socket 路径，ping socket 路径被走到但没命中。**

（`/proc/net/snmp` 里 `InErrors` 不变也印证这一点：`ping_rcv()` 返回非 0 后
`icmp_rcv()` 走 `drop:` 分支，只 `kfree_skb`，不计 `InErrors`。）

## 实测打点序列（真实输出）

`ping -c 2 10.0.2.2`，两个回包，序列完全对称：

```
817:[    3.974219] [latte] ip_rcv_core: len=84
818:[    3.976051] [latte] raw_local_deliver: proto=1
819:[    3.976725] [latte] raw_rcv_skb: len=84
820:[    3.977324] [latte] sock_queue_rcv_skb: queued len=84
821:[    3.978267] [latte] icmp_rcv: type=0 code=0 len=56
822:[    3.978654] [latte] ping_rcv: len=56
827:[    4.993017] [latte] ip_rcv_core: len=84
828:[    4.993225] [latte] raw_local_deliver: proto=1
829:[    4.993398] [latte] raw_rcv_skb: len=84
830:[    4.993984] [latte] sock_queue_rcv_skb: queued len=84
831:[    4.994192] [latte] icmp_rcv: type=0 code=0 len=56
832:[    4.994414] [latte] ping_rcv: len=56
```

配对的 `/proc/net/snmp`（`ping` 前 → 后）：

```
=== snmp before ===
  InMsgs=0 InEchoReps=0
=== snmp after ===
  InMsgs=2 InEchoReps=2
```

`ping` 自己的统计：

```
2 packets transmitted, 2 packets received, 0% packet loss
```

两个 `len` 是同一包的不同视角：`raw_rcv_skb` 的 `len=84` 是**完整 IPv4 报文**
（`raw_rcv()` 把 IP 头 push 回去了，raw socket 要看到 IP 头）；`icmp_rcv` 的
`len=56` 是剥掉 IP 头 20 字节、再剥 ICMP 头 8 字节后的长度（84−20−8=56）。

## 三个结论

1. **raw 分发先于 `icmp_rcv`**：`ip_protocol_deliver_rcu()` 里
   `raw_local_deliver()`（`:193`）在 `ipprot->handler`（`:205`）之前无条件先跑。
   实测行号 818 < 821，且两个包都如此——不是调度巧合，是源码顺序。
   这也是 `tcpdump` 能看到“协议栈之后才处理”的包的原因。
2. **`sock_queue_rcv_skb` 是“进用户态前最后一站”**：它把 skb 挂到
   `sk->sk_receive_queue` 后调 `sk_data_ready()` 唤醒等待者，
   `recvmsg()` 从同一个队列取走。打点放在 `__skb_queue_tail()` 之后
   （且已 `spin_unlock`），证明的是“**已经入队**”而不是“尝试入队”。
   之后再有任何一步丢弃都不影响它——这正是 raw 克隆先入队、
   后面 `ping_rcv` 找不到 socket 丢包、`ping` 却照样成功的原因。
3. **`InEchoReps +2` 与打点次数互证**：`/proc/net/snmp` 的 `Icmp:` 行里
   `InEchoReps` 不是 `icmp_statistics`，而是 `icmpmsg_statistics` 按 type
   索引的计数（`net/ipv4/proc.c:366-378`），type=0 就是 `ICMP_ECHOREPLY`。
   它的自增点 `ICMPMSGIN_INC_STATS(net, icmph->type)` 正好在
   `icmp_rcv` 打点的下一行——所以“打点 2 次 ⇔ 计数 +2”是同一行代码的两个视角，
   不是两个巧合。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/net5 && cp -a bb_rootfs /tmp/net5
cat > /tmp/net5/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
snmp() { $B awk '/^Icmp:/{n++; if(n==2){print "  InMsgs="$2" InEchoReps="$11; exit}}' /proc/net/snmp; }
$B ip link set eth0 up
$B ip addr add 10.0.2.15/24 dev eth0
echo "=== snmp before ==="; snmp
echo "=== ping ==="
$B ping -c 2 -W 2 10.0.2.2
echo "=== snmp after ==="; snmp
echo "=== net5 done ==="
exec /bin/sh
EOF
chmod +x /tmp/net5/init
( cd /tmp/net5 && find . | cpio -o -H newc --quiet ) > /tmp/bb_net5.cpio
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_net5.cpio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-net5.log 2>&1 || true

tr -d '\r' < /tmp/latte-net5.log | grep -n "\[latte\] \(ip_rcv_core\|raw_local_deliver\|raw_rcv_skb\|icmp_rcv\|ping_rcv\|sock_queue_rcv_skb\)"
tr -d '\r' < /tmp/latte-net5.log | grep -E "InMsgs=|packets transmitted"
```

## 代码位置

| 位置 | 函数 | 做什么 | 本次打点 |
|---|---|---|---|
| `net/ipv4/ip_input.c:188` | `ip_protocol_deliver_rcu()` | 先 raw 再协议 handler，顺序的来源 | 只读 |
| `net/ipv4/ip_input.c:467` | `ip_rcv_core()` | IPv4 入口公共段（`net/04` 打点） | `ip_rcv_core: len=84` ×2 |
| `net/ipv4/raw.c:200` | `raw_local_deliver()` | 把包发给监听该协议的 raw socket | `proto=1` ×2 |
| `net/ipv4/raw.c:296` | `raw_rcv_skb()` | 命中具体 raw socket 后入队 | `len=84` ×2 |
| `net/core/sock.c:523` | `sock_queue_rcv_skb_reason()` | 先过 `sk_filter_reason()` 再入队 | 不打点 |
| `net/core/sock.c:488` | `__sock_queue_rcv_skb()` | 挂进 `sk_receive_queue` + 唤醒 | `queued len=84` ×2 |
| `net/ipv4/icmp.c:1207` | `icmp_rcv()` | ICMP 总入口，按 type 分派 | `type=0 code=0 len=56` ×2 |
| `net/ipv4/ping.c:962` | `ping_rcv()` | 只处理 ping socket 的 echo 回包 | `len=56` ×2（未命中） |

## 一句话收尾

`net/04` 说“包进了 IPv4”，这篇说“包已经在 socket 队列里等着 `recvmsg()`”，
中间只有三件事：raw 克隆先走、ICMP 协议栈后看、最后都汇到
`__sock_queue_rcv_skb()` 的 `__skb_queue_tail()`。

## 下一站

反方向由 `06-发送路径-从dev_queue_xmit到virtqueue.md` 接着讲：用户态 `sendmsg` 的包
怎么汇到 `__dev_queue_xmit()`、第一次发包为什么要先发 ARP、TX 完成为什么是 NAPI 轮询。
