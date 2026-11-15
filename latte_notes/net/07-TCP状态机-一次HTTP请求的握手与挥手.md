# 主题七 · TCP 状态机：一次 HTTP 请求的握手与挥手

`net/04`~`06` 走的都是 ICMP，无连接、一次一个包。这篇换成 TCP：
宿主机 `curl` 进来，guest 里的 `busybox httpd` 应答，50 毫秒里走完
**三次握手 → 一个请求一个响应 → 四次挥手**，日志把它摊成 14 行。

## 你已知的

- 收包：网卡 → NAPI → `ip_rcv_core` → raw/ICMP（`net/02`、`net/04`、`net/05`）；
- 发包：`__dev_queue_xmit` → virtqueue（`net/06`）；
- TCP 有 11 个状态（`ESTABLISHED`、`TIME_WAIT`…），`/proc/net/tcp` 的 `st` 列是十六进制。

## 内核里对应什么：两个入口 + 一个唯一迁移点

```
tcp_v4_rcv()                     net/ipv4/tcp_ipv4.c:2203   ← 打点（每段一次）
 ├─ 查 socket：已建立 → tcp_rcv_established()   net/ipv4/tcp_input.c:6260 ← 打点
 │                     （头部预测快路径，再细分数据/ACK/FIN）
 └─ 只有 LISTEN socket → 新建子 socket（一出生就是 SYN_RECV）
                       → tcp_rcv_state_process() 按状态处理

tcp_set_state(sk, new)           net/ipv4/tcp.c:2946       ← 打点（唯一迁移点）
 └─ 三次握手、TIME_WAIT、LAST_ACK 全在这里改状态
```

服务端处理一次连接，状态是这么走的（**被动打开 + 被动关闭**）：

| 状态 | 值 | 谁触发 |
|---|---|---|
| `TCP_LISTEN` | 10 | `listen()`，不在日志里（新连接不从它迁移） |
| `TCP_SYN_RECV` | 3 | 子 socket **创建时**就是这个状态 |
| `TCP_ESTABLISHED` | 1 | 收到第三次握手的 ACK |
| `TCP_CLOSE_WAIT` | 8 | 收到对端 FIN（对端先关） |
| `TCP_LAST_ACK` | 9 | 本端 `close()`，发出 FIN |
| `TCP_CLOSE` | 7 | 收到对端最后一个 ACK |

## 3 个打点

| 打点 | 位置 | 内容 |
|---|---|---|
| `tcp_v4_rcv: len=…` | `net/ipv4/tcp_ipv4.c` | 每个 TCP 段（`len` 是 TCP 头+载荷） |
| `tcp_rcv_established: len=…` | `net/ipv4/tcp_input.c` | 已建立连接上的常规段 |
| `tcp_state: 旧 -> 新 dport=…` | `net/ipv4/tcp.c` | 状态迁移，整台机器所有连接共用这一处 |

## 实测：宿主机 `curl` 打 guest 的 80 端口

QEMU 参数加 `hostfwd=tcp::10022-:80`，宿主机 `curl http://127.0.0.1:10022/`：

```
HOST curl=200

=== tcp6 before (0A=LISTEN) ===
   0: 00000000000000000000000000000000:0050 ... 0A ...        ← LISTEN（十六进制 0A=10）

[   10.559176] [latte] tcp_v4_rcv: len=24            ← SYN（20 头 + 4 字节 MSS 选项）
[   10.567673] [latte] tcp_v4_rcv: len=20            ← 第三次握手的 ACK（纯 ACK 20 字节）
[   10.571988] [latte] tcp_state: 3 -> 1 dport=55154 ← SYN_RECV → ESTABLISHED
[   10.572727] [latte] tcp_v4_rcv: len=99            ← HTTP 请求（99 字节）
[   10.572937] [latte] tcp_rcv_established: len=99
[   10.597343] [latte] tcp_v4_rcv: len=20            ← ACK（纯 ACK 走 established 快路径）
[   10.598289] [latte] tcp_rcv_established: len=20
[   10.601170] [latte] tcp_v4_rcv: len=20
[   10.601197] [latte] tcp_v4_rcv: len=20
[   10.607279] [latte] tcp_rcv_established: len=20
[   10.607573] [latte] tcp_state: 1 -> 8 dport=55154 ← ESTABLISHED → CLOSE_WAIT（收到对端 FIN）
[   10.608362] [latte] tcp_state: 8 -> 9 dport=55154 ← CLOSE_WAIT → LAST_ACK（本端 close）
[   10.609032] [latte] tcp_v4_rcv: len=20            ← 对端对 FIN 的 ACK
[   10.609570] [latte] tcp_state: 9 -> 7 dport=55154 ← LAST_ACK → CLOSE（连接彻底消失）

=== tcp6 after ===
   0: 00000000000000000000000000000000:0050 ... 0A ...        ← 又只剩 LISTEN
```

从 `10.559` 到 `10.609`，**50 毫秒**走完整个过程；`dport=55154` 是对端的临时端口。

两个“没有出现”的细节同样有信息量：

