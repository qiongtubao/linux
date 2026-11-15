# 主题四 · 实时调度：`SCHED_FIFO` 与那 5% 的缝

`sched/02`、`03` 讲的都是“按权重分”，前提是**所有任务平等竞争**。
`SCHED_FIFO` 不参与这套：它只有一个规则——**优先级高的先跑，同优先级先来先跑，不抢完不让**。
这篇实测一个 RT 任务把同一颗 CPU 上的普通任务饿到什么程度，
以及“95%”这个数字从哪来。

## 你已知的

- 两种让出：自己阻塞、被抢占（`sched/01`）；
- nice 与 cgroup 权重都是“分份额”的语言（`sched/02`、`03`）；
- `chrt`/`sched_setscheduler()` 能把进程改成 `SCHED_FIFO`/`SCHED_RR`，需要 root。

## 内核里对应什么：调度类是有顺序的

```
stop_sched_class       ← 迁移线程，看不见
  └─ dl_sched_class   ← SCHED_DEADLINE
      └─ rt_sched_class   ← SCHED_FIFO(1-99) / SCHED_RR，本内核 CONFIG_RT_GROUP_SCHED 未开
          └─ fair_sched_class  ← SCHED_OTHER（EEVDF，02/03 讲的就是它）
              └─ idle_sched_class
```

挑下一个任务时**按类逐个往下问**，所以只要 `rt_sched_class` 里有可跑的任务，
`fair_sched_class` 就永远轮不到——优先级不参与“分”，它直接决定“有没有”。

`SCHED_FIFO` 的两个特点：

- **没有时间片**：只有更高优先级的 RT 任务来了、或自己阻塞/`sched_yield()` 才让出；
- **但整体受 RT 限流**：`sched_rt_runtime_us / sched_rt_period_us`
  = **950000 / 1000000**（`kernel/sched/rt.c:24`，sysctl 在 `:43`）。
  也就是 RT 类最多占 95% 的 CPU，剩下 5% 强制留给普通任务——否则一个写错的 RT 线程能把机器锁死。

改策略的唯一入口是 `__sched_setscheduler()`（`kernel/sched/syscalls.c:514`），
`nice`、`SCHED_FIFO`、cgroup 权重最终都从这里落到 `p->sched_class` 与 `p->prio` 上。

## 打点（1 个）

| 打点 | 位置 | 内容 |
|---|---|---|
| `setscheduler: comm=… pid=… policy=… prio=…` | `__sched_setscheduler()` 入口 | 谁把哪个进程改成了什么策略 |

本次只触发 1 次：探针把自己从 `SCHED_OTHER` 改成 `SCHED_FIFO(1)`：

```
[    3.521340] [latte] setscheduler: comm=rtprobe pid=67 policy=1 prio=1
```

## 实测：RT(1) 与普通任务绑同一颗 CPU，各忙等 1 秒

限流参数（开机默认值，没改）：

```
$ cat /proc/sys/kernel/sched_rt_runtime_us /proc/sys/kernel/sched_rt_period_us
950000
1000000
```

A 组（一个 `SCHED_FIFO(1)`，一个 `SCHED_OTHER`）：

```
[rt] SCHED_FIFO(1)   start=3.499 end=4.499 wall=1.000
[rt] SCHED_FIFO(1) 1s   policy=1 cpu=0 aff0=1 utime=95 ticks
[rt] SCHED_OTHER     start=4.451 end=5.450 wall=0.999
[rt] SCHED_OTHER 1s     policy=0 cpu=0 aff0=1 utime=97 ticks
```

B 组（对照组，两个都是 `SCHED_OTHER`）：

```
[rt] SCHED_OTHER     start=5.486 end=6.487 wall=1.001
[rt] SCHED_OTHER 1s     policy=0 cpu=0 aff0=1 utime=50 ticks
[rt] SCHED_OTHER     start=5.484 end=6.485 wall=1.001
[rt] SCHED_OTHER 1s     policy=0 cpu=0 aff0=1 utime=50 ticks
```

**光看 `utime` 会读错**：A 组里普通任务也有 97 ticks，看起来“两个都拿到了一整秒”。
加上 `start/end` 才发现真相：

| | 开始 | 结束 | `utime` |
|---|---|---|---|
| RT 任务 | 3.499 | 4.499 | **95 ticks** |
| 普通任务 | **4.451** | 5.450 | 97 ticks |

两个时间窗**首尾相接、几乎不重叠**（3.499–4.499 与 4.451–5.450）。
普通任务的 `start` 比它被放行的时刻（≈3.5）晚了 **0.952 秒**——
因为它虽然被唤醒、进了 runqueue，却一直选不上；`read()` 要等它真正拿到 CPU 才返回，
所以那个 `start` 时间戳量到的就是**它被饿住的时长**。

0.952 秒不是巧合：RT 任务在跑了 95%（`0.95 × 1s`）之后被**限流**，
让出 5% 的窗口，普通任务才终于挤进来；紧接着 RT 任务自己跑完退出，普通任务才把剩下的一秒跑完。

对照组（B 组）则是典型的公平分时：两个 `start` 相差 2ms，`utime` 各 50 ticks，
`49+49 ≈ 100` —— 一秒钟被精确对半分。

## 三个结论

1. **`SCHED_FIFO` 不“分 CPU”，它“占 CPU”**：只要它可运行，同核上的
   `SCHED_OTHER` 一秒内连一次都调度不上（普通任务从被唤醒到首次运行隔了 0.95 s）。
