# 主题二 · 公平性：EEVDF 与 nice 权重

`01` 讲了“什么时候换人”，这篇讲**换人时谁该多跑**。
一句话：nice 不是优先级，是**权重**；CPU 时间按权重分，差距可以到几十倍。
两个绑在同一颗 CPU 上的忙循环，一个 nice 0、一个 nice 19，跑 2 秒后的账非常干净。

## 你已知的

- `voluntary`/`nonvoluntary` 两种切换（`sched/01`）；
- `nice` 值用 `nice -n 19 cmd` 或 `setpriority()` 设置，范围 `-20`~`19`；
- 每个进程有个“虚拟运行时间”的概念，跑得越多涨得越快。

## 内核里对应什么：EEVDF 三个概念

本内核（6.18）的 CFS 已经是 **EEVDF**（Earliest Eligible Virtual Deadline First），
不再是老版本的“红黑树最左 + 固定时间片”：

| 概念 | 字段 / 函数 | 意思 |
|---|---|---|
| 基线切片 | `sysctl_sched_base_slice` = **700000 ns**（`kernel/sched/fair.c:79`） | 一次“该给多少 CPU”的基准，`se->slice` 默认取它 |
| 权重 | `sched_prio_to_weight[]`（`kernel/sched/core.c:10342`） | nice 0 → **1024**，nice 19 → **15**；差 **68 倍** |
| 虚拟运行时间 | `calc_delta_fair()`（`kernel/sched/fair.c:290`） | 真实跑 1ms，vruntime 涨 `1ms × 1024/weight` |

摘一段权重表：

```
const int sched_prio_to_weight[40] = {
 /* -20 */     88761,     71755, ...
 /*   0 */      1024,       820,       655,       526,       423,
 ...
 /*  15 */        36,        29,        23,        18,        15,
};
```

选择逻辑（`kernel/sched/fair.c:931` 起）：

1. 只有**合格**（eligible）的任务才有资格被选：`vruntime_eligible()`（`:722`）
   判断“它拿到的服务是否少于应得的”；
2. 合格的里面挑**虚拟截止期最早**的（`vruntime + vslice`）；
3. 于是权重大的任务 vruntime 涨得慢，长期看拿到的 CPU 时间就多。

**为什么“切片”不是固定 0.7ms**：`se->slice` 只是“一次该给多少”，
如果对手不合格（欠的服务太多或太少），当前任务会一直跑下去——
实测里 nice 0 的进程 2 秒只被调度了 23 次，平均每次 86ms，远大于 0.7ms。

## 实测：2 秒、同一颗 CPU、nice 0 vs nice 19

两个子进程都用 `sched_setaffinity` 绑到 CPU 0，各 `spin_ms(2000)`，退出前读自己的
`/proc/self/stat` 与 `/proc/self/schedstat`：

```
[nice] nice=0   utime=196 ticks
[nice] nice=0   run=1976724961 ns wait=34095807 ns pcount=23
[nice] nice=19  utime=3 ticks
[nice] nice=19  run=33714466 ns wait=1983538610 ns pcount=17
```

`/proc/PID/schedstat` 三列的含义（`fs/proc/base.c:511`）：
`sum_exec_runtime`（真跑多久）、`run_delay`（在 runqueue 上等了多久）、
`sched_info.pcount`（被调度上 CPU 的次数）。

| | `utime` | 真跑（run） | 等待（wait） | 被调度次数 |
|---|---|---|---|---|
| nice 0 | **196 ticks** | **1.977 s** | 0.034 s | 23 |
| nice 19 | **3 ticks** | **0.034 s** | **1.98 s** | 17 |

三个比值互相印证：

- 真实运行时间比 `1.977 : 0.034` ≈ **58 : 1**；
- 权重比 `1024 : 15` ≈ **68 : 1**；
- jiffy 粒度（`USER_HZ=100`）下 `utime` 比 `196 : 3` ≈ **65 : 1**。

差异来自启动/退出阶段的零头与 tick 取整，量级完全一致：
**2 秒墙钟里，nice 19 只拿到约 34ms，其余时间都在等**。

## 三个结论

