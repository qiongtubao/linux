# 主题七 · LRU 与“第二次机会”：访问过的页凭什么活得更久

`memory/04` 说了“内存紧张时 kswapd 会回收”，`memory/05` 实测了“没有 swap 时回收空转”。
这篇钻进去看**回收时先挑谁**：页被分到 active / inactive 两条链表上，
“再访问一次”就是给一张复活券。全部有实测计数。

## 你已知的

- 页从 buddy 来，用完要还回去（`memory/02`）；
- 紧张时 kswapd / 直接回收会扫页，扫不到可回收页就 OOM（`memory/04`、`memory/05`）；
- 缺页时才分配页，`fork` 之后一写就 COW（`memory/06`）。

## 内核里对应什么：两条链表 + 一个引用位

每个 zone 的每个 memcg 有 5 条 LRU 链表，关键是前两条：

| 链表 | 页的状态 | 回收时的待遇 |
|---|---|---|
| `LRU_INACTIVE_*` | 刚进来的新页、被降级的旧页 | **先扫这里**，没人再访问就丢 |
| `LRU_ACTIVE_*` | 被访问过至少两次的页 | 先降级到 inactive 尾，下一轮才可能被丢 |

状态机全写在 `folio_mark_accessed()` 的注释里（`mm/swap.c`）：

```
* inactive,unreferenced -> inactive,referenced      ← 第一次访问：只置引用位
* inactive,referenced   -> active,unreferenced      ← 第二次访问：搬到 active（发券）
* active,unreferenced   -> active,referenced        ← 已 active，再置引用位
```

- “置引用位”只是改 `PG_referenced` 标志，**不动链表**——所以第一次访问很便宜；
- “搬到 active”才真正摘链、改链表，唯一实现是 `lru_activate()`，
  并且 `PGACTIVATE` 计数就在它里面 `+1`（`mm/swap.c:320`）。

回收侧对应两个函数：`shrink_inactive_list()`（`mm/vmscan.c:2009`，先扫 inactive）和
`shrink_active_list()`（`mm/vmscan.c:2130`，inactive 不够时把 active 降级）。

## 三个打点（+ 一个已有的）

| 打点 | 位置 | 记什么 |
|---|---|---|
| `mark_accessed: pfn=… ref=… act=…` | `mm/swap.c` `folio_mark_accessed()` | 访问时的页状态，能直接看出走了哪条转移 |
| `lru_activate: pfn=… nr=…` | `mm/swap.c` `lru_activate()` | inactive → active 的搬运，`PGACTIVATE` 的同一处 |
| `shrink_node: order=… target=…` | `mm/vmscan.c` `shrink_node()` | 一轮回收的入口（扫描/回收量看 vmstat） |
| `balance_pgdat: order=…` | `mm/vmscan.c`（`memory` 站已有） | kswapd 每轮醒来的入口 |

## 实测：1MB 文件读两次，再压内存（256MB 机器）

被测文件在 ext4 上（`/dev/vda`，`vfs/05` 那块盘），先 `drop_caches` 保证没缓存。
读第一遍、读第二遍，各看一次 `/proc/vmstat` 与 `/proc/zoneinfo`：

| 时刻 | `pgactivate` | `pgscan_kswapd` | `pgsteal_kswapd` | `active_file` | `inactive_file` |
|---|---|---|---|---|---|
| 开始 | **498** | 0 | 0 | 1 | 6 |
| 读第一遍（1MB） | **498**（不变） | 0 | 0 | 0 | **260** |
| 读第二遍（1MB） | **765**（+267） | 0 | 0 | **243** | 19 |
| 压 200MB 匿名内存 | 798 | **245** | **227** | **1** | 4 |

**第一遍读：只进 inactive。** `pgactivate` 一动不动，`inactive_file` 从 6 涨到 260
（1MB = 256 页），打点长这样：

```
[latte] mark_accessed: pfn=13301 ref=0 act=0     ← inactive + 未引用 → 只置引用位
[latte] mark_accessed: pfn=10724 ref=0 act=0
```

**第二遍读：同一批页被搬到 active。** `pgactivate` 498 → 765（+267 ≈ 256 页），
`active_file` 243，`lru_activate` 打点 248 次（第一遍只有 31 次噪声）。
同一个 pfn 的三连特别能说明问题：

```
[latte] mark_accessed: pfn=10608 ref=0 act=0     ← 第一次：inactive 未引用
[latte] mark_accessed: pfn=10608 ref=1 act=0     ← 第二次：已引用 → 该搬了
[latte] lru_activate: pfn=10608 nr=1             ← 搬进 active（PGACTIVATE +1）
[latte] mark_accessed: pfn=10608 ref=0 act=1     ← 第三次：已在 active，引用位被清
```

**压力阶段：连 active 页也会被回收。** 用 `/bin/memhog 200` 按住 200MB 匿名内存
（`-m 256M`），kswapd 扫了 245 页、真回收 227 页，而 `active_file` 从 243 掉到 **1**。
同一阶段 `do_anonymous_page` 打点 51336 次 ≈ 200MB/4KB = 51200 页，对得上。
`shrink_node` 347 次、`balance_pgdat` 27 次，说明回收确实跑了很多轮。

