# 主题六 · slave 与命名空间：只收不发的那一半

`mount/05` 实测了 shared：同组里挂一次，两份都会出现。这篇补它的兄弟 **slave**：
**上游挂的东西能下来，我挂的东西上不去**。要造出“上游”，必须先有自己的 mount namespace。

## 你已知的

- 挂载传播靠 peer group（`mount/05`）：`mnt_group_id` + `mnt_share`，`propagate_mnt()` 挨个复制；
- 每个 mount namespace 有自己的一棵挂载树（`mount/02`）；
- `mount -o rslave` 就是 `MS_SLAVE|MS_REC`。

## 内核里对应什么：master 链

`unshare(CLONE_NEWNS)` 时，内核把当前挂载树**整棵复制**一份：

```
unshare(CLONE_NEWNS)
 └─ copy_mnt_ns()                  fs/namespace.c:4117
     为每个 mount 造一个副本（新的 struct mount，指向同一 superblock）
```

复制完成后，两边是**两个独立**的 mount，默认仍各自 shared（互相是 peer）。
要建立“上下游”，得显式改类型：

```
mount(NULL, "/up", NULL, MS_SLAVE|MS_REC, NULL)
 └─ do_change_type()               fs/namespace.c:2858
     ├─ 把这个 mount 从 peer group 里摘掉
     └─ 记下 mnt_master = 上游那一份   fs/pnode.c:114
```

之后：

- **上游挂载 → 下行**：上游 `propagate_mnt()` 顺着 `mnt_master` 链把新挂载复制到
  slave 一侧（`fs/pnode.c:95`）；
- **下游挂载 → 不上行**：slave 自己的挂载没有 master 可以往上传，只在本命名空间生效。

一个 mount 只能是四种类型之一：`private`（默认，组号 0）、`shared`（双向）、
`slave`（单向收）、`unbindable`（连 bind 都不许）。

## 打点（沿用 `mount/05` 的两个）

| 打点 | 位置 | 这次看什么 |
|---|---|---|
| `attach_mnt: src_group=… moving=…` | `fs/namespace.c` | 挂载落在哪个组 |
| `propagate_mnt: dest_group=… peers=…` | `fs/pnode.c` | 上行传播发生了几次 |

## 实测：父挂 `/up/sub`，子挂 `/up/childmnt`

准备：`tmpfs` 挂 `/up`，建好 `/up/sub` 与 `/up/childmnt`，把 `/up` 设为 shared。
然后 `nsprobe`：子进程 `unshare(CLONE_NEWNS)` + 把 `/up` 设为 **slave**，
父进程在 `/up/sub` 挂 tmpfs，子进程在 `/up/childmnt` 挂 tmpfs。

```
--- child (slave) 看到的 mounts ---
  none /up tmpfs rw,relatime 0 0
  none /up/sub tmpfs rw,relatime 0 0          ← 上游挂的，收到了
  none /up/childmnt tmpfs rw,relatime 0 0     ← 自己挂的
--- parent 看到的 mounts ---
  none /up tmpfs rw,relatime 0 0
  none /up/sub tmpfs rw,relatime 0 0          ← 自己挂的
                                              ← 没有 /up/childmnt
```

父命名空间的 mountinfo：

```
24 2 0:21 / /up     rw,relatime shared:1 - tmpfs none rw
30 24 0:22 / /up/sub rw,relatime shared:2 - tmpfs none rw
```

打点：

```
[latte] attach_mnt: src_group=0 moving=0
[latte] propagate_mnt: dest_group=1 peers=0     ← 唯一一次传播
```

三个证据方向一致：

1. **下行成立**：子进程的 `/up/sub` 不是它挂的，是父进程挂载后被传播过去的；
2. **上行不成立**：父进程完全看不到 `/up/childmnt`；
3. **`peers=0` 恰好说明了机制**：`propagate_mnt()` 里我数的是 `dest_mnt->mnt_share`
   （对等组）。slave 不是 peer，它挂在 `mnt_master` 链上，所以 share 列表是空的——
   传播仍然发生了，只是走的另一条链。

## 三个结论

1. **`unshare(CLONE_NEWNS)` 复制的是整棵树，不是“空的树”**：新命名空间一开始
   看到的是同一批 superblock 的副本，`/up` 在两边都存在、都指向同一个 tmpfs。
   隔离的是“挂载关系”，不是“数据”。
2. **slave 是单向阀门**：上游挂载顺着 `mnt_master` 下来（子看到 `/up/sub`），
   下游挂载没有反向通道（父看不到 `/up/childmnt`）。
   这就是容器把根挂载设成 `slave`（或 `private`）的原因：
   宿主机的挂载变化可以进容器，容器的挂载折腾不会污染宿主机。
