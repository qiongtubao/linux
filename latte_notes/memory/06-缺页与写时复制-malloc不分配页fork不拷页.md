# 主题六 · 缺页与写时复制：malloc 不分配页，fork 不拷页

`memory/01`~`05` 讲的是**分配器**（页从哪来、紧张时谁回收）。这篇讲**分配器之前的那个入口**：
用户态碰一个地址 → CPU 抛 `#PF` → 内核才去要页。两个反直觉事实都在这一跳上：
**`malloc`/`mmap` 拿到的只是地址区间**，**`fork` 之后父子共享同一批物理页**。

## 你已知的

- 物理页按 order 从 buddy 拿（`memory/02`），小对象由 slab 切（`memory/03`）；
- 进程有独立的页表，`execve` 会换掉整个地址空间（`execve/02`、`execve/03`）；
- `/proc/vmstat` 里 `pgfault` 是累计缺页数，`/proc/PID/stat` 的 `minflt`/`majflt` 是本进程的。

## 内核里对应什么：#PF 的三级跳

```
用户态 mov [addr], 1   ← 页表里没有或只读
 └─ CPU 抛 #PF（error_code 编码了原因）→ exc_page_fault
     └─ do_user_addr_fault()          arch/x86/mm/fault.c:1210   ← 打点（带 err）
         └─ handle_mm_fault()         mm/memory.c:6475           ← 打点（带 flags）
             ├─ 页不存在、匿名映射 → do_anonymous_page()  mm/memory.c:5139  ← 打点
             ├─ 页存在但只读（COW）→ do_wp_page() → wp_page_copy()  mm/memory.c:3659  ← 打点
             └─ 文件映射 → do_fault() → filemap_fault()/shmem_fault()
```

`error_code` 的低 5 位是解读一切的钥匙（`x86` 手册 P 位、W/R 位、U/S 位）：

| err | 二进制 | 含义 | 实测对应 |
|---|---|---|---|
| `0x6` | `110` | 用户态、写、**页不存在** | 第一次写匿名页 → `do_anonymous_page` |
| `0x7` | `111` | 用户态、写、**页存在但只读** | 写 COW 页 → `wp_page_copy` |
| `0x3` | `011` | **内核态**、写、页存在 | `copy_to_user`/`put_user` 写用户页 |
| `0xe` | `1110` | 取指路径（配合 `flags` 的 `FAULT_FLAG_INSTRUCTION`） | 首次执行某页代码 |

`fork` 是另一半故事，它**不碰上面的路径，也不拷任何用户数据**：

```
fork() → copy_process → copy_mm → dup_mmap()   mm/mmap.c:1720   ← 打点
    └─ 逐个 vma 复制页表项，并把父子两侧的 PTE 都清掉可写位
        （谁写谁触发 err=0x7，这时才真正拷）
```

所以 `fork` 的成本 ≈ 页表项数，不是内存大小；`exec` 一个程序动不动几毫秒、
`fork` 只要几十微秒，原因就在这里。

## 代码位置与 5 个打点

| 打点 | 位置 | 记什么 |
|---|---|---|
| `do_user_addr_fault: addr=… err=…` | `arch/x86/mm/fault.c` | #PF 的 C 入口，`err` 是硬件给的 |
| `handle_mm_fault: addr=… flags=…` | `mm/memory.c` | mm 层总入口，`flags` 是内核自己的意图位 |
| `do_anonymous_page: addr=…` | `mm/memory.c` | 真的向 buddy/slab 要了一页 |
| `wp_page_copy: addr=…` | `mm/memory.c` | 真的把旧页内容拷了一份 |
| `dup_mmap: vmas=…` | `mm/mmap.c` | fork 复制地址空间，`vmas` 父进程当时的 vma 数 |

`flags` 也能读：`0x1255` = `WRITE|ALLOW_RETRY|KILLABLE|USER|INTERRUPTIBLE|VMA_LOCK`，
即“用户态写缺页”；`0x1354` 把 `WRITE` 换成 `INSTRUCTION`（`1<<8`），是取指缺页；
`0x215` 没有 `VMA_LOCK`、但有 `WRITE|USER`，是内核替用户写的路径。

## 实测 A：`mmap` 三个页，写一个才给一个

测试程序先 `mmap` 3 页（12KB），然后**逐页写一个字节**：

```
[prog] mmap 3 pages base=0x7f402da51000 minflt=55 majflt=0     ← 此刻 0 次新缺页
[latte] do_user_addr_fault: addr=7f402da51000 err=6
[latte] handle_mm_fault: addr=7f402da51000 flags=0x1255
[latte] do_anonymous_page: addr=7f402da51000
[prog] write page0 minflt=56 majflt=0                            ← +1
[latte] do_user_addr_fault: addr=7f402da52000 err=6
[latte] handle_mm_fault: addr=7f402da52000 flags=0x1255
[latte] do_anonymous_page: addr=7f402da52000
[prog] write page1 minflt=57 majflt=0                            ← +1
[latte] do_user_addr_fault: addr=7f402da53000 err=6
[latte] handle_mm_fault: addr=7f402da53000 flags=0x1255
[latte] do_anonymous_page: addr=7f402da53000
[prog] write page2 minflt=58 majflt=0                            ← +1
```

