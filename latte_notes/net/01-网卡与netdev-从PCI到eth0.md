# 主题一 · 网卡与 netdev：从 PCI 设备到 eth0

## 你已知的

插一块网卡、装好驱动，系统里就出现 `eth0`。Linux 里“网卡”在内核中的代表是
`struct net_device`（简称 netdev），它和字符/块设备一样，是设备模型的一员。

## 从硬件到 eth0 的四步

```
QEMU/真实硬件：PCI 设备出现（本次是 virtio-net-pci）
 └─ PCI 总线枚举 → 找到匹配的驱动（virtio_net）
     └─ virtnet_probe()                    ← 打点
         ├─ alloc_etherdev()              分配 net_device
         ├─ 建收发队列（virtqueue）
         ├─ register_netdev(dev)          注册 → 内核给它分配名字 eth0
         └─ 申请 MSI-X 中断向量
```

本次实测能看到这三步的产物：

```
$ ip link
2: eth0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop qlen 1000
    link/ether 52:54:00:12:34:56

$ grep virtio /proc/interrupts
 24:  0  PCI-MSIX-0000:00:03.0  0-edge  virtio0-config
 25:  3  PCI-MSIX-0000:00:03.0  1-edge  virtio0-input.0
 26:  6  PCI-MSIX-0000:00:03.0  2-edge  virtio0-output.0
```

`virtio0-input.0` 就是“收包队列的中断”，`virtio0-output.0` 是发送完成的中断。

## net_device 里有什么

| 字段/成员 | 作用 |
|---|---|
| `name` | `eth0`（内核生成，可用 `ip link set name` 改） |
| `mtu` | 最大传输单元，以太网默认 1500 |
| `netdev_ops` | 驱动提供的操作表：`ndo_start_xmit`、`ndo_open`… |
| `napi_list` / `napi` | NAPI 实例：轮询收包的入口 |
| `stats` | 收发包/字节/错误计数（`/proc/net/dev` 的来源） |
| `dev_addr` | MAC 地址 |

## virtio-net 特殊在哪

它是**半虚拟化**设备：网卡不是真的，是 QEMU 在用户态模拟的一个“队列接口”。

| 机制 | 说明 |
|---|---|
| virtqueue | 环形缓冲：驱动把空 buffer 放进去，QEMU 收到包就填进去 |
| MSI-X | 每个队列一个中断向量（本次 3 个：config/input/output） |
| 收包流程 | QEMU 填 buffer → 触发 `virtio0-input.0` 中断 → 驱动轮询取走 |

## 为什么不是“一个包一个中断”

那会把 CPU 打爆（万兆网卡每秒几百万包）。所以用 **NAPI**：中断只负责“通知有包”，
驱动随后在一次轮询里按 budget 批量收（主题二）。本次 `/proc/interrupts` 里
`virtio0-input.0` 只有 3 次、而 `/proc/net/dev` 显示收了 6 个包——中断次数少于包数，
正是批量收的证据。

## 代码位置

| 位置 | 内容 |
|---|---|
| `drivers/net/virtio_net.c:6782` `virtnet_probe()` | 探测与注册（打点） |
| `include/linux/netdevice.h` | `struct net_device` |
| `net/core/dev.c` | `register_netdev()` / `netif_receive_skb()` |
| `drivers/net/virtio_net.c` | `virtnet_receive()`（从 virtqueue 取包） |
