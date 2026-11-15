# 主题三 · cgroup 的 CPU 权重：组与组之间按份额分

`sched/02` 讲的是**一个组内**按 nice 权重分。这篇讲**组和组之间**：
把两个进程放进两个 cgroup，`cpu.weight` 设成 100 与 300，
它们在同一颗 CPU 上应该拿到 1:3 —— 实测精确落在这个比例上。

## 你已知的

- nice 是权重，nice 0 = 1024、nice 19 = 15（`sched/02`）；
- cgroup 是“把进程分组”的机制，v2 是单层树，挂在 `/sys/fs/cgroup`；
- 想看某组用了多少 CPU，有 `cpu.stat`。

## 内核里对应什么：也是权重，只是换了个量纲

cgroup v2 的 `cpu.weight` 范围 **1~10000，默认 100**（`include/linux/cgroup.h:40`），
写入时被换算成调度器内部的 shares：

```c
static inline unsigned long sched_weight_from_cgroup(unsigned long cgrp_weight)
{
	return DIV_ROUND_CLOSEST_ULL(cgrp_weight * 1024, CGROUP_WEIGHT_DFL);
}
```

`kernel/sched/sched.h:258`，于是：

| `cpu.weight` | shares | 等价于 |
|---|---|---|
| 100（默认） | 1024 | nice 0 |
| 300 | 3072 | 三个 nice 0 |
| 1 | 10 | 比 nice 19（15）还低 |

每个 `task_group` 有一个组级的调度实体（`se`），参与的就是**同一套 EEVDF**，
所以 `sched/02` 里那套 `vruntime_eligible()` / 权重逻辑，在组之间同样成立——
只不过竞争单位从“任务”变成了“组”。

写入路径：`echo 300 > cpu.weight` →
`cpu_weight_write_u64()`（`kernel/sched/core.c:10141`）
→ `sched_group_set_shares()`（`:10152`）→ 组内所有任务的份额一起变。

记账在另一处：`/sys/fs/cgroup/<组>/cpu.stat` 的 `usage_usec`
（`kernel/cgroup/rstat.c:731`），它统计的是这一组里所有任务实际消耗的 CPU 时间。

## 实测：两个组，同一个 CPU，各跑 2 秒

```
=== weights ===
100            ← A 默认值
100            ← B 默认值
100            ← A 写入 100 后
300            ← B 写入 300 后

=== cpu.max 是否存在 ===
cpu.idle cpu.stat cpu.stat.local cpu.weight cpu.weight.nice

=== cgprobe ===
[cg] A(weight=100) utime=50 ticks
[cg] B(weight=300) utime=149 ticks

=== cpu.stat ===
A: usage_usec 510920   user_usec 500922   system_usec 9998
B: usage_usec 1516697  user_usec 1502663  system_usec 14034
```

三组数字互相印证（墙钟都是 2 秒）：

| 组 | `cpu.weight` | shares | `utime` | `usage_usec` | 份额 |
|---|---|---|---|---|---|
| A | 100 | 1024 | **50 ticks** | 0.511 s | **1/4** |
| B | 300 | 3072 | **149 ticks** | 1.517 s | **3/4** |

`50 : 149` 与 `0.511 s : 1.517 s` 都是 **1 : 3**，就是 `cpu.weight` 之比；
`user_usec` 几乎等于 `usage_usec`，说明这 2 秒全在用户态忙等
（`system_usec` 只有 10~14 ms，是启动和收尾的系统调用）。

**一个必须写下来的负面结果**：`cpu.max` 在这个内核里**不存在**——
`ls /sys/fs/cgroup/A/` 只有 `cpu.idle cpu.stat cpu.stat.local cpu.weight cpu.weight.nice`。
因为 `.config` 里 `# CONFIG_CFS_BANDWIDTH is not set`（`:199`）。
于是本内核**只能按份额分，不能封顶**（设不了“本组最多用 50% 的核”）。
这是两个独立的能力：`FAIR_GROUP_SCHED` 管“按权重分”，`CFS_BANDWIDTH` 才管“限速”。

## 三个结论

1. **cgroup 的 CPU 分配是“按权重分”，不是“按核数切”**：`100 : 300` 精确对应
   `1 : 3` 的 CPU 时间（实测 50:149 ticks、0.511s:1.517s）。
   所以给一组设 `cpu.weight=300` 并不会让它“独享三颗核”，只是在竞争时占三份。
2. **`cpu.weight` 就是“组版的 nice”**：默认 100 ↔ shares 1024 ↔ nice 0；
   换算表在 `kernel/sched/sched.h:258`。任务级的 nice 与组级的 weight 可以叠加：
   组间先按 weight 分，组内再按 nice 分。
3. **份额 ≠ 上限**：想要“最多 50%”必须用 `cpu.max`（`CFS_BANDWIDTH`），
   本内核没开，所以这一项**实测不了**——这是配置缺口，不是实验失败。
   反过来，`cpu.weight` 也不受配额限制：只要别人不抢，100 的组照样能跑满一颗核。

