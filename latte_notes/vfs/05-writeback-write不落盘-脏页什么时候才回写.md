# 主题五 · writeback：`write()` 不落盘，脏页什么时候才回写

`vfs/03` 追到「`open` 拿到 `file`」，这篇接着看 `write()` 的另一半：
**数据到底什么时候离开内存**。结论先说：`write()` 返回成功 ≠ 落盘，
它只把数据放进 page cache 并把页标脏；真正写盘由另外三条触发线推动。

## 你已知的

- `open("/f")` → `path_openat` → `vfs_open`，拿到 `inode` + `file`（`vfs/03`）；
- 每个 inode 有一个 `address_space`，文件内容是**按页缓存在内存里**的（`vfs/01` 的三件套）；
- `sync` 是 shell 命令，`fsync` 是系统调用，两者都“让数据落盘”。

## 内核里对应什么：两条不相连的链

**写**（同步完成，但只到内存）：

```
write(fd, buf, 1MB)
 └─ vfs_write → file->f_op->write_iter → ext4_file_write_iter
     └─ generic_perform_write()        mm/filemap.c:4228         ← 打点
         ├─ 拷用户数据进 page cache 的页（这一步会触发缺页，见 memory/06）
         ├─ folio_mark_dirty()：页挂进脏页链表
         └─ balance_dirty_pages()      mm/page-writeback.c:1811   ← 打点（限流）
```

**回写**（异步，三条触发线：脏页超阈值、`dirty_expire_centisecs` 到期、`sync`/`fsync`）：

```
wb_workfn（flusher 线程） / ksys_sync / fsync
 └─ do_writepages()                    mm/page-writeback.c:2598   ← 打点
     └─ ext4_writepages()              fs/ext4/inode.c:3009        ← 打点
         └─ submit_bio_noacct()        block/blk-core.c:790         ← 打点
             └─ blk-mq → virtio_blk → /dev/vda → QEMU 的镜像文件
```

两条链**不在同一个调用栈里**：写的时候回写可能一点没发生；回写的时候写者可能在睡。
`balance_dirty_pages()` 就是让两者“咬合”的地方：脏页超过阈值时，写者在这里被按着等。

## 实测 A：默认阈值，8MB 全留在内存

虚拟磁盘 64MB ext4，`dd` 写 8MB（`bs=1M count=8`）。日志里只有“写”，没有“回写”：

```
[latte] generic_perform_write: bytes=1048576
[latte] balance_dirty_pages: dirtied=32
[latte] generic_perform_write: bytes=1048576
[latte] balance_dirty_pages: dirtied=256
...（同样的 8 组）
```

- `generic_perform_write` **8 次**（每 1MB 一次），`balance_dirty_pages` **8 次**；
- `do_writepages` / `ext4_writepages` / 数据 `submit_bio` **各 0 次**——
  这段时间里只有 2 个 4KB 的 `submit_bio: opf=0x3000`（opf 低 8 位是操作码，
  `0`=读，是挂载/日志的元数据读，和这 8MB 无关）；
- `/proc/meminfo`：

```
=== mem before ===   Dirty: 0 kB     Writeback: 0 kB
=== mem after write === Dirty: 8192 kB  Writeback: 0 kB
```

**8MB 全部还在内存里**。`dd` 早就返回“8+0 records out”了，盘上一个字节没写——
这就是“断电丢数据”的窗口，也是 `fsync` 存在的理由。

## 实测 B：把阈值调低，回写线程自己动起来

同一份 8MB，先把 `dirty_background_bytes` 调到 4MB、`dirty_bytes` 调到 8MB
（默认是按内存比例算的：`dirty_background_ratio=10`、`dirty_ratio=20`，
2G 机器上约 190MB / 390MB —— 同内核之前实测到 `/proc/vmstat` 里
`nr_dirty_background_threshold=49573`、`nr_dirty_threshold=99268` 页，×4KB 就是这个量级；
8MB 根本惊不动它）：

