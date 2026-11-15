# 主题六 · signalfd：把信号变成可 poll 的文件描述符

`jobs/05` 讲的是信号的**经典路径**：发出去 → pending 位 → 回用户态时摆 frame 跳 handler。
这篇讲另一条路径：`signalfd()`。它把信号变成**一个 fd**，用 `read()` 拿走，
用 `poll()` 等——不用再写异步 handler，也不用手动处理 `EINTR`。

一条必须记住的前提：**只有被阻塞的信号才会进 signalfd**，否则它先走 handler 或默认动作。

## 你已知的

- 发信号与投递是两条链（`jobs/05`）：`__send_signal_locked()` 记 pending，
  `handle_signal()` 在回用户态时摆 frame；
- 标准信号只有一位 pending、实时信号会排队（`jobs/05`、`jobs/06` 都用到这个区别）；
- `sigprocmask(SIG_BLOCK)` 可以把信号压在 pending 里不投递。

## 内核里对应什么：同一个 `dequeue_signal()`，两条消费路径

```
① 经典路径（jobs/05）
exit_to_user_mode → get_signal() → dequeue_signal(&current->blocked, …) → 摆 frame → handler

② signalfd 路径（本篇）
read(signalfd_fd)
 └─ signalfd_read_iter()          fs/signalfd.c:201      ← 打点
     ├─ signalfd_dequeue()        fs/signalfd.c:154
     │   └─ dequeue_signal(&ctx->sigmask, …)  fs/signalfd.c:162
     │       从 pending 集合里**摘掉**一个信号（谁先拿算谁的）
     └─ signalfd_copyinfo()：把 kernel_siginfo 转成 struct signalfd_siginfo
```

两处关键：

1. **`dequeue_signal()` 是共用的**：信号要么被 handler 拿走，要么被 `read(fd)` 拿走，
   不会两边都有。所以 `signalfd` 的 mask 必须同时被 `sigprocmask` 阻塞，
   否则信号在“回到用户态”那一刻就被 handler 消费掉了。
2. **`signalfd` 存的是取反的掩码**：`do_signalfd4()` 里有一句 `signotset(mask)`，
   内核 `sigset_t` 的位图是“**1 表示不在集合里**”。打点里能直接看到这个细节。

## 2 个打点

| 打点 | 位置 | 内容 |
|---|---|---|
| `signalfd: ufd=… flags=… mask0=0x…` | `fs/signalfd.c` `do_signalfd4()` | 创建时的 `ufd`、`SFD_*` 标志、取反后的第一个 64 位字 |
| `signalfd_read: count=…` | `fs/signalfd.c` `signalfd_read_iter()` | 这次读最多取几个 `siginfo` |

## 实测：四种情况

探针进程先给 `SIGUSR1` 装一个空 handler，然后：

```
[sfd] A: handler_hits=1（未阻塞 → 走 handler）
[sfd] A: 未阻塞时读 signalfd    读到 0 个 (最后 signo=-1) Resource temporarily unavailable
[sfd] B: handler_hits=1（阻塞后不再变）
[sfd] B: 已阻塞时读 signalfd    读到 1 个 (最后 signo=10) Resource temporarily unavailable
[sfd] C: SIGRTMIN 连发两次       读到 2 个 (最后 signo=34) Resource temporarily unavailable
[sfd] D: 无信号时 poll=0（200ms 超时）revents=0x0
[sfd] D: 有信号时 poll=1 revents=0x1
[sfd] D: poll 之后读              读到 1 个 (最后 signo=10) Resource temporarily unavailable
```

四组结论各自明确：

| 场景 | handler | signalfd |
|---|---|---|
| A 不阻塞 + 装了 handler | **跑了 1 次** | **读不到**（`EAGAIN`） |
| B 先 `SIG_BLOCK` 再建 fd | 不再被调用（仍是 1） | 读到 **1** 个（`signo=10`＝`SIGUSR1`） |
| C 实时信号连发两次 | —— | 读到 **2** 个（排队的两个） |
| D `poll()` 等 | —— | 无信号 200ms 超时返回 0；有信号立刻返回 1 |

B 组发了两遍 `SIGUSR1` 却只读到 1 个 —— 标准信号只有一位 pending，
和 `jobs/05` 的结论完全一致；C 组的实时信号才是真队列。

对应打点（同一次运行）：

```
signalfd: ufd=-1 flags=0x800 mask0=0xfffffffffffffdff
signalfd_read: count=1
signalfd: ufd=-1 flags=0x800 mask0=0xfffffffffffffdff
signalfd_read: count=1
signalfd_read: count=1
signalfd: ufd=-1 flags=0x800 mask0=0xfffffffdffffffff     ← 换成 SIGRTMIN
signalfd_read: count=1 ×3
signalfd: ufd=-1 flags=0x800 mask0=0xfffffffffffffdff
signalfd_read: count=1 ×2
```

两个字段值得盯一眼：

- `flags=0x800` 就是 `SFD_NONBLOCK`（= `O_NONBLOCK`，`do_signalfd4()` 里
  `BUILD_BUG_ON` 保证了两者相等）；
- `mask0=0xfffffffffffffdff` 是**取反**的掩码：第 9 位为 0 表示“**只有** `SIGUSR1`
  （signo=10，bit=9）在这个集合里”，其余位是 1 表示不在。
  第三次创建时掩码变成 `0xfffffffdffffffff`（第 33 位为 0）——那是 `SIGRTMIN`（34）。
  如果不知道 `signotset()` 这层取反，会把这两个数字读反。

## 三个结论

1. **signalfd 与 handler 是“二选一”**：两者都最终调 `dequeue_signal()` 从同一个
   pending 集合里取信号。不阻塞 → 回用户态时被 handler 取走（A 组 handler 跑了、
   fd 读不到）；阻塞 → handler 不再被调用，信号留在 pending 里等 `read()`（B 组）。