## 复现

需要 cgroup v2（本内核 `CONFIG_CGROUPS=y`、`CONFIG_CGROUP_SCHED=y`、
`CONFIG_FAIR_GROUP_SCHED=y`）与 `-smp 2`（多核不绑核就看不出竞争）：

```c
/* cgprobe.c: 两个 cgroup 各跑一个忙循环，权重 100 vs 300 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <sys/wait.h>

static void pin_cpu0(void)
{
	cpu_set_t s;
	CPU_ZERO(&s); CPU_SET(0, &s);
	sched_setaffinity(0, sizeof(s), &s);
}

static void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0) { fprintf(stderr, "open %s: %m\n", path); return; }
	if (write(fd, val, strlen(val)) < 0) fprintf(stderr, "write %s: %m\n", path);
	close(fd);
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

int main(void)
{
	int pa[2], pb[2];
	pid_t a, b;
	char buf[32];

	if (pipe(pa) || pipe(pb)) { perror("pipe"); return 1; }

	a = fork();
	if (a == 0) {
		pin_cpu0();
		if (read(pa[0], buf, 1) != 1) _exit(2);
		spin_ms(2000);
		printf("[cg] A(weight=100) utime=%lu ticks\n", utime_ticks());
		fflush(stdout);
		_exit(0);
	}
	b = fork();
	if (b == 0) {
		pin_cpu0();
		if (read(pb[0], buf, 1) != 1) _exit(2);
		spin_ms(2000);
		printf("[cg] B(weight=300) utime=%lu ticks\n", utime_ticks());
		fflush(stdout);
		_exit(0);
	}

	/* 先把两个孩子挪进各自的 cgroup，再同时放行 */
	snprintf(buf, sizeof(buf), "%d", a);
	write_file("/sys/fs/cgroup/A/cgroup.procs", buf);
	snprintf(buf, sizeof(buf), "%d", b);
	write_file("/sys/fs/cgroup/B/cgroup.procs", buf);
	write(pa[1], "x", 1);
	write(pb[1], "x", 1);

	waitpid(a, 0, 0);
	waitpid(b, 0, 0);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/cgprobe /tmp/cgprobe.c
make -j16 bzImage
rm -rf /tmp/sched3 && cp -a bb_rootfs /tmp/sched3
cp /tmp/cgprobe /tmp/sched3/bin/cgprobe
cat > /tmp/sched3/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B mkdir -p /sys/fs/cgroup
$B mount -t cgroup2 none /sys/fs/cgroup || echo "CGROUP2 MOUNT FAILED"
echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control || echo "SUBTREE FAILED"
$B mkdir -p /sys/fs/cgroup/A /sys/fs/cgroup/B
echo 100 > /sys/fs/cgroup/A/cpu.weight
echo 300 > /sys/fs/cgroup/B/cpu.weight
$B cat /sys/fs/cgroup/A/cpu.weight /sys/fs/cgroup/B/cpu.weight
echo "=== cpu.max? ==="
$B ls /sys/fs/cgroup/A/
/bin/cgprobe
$B grep usage_usec /sys/fs/cgroup/A/cpu.stat
$B grep usage_usec /sys/fs/cgroup/B/cpu.stat
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/sched3/init
( cd /tmp/sched3 && find . | cpio -o -H newc --quiet ) > /tmp/bb_sched3.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_sched3.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 panic=1" \
  -nographic -m 1G -smp 2 -no-reboot > /tmp/latte-sched3.log 2>&1 || true
tr -d '\r' < /tmp/latte-sched3.log | sed -n '/=== cpu.max/,/done/p'
```

`echo "+cpu" > cgroup.subtree_control` 这步不能少：v2 默认不启用控制器，
不启用的话 `A/cpu.weight` 根本不会出现（只会报 `No such file`）。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `include/linux/cgroup.h:40` | `CGROUP_WEIGHT_DFL` | `cpu.weight` 的默认值 100（范围 1~10000） |
| `kernel/sched/sched.h:258` | `sched_weight_from_cgroup()` | `weight × 1024 / 100` → shares |
| `kernel/sched/core.c:10141` | `cpu_weight_write_u64()` | 写 `cpu.weight` 的入口（含范围检查） |
| `kernel/sched/core.c:10152` | `sched_group_set_shares()` | 把新份额应用到整个 task_group |
| `kernel/cgroup/rstat.c:731` | `cpu.stat` 输出 | `usage_usec` / `user_usec` / `system_usec` |
| `.config:199` | `# CONFIG_CFS_BANDWIDTH is not set` | 所以没有 `cpu.max`，只能分份额不能限速 |

## 一句话收尾

`sched/02` 的 nice 与这里的 `cpu.weight` 是同一件事的两种量纲：
**先按组权重分，再按任务 nice 分**。想限速（`cpu.max`）得另外开
`CONFIG_CFS_BANDWIDTH`——份额和上限是两套机制，别指望一个顶两个。