2. **95% 是硬线，来自 RT 限流**：`sched_rt_runtime_us=950000` / `sched_rt_period_us=1000000`。
   RT 任务的 `utime` 正好 95 ticks，与预算一分不差；剩下 5% 是普通任务唯一的活路。
   把它设成 `-1`（不限制）在这台机器上就等于“普通任务饿死到 RT 退出为止”——本次没这么测，
   因为会连 shell 一起卡住。
3. **读数要配时间窗**：`utime` 只说明“用了多少 CPU”，不说明“什么时候用的”。
   两个进程各自跑满一秒、却是串行的，合计必然超过墙钟——
   A 组 `utime` 相加 192 ticks > 100 ticks/秒，这个“不可能”就是发现问题的入口。
   `start/end`（`clock_gettime(CLOCK_MONOTONIC)`）才是判断重叠的唯一依据。

## 复现

需要 root（`sched_setscheduler` 改 RT 要 `CAP_SYS_NICE`）与 `-smp 2`（把两个任务都绑到 CPU0，
留一颗给 shell，避免自己把控制台饿死）：

```c
/* rtprobe.c: RT(1) vs SCHED_OTHER 抢同一颗 CPU；b 模式是两个普通任务做对照 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <sys/wait.h>

static void pin_cpu0(void)
{
	cpu_set_t s;
	CPU_ZERO(&s); CPU_SET(0, &s);
	sched_setaffinity(0, sizeof(s), &s);
}

static unsigned long utime_ticks(void)
{
	FILE *f = fopen("/proc/self/stat", "r");
	char buf[1024], *p;
	unsigned long ut = 0;
	int i = 0;

	if (!f) return 0;
	if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
	fclose(f);
	p = strrchr(buf, ')');
	if (!p) return 0;
	p++;
	while (*p && i < 13) {
		while (*p == ' ') p++;
		if (!*p) break;
		i++;
		if (i == 12) { ut = strtoul(p, 0, 10); break; }
		while (*p && *p != ' ') p++;
	}
	return ut;
}

static void spin_ms(int ms)
{
	struct timespec t0, t1;
	long i;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		for (i = 0; i < 20000; i++) __asm__ volatile("" ::: "memory");
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= ms)
			break;
	}
}

static void child(int rfd, int rt)
{
	char c;
	struct timespec t0, t1;
	struct sched_param sp = { .sched_priority = 1 };

	pin_cpu0();
	if (rt && sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
		perror("sched_setscheduler");
	if (read(rfd, &c, 1) != 1)
		_exit(2);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	spin_ms(1000);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("[rt] %-18s start=%.3f end=%.3f wall=%.3f\n",
	       rt ? "SCHED_FIFO(1)" : "SCHED_OTHER",
	       t0.tv_sec + t0.tv_nsec / 1e9, t1.tv_sec + t1.tv_nsec / 1e9,
	       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
	printf("[rt] %-18s policy=%d cpu=%d utime=%lu ticks\n",
	       rt ? "SCHED_FIFO(1) 1s" : "SCHED_OTHER 1s",
	       sched_getscheduler(0), sched_getcpu(), utime_ticks());
	fflush(stdout);
	_exit(0);
}

int main(int argc, char **argv)
{
	int rt = !(argc > 1 && argv[1][0] == 'b');
	int pa[2], pb[2];
	pid_t a, b;

	if (pipe(pa) || pipe(pb)) { perror("pipe"); return 1; }
	a = fork();
	if (a == 0) child(pa[0], rt);
	b = fork();
	if (b == 0) child(pb[0], 0);
	write(pa[1], "x", 1);
	write(pb[1], "x", 1);
	waitpid(a, 0, 0);
	waitpid(b, 0, 0);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/rtprobe /tmp/rtprobe.c
make -j16 bzImage
rm -rf /tmp/sched4 && cp -a bb_rootfs /tmp/sched4
cp /tmp/rtprobe /tmp/sched4/bin/rtprobe
cat > /tmp/sched4/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B cat /proc/sys/kernel/sched_rt_runtime_us /proc/sys/kernel/sched_rt_period_us
/bin/rtprobe a
/bin/rtprobe b
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/sched4/init
( cd /tmp/sched4 && find . | cpio -o -H newc --quiet ) > /tmp/bb_sched4.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_sched4.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 panic=1" \
  -nographic -m 1G -smp 2 -no-reboot > /tmp/latte-sched4.log 2>&1 || true
tr -d '\r' < /tmp/latte-sched4.log | grep -E "rt\]|^[0-9]{6,}"
```

**别把 shell 也绑到 CPU0**：RT 任务在跑的那一秒里，同核上的普通任务完全选不上，
如果 shell 也被绑上去，你会以为机器死了（其实只是在等那 5%）。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `kernel/sched/rt.c:24` | `sysctl_sched_rt_runtime` | RT 限流预算 950000 µs |
| `kernel/sched/rt.c:43` | `sched_rt_runtime_us` sysctl | `/proc/sys/kernel/sched_rt_runtime_us` |
| `kernel/sched/syscalls.c:514` | `__sched_setscheduler()` | 改策略的唯一入口（nice/FIFO/RR 都走这里） |
| `include/uapi/linux/sched.h` | `SCHED_FIFO` / `SCHED_RR` | `1` / `2`（`SCHED_OTHER`=0） |
| `kernel/sched/core.c:5150` | `finish_task_switch()` | 切换点（`sched/01` 的打点） |

## 一句话收尾

`SCHED_OTHER` 在**分** CPU，`SCHED_FIFO` 在**抢** CPU：
前者靠权重（nice / `cpu.weight`），后者只看优先级，唯一的约束是那 5% 的限流缝。
读调度数据时，`utime` 回答“用了多少”，`start/end` 回答“什么时候用的”——两个都要看。