3. **`mnt_share` 与 `mnt_master` 是两套关系**：shared 用前者（对等），slave 用后者（上游）。
   `propagate_mnt()` 两套都遍历，所以本次 `peers=0`（没有对等成员）
   却仍然发生了一次传播——这正是“只看 peers 会误判”的例子，
   判断成员要看 `/proc/self/mountinfo` 的 `shared:`/`master:` 标记。

## 复现

```c
/* nsprobe.c: 子命名空间把 /up 变 slave，验证单向传播 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>

static void show(const char *tag)
{
	FILE *f = fopen("/proc/self/mounts", "r");
	char line[512];

	printf("--- %s ---\n", tag);
	if (!f) { perror("mounts"); return; }
	while (fgets(line, sizeof(line), f))
		if (strstr(line, " /up"))
			printf("  %s", line);
	fclose(f);
}

int main(void)
{
	int p2c[2], c2p[2];
	pid_t pid;
	char c;

	if (pipe(p2c) || pipe(c2p)) { perror("pipe"); return 1; }

	pid = fork();
	if (pid == 0) {
		if (unshare(CLONE_NEWNS) < 0) { perror("unshare"); _exit(1); }
		if (mount(NULL, "/up", NULL, MS_SLAVE | MS_REC, NULL) < 0) {
			perror("make slave");
			_exit(1);
		}
		write(c2p[1], "r", 1);          /* 已就绪 */
		if (read(p2c[0], &c, 1) != 1)
			_exit(1);
		if (mount("none", "/up/childmnt", "tmpfs", 0, NULL) < 0)
			perror("child mount");
		show("child (slave) 看到的 mounts");
		_exit(0);
	}

	if (read(c2p[0], &c, 1) != 1)
		return 1;
	if (mount("none", "/up/sub", "tmpfs", 0, NULL) < 0)
		perror("parent mount /up/sub");
	write(p2c[1], "g", 1);
	waitpid(pid, 0, 0);
	show("parent 看到的 mounts");
	return 0;
}
```

`mntprobe`（挂 tmpfs / 设 shared）的源码见 `mount/05`。

```bash
cd /home/dong/Documents/github/linux
gcc -static -O0 -o /tmp/nsprobe /tmp/nsprobe.c
gcc -static -O0 -o /tmp/mntprobe /tmp/mntprobe.c
make -j16 bzImage
rm -rf /tmp/mount6 && cp -a bb_rootfs /tmp/mount6
cp /tmp/nsprobe /tmp/mntprobe /tmp/mount6/bin/
cat > /tmp/mount6/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B mkdir -p /up
/bin/mntprobe tmpfs /up
$B mkdir -p /up/sub /up/childmnt
/bin/mntprobe shared /up
/bin/nsprobe
$B grep -E " /up(/| )" /proc/self/mountinfo
$B dmesg | $B grep -E "attach_mnt|propagate_mnt" | $B tail -8
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/mount6/init
( cd /tmp/mount6 && find . | cpio -o -H newc --quiet ) > /tmp/bb_mount6.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_mount6.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 log_buf_len=2M panic=1" \
  -nographic -m 512M -no-reboot > /tmp/latte-mount6.log 2>&1 || true
tr -d '\r' < /tmp/latte-mount6.log | sed -n '/tmpfs on \/up/,/done/p'
```

两个顺序约束：

- **`/up/sub` 与 `/up/childmnt` 必须在 `shared` 之前 `mkdir`**：目录建在 tmpfs 里，
  设成 shared 之后再建也来得及，但 fork 之后子进程就不能改父的树了。
- **父进程要等子进程“就绪”再挂 `/up/sub`**：如果父先挂、子后 `unshare`，
  那次挂载会被整棵树一起复制过去，就分不清是“传播”还是“复制”了。管道同步就是为这个。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `fs/namespace.c:4117` | `copy_mnt_ns()` | `unshare(CLONE_NEWNS)` 时复制整棵挂载树 |
| `fs/namespace.c:2858` | `do_change_type()` | 处理 `MS_SHARED/PRIVATE/SLAVE/UNBINDABLE` |
| `fs/pnode.c:93` | `change_mnt_propagation()` | 真正改类型：摘 peer 组 / 记 `mnt_master` |
| `fs/pnode.c:114` | 同函数内 | `mnt->mnt_master = m`，并挂进上游的 `mnt_slave_list` |
| `fs/pnode.c:312` | `propagate_mnt()` | 两套关系都遍历：先 peer（`mnt_share`），再 slave（`mnt_master`） |
| `fs/mount.h` | `mnt_master` / `mnt_share` | 上游链 vs 对等组，两个不同的链表 |

## 一句话收尾

`shared` 是“**广场**”：谁挂东西大家都看见；`slave` 是“**单向喇叭**”：
上游的通知能收到，自己的动静不外传。容器把根挂载设成 slave/private，
就是为了既能跟随宿主机的变化，又不把容器里的挂载泄漏出去。
