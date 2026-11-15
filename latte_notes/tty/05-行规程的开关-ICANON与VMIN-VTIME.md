# 主题五 · 行规程的开关：ICANON 与 VMIN/VTIME

`tty/01` 说行规程负责回显、行缓冲、控制字符，`tty/04` 实测了 `Ctrl-C`。
这篇把“行缓冲”这层拆开：**一行到底什么时候算“输入完成”**？
答案全在 `termios` 的三个开关上，而且不设对会出现“程序卡住读不到数据”。

## 你已知的

- `UART → tty → 行规程(n_tty) → read()` 这条链（`tty/02`）；
- 行规程管回显、行编辑、`Ctrl-C` 转信号（`tty/01`、`tty/04`）；
- `termios` 是 `tcgetattr`/`tcsetattr` 读写的结构体。

## 内核里对应什么：三个开关，两个函数

`termios` 里跟“读”相关的三组东西：

| 字段 | 关键位 | 作用 |
|---|---|---|
| `c_lflag` | `ICANON` | **规范模式**：攒到换行（或 EOF）才交付一整行 |
| `c_cc[VMIN]` | 0~255 | 非规范模式下：**至少**凑够几个字节才返回 |
| `c_cc[VTIME]` | 0~255（×100ms） | 非规范模式下：最多等多少个 0.1 秒 |
| `c_oflag` | `OPOST`+`ONLCR` | 输出加工：把 `\n` 变成 `\r\n` |

两个打点位置：

```
tcsetattr(fd, …)
 └─ n_tty_set_termios()          drivers/tty/n_tty.c:1799   ← 打点（配置生效）
     └─ 把 ICANON/VMIN/VTIME 记进 ldata

read(fd, …)
 └─ n_tty_read()                 drivers/tty/n_tty.c:2209   ← 打点（这次读在等什么）
     ├─ 规范模式：没等到换行就睡（`wait_for_input`）
     └─ 非规范：凑够 VMIN 或等到 VTIME 超时
```

注意 `VMIN` 和 `VTIME` **只在 `ICANON` 关掉时才有意义**：
规范模式下内核按“行”交付，VMIN/VTIME 被忽略（`ICANON` 下 `VMIN` 恒当 1 处理）。

## 2 个打点

| 打点 | 位置 | 内容 |
|---|---|---|
| `set_termios: icanon=… vmin=… vtime=… onlcr=…` | `n_tty_set_termios()` | 每次 `tcsetattr` 的四个关键开关 |
| `n_tty_read: icanon=… vmin=… vtime=… nr=…` | `n_tty_read()` | 这次 `read()` 受哪套规则支配 |

## 实测：自己造一对 pty，逐项验证

用 `/dev/ptmx` 造一对 pty：写 master = 模拟键盘输入，读 slave = 模拟程序读终端。
（不用串口控制台，是因为读它会和 shell 抢输入。）

**① 规范模式：没有换行就不给**

```
[tty] ICANON: 写 "abc" 后 read → -1 (Resource temporarily unavailable)
[tty] ICANON: 写 "\n" 后 read → 4 字节: 61 62 63 0a
```

写 3 个字节后非阻塞读直接 `EAGAIN`；补一个换行，一次拿到 `abc\n` 四个字节——
**行规程把“回车”当成了交付信号**，这就是 shell 里 `read` 要按回车的原因。

**② `VMIN=0` + `VTIME=3`：最多等 0.3 秒**

```
[tty] VMIN=0 VTIME=3: read=0，等了 304 ms
```

没有任何输入时 `read()` 也能返回（返回 0），耗时正好 ≈300ms。
这是“带超时的轮询读”，串口程序常用这套。

**③ `VMIN=5`：凑够 5 个字节才给**

```
[tty] VMIN=5 + O_NONBLOCK: 写 3 字节后 read → 3（无视 VMIN）
[tty] VMIN=5 阻塞: read=5，等了 205 ms
[tty] VMIN=5: 补齐 5 字节后 read → 5 字节: 61 62 63 64 65
```

两件容易踩的事都在这里：

- **`O_NONBLOCK` 会让 `VMIN` 失效**：只有 3 个字节也立刻返回 3；
- **阻塞时确实是“等够 VMIN”**：写 3 字节后 `read` 卡住，200ms 后子进程补 2 字节，
  `read` 才返回 5 个字节（`61 62 63 64 65`）。

**④ `ONLCR`：输出时 `\n` 变 `\r\n`**

