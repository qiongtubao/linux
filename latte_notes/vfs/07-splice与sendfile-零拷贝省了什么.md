# 主题七 · splice / sendfile：零拷贝到底省了什么

`vfs/06` 讲了 `mmap` 把 page cache 页**映射**进进程；这篇讲另一种“不拷”：
**`sendfile()` 让文件页直接变成 socket 里的 skb**，一个字节都不经过用户缓冲。
实测对照组是 `read()+write()` 循环，两者的打点差异非常干脆。

## 你已知的

- 文件内容按页缓存在 `address_space` 里（`vfs/01`、`vfs/05`）；
- 发一个包要从 socket 到 `__dev_queue_xmit`（`net/06`）；
- TCP 发送时会把数据放进 skb（`net/09` 的 `tcp_transmit`）。

## 内核里对应什么：两条搬数据的路

**普通读+写**（每个字节两次拷贝）：

```
read(fd, buf, 64K)   → 复制 page cache 页 → 用户 buf          （拷贝 1）
write(sock, buf, 64K) → 复制 用户 buf → skb 的线性区/分片      （拷贝 2）
```

**sendfile**（零拷贝）：

```
sendfile(sock, file, NULL, 4MB)
 └─ do_sendfile()                     fs/read_write.c:1303   ← 打点
     └─ splice_direct_to_actor()      fs/splice.c:1027
         └─ file->f_op->splice_read = filemap_splice_read()   mm/filemap.c:2997 ← 打点
             把 page cache 的页**引用**挂进管道（不拷数据）
             └─ iter_file_splice_write() 组装 iov，置 MSG_SPLICE_PAGES   fs/splice.c:876
                 └─ tcp_sendmsg_locked() 的 MSG_SPLICE_PAGES 分支   net/ipv4/tcp.c:1310 ← 打点
                     └─ skb_splice_from_iter()  net/core/skbuff.c:7251
                         把页直接挂成 skb 的 frag（SKBFL_SHARED_FRAG）
```

关键差别：`sendfile` 里页的所有权被“借”给了 skb（引用计数 + 共享 frag 标记），
**没有 `copy_from_user`/`copy_to_user`**，也没有用户缓冲参与。

## 3 个打点

| 打点 | 位置 | 内容 |
|---|---|---|
| `do_sendfile: count=…` | `fs/read_write.c` | 一次系统调用的请求字节数 |
| `filemap_splice_read: len=…` | `mm/filemap.c` | 每次从 page cache 抓多少页进管道 |
| `tcp_splice_pages: len=…` | `net/ipv4/tcp.c` | 走 `MSG_SPLICE_PAGES` 分支（零拷贝路径） |

## 实测：同一个 4MB 文件，两种发法

两次独立启动（避免 `dmesg` 累积混入），每次只跑一种模式，宿主机用
`socat ... SYSTEM:'cat > /dev/null'` 收数据：

| 模式 | 发送字节 | 耗时 | `do_sendfile` | `filemap_splice_read` | `tcp_splice_pages` | `tcp_transmit` |
|---|---|---|---|---|---|---|
| A `sendfile` | 4194304 | **357.3 ms** | **1** | **64** | **193** | 237 |
| B `read+write` | 4194304 | **395.9 ms** | 0 | 0 | 0 | 238 |

打点原文（A 组）：

```
[latte] do_sendfile: count=4194304
[latte] filemap_splice_read: len=65536
[latte] tcp_splice_pages: len=0
...（filemap_splice_read 共 64 次，tcp_splice_pages 共 193 次）
```

三个数字都对得上：

- `do_sendfile` 1 次：探针把所有字节交给一次 `sendfile()` 循环；
- `filemap_splice_read` **64 次** = 4MB ÷ **64KB**——内核每次最多搬一个管道容量，
  所以是“分块搬页”，不是一次性 4MB；
- `tcp_splice_pages` **193 次**：每个组好的 skb 走一次零拷贝分支；
- B 组这三个计数**全是 0**，而 `tcp_transmit` 照样 238 次——它走的是普通
  `copy_from_iter` 路径，压根不碰 splice。

耗时上 `sendfile` 快 **38.6 ms（约 10%）**。注意这是 QEMU + slirp 的软件收发环境，
大部分时间花在 slirp 自己身上，所以 10% 已经是不小的份额——
零拷贝省的是**内核态/用户态的拷贝与缓冲管理**，不是网络时延。

## 三个结论

1. **零拷贝省的是“两次拷贝”，可以用打点直接看出来**：`sendfile` 走
   `filemap_splice_read` + `MSG_SPLICE_PAGES`（193 次），`read+write` 一次都不走。
   这条分界线比“耗时差多少”更硬——耗时随环境抖动，打点不会。
2. **它仍然是分块的**：`filemap_splice_read` 64 次 = 4MB/64KB（管道容量），
   说明内核是把 page cache 的页一批批挂进管道再转给 socket，而不是整文件一次性搬。
   所以“零拷贝”不等于“一次系统调用搬完”，大文件仍然是 64KB 一轮。
3. **它不改变 TCP 做的事**：两种模式下 `tcp_transmit` 都是 237/238 次——
   分段、GSO、拥塞控制照旧（`net/09`）。`sendfile` 只改数据从哪来，
   不改它怎么发。

## 复现