```
echo 4194304 > /proc/sys/vm/dirty_background_bytes
echo 8388608 > /proc/sys/vm/dirty_bytes
```

再写 8MB，日志立刻变成“写—回写”交替：

```
[latte] generic_perform_write: bytes=1048576
[latte] balance_dirty_pages: dirtied=0
[latte] do_writepages: nr_to_write=1024          ← 一次最多 1024 页 = 4MB
[latte] ext4_writepages: nr_to_write=1024
[latte] submit_bio: opf=0x83000 size=4096        ← 日志/元数据的 4KB 读
[latte] submit_bio: opf=0x1 size=1048576         ┐
[latte] submit_bio: opf=0x1 size=1048576         │ 数据：4 个 1MB
[latte] submit_bio: opf=0x1 size=1048576         │ （opf 低 8 位 =1 = WRITE）
[latte] submit_bio: opf=0x1 size=1048576         ┘
[latte] do_writepages: nr_to_write=1024
[latte] ext4_writepages: nr_to_write=1024
[latte] do_writepages: nr_to_write=1024
[latte] ext4_writepages: nr_to_write=1024
[latte] submit_bio: opf=0x1 size=4194304         ← 下一批合成一个 4MB bio
[latte] balance_dirty_pages: dirtied=32
[latte] balance_dirty_pages: dirtied=32          ← 写者开始被限流（密集循环）
...
```

两个阶段的计数对照：

| 阶段（都是写 8MB） | `generic_perform_write` | `balance_dirty_pages` | `do_writepages` | `ext4_writepages` | `submit_bio` | 写后 `Dirty` |
|---|---|---|---|---|---|---|
| A 默认阈值 | 8 | **8** | 0 | 0 | **2**（4KB 读，非数据） | **8192 kB** |
| B 阈值 4M/8M | 8 | **78** | 4 | 4 | 7 | **4092 kB** |

读数也变了：`Dirty` 从 8192 被压到 4092——超出的部分已经被回写掉，
剩下的还在阈值之内，没人催它。

## 实测 C：`sync` 才是“强制落盘”

```
=== sync ===
=== mem after sync ===  Dirty: 0 kB   Writeback: 24 kB
```

`sync` 之后脏页归零（`Writeback: 24 kB` 是收尾时还在飞的残量）。
这一阶段单独统计：`do_writepages` 2 次、`ext4_writepages` 1 次、`submit_bio` **16 次**——
把 A、B 两阶段攒下的脏页加上日志提交一起清掉。

## 三个结论

1. **`write()` 只写内存**：A 阶段 8MB 全在 page cache（`Dirty: 8192 kB`），
   数据 `submit_bio` 一个都没有。进程读到自己的写立即可见，别人打开同一文件也可见
   （共用 page cache），但**掉电就没了**。
2. **脏页阈值管的是“写者速度”，不是“什么时候写盘”**：A 阶段 `balance_dirty_pages`
   每 1MB 被调一次（`dirtied=256`）却一次没睡；B 阶段它被调 78 次、`dirtied=32` 的
   密集出现，说明写者真被按住了。回写的启动时机由 `dirty_background_bytes`/
   `dirty_expire_centisecs` 和 flusher 线程决定，和你的 `write()` 返回值无关。
3. **回写是批量、可合并、可异步的**：`do_writepages: nr_to_write=1024` 是“一次最多
   4MB”；同一批脏页先以 4 个 1MB bio 下发、下一轮变成一个 4MB bio，说明块层还在合并。
   所以“写完立刻拔电丢多少”取决于阈值、定时器和有没有人 `fsync`，不是确定的字节数。

## 复现

需要一块可写的真实文件系统（rootfs 是 tmpfs，没有 writeback）：

