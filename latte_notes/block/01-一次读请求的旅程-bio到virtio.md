# 主题一 · 一次读请求的旅程：bio 到 virtio

`vfs/06` 里看到过一次 `submit_bio: size=4194304`（预读把整个文件读进来），
但那个 bio 最后怎么变成设备能执行的 IO，当时没追。这篇补上：
**同一个 2MB 的 bio，可能变成 1 个请求，也可能变成 16 个**，取决于队列参数。

## 你已知的

- 文件页在 page cache 里，缺页时发起预读、回写时 `submit_bio()`（`vfs/05`、`vfs/06`）；
- 设备端是 virtio-blk（`/dev/vda`），收发的最终形态是 virtqueue（`net/06` 里同款传输机制）；
- `/sys/block/vda/queue/` 下有一堆旋钮。

## 内核里对应什么：bio → request → tag → 驱动

```
submit_bio()                       block/bio.c              ← 打点（vfs/05 已有）
 └─ blk_mq_submit_bio()            block/blk-mq.c:3120      ← 打点
     ├─ 按 max_sectors_kb 把 bio 拆成若干 request
     │    （2MB 的 bio，在 4096KB 上限下是 1 个；在 128KB 上限下是 16 个）
     ├─ 取 tag（队列深度 nr_requests 限制了并发）
     ├─ 交给调度器排队：本机默认 mq-deadline
     └─ blk_mq_start_request()     block/blk-mq.c:1350      ← 打点
         └─ virtio_blk 的 queue_rq → virtqueue → QEMU
             └─ blk_mq_end_request()  block/blk-mq.c:1158   ← 打点
```

| 概念 | 是谁的单位 | 关键字段 |
|---|---|---|
| **bio** | 文件系统 / page cache | 一组页 + 起始扇区 + 操作（`bi_opf`） |
| **request** | 块层 / 驱动 | 由 bio 拆出，≤ `max_sectors_kb`，占一个 tag |
| **tag** | 队列深度 | 总数 = `nr_requests`（本机 256） |

本机队列参数（实测）：

```
scheduler:      none [mq-deadline] kyber
max_sectors_kb: 4096
nr_requests:    256
rotational:     1
```

## 3 个打点

| 打点 | 位置 | 内容 |
|---|---|---|
| `submit_bio: opf=… size=…` | `block/bio.c`（`vfs/05` 已有） | 文件系统交下来的 bio |
| `blk_mq_submit_bio: opf=… size=…` | `block/blk-mq.c` | 进入块层的同一份数据（对比可看出有没有拆） |
| `blk_start_rq: opf=… sector=… bytes=…` | `block/blk-mq.c` | 实际下发的请求：扇区与字节数 |
| `blk_end_rq: bytes=… err=…` | `block/blk-mq.c` | 完成回调 |

## 实测：读同一个 4MB 文件，只改一个旋钮

`dd if=/mnt/big.bin of=/dev/null bs=1M`，两侧都先 `drop_caches`。

**A 组：`max_sectors_kb=4096`（默认）**

```
submit_bio=3      blk_mq_submit_bio=3      start_rq=3      end_rq=3
bio 尺寸:     2 × 2097152 + 1 × 4096
请求尺寸:     2 × 2097152 + 1 × 4096
```

3 个 bio 原样变成 3 个请求：2MB 没超过 4096KB 的上限，**一个都不拆**。

**B 组：把上限改成 `max_sectors_kb=128`，再读一次**

```
submit_bio=3      blk_mq_submit_bio=33     start_rq=33     end_rq=33
bio 尺寸:     2 × 2097152 + 1 × 4096
请求尺寸:     32 × 131072 + 1 × 4096
```

同样的 3 个 bio，进了块层就变成 **33 个请求**：两个 2MB 的 bio 各拆成 16 个 128KB，
再加上那个 4KB 的元数据 bio。原始行：

```
[latte] submit_bio: opf=0x80000 size=2097152          ← 文件系统交下来：一个 2MB bio
[latte] blk_mq_submit_bio: opf=0x80000 size=2097152   ← 进块层时还是 2MB
[latte] blk_start_rq: opf=0x84700 sector=16520 bytes=131072
[latte] blk_start_rq: opf=0x84700 sector=16776 bytes=131072
[latte] blk_end_rq: bytes=131072 err=0
```

两个细节值得看：

- `sector` 从 `16520` 到 `16776`，**步长 256** —— 256 × 512B = 128KB，正是拆分粒度；
- `opf` 从 `0x80000` 变成 `0x84700`：`0x80000` 是 `REQ_RAHEAD`（预读），
  拆出的请求额外带上同步/插入等标志位；而 `0x3000` 那个是 `REQ_META`（读 inode/位图）。

## 三个结论

1. **bio 与 request 不是一一对应**：拆分由 `max_sectors_kb` 决定，同一个 2MB bio
   在 4096KB 上限下是 1 个请求、128KB 上限下是 16 个。文件系统对此**完全无感**——
   它只管交 bio。