`mmap` 只改了页表/`vma` 结构，**一页物理内存都没要**；`minflt` 一次+1，三次写才三页。
`err=6` 的 `P=0` 说明当时连页表项都没有。

## 实测 B：`fork` 只复制页表，一碰才拷

```
[latte] dup_mmap: vmas=12                     ← fork：复制 12 个 vma 的页表项
[latte] wp_page_copy: addr=312f3000           ← 子进程继续跑自己的栈/堆 → 立刻开始拷
...
[prog] child after fork minflt=8 majflt=0     ← 子进程的 min_flt 从 0 起算（fork.c:1518 清零）
[latte] do_user_addr_fault: addr=7f402da51000 err=7
[latte] handle_mm_fault: addr=7f402da51000 flags=0x1255
[latte] wp_page_copy: addr=7f402da51000       ← 子进程写 page0 → 拷这一页
[prog] child write page0 (COW) minflt=12 majflt=0
```

`dup_mmap` 之后那串 `wp_page_copy`（地址是栈、堆、代码页）正是“**不碰就不拷、一碰就拷**”：
子进程继续执行就必然要写自己的栈，于是继承来的每一页在被写时才复制一份。

## 实测 C：拷不拷，看引用计数

同一个程序的两次写，结果不同：

```
[prog] parent while child alive minflt=60 majflt=0
[latte] do_user_addr_fault: addr=7f402da52000 err=7
[latte] handle_mm_fault: addr=7f402da52000 flags=0x1255
[latte] wp_page_copy: addr=7f402da52000        ← 子进程还活着（页被 2 个 MM 共享）→ 真拷
[prog] parent write page1 (COW) minflt=62 majflt=0

[latte] do_exit: faultprobe code=0             ← 子进程退出，这页的引用归 1
[latte] release_task: faultprobe
[prog] parent after child exit minflt=62 majflt=0
[latte] do_user_addr_fault: addr=7f402da53000 err=7
[latte] handle_mm_fault: addr=7f402da53000 flags=0x1255
[prog] parent write page2 (reuse) minflt=63 majflt=0   ← 没有 wp_page_copy：直接复用
```

两次都是 `err=7`（页存在、只读、用户写），但只有第一次调用了 `wp_page_copy()`。
区别在**页的引用计数**：还有别的地址空间映射着它才必须分配新页+拷贝；
只剩自己时内核直接把旧页改成可写（`do_wp_page()` 的复用快路径）。
这也解释了为什么“COW 页第一次写不一定慢”——要看当时还有没有别人共享。

## 三个结论

1. **`mmap`/`malloc` 只是记账，不占物理页**：真正要页的是第一次写触发的 `#PF`
   （匿名映射 → `do_anonymous_page`）。所以“进程用了多少内存”看 RSS，不看虚拟大小。
2. **`fork` 不拷数据，只复制页表并把两侧写成只读**：`dup_mmap` 打完点之后，
   日志里立刻出现的一串 `wp_page_copy` 都是“子进程自己继续跑”造成的，不是 `fork` 本身。
3. **写时复制里“复制”是可选的**：`err=7` 只说明“这页不能直接写”，
   到底分配新页还是就地复用，由引用计数决定（实测 C 的两次写，一次拷、一次不拷）。

## 复现

代码 60 行不到，不需要 stdio（`printf` 自己会触发缺页，把计数弄花），
直接 `write(1, …)` 输出；`/proc/self/stat` 的 `minflt` 是第 10 字段。

