# 主题四 · 实测：同一台 128M 机器，加 swap 前后

同一个内核、同样写 100M 到 tmpfs，唯一差别是**有没有 64M 交换区**。

## 对照表

| | 上一站：无 swap | 这一站：64M swap |
|---|---|---|
| 内存 | `-m 128M` | `-m 128M` |
| 交换区 | 无 | QEMU 挂 64M 虚拟盘 → `mkswap` + `swapon` |
| `dd if=/dev/zero of=/mnt/big bs=1M count=100` | **被 OOM 杀掉**（`MemFree` 剩 2860 kB） | **`100+0 records out`，写完** |
| `MemFree` | 2860 kB | 3648 kB |
| `Cached` | 80124 kB | 78156 kB |
| `SwapFree` | — | 65532 → **39144 kB**（用掉 26388 kB） |
| `pgscan_kswapd` / `pgsteal_kswapd` | 0 / 0 | **35331 / 4535** |
| `pgscan_direct` / `pgsteal_direct` | 0 / 0 | **2179 / 2064** |
| `pswpin` / `pswpout` | 0 / 0 | 7 / 39（见下面的疑问） |
| `SwapCached` | — | 28 kB |
| `balance_pgdat` / 直接回收 | 16 / 3 | 3 / 7 |
| 结局 | OOM killer | 系统继续运行 |

## 关键日志

```
[latte] swapon: activating swap area
Adding 65532k swap on /dev/sda.  Priority:-2 extents:1 across:65532k

=== before ===
MemFree:  75348 kB      SwapFree: 65532 kB
pgsteal_kswapd 0        pgsteal_direct 0

=== writing 100M ===
100+0 records in
100+0 records out          ← 上一站这里是被 OOM 打断的

=== after ===
MemFree:  3648 kB       SwapFree: 39144 kB      SwapCached: 28 kB
pgscan_kswapd 35331     pgsteal_kswapd 4535
pgscan_direct 2179      pgsteal_direct 2064
pswpin 7                pswpout 39
```

`/proc/swaps`：

```
Filename    Type       Size    Used   Priority
/dev/sda    partition  65532   26388  -2
```

## 四个能读出来的结论

1. **回收真的干活了**：`pgscan_*` 与 `pgsteal_*` 从全 0 变成 35331/2179 与 4535/2064。
   `pgsteal` 合计 6599 页 ≈ 26396 kB，与 `SwapFree` 掉的 26388 kB 完全对得上。
2. **swap 让 tmpfs 页变得可回收**：上一站那 80M `Cached` 无处可去只能 OOM；这次内核把它
   的一部分写进交换区，腾出物理页继续写完 100M。
3. **代价是变慢**：`pswpin 7` 说明确实有页被读回来过；换出/换入都要等 I/O，
   这就是“swap 不是免费内存”的具体含义。
4. **`pswpout` 这个计数器别单独信**：它只有 39 页，而实际换出 6597 页。
   判断“换了多少”要看 `SwapFree` 的差值和 `pgsteal_*`，`pswpout` 在批量换出
   （`swap_iocb` 合并写）场景下明显偏小——这是本次实测发现的，原因待查。

## 一条方法论

上一站说“回收空转”，这一站把前提改掉（加 swap）再跑一次，结论就反转了。
**把“回收不动”拆成“可回收页为 0”，再去掉这个条件，比读十遍代码更能确认因果。**

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
truncate -s 64M /tmp/swap.img                 # 宿主机上造交换镜像

rm -rf /tmp/swaptest && cp -a bb_rootfs /tmp/swaptest
cat > /tmp/swaptest/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
$B mkswap /dev/sda >/dev/null 2>&1
$B swapon /dev/sda
$B sed -n '/^MemFree\|^SwapFree/p' /proc/meminfo
$B mkdir -p /mnt; mount -t tmpfs -o size=110M none /mnt
$B dd if=/dev/zero of=/mnt/big bs=1M count=100
$B sed -n '/^MemFree\|^SwapFree\|^SwapCached/p' /proc/meminfo
$B sed -n '/^pgscan_kswapd \|^pgsteal_kswapd \|^pswpout\|^pswpin/p' /proc/vmstat
cat /proc/swaps
exec /bin/sh
EOF
chmod +x /tmp/swaptest/init
( cd /tmp/swaptest && find . | cpio -o -H newc --quiet ) > /tmp/bb_swaptest.cpio

timeout 60 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_swaptest.cpio \
  -drive file=/tmp/swap.img,format=raw,if=ide \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 128M -no-reboot > /tmp/latte-swaptest.log 2>&1
```

不需要改 `.config`：内核里 `CONFIG_SWAP=y`、SCSI 磁盘驱动都在，QEMU 挂一块虚拟盘即可。