2. **块层是“翻译 + 排队”层**：翻译（拆 bio、分配 tag）+ 排队（调度器按规则挑顺序）+
   完成回调。`nr_requests=256` 是同时在飞的请求上限，超过就要等 tag。
3. **两个默认值容易误解**：
   - 默认调度器是 **`mq-deadline`** 而不是 `none`（`[]` 括起来的才是当前值）；
   - virtio-blk 默认自称 **`rotational=1`**（机械盘），所以调度器会按“有寻道代价”来排序。
     虚拟盘想按 SSD 处理，可以 `echo 0 > /sys/block/vda/queue/rotational`。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/block1 && cp -a bb_rootfs /tmp/block1
cat > /tmp/block1/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B mkdir -p /mnt
$B mount -t ext4 /dev/vda /mnt || echo "MOUNT FAILED"
$B dd if=/dev/urandom of=/mnt/big.bin bs=1M count=4 2>/dev/null
$B sync
echo -n "scheduler:      "; $B cat /sys/block/vda/queue/scheduler
echo -n "max_sectors_kb: "; $B cat /sys/block/vda/queue/max_sectors_kb
echo -n "nr_requests:    "; $B cat /sys/block/vda/queue/nr_requests

echo "=== A: max_sectors_kb=4096 ==="
echo 3 > /proc/sys/vm/drop_caches
M=$($B dmesg | $B wc -l)
$B dd if=/mnt/big.bin of=/dev/null bs=1M 2>/dev/null
$B dmesg | $B tail -n +$((M+1)) > /pA.txt
$B grep -c "\[latte\] submit_bio:" /pA.txt
$B grep -c "\[latte\] blk_mq_submit_bio:" /pA.txt
$B grep -c "\[latte\] blk_start_rq:" /pA.txt

echo "=== B: max_sectors_kb=128 ==="
echo 128 > /sys/block/vda/queue/max_sectors_kb
echo 3 > /proc/sys/vm/drop_caches
M=$($B dmesg | $B wc -l)
$B dd if=/mnt/big.bin of=/dev/null bs=1M 2>/dev/null
$B dmesg | $B tail -n +$((M+1)) > /pB.txt
$B grep -c "\[latte\] submit_bio:" /pB.txt
$B grep -c "\[latte\] blk_mq_submit_bio:" /pB.txt
$B grep -c "\[latte\] blk_start_rq:" /pB.txt
$B grep "\[latte\] submit_bio:" /pB.txt | $B head -2
$B grep "\[latte\] blk_start_rq:" /pB.txt | $B head -3
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/block1/init
( cd /tmp/block1 && find . | cpio -o -H newc --quiet ) > /tmp/bb_block1.cpio
timeout 60 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_block1.cpio \
  -drive file=/tmp/disk7.img,if=virtio,format=raw \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 log_buf_len=8M panic=1" \
  -nographic -m 512M -no-reboot > /tmp/latte-block.log 2>&1 || true
tr -d '\r' < /tmp/latte-block.log | sed -n '/scheduler:/,/done/p'
```

**`log_buf_len=8M` 是必须的**：默认 2MB 缓冲区在“读 4MB 文件”这种实验里会被
缺页打点灌满（一次读就上万行），`dmesg | wc -l` 得到的行号随后会因旧行被淘汰而错位，
`tail -n +N` 就切不出这一阶段的内容——第一版实验里 B 组全是 0，就是这个原因。

另一个前提：盘里要留出空间。`disk7.img` 是 64MB 的新镜像，
如果在 `swap/05` 那块塞了 32MB swapfile 的 `disk.img` 上做，`dd` 会 ENOSPC 静默截断。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `block/bio.c` | `submit_bio()` / `submit_bio_noacct()` | 文件系统交 bio 的入口 |
| `block/blk-mq.c:3120` | `blk_mq_submit_bio()` | 拆 bio、取 tag、入调度器 |
| `block/blk-mq.c:1350` | `blk_mq_start_request()` | 请求下发给驱动（分配 tag 后） |
| `block/blk-mq.c:1158` | `blk_mq_end_request()` | 完成回调 |
| `block/blk-mq.c:1179` | `blk_mq_end_request_batch()` | 批量完成（一次 32 个 tag） |
| `block/mq-deadline.c` | `dd_dispatch_request()` | 默认调度器的挑选逻辑 |
| `/sys/block/vda/queue/` | `max_sectors_kb` / `nr_requests` / `scheduler` / `rotational` | 本实验的四个旋钮 |

## 一句话收尾

块层不做“业务”，只做三件事：**把 bio 拆成设备能吃的请求、按调度规则排队、发下去收回来**。
文件系统看到的是 bio，磁盘看到的是扇区——中间那次 1→16 的拆分，由 `max_sectors_kb` 说了算。