```c
/* copyprobe.c: s=sendfile / rw=read+write，同样的文件、同样的目标 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int connect_to(const char *ip, int port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_pton(AF_INET, ip, &sa.sin_addr);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("connect");
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "s";
	const char *ip = argc > 2 ? argv[2] : "10.0.2.2";
	int port = argc > 3 ? atoi(argv[3]) : 10080;
	const char *path = argc > 4 ? argv[4] : "/mnt/big.bin";
	struct stat st;
	int fd, sk;
	ssize_t n;
	double t0, t1;
	size_t sent = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	fstat(fd, &st);
	sk = connect_to(ip, port);
	if (sk < 0) return 1;

	t0 = now();
	if (mode[0] == 's') {
		off_t off = 0;
		while (off < st.st_size) {
			n = sendfile(sk, fd, &off, st.st_size - off);
			if (n <= 0) break;
			sent += n;
		}
	} else {
		static char buf[65536];
		while ((n = read(fd, buf, sizeof(buf))) > 0) {
			ssize_t off = 0;
			while (off < n) {
				ssize_t w = write(sk, buf + off, n - off);
				if (w <= 0) { perror("write"); return 1; }
				off += w;
			}
			sent += n;
		}
	}
	t1 = now();
	printf("[copyprobe] mode=%s sent=%zu bytes in %.1f ms\n",
	       mode, sent, (t1 - t0) * 1000);
	fflush(stdout);
	sleep(1);
	close(sk);
	close(fd);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/copyprobe /tmp/copyprobe.c
rm -f /tmp/disk7.img
qemu-img create -f raw /tmp/disk7.img 64M ; mkfs.ext4 -q -F /tmp/disk7.img
make -j16 bzImage
rm -rf /tmp/vfs7 && cp -a bb_rootfs /tmp/vfs7
cp /tmp/copyprobe /tmp/vfs7/bin/copyprobe

for m in s rw; do
  cat > /tmp/vfs7/init <<EOF
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
\$B mkdir -p /mnt
\$B mount -t ext4 /dev/vda /mnt || echo "MOUNT FAILED"
\$B ip link set eth0 up
\$B ip addr add 10.0.2.15/24 dev eth0
\$B dd if=/dev/urandom of=/mnt/big.bin bs=1M count=4
\$B ls -l /mnt/big.bin
\$B sync
echo 3 > /proc/sys/vm/drop_caches
/bin/copyprobe $m 10.0.2.2 10080 /mnt/big.bin
\$B dmesg > /dm.txt
\$B grep -c do_sendfile /dm.txt
\$B grep -c filemap_splice_read /dm.txt
\$B grep -c tcp_splice_pages /dm.txt
\$B grep -c tcp_transmit /dm.txt
EOF
  chmod +x /tmp/vfs7/init
  ( cd /tmp/vfs7 && find . | cpio -o -H newc --quiet ) > /tmp/bb_vfs7.cpio
  socat -u TCP-LISTEN:10080,reuseaddr,fork SYSTEM:'cat > /dev/null' >/dev/null 2>&1 &
  sleep 1
  timeout 50 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
    -initrd /tmp/bb_vfs7.cpio \
    -drive file=/tmp/disk7.img,if=virtio,format=raw \
    -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 log_buf_len=2M panic=1" \
    -nographic -m 256M -no-reboot > /tmp/latte-vfs7-$m.log 2>&1 || true
  pkill -f "TCP-LISTEN:10080"
  tr -d '\r' < /tmp/latte-vfs7-$m.log | grep -E "copyprobe|^[0-9]+$"
done
```

四个“必须这么做”的点，都是踩出来的：

- **两次独立启动**：`dmesg` 是累积的，一次启动里跑两个模式会让 B 组计数混进 A 组的行。
- **`log_buf_len=2M`**：默认 250KB 只放得下约 4000 行，启动日志就把 A 组开头的
  `do_sendfile` 挤掉了（第一次实验里它显示 0，其实跑过）。
- **镜像要新建**：`swap/05` 在同一块 `disk.img` 里留了 32MB 的 swapfile，
  再写 4MB 文件会 ENOSPC，而 `dd ... 2>/dev/null` 会把错误吞掉、文件被截断成 2.78MB。
- **必须给 guest 配 IP**：`copyprobe` 会直接报 `Network is unreachable`。

## 代码位置

| 位置 | 函数 | 做什么 |
|---|---|---|
| `fs/read_write.c:1303` | `do_sendfile()` | `sendfile`/`sendfile64` 的共同实现 |
| `fs/splice.c:1027` | `splice_direct_to_actor()` | 文件 → 管道的直通循环 |
| `mm/filemap.c:2997` | `filemap_splice_read()` | 把 page cache 页挂进管道（不拷） |
| `fs/splice.c:876` | `iter_file_splice_write()` | 组装 iov 并置 `MSG_SPLICE_PAGES` |
| `net/ipv4/tcp.c:1310` | `tcp_sendmsg_locked()` | 零拷贝分支：页挂成 skb frag |
| `net/core/skbuff.c:7251` | `skb_splice_from_iter()` | 实际把页拼进 skb（可能退化为拷贝） |

## 一句话收尾

`read+write` 是“搬两趟”，`sendfile` 是“把页的引用递过去”：
打点上就是 `filemap_splice_read` + `MSG_SPLICE_PAGES` 有没有出现。
但它在 TCP 层什么都没改——分段、GSO、重传照旧，零拷贝只是让数据**从哪儿来**变便宜了。