2. **排队规则不变**：标准信号合并（发 2 次读到 1 个），实时信号排队（发 2 次读到 2 个）。
   `signalfd` 只换了“取”的方式，没换“存”的规则。
3. **它把异步的信号变成同步的 IO**：`poll()` 等 fd 和等 socket 一模一样——
   无信号 200ms 超时返回 0，有信号立刻返回 1，`read()` 之后再也不会遇到 `EINTR`。
   这就是“用事件循环处理信号”的推荐姿势。

## 复现

```c
/* sigfdprobe.c: handler 与 signalfd 二选一；只有被阻塞的信号才进 fd */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>

static volatile int handler_hits;

static void on_usr1(int sig)
{
	(void)sig;
	handler_hits++;
}

static void drain(const char *tag, int sfd)
{
	struct signalfd_siginfo si;
	ssize_t r;
	int n = 0, last = -1;

	while ((r = read(sfd, &si, sizeof(si))) == (ssize_t)sizeof(si)) {
		n++;
		last = (int)si.ssi_signo;
	}
	printf("[sfd] %-30s 读到 %d 个 (最后 signo=%d) %s\n",
	       tag, n, last, r < 0 ? strerror(errno) : "");
}

int main(void)
{
	struct sigaction sa;
	sigset_t mask, rtmask;
	struct pollfd pfd;
	int sfd, pr;

	setvbuf(stdout, NULL, _IONBF, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);

	/* A) 不阻塞：信号走 handler，signalfd 读不到 */
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sfd = signalfd(-1, &mask, SFD_NONBLOCK);
	kill(getpid(), SIGUSR1);
	usleep(50000);
	printf("[sfd] A: handler_hits=%d（未阻塞 → 走 handler）\n", handler_hits);
	drain("A: 未阻塞时读 signalfd", sfd);
	close(sfd);

	/* B) 先阻塞再建 signalfd */
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sfd = signalfd(-1, &mask, SFD_NONBLOCK);
	kill(getpid(), SIGUSR1);
	kill(getpid(), SIGUSR1);
	usleep(50000);
	printf("[sfd] B: handler_hits=%d（阻塞后不再变）\n", handler_hits);
	drain("B: 已阻塞时读 signalfd", sfd);
	close(sfd);

	/* C) 实时信号：发两次读到两个 */
	sigemptyset(&rtmask);
	sigaddset(&rtmask, SIGRTMIN);
	sigprocmask(SIG_BLOCK, &rtmask, NULL);
	sfd = signalfd(-1, &rtmask, SFD_NONBLOCK);
	kill(getpid(), SIGRTMIN);
	kill(getpid(), SIGRTMIN);
	usleep(50000);
	drain("C: SIGRTMIN 连发两次", sfd);
	close(sfd);

	/* D) poll 等它 */
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sfd = signalfd(-1, &mask, SFD_NONBLOCK);
	pfd.fd = sfd;
	pfd.events = POLLIN;
	pr = poll(&pfd, 1, 200);
	printf("[sfd] D: 无信号时 poll=%d（200ms 超时）revents=0x%x\n", pr, pfd.revents);
	kill(getpid(), SIGUSR1);
	pr = poll(&pfd, 1, 200);
	printf("[sfd] D: 有信号时 poll=%d revents=0x%x\n", pr, pfd.revents);
	drain("D: poll 之后读", sfd);
	close(sfd);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/sigfdprobe /tmp/sigfdprobe.c
make -j16 bzImage
rm -rf /tmp/jobs6 && cp -a bb_rootfs /tmp/jobs6
cp /tmp/sigfdprobe /tmp/jobs6/bin/sigfdprobe
cat > /tmp/jobs6/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
/bin/sigfdprobe
$B dmesg > /dm.txt
$B grep -E "signalfd: |signalfd_read:" /dm.txt
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/jobs6/init
( cd /tmp/jobs6 && find . | cpio -o -H newc --quiet ) > /tmp/bb_jobs6.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_jobs6.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 log_buf_len=2M panic=1" \
  -nographic -m 512M -no-reboot > /tmp/latte-jobs6.log 2>&1 || true
tr -d '\r' < /tmp/latte-jobs6.log | sed -n '/sigfdprobe/,/done/p'
```

一个踩过的坑：D 组最初想用“发一个不在 mask 里的信号”验证“不会唤醒”，
结果那个信号没人处理、按默认动作**把进程杀掉了**（`rc=140` = 128+12，`SIGUSR2`），
后面几步全没跑。要么给它也装 handler，要么就别发。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `fs/signalfd.c:251` | `do_signalfd4()` | 创建：`signotset()` 把掩码取反后存进 `signalfd_ctx` |
| `fs/signalfd.c:201` | `signalfd_read_iter()` | 读：循环 `count` 次取 `siginfo` |
| `fs/signalfd.c:154` | `signalfd_dequeue()` | 真正从 pending 里摘信号（非阻塞时直接 `-EAGAIN`） |
| `fs/signalfd.c:162` | `dequeue_signal()` 调用点 | **与 handler 共用**的取信号函数 |
| `kernel/signal.c:603` | `__dequeue_signal()` | 从 `sigpending` 里摘一个，标准/实时信号规则都在这里 |
| `include/linux/signal.h` | `sigset_t` / `signotset()` | 内核 `sigset_t` 是“1 = 不在集合里”的取反位图 |

## 一句话收尾

`signalfd` 没有改变信号的产生与排队，只是把**取信号的动作**从“回用户态时内核替你取”
变成了“你自己 `read()` 取”。前提只有一个：**先把信号阻塞住**，
否则它在你建好 fd 之前就被 handler 拿走了。
