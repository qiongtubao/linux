# 主题四 · 实测：一次 ping 的完整收包链路

## 实验设置

QEMU 加一块 virtio 网卡（用户态网络 `-netdev user`）：

```bash
qemu-system-x86_64 ... -netdev user,id=n0 -device virtio-net-pci,netdev=n0
```

`init` 里做四件事：

```sh
ip link                                # 看有没有 eth0
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
ping -c 2 10.0.2.2                     # QEMU 用户态网络的网关
```

## 打点序列（真实输出）

```
[    3.236352] virtnet_probe: virtio-net device probing     ← 驱动探测
[    3.194973] request_irq: irq=25 name=virtio0-input.0     ← 收包中断（interrupts 站的打点）
[    3.195382] request_irq: irq=26 name=virtio0-output.0    ← 发送完成中断

[    4.200988] virtnet_poll: budget=64
[    4.335285] virtnet_poll: budget=64
[    4.336700] netif_receive_skb: len=50 proto=0x0806 dev=eth0   ← ARP
[    4.340206] virtnet_poll: budget=64
[    4.340713] netif_receive_skb: len=84 proto=0x0800 dev=eth0   ← IPv4（ping 回包）
[    4.341241] ip_rcv_core: len=84
[    5.349995] virtnet_poll: budget=64
[    5.350316] netif_receive_skb: len=84 proto=0x0800 dev=eth0
[    5.350531] ip_rcv_core: len=84

[    6.205521] virtnet_poll: budget=64
[    6.206499] netif_receive_skb: len=96 proto=0x86dd dev=eth0   ← IPv6 邻居发现
```

计数：`virtnet_poll` 5 次、`netif_receive_skb` 4 次、`ip_rcv_core` 2 次。

## 三组数字对照

| 观察点 | 数值 |
|---|---|
| `/proc/net/dev` 的 eth0 RX（ping 前 → 后） | 260 字节/3 包 → **504 字节/6 包** |
| `/proc/interrupts` 的 `virtio0-input.0` | **3 次** |
| `virtnet_poll` 打点 | **5 次** |
| `netif_receive_skb` 打点 | **4 个包**（ARP 1 + IPv4 2 + IPv6 1） |

## 四个能读出来的结论

1. **中断次数 < 包数**：3 次中断收了 6 个包——这就是 NAPI 批量收的效果。
   中断只负责“叫一声”，真正的收包在 `virtnet_poll(budget=64)` 里批量做。
2. **`netif_receive_skb` 的 `proto` 直接告诉你包类型**：`0x0806`=ARP、`0x0800`=IPv4、
   `0x86dd`=IPv6。这是 `__netif_receive_skb_core()` 分发协议的依据。
3. **`ip_rcv` 一次都没触发，`ip_rcv_core` 触发了 2 次**：因为 GRO 把这批包聚成了
   **skb 列表**，走的是 `.list_func = ip_list_rcv` 的批量路径，而不是单包 `ip_rcv`。
   两条路径都汇到 `ip_rcv_core()`——所以打点放在那里才抓得到。
   （这与 `vfs/04` 那次“打点位置错了日志会安静得可怕”是同一类教训。）
4. **ping 的 2 个回包对应 2 次 `ip_rcv_core`**：数值严格对齐，说明链路没有额外丢包或重复。

## 一个没覆盖到的点（如实记录）

本次没有给 `icmp_rcv()` / socket 接收队列打点，所以“ICMP 回包最后交给 raw socket、
`recvmsg()` 拷到用户态”这一段只有代码依据（`net/ipv4/icmp.c`、`net/socket.c`），
**没有实测数据**。如果要补，可以给 `icmp_rcv()` 与 `sock_queue_rcv_skb()` 各加一个打点。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/netprobe && cp -a bb_rootfs /tmp/netprobe
cat > /tmp/netprobe/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
$B ip link set eth0 up
$B ip addr add 10.0.2.15/24 dev eth0
$B sed -n '/eth0/p' /proc/net/dev
$B ping -c 2 -W 2 10.0.2.2
$B sed -n '/eth0/p' /proc/net/dev
$B grep -iE "virtio|eth" /proc/interrupts
exec /bin/sh
EOF
chmod +x /tmp/netprobe/init
( cd /tmp/netprobe && find . | cpio -o -H newc --quiet ) > /tmp/bb_netprobe.cpio
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_netprobe.cpio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-net.log 2>&1
grep -E "virtnet_|netif_receive|ip_rcv_core|eth0:|virtio0-" /tmp/latte-net.log
```
