# 主题六 · mmap 与 page cache：文件页怎么进进程

`vfs/05` 讲了 `write()` 只到 page cache，这篇讲另一半：**`mmap` 之后，文件内容怎么“变成”
你的内存**，以及写这种内存和写文件有什么不同。实测里出现了一个很能说明问题的大数字：
第一次访问 4MB 文件，内核一次读盘读了 **4MB**。

## 你已知的

- 文件内容按页缓存在 `address_space` 里（`vfs/01`、`vfs/05`）；
- 缺页时才分配物理页，写要拷的时候才拷（`memory/06`）；
- 脏页由阈值/定时/`sync` 三条线推动回写（`vfs/05`）。

## 内核里对应什么：mmap 不读文件，只建 vma

```
mmap(fd, 4MB, PROT_READ|PROT_WRITE, MAP_SHARED)
 └─ do_mmap → mmap_region：只建 vma，记下 file + offset，一个字节都没读
     （此后访问这块地址才触发缺页）
     └─ 第一次访问 → do_fault()            mm/memory.c:5831   ← 打点
         └─ vma->vm_ops->fault = filemap_fault()  mm/filemap.c:3448 ← 打点
             ├─ 查 page cache：命中就直接把该页挂进页表
             └─ 未命中 → 发起读盘（还会顺带预读，见实测）
```

关键点：**页表项指向的就是 page cache 的那一页**，没有第二份内存。
所以 `read()` 是“内核拷到你的 buffer”，`mmap()` 是“你的地址直接指向缓存页”——
后者少一次拷贝，代价是每次访问都可能触发缺页。

写的时候分两种：

| 映射方式 | 写发生什么 | 落盘 |
|---|---|---|
| `MAP_SHARED` | 直接改 page cache 那一页（不拷） | 需要 `msync`/回写线程/`sync` |
| `MAP_PRIVATE` | 触发 COW：`wp_page_copy()` 给你一份私有副本（`memory/06`） | 永远不会落盘 |

`msync(MS_SYNC)` 的落点：

```
msync(addr, len, MS_SYNC) → ksys_msync → vfs_fsync_range(file, s, e, 1)
                                          fs/sync.c:179   ← 打点（datasync=1）
 └─ ext4_file_fsync → do_writepages → ext4_writepages → submit_bio   （vfs/05 的打点）
```

注意 `mm/msync.c:96` 传的 `datasync=1`：`msync` 保证的是**数据 + 访问数据所需的最小元数据**，
不是“所有元数据”，这和 `fsync` 有细微差别。

## 打点（3 新 + 复用 4 个）

| 打点 | 位置 | 记什么 |
|---|---|---|
| `do_fault: addr=… write=…` | `mm/memory.c` `do_fault()` | 文件页缺页的分派点，`write=1` 说明是写缺页 |
| `filemap_fault: addr=… idx=…` | `mm/filemap.c` `filemap_fault()` | `idx` 是文件内的页号 |
| `vfs_fsync_range: datasync=…` | `fs/sync.c` `vfs_fsync_range()` | `msync`/`fsync` 的统一出口 |
| `do_writepages` / `ext4_writepages` / `submit_bio` | 见 `vfs/05` | 真正下发 IO |
| `wp_page_copy` | `mm/memory.c` | `MAP_PRIVATE` 写的 COW |

## 实测：4MB 文件 mmap，先共享写再私有写