## 三个结论

1. **第一次访问很便宜，第二次访问才“升级”**：置引用位不动链表，所以读一遍的页
   仍然待在 inactive 头几个位置，随时可能被丢；读第二遍才搬 active。
   这就是“`dd` 读一遍和 `cat` 两遍”在缓存热度上的差别来源。
2. **“第二次机会”只保一轮**：active 页在压力下先被 `shrink_active_list()` 降级
   （清引用位、挪到 inactive 尾），下一轮扫描时若仍没被访问就被回收。
   实测 `active_file` 243 → 1 就是这一过程，不是“进了 active 就安全”。
3. **回收优先拿文件页开刀**：`pgsteal_kswapd` 227 页全部来自文件页
   （匿名页没有 swap 换不出去，`memory/05` 的“空转”结论在这里再次成立）。
   所以“读过的文件缓存被丢”比“进程的堆被换走”更早发生——除非你配了 swap。

## 复现

需要一块真实文件系统（rootfs 是 tmpfs，页不可回收）+ 一个吃内存的小程序：

```c
/* memhog.c: 按住 MB 兆匿名内存（每页写一次），睡 3 秒 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? strtoul(argv[1], 0, 10) : 256;
	size_t len = mb << 20;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	for (size_t i = 0; i < len; i += 4096)
		p[i] = 1;
	printf("[memhog] touched %zu MB\n", mb);
	sleep(3);
	return 0;
}
```

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/memhog /tmp/memhog.c
qemu-img create -f raw /tmp/disk.img 64M ; mkfs.ext4 -q -F /tmp/disk.img
make -j16 bzImage
rm -rf /tmp/mem7 && cp -a bb_rootfs /tmp/mem7
cp /tmp/memhog /tmp/mem7/bin/memhog
cat > /tmp/mem7/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
vm() { $B grep -E "^(pgactivate|pgscan_kswapd|pgsteal_kswapd|pgscan_direct|pgsteal_direct)" /proc/vmstat; }
zi() { $B awk '/nr_active_file/{af+=$2} /nr_inactive_file/{if_+=$2} /nr_active_anon/{aa+=$2} /nr_inactive_anon/{ia+=$2} END{printf "  active_file=%d inactive_file=%d active_anon=%d inactive_anon=%d\n", af, if_, aa, ia}' /proc/zoneinfo; }
$B mkdir -p /mnt
$B mount -t ext4 /dev/vda /mnt || echo "MOUNT FAILED"
$B dd if=/dev/zero of=/mnt/f.bin bs=1M count=1 2>/dev/null
$B sync
echo 3 > /proc/sys/vm/drop_caches
echo "=== before ==="; vm; zi
echo "=== read#1 (1MB) ==="
$B dd if=/mnt/f.bin of=/dev/null bs=1M 2>/dev/null
vm; zi
echo "=== read#2 (1MB) ==="
$B dd if=/mnt/f.bin of=/dev/null bs=1M 2>/dev/null
vm; zi
echo "=== pressure: 200MB anon on 256MB ==="
/bin/memhog 200
vm; zi
echo "=== mem7 done ==="
exec /bin/sh
EOF
chmod +x /tmp/mem7/init
( cd /tmp/mem7 && find . | cpio -o -H newc --quiet ) > /tmp/bb_mem7.cpio
timeout 150 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_mem7.cpio \
  -drive file=/tmp/disk.img,if=virtio,format=raw \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 256M -no-reboot > /tmp/latte-mem7.log 2>&1 || true

tr -d '\r' < /tmp/latte-mem7.log | grep -E "^(pgactivate|pgscan|pgsteal|  active_file|=== )"
tr -d '\r' < /tmp/latte-mem7.log | grep -E "\[latte\] (lru_activate|shrink_node)" | head -20
```

**别贪大**：文件改成 4MB（1024 页）时，`mark_accessed` 打点会有 13000 行、
串口日志 17MB、跑满 4 分钟——第一次做就踩了这个坑。1MB 足够看清三连转移。

## 代码位置

| 位置 | 函数 | 做什么 |
|---|---|---|
| `mm/swap.c:460` | `folio_mark_accessed()` | 访问落点：置引用位 or 搬 active（三条转移） |
| `mm/swap.c:304` | `lru_activate()` | inactive → active 的唯一搬运点，`PGACTIVATE` +1 |
| `mm/vmscan.c:2009` | `shrink_inactive_list()` | 扫 inactive，够冷就直接回收/换出 |
| `mm/vmscan.c:2130` | `shrink_active_list()` | 把 active 降级到 inactive 尾（清引用位） |
| `mm/vmscan.c:6079` | `shrink_node()` | 一轮回收的总调度 |
| `mm/filemap.c:1973` 等 | `filemap_read()` 路径 | 读文件时调 `folio_mark_accessed()` 的地方 |

## 一句话收尾

`memory/06` 说“写才给页”，这篇说“**再访问一次才给活路**”：
LRU 不是一个精确的“最久未使用”，而是一个两档的近似——
第一次访问做记号，第二次访问升档，压力下被降档再淘汰。
`pgactivate` 这个计数器，量的就是“发出去多少张复活券”。