1. **nice 是权重，不是“优先级数字”**：nice 0 与 nice 19 的 CPU 份额是
   `1024 : 15`（`sched_prio_to_weight`）。不要按 `1/(nice+20)` 之类线性去估。
2. **低权重进程的代价是“等”，不是“慢”**：nice 19 的 `run_delay` 是 1.98 s——
   它几乎整段时间都躺在 runqueue 上排队；一旦上 CPU，执行速度并没有变慢
   （CPU 不会因为你 nice 高而跑得慢）。
3. **它也不怎么被“抢占”，只是选不上**：`pcount` 只有 17/23 次。
   nice 19 的 vruntime 涨得快，很快就变得**不合格**（`vruntime_eligible()` 为假），
   调度的第一关就被过滤掉；于是 nice 0 可以连续跑很久。
   所以 `nonvoluntary_ctxt_switches`（`01` 里那个计数）衡量不了“被饿着”，
   要看 `run_delay`。

## 复现

探针的 `b` 模式（完整源码见 `01`，这里只列关键行）：

```c
static void phase_b(void)
{
	pid_t p0, p19;

	fflush(stdout);
	p0 = fork();
	if (p0 == 0) {
		pin_cpu0();                       /* sched_setaffinity 到 CPU0 */
		setpriority(PRIO_PROCESS, 0, 0);
		spin_ms(2000);
		printf("[nice] nice=0   utime=%lu ticks\n", utime_ticks());
		schedstat("nice=0");              /* 读 /proc/self/schedstat */
		fflush(stdout);
		_exit(0);
	}
	p19 = fork();
	if (p19 == 0) {
		pin_cpu0();
		setpriority(PRIO_PROCESS, 0, 19);
		spin_ms(2000);
		printf("[nice] nice=19  utime=%lu ticks\n", utime_ticks());
		schedstat("nice=19");
		fflush(stdout);
		_exit(0);
	}
	waitpid(p0, 0, 0);
	waitpid(p19, 0, 0);
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/schedprobe /tmp/schedprobe.c
make -j16 bzImage
rm -rf /tmp/sched1 && cp -a bb_rootfs /tmp/sched1
cp /tmp/schedprobe /tmp/sched1/bin/schedprobe
cat > /tmp/sched1/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
/bin/schedprobe b
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/sched1/init
( cd /tmp/sched1 && find . | cpio -o -H newc --quiet ) > /tmp/bb_sched1.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_sched1.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 panic=1" \
  -nographic -m 1G -smp 2 -no-reboot > /tmp/latte-sched.log 2>&1 || true
tr -d '\r' < /tmp/latte-sched.log | grep -E "nice\]|probe\]"
```

一定要 `pin_cpu0()`：不绑核的话两个进程会各占一颗 CPU（本机 `-smp 2`），
跑出来是 `200 : 200`，看不出权重——这也是“竞争才会显现调度策略”的例子。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `kernel/sched/fair.c:79` | `sysctl_sched_base_slice` | 基线切片 700000 ns（0.7ms） |
| `kernel/sched/fair.c:290` | `calc_delta_fair()` | 真实时间 → vruntime：按 `NICE_0_LOAD/weight` 缩放 |
| `kernel/sched/fair.c:722` | `vruntime_eligible()` | 第一关：这任务“还欠不欠服务” |
| `kernel/sched/fair.c:931` | 选择逻辑注释 | 合格里挑虚拟截止期最早的 |
| `kernel/sched/core.c:10342` | `sched_prio_to_weight[]` | nice → 权重（nice 0 = 1024，nice 19 = 15） |
| `fs/proc/base.c:511` | `proc_pid_schedstat()` | `/proc/PID/schedstat` 三列：run / wait / pcount |
| `kernel/sched/core.c` | `setpriority` 路径 | `nice()`/`setpriority()` → `sched_setscheduler` 改权重 |

## 一句话收尾

“公平”不是“平均分”，而是**按权重分**：nice 0 与 nice 19 的份额是 1024 : 15。
低权重进程拿到的不是更慢的 CPU，而是**更多的等待**——
`run_delay` 才是它的痛点，`utime` 只是结果。