```
[mmap] file=/mnt/f.bin size=4194304 dirty=0 kB
[mmap] shared=0x7f39d6d1f000
[latte] do_fault: addr=7f39d6d1f000 write=0        ← 第一次访问 → 文件页缺页
[latte] filemap_fault: addr=7f39d6d1f000 idx=0     ← page cache 里没有（刚 drop_caches）
[latte] submit_bio: opf=0x80000 size=4194304       ← 一次读盘 4MB（预读整个文件）
[latte] do_fault: addr=7f39d6d1f000 write=0        ← 读盘完成后重试这一次缺页
[mmap] first touch byte=0  dirty=0 kB
[mmap] after shared write dirty=0 kB
[latte] vfs_fsync_range: datasync=1                ← msync(MS_SYNC)
[latte] do_writepages: nr_to_write=9223372036854775807   ← LLONG_MAX：sync 语义，不限量
[latte] ext4_writepages: nr_to_write=9223372036854775807
[latte] submit_bio: opf=0x801 size=8192            ← 只写我们改过的 2 页（8KB）
[latte] submit_bio: opf=0x40801 size=0             ← 日志提交（0 字节数据）
[mmap] after msync dirty=0 kB
[latte] do_fault: addr=7f39d6d1e000 write=0        ← MAP_PRIVATE 首次读
[mmap] private=0x7f39d6d1e000 first byte=1
[latte] wp_page_copy: addr=7f39d6d1e000            ← 私有写 → COW 出一份副本
[mmap] private write done (COW), dirty=0 kB
```

四个能直接读出来的事实：

1. **预读不是一页一页**：`submit_bio size=4194304` 说明内核一次就把 4MB 全读进来了
   （`filemap_fault` 只被调了 1 次，后面的页在缺页时直接命中缓存）。
2. **`msync` 才发写 IO**：`submit_bio size=8192` 出现在 `vfs_fsync_range` 之后，
   大小正好是程序改过的那 2 页。在此之前一个数据写 bio 都没有。
3. **`nr_to_write` 是 `LLONG_MAX`**：对照 `vfs/05` 里回写线程的 `nr_to_write=1024`（4MB 上限），
   `sync` 类调用是“写到干净为止”，没有批量上限。
4. **`MAP_PRIVATE` 写立刻 COW**：`wp_page_copy` 出现一次，文件内容不变；
   而 `MAP_SHARED` 写从头到尾**没有** `wp_page_copy` ——它直接改缓存页。

一个如实记录的观察：`Dirty` 读数在本次全过程都是 `0 kB`，即使程序已经改了 2 页。
所以**别用 `/proc/meminfo` 的 `Dirty` 判断“mmap 有没有写脏”**——
这种情况要看 `msync` 时的 `submit_bio` 大小（本次 8192 = 2 页，对得上）。
小脏页在总账上不显眼，是被 meminfo 口径吃掉了，不是没脏。

## 三个结论

1. **`mmap` 不读文件，缺页才读，而且一次读很多**：只建 `vma`；第一次访问走
   `do_fault → filemap_fault`，缓存未命中就发起预读（实测一次 4MB）。
   文件访问的“第一页特别慢、后面飞快”就是这个原因。
2. **共享映射是零拷贝的别名**：页表项指向 page cache 页，写就是改缓存，
   所以 `MAP_SHARED` 写完别处 `read()` 立刻能看到；要落盘必须 `msync`/回写。
3. **私有映射在第一次写时分裂**：`MAP_PRIVATE` 读共用缓存页，写立刻 `wp_page_copy`
   拷一份私有副本（接 `memory/06`），文件不受影响——这正是 `fork` 后 COW 的同一套机制。

## 复现