- **没有 `10 -> 3`（LISTEN → SYN_RECV）**：新连接的子 socket 是在 `SYN_RECV` 状态
  **直接创建**的（`net/ipv4/tcp_minisocks.c:969` 的注释就写着 “child->sk_state will be
  TCP_SYN_RECV”），所以第一次 `tcp_set_state` 就是 `3 -> 1`。
- **没有 TIME_WAIT**：本次是**被动关闭**（对端先发 FIN），本端走
  `CLOSE_WAIT → LAST_ACK → CLOSE`。`TIME_WAIT` 只落在**主动关闭方**，
  也就是宿主机上那个 curl 的 socket——guest 里根本看不到。

## 三个结论

1. **一次短连接的 TCP 状态迁移只有 4 次**：`3→1`、`1→8`、`8→9`、`9→7`。
   这 4 行就能判断一个连接是“谁先关的”：
   主动关闭方会是 `1→4→5→6(TIME_WAIT)`，被动关闭方才是本文的 `1→8→9→7`。
2. **`tcp_state` 打点是全局的、极低频**：所有连接的状态变化都汇到
   `tcp_set_state()` 这一处，日志量完全由连接数决定，不像收包路径那样随流量爆。
   排查“连接卡在某个状态”时，这个位置比 `/proc/net/tcp` 更好用。
3. **`/proc/net/tcp` 是快照，抓不住短连接**：本次连接活 50ms，两次打印隔着 10 秒，
   前中后都只看到 `0A`（LISTEN）。要抓状态要么靠打点，要么在连接活跃时反复读。
   另外**这里有个坑**：busybox `httpd` 默认绑 IPv6（`netstat -ltn` 显示 `:::80`），
   所以它的监听 socket 在 `/proc/net/tcp6` 里，`/proc/net/tcp` 是**空的**——
   第一次实验就被这个坑骗了，以为是 httpd 没起来。

## 复现

监听在 guest，客户端在宿主机（QEMU 的 `hostfwd` 把宿主 `127.0.0.1:10022` 转发到 guest 的 `80`）：

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/net7 && cp -a bb_rootfs /tmp/net7
cat > /tmp/net7/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B ip link set eth0 up
$B ip addr add 10.0.2.15/24 dev eth0
$B mkdir -p /tmp/www
echo "hello-from-guest" > /tmp/www/index.html
$B httpd -p 80 -h /tmp/www
$B sleep 1
echo "=== tcp6 before (0A=LISTEN) ==="
$B cat /proc/net/tcp6
echo "=== waiting for host curl ==="
$B sleep 10
echo "=== tcp6 after ==="
$B cat /proc/net/tcp6
echo "=== dmesg ==="
$B dmesg | $B grep -E "\[latte\] (tcp_state|tcp_v4_rcv|tcp_rcv_established)"
echo "=== net7 done ==="
exec /bin/sh
EOF
chmod +x /tmp/net7/init
( cd /tmp/net7 && find . | cpio -o -H newc --quiet ) > /tmp/bb_net7.cpio

# 宿主机在 12 秒后打一发请求（guest 已起来、还在 sleep 窗口里）
( sleep 12; curl -s -m 3 -o /dev/null -w "HOST curl=%{http_code}\n" \
    http://127.0.0.1:10022/ ) &
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_net7.cpio \
  -netdev user,id=n0,hostfwd=tcp::10022-:80 -device virtio-net-pci,netdev=n0 \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 panic=1" \
  -nographic -m 1G -no-reboot > /tmp/latte-net7.log 2>&1 || true
tr -d '\r' < /tmp/latte-net7.log | sed -n '/=== tcp6 before/,/net7 done/p'
```

时间要留够：`curl` 必须落在 guest 的 `sleep 10` 窗口里，否则看不到 `after` 快照。
端口 10022 是宿主机侧写的，guest 里仍然是 80——这是 `hostfwd` 的语义。

## 代码位置

| 位置 | 函数 | 做什么 |
|---|---|---|
| `net/ipv4/tcp_ipv4.c:2203` | `tcp_v4_rcv()` | TCP 段总入口：查 socket 再分派 |
| `net/ipv4/tcp_input.c:6260` | `tcp_rcv_established()` | 已建立连接快路径（头部预测 → 数据/ACK/FIN） |
| `net/ipv4/tcp.c:2946` | `tcp_set_state()` | 状态迁移的唯一实现，所有连接共用 |
| `net/ipv4/tcp_minisocks.c:969` | `tcp_child_process()` 注释 | 新连接子 socket 一出生就是 `TCP_SYN_RECV` |
| `include/net/tcp_states.h` | `TCP_*` 枚举 | `1`=ESTABLISHED、`3`=SYN_RECV、`7`=CLOSE、`8`=CLOSE_WAIT、`9`=LAST_ACK |

## 一句话收尾

TCP 的“状态机”在日志里就是 `tcp_set_state` 的一行行 `旧 -> 新`：
四个迁移能说清一次短连接的生死，也能立刻分清“谁先关的”——
`FIN_WAIT/TIME_WAIT` 是主动方，`CLOSE_WAIT/LAST_ACK` 是被动方。