```bash
cd /home/dong/Documents/github/linux
qemu-img create -f raw /tmp/disk.img 64M
mkfs.ext4 -q -F /tmp/disk.img
make -j16 bzImage
rm -rf /tmp/vfs5 && cp -a bb_rootfs /tmp/vfs5
cat > /tmp/vfs5/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mem() { $B awk '/^(Dirty|Writeback):/{printf "  %s %s kB\n", $1, $2}' /proc/meminfo; }
$B mkdir -p /mnt
$B mount -t ext4 /dev/vda /mnt || echo "MOUNT FAILED"
$B df /mnt
echo "=== mem before ==="; mem
echo "=== A: 8MB write, default thresholds ==="
$B dd if=/dev/zero of=/mnt/a.bin bs=1M count=8 2>&1 | $B tail -1
echo "=== mem after write ==="; mem
echo "=== B: lower thresholds, write 8MB ==="
echo 4194304 > /proc/sys/vm/dirty_background_bytes
echo 8388608 > /proc/sys/vm/dirty_bytes
$B dd if=/dev/zero of=/mnt/b.bin bs=1M count=8 2>&1 | $B tail -1
echo "=== mem after low-threshold write ==="; mem
echo "=== sync ==="
$B sync
echo "=== mem after sync ==="; mem
echo "=== vfs5 done ==="
exec /bin/sh
EOF
chmod +x /tmp/vfs5/init
( cd /tmp/vfs5 && find . | cpio -o -H newc --quiet ) > /tmp/bb_vfs5.cpio
timeout 120 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_vfs5.cpio \
  -drive file=/tmp/disk.img,if=virtio,format=raw \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-vfs5.log 2>&1 || true

tr -d '\r' < /tmp/latte-vfs5.log | grep -E "^(=== mem|  Dirty|  Writeback)"
tr -d '\r' < /tmp/latte-vfs5.log | grep -E "\[latte\] (generic_perform_write|balance_dirty_pages|do_writepages|ext4_writepages|submit_bio)"
```

计数可以按阶段切（日志里 `===` 那几行就是分隔符）：

```bash
tr -d '\r' < /tmp/latte-vfs5.log | awk '/=== A: 8MB/,/=== mem after write/' \
  | grep -cE "\[latte\] submit_bio"        # A 阶段：只有元数据读
tr -d '\r' < /tmp/latte-vfs5.log | awk '/=== B: lower/,/=== mem after low-threshold/' \
  | grep -cE "\[latte\] (do_writepages|ext4_writepages)"
```

## 代码位置

| 位置 | 函数 | 做什么 | 本次打点 |
|---|---|---|---|
| `mm/filemap.c:4228` | `generic_perform_write()` | `write()` 主干：拷进 page cache、标脏 | 8+8 次（每次 1MB） |
| `mm/page-writeback.c:1811` | `balance_dirty_pages()` | 脏页限流：超阈值就把写者按住等回写 | A 8 次 / B 78 次 |
| `mm/page-writeback.c:2598` | `do_writepages()` | 回写总闸：派给文件系统的 `writepages` | B 4 次、sync 2 次 |
| `fs/ext4/inode.c:3009` | `ext4_writepages()` | ext4：攒脏页 → 配 bio → 交块层 | B 4 次、sync 1 次 |
| `block/blk-core.c:790` | `submit_bio_noacct()` | 块层入口：bio 进请求队列（合并/排程） | 数据 bio = 1MB/4MB |
| `fs/ext4/file.c` | `ext4_file_write_iter()` | 把 `write_iter` 接到 `generic_perform_write` | 只读 |

## 一句话收尾

`write()` 是“把数据交给内核”，页缓存替你兜着；`fsync()`/`sync` 才是“把数据交给设备”，
中间那段时间由 `Dirty` 计数器和 flusher 线程说了算——
`vfs/02` 里的 dentry 缓存是“读的缓存”，这篇的 page cache 是“写的缓存”，两者都靠“延迟”换性能。