```c
/* mapprobe.c: MAP_SHARED 写 + msync 落盘；MAP_PRIVATE 写 → COW */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static long dirty_kb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "Dirty: %ld kB", &v) == 1)
			break;
	fclose(f);
	return v;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/mnt/f.bin";
	struct stat st;
	char *sh, *pr;
	int fd = open(path, O_RDWR);

	if (fd < 0) { perror("open"); return 1; }
	fstat(fd, &st);
	printf("[mmap] file=%s size=%ld dirty=%ld kB\n", path,
	       (long)st.st_size, dirty_kb());

	sh = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (sh == MAP_FAILED) { perror("mmap shared"); return 1; }
	printf("[mmap] shared=%p\n", sh);
	printf("[mmap] first touch byte=%d  dirty=%ld kB\n", sh[0], dirty_kb());
	sh[0] ^= 1;
	sh[4096] = 'S';
	printf("[mmap] after shared write dirty=%ld kB\n", dirty_kb());
	if (msync(sh, 8192, MS_SYNC) != 0)
		perror("msync");
	printf("[mmap] after msync dirty=%ld kB\n", dirty_kb());

	pr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (pr == MAP_FAILED) { perror("mmap private"); return 1; }
	printf("[mmap] private=%p first byte=%d\n", pr, pr[0]);
	pr[0] = 'P';
	printf("[mmap] private write done (COW), dirty=%ld kB\n", dirty_kb());

	munmap(pr, 4096);
	munmap(sh, st.st_size);
	close(fd);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/mapprobe /tmp/mapprobe.c
qemu-img create -f raw /tmp/disk.img 64M ; mkfs.ext4 -q -F /tmp/disk.img
make -j16 bzImage
rm -rf /tmp/vfs6 && cp -a bb_rootfs /tmp/vfs6
cp /tmp/mapprobe /tmp/vfs6/bin/mapprobe
cat > /tmp/vfs6/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B mkdir -p /mnt
$B mount -t ext4 /dev/vda /mnt || echo "MOUNT FAILED"
$B dd if=/dev/zero of=/mnt/f.bin bs=1M count=4 2>/dev/null
$B sync
echo 3 > /proc/sys/vm/drop_caches
echo "=== mapprobe ==="
/bin/mapprobe /mnt/f.bin
echo "=== vfs6 done ==="
exec /bin/sh
EOF
chmod +x /tmp/vfs6/init
( cd /tmp/vfs6 && find . | cpio -o -H newc --quiet ) > /tmp/bb_vfs6.cpio
timeout 60 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_vfs6.cpio \
  -drive file=/tmp/disk.img,if=virtio,format=raw \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 1G -no-reboot > /tmp/latte-vfs6.log 2>&1 || true

tr -d '\r' < /tmp/latte-vfs6.log | awk '/=== mapprobe ===/,/=== vfs6 done ===/' \
  | grep -E "\[mmap\]|\[latte\] (do_fault|filemap_fault|vfs_fsync_range|do_writepages|ext4_writepages|submit_bio|wp_page_copy)"
```

`drop_caches` 那步别省：不 drop 的话 `filemap_fault` 直接命中缓存，看不到那个 4MB 的读盘。

## 代码位置

| 位置 | 函数 | 做什么 |
|---|---|---|
| `mm/memory.c:5831` | `do_fault()` | 文件页缺页分派：调 `vma->vm_ops->fault` |
| `mm/filemap.c:3448` | `filemap_fault()` | 普通文件：查 page cache / 发起预读 / 等 IO |
| `mm/memory.c:3659` 附近 | `wp_page_copy()` | `MAP_PRIVATE` 写的 COW（`memory/06` 同款） |
| `mm/msync.c:96` | `ksys_msync()` | `MS_SYNC` → `vfs_fsync_range(..., datasync=1)` |
| `fs/sync.c:179` | `vfs_fsync_range()` | 转给 `file->f_op->fsync`（ext4 就是 `ext4_file_fsync`） |
| `mm/page-writeback.c:2598` | `do_writepages()` | 回写总闸（`vfs/05`） |
| `block/blk-core.c:790` | `submit_bio_noacct()` | 块层入口，本次看到 4MB 读、8KB 写 |

## 一句话收尾

`read()` 是“把缓存页拷进你的 buffer”，`mmap()` 是“把你的地址直接接到缓存页上”：
省掉一次拷贝，换来每次第一次访问都要缺页。
`MAP_SHARED` 写的是缓存本身（要 `msync` 才落盘），`MAP_PRIVATE` 写的是自己的副本
（`wp_page_copy`，永不落盘）——这两种“写文件”的差别，就是 `vfs/05` 和 `memory/06` 的交点。