```
[tty] ONLCR=1 时输出 "\n" → 2 字节: 0d 0a
[tty] ONLCR=0 时输出 "\n" → 1 字节: 0a
```

写一个 `\n` 到终端，开 `ONLCR` 时对端收到 `\r\n` 两个字节，关掉只收到 `\n`。
这就是“终端里换行要 `\r\n`”的来历——不是程序写的，是**行规程在输出侧补的**。

对应的内核打点（同一次运行，15 条里截取关键几行）：

```
set_termios: icanon=1 vmin=1 vtime=0 onlcr=1     ← 初始：规范模式
n_tty_read: icanon=1 vmin=1 vtime=0 nr=64        ← 读 "abc"，等换行
n_tty_read: icanon=1 vmin=1 vtime=0 nr=64        ← 换行到了，返回 4 字节
set_termios: icanon=0 vmin=0 vtime=3 onlcr=1     ← 切 raw + VTIME=3
n_tty_read: icanon=0 vmin=0 vtime=3 nr=64        ← 304ms 后返回 0
set_termios: icanon=0 vmin=5 vtime=0 onlcr=1     ← VMIN=5
n_tty_read: icanon=0 vmin=5 vtime=0 nr=64        ← 非阻塞，返回 3
n_tty_read: icanon=0 vmin=5 vtime=0 nr=64        ← 阻塞，205ms 后返回 5
```

配置变化与读的行为一一对上：每次 `tcsetattr` 前脚一条 `set_termios`，
后脚每次 `read` 都带着当时的 `icanon/vmin/vtime`。

## 三个结论

1. **`ICANON` 是“攒到换行”，`VMIN/VTIME` 才管“等多少”**：二者互斥——
   规范模式下 VMIN/VTIME 不参与（内核按行交付）；关掉 `ICANON` 之后才轮到这两个数字。
   很多“程序读终端卡住”的问题就是把 raw 模式和规范模式搞混了。
2. **`O_NONBLOCK` 会无视 `VMIN`**：这是实测里最反直觉的一条——
   同一份 termios，阻塞读等够 5 字节才回，非阻塞读有 3 字节就回 3 字节。
   所以“凑包”的逻辑必须自己写，不能指望 `VMIN` 帮你。
3. **`\r\n` 是行规程加工出来的**：程序只写 `\n`，串口对端收到 `\r\n`（`ONLCR=1`）。
   这也解释了 `tty/02` 里“一个字节的旅程”最后一站为什么要过 `c_oflag`。

## 复现