```c
/* faultprobe.c: 依次造出匿名页缺页与写时复制，并打印 minflt/majflt */
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

static char buf[512];
static int blen;

static void put(const char *s) { while (*s) buf[blen++] = *s++; }
static void puthex(unsigned long v)
{
	static const char h[] = "0123456789abcdef";
	char t[18]; int i = 0;
	buf[blen++] = '0'; buf[blen++] = 'x';
	if (!v) { buf[blen++] = '0'; return; }
	while (v) { t[i++] = h[v & 0xf]; v >>= 4; }
	while (i) buf[blen++] = t[--i];
}
static void putnum(unsigned long v)
{
	char t[24]; int i = 0;
	if (!v) { buf[blen++] = '0'; return; }
	while (v) { t[i++] = '0' + v % 10; v /= 10; }
	while (i) buf[blen++] = t[--i];
}
static int slurp(const char *path, char *dst, int cap)
{
	int fd = open(path, O_RDONLY), n;
	if (fd < 0) return -1;
	n = read(fd, dst, cap - 1);
	close(fd);
	if (n < 0) return -1;
	dst[n] = 0;
	return n;
}
static void faults(unsigned long *minflt, unsigned long *majflt)
{
	static char s[1024];
	char *p; int i = 0;
	*minflt = *majflt = 0;
	if (slurp("/proc/self/stat", s, sizeof(s)) < 0) return;
	p = strrchr(s, ')');
	if (!p) return;
	p++;
	while (*p) {
		char *tok;
		while (*p == ' ') p++;
		if (!*p) break;
		tok = p;
		while (*p && *p != ' ') p++;
		if (*p) { *p = 0; p++; }
		i++;
		if (i == 8) *minflt = strtoul(tok, 0, 10);
		else if (i == 10) *majflt = strtoul(tok, 0, 10);
	}
}
static void vmstat(unsigned long *pgfault, unsigned long *pgmaj)
{
	static char s[8192];
	char *p;
	*pgfault = *pgmaj = 0;
	if (slurp("/proc/vmstat", s, sizeof(s)) < 0) return;
	p = strstr(s, "\npgfault ");
	if (p) *pgfault = strtoul(p + 9, 0, 10);
	p = strstr(s, "\npgmajfault ");
	if (p) *pgmaj = strtoul(p + 12, 0, 10);
}
static void report(const char *label, void *base)
{
	unsigned long minflt, majflt;
	faults(&minflt, &majflt);
	blen = 0;
	put("[prog] "); put(label);
	if (base) { put(" base="); puthex((unsigned long)base); }
	put(" minflt="); putnum(minflt); put(" majflt="); putnum(majflt); put("\n");
	write(1, buf, blen);
}

int main(void)
{
	volatile char *p;
	unsigned long f0, m0, f1, m1;
	pid_t pid;

	vmstat(&f0, &m0);
	p = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return 1;
	report("mmap 3 pages", (void *)p);

	p[0] = 1;    report("write page0", 0);
	p[4096] = 2; report("write page1", 0);
	p[8192] = 3; report("write page2", 0);

	pid = fork();
	if (pid == 0) {
		report("child after fork", 0);
		p[0] = 4;
		report("child write page0 (COW)", 0);
		sleep(2);	/* 保持子进程存活：page1 仍被两个地址空间共享 */
		_exit(0);
	}
	sleep(1);
	report("parent while child alive", 0);
	p[4096] = 5;		/* 引用计数 2 → 必须真拷 */
	report("parent write page1 (COW)", 0);

	waitpid(pid, 0, 0);
	report("parent after child exit", 0);
	p[8192] = 6;		/* 引用计数 1 → 直接复用，不拷 */
	report("parent write page2 (reuse)", 0);

	vmstat(&f1, &m1);
	blen = 0;
	put("[prog] vmstat pgfault +"); putnum(f1 - f0);
	put(" pgmajfault +"); putnum(m1 - m0); put("\n");
	write(1, buf, blen);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/faultprobe /tmp/faultprobe.c   # 交叉不了就本机静态编译
make -j16 bzImage
rm -rf /tmp/net7 && cp -a bb_rootfs /tmp/net7
cp /tmp/faultprobe /tmp/net7/bin/faultprobe
cat > /tmp/net7/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "=== faultprobe ==="
/bin/faultprobe
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/net7/init
( cd /tmp/net7 && find . | cpio -o -H newc --quiet ) > /tmp/bb_net7.cpio
timeout 90 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_net7.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-net7.log 2>&1 || true
tr -d '\r' < /tmp/latte-net7.log | awk '/\[prog\] mmap 3 pages/,/vmstat pgfault/' \
  | grep -E "\[prog\]|\[latte\] (dup_mmap|do_anonymous_page|wp_page_copy)"
# 只看 #PF 的原因码：6=页不存在、7=页只读（COW）
tr -d '\r' < /tmp/latte-net7.log | awk '/\[prog\] mmap 3 pages/,/vmstat pgfault/' \
  | grep -E "do_user_addr_fault: addr=[0-9a-f]+ err=[67]"
```

两次运行的地址不会一样（用户态 ASLR 开着），要看的是**地址之间的关系**：
`base` / `base+0x1000` / `base+0x2000` 三页，以及最后那次写为什么没有 `wp_page_copy`。

## 代码位置

| 位置 | 函数 | 做什么 |
|---|---|---|
| `arch/x86/mm/fault.c:1210` | `do_user_addr_fault()` | 用户态 #PF 的 C 入口，`error_code` 由硬件给 |
| `mm/memory.c:6475` | `handle_mm_fault()` | mm 层总入口；也会被 GUP（`mm/gup.c:1126`）直接调用 |
| `mm/memory.c:5139` | `do_anonymous_page()` | 匿名页第一次写：这时才向 buddy 要页 |
| `mm/memory.c:3659` | `wp_page_copy()` | COW 真拷：分配新页 + 复制内容 + 改页表 |
| `mm/memory.c:4049` | `do_wp_page()` | 决定“拷还是复用”的地方，本页没打点（复用时不进 `wp_page_copy`） |
| `mm/mmap.c:1720` | `dup_mmap()` | fork 复制地址空间：只复制页表项并清可写位 |
| `kernel/fork.c:1518` | `copy_process()` | `tsk->min_flt = tsk->maj_flt = 0`：子进程缺页计数从 0 起 |

## 一句话收尾

`memory/01`~`05` 回答“页从哪来”，这篇回答“**什么时候才来要**”：
`mmap` 只记账，`fork` 只复制页表，真正分配和复制都压在第一次写上——
这也正是“分配器日志为什么安静、缺页日志为什么吵”的原因。