```c
/* ttyprobe.c: 用一对 pty 验证 ICANON / VMIN+VTIME / ONLCR */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <sys/wait.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void dump(const char *tag, const char *buf, ssize_t n)
{
	ssize_t i;

	printf("[tty] %s → %zd 字节:", tag, n);
	for (i = 0; i < n; i++)
		printf(" %02x", (unsigned char)buf[i]);
	printf("\n");
}

int main(void)
{
	int m, s, fl;
	char sp[64], buf[64];
	ssize_t r;
	double t0, t1;
	struct termios t;

	setvbuf(stdout, NULL, _IONBF, 0);

	m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0) { perror("posix_openpt"); return 1; }
	if (grantpt(m) || unlockpt(m)) { perror("grantpt/unlockpt"); return 1; }
	snprintf(sp, sizeof(sp), "%s", ptsname(m));
	s = open(sp, O_RDWR | O_NOCTTY);
	if (s < 0) { perror("open slave"); return 1; }
	printf("[tty] slave=%s\n", sp);

	tcgetattr(s, &t);
	t.c_lflag &= ~ECHO;

	/* 1) 规范模式：没有换行就不返回 */
	t.c_lflag |= ICANON;
	tcsetattr(s, TCSANOW, &t);
	write(m, "abc", 3);
	fl = fcntl(s, F_GETFL);
	fcntl(s, F_SETFL, fl | O_NONBLOCK);
	r = read(s, buf, sizeof(buf));
	printf("[tty] ICANON: 写 \"abc\" 后 read → %zd (%s)\n",
	       r, r < 0 ? strerror(errno) : "有数据");
	fcntl(s, F_SETFL, fl);
	write(m, "\n", 1);
	r = read(s, buf, sizeof(buf));
	dump("ICANON: 写 \"\\n\" 后 read", buf, r);

	/* 2) VMIN=0/VTIME=3：没数据也最多等 0.3 秒 */
	t.c_lflag &= ~ICANON;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 3;
	tcsetattr(s, TCSANOW, &t);
	t0 = now();
	r = read(s, buf, sizeof(buf));
	t1 = now();
	printf("[tty] VMIN=0 VTIME=3: read=%zd，等了 %.0f ms\n", r, (t1 - t0) * 1000);

	/* 3a) 非阻塞 + VMIN=5：O_NONBLOCK 无视 VMIN */
	t.c_cc[VMIN] = 5;
	t.c_cc[VTIME] = 0;
	tcsetattr(s, TCSANOW, &t);
	write(m, "abc", 3);
	fcntl(s, F_SETFL, fl | O_NONBLOCK);
	r = read(s, buf, sizeof(buf));
	printf("[tty] VMIN=5 + O_NONBLOCK: 写 3 字节后 read → %zd（无视 VMIN）\n", r);
	fcntl(s, F_SETFL, fl);

	/* 3b) 阻塞 + VMIN=5：等子进程补齐 5 字节 */
	write(m, "abc", 3);
	if (fork() == 0) {
		usleep(200000);
		if (write(m, "de", 2) < 0)
			perror("child write");
		_exit(0);
	}
	t0 = now();
	r = read(s, buf, sizeof(buf));
	t1 = now();
	printf("[tty] VMIN=5 阻塞: read=%zd，等了 %.0f ms\n", r, (t1 - t0) * 1000);
	dump("VMIN=5: 补齐 5 字节后 read", buf, r);
	wait(NULL);

	/* 4) ONLCR：输出把 \n 变成 \r\n */
	t.c_oflag |= OPOST | ONLCR;
	tcsetattr(s, TCSANOW, &t);
	write(s, "\n", 1);
	r = read(m, buf, sizeof(buf));
	dump("ONLCR=1 时输出 \"\\n\"", buf, r);

	t.c_oflag &= ~ONLCR;
	tcsetattr(s, TCSANOW, &t);
	write(s, "\n", 1);
	r = read(m, buf, sizeof(buf));
	dump("ONLCR=0 时输出 \"\\n\"", buf, r);

	close(s);
	close(m);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/ttyprobe /tmp/ttyprobe.c
make -j16 bzImage
rm -rf /tmp/tty5 && cp -a bb_rootfs /tmp/tty5
cp /tmp/ttyprobe /tmp/tty5/bin/ttyprobe
cat > /tmp/tty5/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B mkdir -p /dev/pts
$B test -e /dev/ptmx || $B mknod /dev/ptmx c 5 2
$B mount -t devpts none /dev/pts
/bin/ttyprobe
$B dmesg > /dm.txt
$B grep -E "set_termios|n_tty_read" /dm.txt | $B tail -12
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/tty5/init
( cd /tmp/tty5 && find . | cpio -o -H newc --quiet ) > /tmp/bb_tty5.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_tty5.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 log_buf_len=2M panic=1" \
  -nographic -m 512M -no-reboot > /tmp/latte-tty5.log 2>&1 || true
tr -d '\r' < /tmp/latte-tty5.log | sed -n '/ttyprobe/,/done/p'
```

两个环境前提，缺一个就报 `posix_openpt: No such file or directory`：

- **`/dev/ptmx` 存在**（`crw-rw-rw- 5:2`）；
- **`/dev/pts` 挂上 `devpts`**：从设备的设备节点是 `unlockpt()` 时由 devpts 动态创建的，
  没挂 devpts 就没有 `/dev/pts/N`。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `drivers/tty/n_tty.c:1799` | `n_tty_set_termios()` | 把 ICANON/VMIN/VTIME/ONLCR 记进行规程状态 |
| `drivers/tty/n_tty.c:2209` | `n_tty_read()` | 按上面的开关决定“现在返回还是继续睡” |
| `drivers/tty/n_tty.c` | `wait_for_input()` | 规范模式下等换行的地方 |
| `include/uapi/asm-generic/termbits.h` | `ICANON`/`VMIN`/`VTIME`/`ONLCR` | 这些位和下标来自这里 |
| `drivers/tty/tty_io.c` | `tty_mode_ioctl()`/`tty_set_termios()` | `tcsetattr` → 最终调到 `n_tty_set_termios` |

## 一句话收尾

终端能不能“读到东西”，取决于三个数字：**要不要攒到换行（ICANON）、
至少凑几个字节（VMIN）、最多等多久（VTIME）**。
而屏幕上那个 `\r\n`，是行规程替程序补的。
