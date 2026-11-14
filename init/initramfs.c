// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/async.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/dirent.h>
#include <linux/syscalls.h>
#include <linux/utime.h>
#include <linux/file.h>
#include <linux/kstrtox.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/init_syscalls.h>
#include <linux/umh.h>
#include <linux/security.h>
#include <linux/overflow.h>

#include "do_mounts.h"
#include "initramfs_internal.h"

static __initdata bool csum_present;
static __initdata u32 io_csum;

static ssize_t __init xwrite(struct file *file, const unsigned char *p,
		size_t count, loff_t *pos)
{
	ssize_t out = 0;

	/* sys_write only can write MAX_RW_COUNT aka 2G-4K bytes at most */
	while (count) {
		ssize_t rv = kernel_write(file, p, count, pos);

		if (rv < 0) {
			if (rv == -EINTR || rv == -EAGAIN)
				continue;
			return out ? out : rv;
		} else if (rv == 0)
			break;

		if (csum_present) {
			ssize_t i;

			for (i = 0; i < rv; i++)
				io_csum += p[i];
		}

		p += rv;
		out += rv;
		count -= rv;
	}

	return out;
}

static __initdata char *message;
static void __init error(char *x)
{
	if (!message)
		message = x;
}

#define panic_show_mem(fmt, ...) \
	({ show_mem(); panic(fmt, ##__VA_ARGS__); })

/* link hash */

#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata struct hash {
	int ino, minor, major;
	umode_t mode;
	struct hash *next;
	char name[N_ALIGN(PATH_MAX)];
} *head[32];
static __initdata bool hardlink_seen;

static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

static char __init *find_link(int major, int minor, int ino,
			      umode_t mode, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		if (((*p)->mode ^ mode) & S_IFMT)
			continue;
		return (*p)->name;
	}
	q = kmalloc(sizeof(struct hash), GFP_KERNEL);
	if (!q)
		panic_show_mem("can't allocate link hash entry");
	q->major = major;
	q->minor = minor;
	q->ino = ino;
	q->mode = mode;
	strscpy(q->name, name);
	q->next = NULL;
	*p = q;
	hardlink_seen = true;
	return NULL;
}

static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; hardlink_seen && p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			kfree(q);
		}
	}
	hardlink_seen = false;
}

#ifdef CONFIG_INITRAMFS_PRESERVE_MTIME
static void __init do_utime(char *filename, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	init_utimes(filename, t);
}

static void __init do_utime_path(const struct path *path, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	vfs_utimes(path, t);
}

static __initdata LIST_HEAD(dir_list);
struct dir_entry {
	struct list_head list;
	time64_t mtime;
	char name[];
};

static void __init dir_add(const char *name, size_t nlen, time64_t mtime)
{
	struct dir_entry *de;

	de = kmalloc(struct_size(de, name, nlen), GFP_KERNEL);
	if (!de)
		panic_show_mem("can't allocate dir_entry buffer");
	INIT_LIST_HEAD(&de->list);
	strscpy(de->name, name, nlen);
	de->mtime = mtime;
	list_add(&de->list, &dir_list);
}

static void __init dir_utime(void)
{
	struct dir_entry *de, *tmp;
	list_for_each_entry_safe(de, tmp, &dir_list, list) {
		list_del(&de->list);
		do_utime(de->name, de->mtime);
		kfree(de);
	}
}
#else
static void __init do_utime(char *filename, time64_t mtime) {}
static void __init do_utime_path(const struct path *path, time64_t mtime) {}
static void __init dir_add(const char *name, size_t nlen, time64_t mtime) {}
static void __init dir_utime(void) {}
#endif

static __initdata time64_t mtime;

/* cpio header parsing */

static __initdata unsigned long ino, major, minor, nlink;
static __initdata umode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;
static __initdata u32 hdr_csum;

/*
 * 解析 newc 头：6 字节 magic 之后跟 13 个 8 字节十六进制字段。
 * 结果全写进全局变量，供 do_header()/do_name() 取用。
 */
static void __init parse_header(char *s)
{
	unsigned long parsed[13];
	int i;

	/* 跳过 6 字节 magic，每 8 字节读一个字段 */
	for (i = 0, s += 6; i < 13; i++, s += 8)
		parsed[i] = simple_strntoul(s, NULL, 16, 8);

	ino = parsed[0];
	mode = parsed[1];
	uid = parsed[2];
	gid = parsed[3];
	nlink = parsed[4];
	mtime = parsed[5]; /* breaks in y2106 */
	body_len = parsed[6];
	major = parsed[7];
	minor = parsed[8];
	rdev = new_encode_dev(MKDEV(parsed[9], parsed[10]));
	name_len = parsed[11];
	hdr_csum = parsed[12];
}

/* FSM */

/*
 * cpio 条目解析的状态机（FSM）：Start 找头、GotHeader 解析头、
 * Collect 攒够数据、SkipIt 跳到下一个头、GotName 建节点、
 * CopyFile 写正文、GotSymlink 建链接、Reset 吃填充。
 */
static __initdata enum state {
	Start,
	Collect,
	GotHeader,
	SkipIt,
	GotName,
	CopyFile,
	GotSymlink,
	Reset
} state, next_state;

static __initdata char *victim;
static unsigned long byte_count __initdata;
static __initdata loff_t this_header, next_header;

/* 数据够就就地取：游标前移，剩余量扣减 */
static inline void __init eat(unsigned n)
{
	victim += n;
	this_header += n;
	byte_count -= n;
}

static __initdata char *collected;
static long remains __initdata;
static __initdata char *collect;

/* 够 size 字节就直接切下一状态，不够就转 Collect 分多次攒 */
static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (byte_count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

static __initdata char *header_buf, *symlink_buf, *name_buf;

/* 条目起点：先收 110 字节的 cpio 头 */
static int __init do_start(void)
{
	read_into(header_buf, CPIO_HDRLEN, GotHeader);
	return 0;
}

/* 把分块到达的数据拼进 collect，攒满 remains 再切 next_state */
static int __init do_collect(void)
{
	unsigned long n = remains;
	if (byte_count < n)
		n = byte_count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

/*
 * 拿到 110 字节头后校验 magic、解析字段，算出下一条目的偏移；
 * 再按类型决定：符号链接整条收，文件/目录只先收名字，其余跳过。
 */
static int __init do_header(void)
{
	/* newc 两种格式：070701 无校验和，070702 带逐文件校验和 */
	if (!memcmp(collected, "070701", 6)) {
		csum_present = false;
	} else if (!memcmp(collected, "070702", 6)) {
		csum_present = true;
	} else {
		if (memcmp(collected, "070707", 6) == 0)
			error("incorrect cpio method used: use -H newc option");
		else
			error("no cpio magic");
		return 1;
	}
	parse_header(collected);
	/* 名字和正文都按 4 字节对齐，先算下一条目的绝对位置 */
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	/* 默认跳过正文，等看清条目类型再决定要不要收 */
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	/* 符号链接：名字与目标路径连成一段，整条收下来 */
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	/* 普通文件与空正文条目（目录/设备）：只先把名字收下来 */
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

/* 跳过本条剩余正文与对齐填充，直到下一条目的头 */
static int __init do_skip(void)
{
	if (this_header + byte_count < next_header) {
		eat(byte_count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

/* 吃掉条目之间的 '\0' 填充，顺带检查 4 字节对齐 */
static int __init do_reset(void)
{
	while (byte_count && *victim == '\0')
		eat(1);
	if (byte_count && (this_header & 3))
		error("broken padding");
	return 1;
}

/* 同名旧节点类型不同时先删掉，否则 mkdir/mknod 建不出来 */
static void __init clean_path(char *path, umode_t fmode)
{
	struct kstat st;

	if (!init_stat(path, &st, AT_SYMLINK_NOFOLLOW) &&
	    (st.mode ^ fmode) & S_IFMT) {
		if (S_ISDIR(st.mode))
			init_rmdir(path);
		else
			init_unlink(path);
	}
}

/* nlink >= 2 才可能是硬链接；建成功返回 1，失败返回 -1 */
static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, mode, collected);
		if (old) {
			clean_path(collected, 0);
			return (init_link(old, collected) < 0) ? -1 : 1;
		}
	}
	return 0;
}

static __initdata struct file *wfile;
static __initdata loff_t wfile_pos;

/*
 * 拿到条目名字后按类型落地：目录 mkdir、普通文件打开待写、
 * 设备与管道 mknod、TRAILER!!! 表示归档到此结束。
 */
static int __init do_name(void)
{
	state = SkipIt;
	next_state = Reset;

	/* 名字必须以 '\0' 结尾，否则整个归档是坏的 */
	/* name_len > 0 && name_len <= PATH_MAX checked in do_header */
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs name without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}

	/* TRAILER!!! 就是归档结束标记，硬链接表也该清了 */
	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		return 0;
	}
	printk(KERN_INFO "[latte] cpio node: %s\n", collected);
	/* 同名但类型冲突的旧节点先删掉，否则等下建不出来 */
	clean_path(collected, mode);
	if (S_ISREG(mode)) {
		int ml = maybe_link();
		if (ml >= 0) {
			int openflags = O_WRONLY|O_CREAT|O_LARGEFILE;
			if (ml != 1)
				openflags |= O_TRUNC;
			/* 硬链接命中时不能截断，否则已有正文会被清空 */
			wfile = filp_open(collected, openflags, mode);
			if (IS_ERR(wfile))
				return 0;
			wfile_pos = 0;
			io_csum = 0;

			vfs_fchown(wfile, uid, gid);
			vfs_fchmod(wfile, mode);
			if (body_len)
				vfs_truncate(&wfile->f_path, body_len);
			/* 切去写正文，正文在后续缓冲区里陆续到达 */
			state = CopyFile;
		}
	} else if (S_ISDIR(mode)) {
		/* 目录建出来后再补 owner、权限和 mtime */
		init_mkdir(collected, mode);
		init_chown(collected, uid, gid, 0);
		init_chmod(collected, mode);
		dir_add(collected, name_len, mtime);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			/* 设备节点靠 rdev（主次设备号）才能建对 */
			init_mknod(collected, mode, rdev);
			init_chown(collected, uid, gid, 0);
			init_chmod(collected, mode);
			do_utime(collected, mtime);
		}
	}
	return 0;
}

/* 把正文分块写进 do_name() 打开的文件，写完补 mtime 并查校验和 */
static int __init do_copy(void)
{
	/* 这块缓冲里已经含了全部正文，写完就能收尾 */
	if (byte_count >= body_len) {
		if (xwrite(wfile, victim, body_len, &wfile_pos) != body_len)
			error("write error");

		do_utime_path(&wfile->f_path, mtime);
		fput(wfile);
		/* 只有 070702 格式才带逐文件校验和 */
		if (csum_present && io_csum != hdr_csum)
			error("bad data checksum");
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		/* 正文还没到齐：写多少算多少，等下一块缓冲 */
		if (xwrite(wfile, victim, byte_count, &wfile_pos) != byte_count)
			error("write error");
		body_len -= byte_count;
		eat(byte_count);
		return 1;
	}
}

/* 建符号链接：名字与目标路径收在同一块缓冲里 */
static int __init do_symlink(void)
{
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs symlink without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}
	/* 名字与目标之间隔着 4 字节对齐填充，末尾补 '\0' */
	collected[N_ALIGN(name_len) + body_len] = '\0';
	printk(KERN_INFO "[latte] cpio symlink: %s\n", collected);
	clean_path(collected, 0);
	/* 目标路径的起点 = 名字按 4 字节对齐之后的地址 */
	init_symlink(collected + N_ALIGN(name_len), collected);
	init_chown(collected, uid, gid, AT_SYMLINK_NOFOLLOW);
	do_utime(collected, mtime);
	state = SkipIt;
	next_state = Reset;
	return 0;
}

/*
 * 8 个状态到处理函数的映射：write_buffer() 反复调 actions[state]，
 * 状态在 do_header()/do_name() 里切换，直到当前缓冲吃完。
 */
static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

/* FSM 主循环：把当前缓冲区喂给 actions[state]，直到吃完这段数据 */
static long __init write_buffer(char *buf, unsigned long len)
{
	/* byte_count 与 victim 就是 FSM 的剩余量和当前游标 */
	byte_count = len;
	victim = buf;

	/* 返回 0 表示换状态继续，返回 1 表示这块数据吃完了 */
	while (!actions[state]())
		;
	return len - byte_count;
}

static long __init flush_buffer(void *bufv, unsigned long len)
{
	char *buf = bufv;
	long written;
	long origLen = len;
	if (message)
		return -1;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else if (c == 0) {
			buf += written;
			len -= written;
			state = Reset;
		} else
			error("junk within compressed archive");
	}
	return origLen;
}

static unsigned long my_inptr __initdata; /* index of next byte to be processed in inbuf */

#include <linux/decompress/generic.h>

/**
 * unpack_to_rootfs - decompress and extract an initramfs archive
 * @buf: input initramfs archive to extract
 * @len: length of initramfs data to process
 *
 * Returns: NULL for success or an error message string
 *
 * This symbol shouldn't be used externally. It's available for unit tests.
 */
char * __init unpack_to_rootfs(char *buf, unsigned long len)
{
	/*
	 * 把一个归档（可能被 gzip/xz/zstd 压过）铺进 rootfs：
	 * 先探压缩格式，解出的字节流喂给 FSM；成功返回 NULL。
	 */
	long written;
	decompress_fn decompress;
	const char *compress_name;
	/* 头、符号链接、文件名三块缓冲一次分配，省得反复申请 */
	struct {
		char header[CPIO_HDRLEN];
		char symlink[PATH_MAX + N_ALIGN(PATH_MAX) + 1];
		char name[N_ALIGN(PATH_MAX)];
	} *bufs = kmalloc(sizeof(*bufs), GFP_KERNEL);

	if (!bufs)
		panic_show_mem("can't allocate buffers");

	printk(KERN_INFO "[latte] unpack_to_rootfs: %lu bytes\n", len);
	header_buf = bufs->header;
	symlink_buf = bufs->symlink;
	name_buf = bufs->name;

	/* FSM 归零，从归档的第一个字节重新开始解析 */
	state = Start;
	this_header = 0;
	message = NULL;
	while (!message && len) {
		loff_t saved_offset = this_header;
		/* 未压缩 cpio 的 magic 以 '0' 开头，且 4 字节对齐 */
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		/* 按 magic 认出压缩格式，认不出的直接报错 */
		decompress = decompress_method(buf, len, &compress_name);
		pr_debug("Detected %s compressed data\n", compress_name);
		if (decompress) {
			/* 解出来的字节流直接交给 flush_buffer() 喂 FSM */
			int res = decompress(buf, len, NULL, flush_buffer, NULL,
				   &my_inptr, error);
			if (res)
				error("decompressor failed");
		} else if (compress_name) {
			pr_err("compression method %s not configured\n",
			       compress_name);
			error("decompressor failed");
		} else
			error("invalid magic at start of compressed archive");
		/* 归档末尾必须是 TRAILER!!! 加 '\0' 填充 */
		if (state != Reset)
			error("junk at the end of compressed archive");
		this_header = saved_offset + my_inptr;
		buf += my_inptr;
		len -= my_inptr;
	}
	/* 目录 mtime 要等子项全部建完才能补 */
	dir_utime();
	/* free any hardlink state collected without optional TRAILER!!! */
	free_hash();
	kfree(bufs);
	printk(KERN_INFO "[latte] unpack_to_rootfs: done, message=%s\n",
	       message ? message : "(none)");
	return message;
}

static int __initdata do_retain_initrd;

static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

#ifdef CONFIG_ARCH_HAS_KEEPINITRD
static int __init keepinitrd_setup(char *__unused)
{
	do_retain_initrd = 1;
	return 1;
}
__setup("keepinitrd", keepinitrd_setup);
#endif

static bool __initdata initramfs_async = true;
static int __init initramfs_async_setup(char *str)
{
	return kstrtobool(str, &initramfs_async) == 0;
}
__setup("initramfs_async=", initramfs_async_setup);

extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

static BIN_ATTR(initrd, 0440, sysfs_bin_attr_simple_read, NULL, 0);

void __init reserve_initrd_mem(void)
{
	phys_addr_t start;
	unsigned long size;

	/* Ignore the virtul address computed during device tree parsing */
	initrd_start = initrd_end = 0;

	if (!phys_initrd_size)
		return;
	/*
	 * Round the memory region to page boundaries as per free_initrd_mem()
	 * This allows us to detect whether the pages overlapping the initrd
	 * are in use, but more importantly, reserves the entire set of pages
	 * as we don't want these pages allocated for other purposes.
	 */
	start = round_down(phys_initrd_start, PAGE_SIZE);
	size = phys_initrd_size + (phys_initrd_start - start);
	size = round_up(size, PAGE_SIZE);

	if (!memblock_is_region_memory(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx is not a memory region",
		       (u64)start, size);
		goto disable;
	}

	if (memblock_is_region_reserved(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx overlaps in-use memory region\n",
		       (u64)start, size);
		goto disable;
	}

	memblock_reserve(start, size);
	/* Now convert initrd to virtual addresses */
	initrd_start = (unsigned long)__va(phys_initrd_start);
	initrd_end = initrd_start + phys_initrd_size;
	initrd_below_start_ok = 1;

	return;
disable:
	pr_cont(" - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}

/*
 * weak 默认实现：先从 memblock 记账里摘掉这块保留区（reserved
 * region），再交给 free_reserved_area() 还给 buddy。
 */
void __weak __init free_initrd_mem(unsigned long start, unsigned long end)
{
#ifdef CONFIG_ARCH_KEEP_MEMBLOCK
	unsigned long aligned_start = ALIGN_DOWN(start, PAGE_SIZE);
	unsigned long aligned_end = ALIGN(end, PAGE_SIZE);

	/* memblock 记账里也要摘掉，否则统计会重复 */
	memblock_free((void *)aligned_start, aligned_end - aligned_start);
#endif

	/* 保留区最终由 free_reserved_area() 还给 buddy 并打日志 */
	printk(KERN_INFO "[latte] free_initrd_mem -> free_reserved_area\n");
	free_reserved_area((void *)start, (void *)end, POISON_FREE_INITMEM,
			"initrd");
}

#ifdef CONFIG_CRASH_RESERVE
static bool __init kexec_free_initrd(void)
{
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start >= crashk_end || initrd_end <= crashk_start)
		return false;

	/*
	 * Initialize initrd memory region since the kexec boot does not do.
	 */
	memset((void *)initrd_start, 0, initrd_end - initrd_start);
	if (initrd_start < crashk_start)
		free_initrd_mem(initrd_start, crashk_start);
	if (initrd_end > crashk_end)
		free_initrd_mem(crashk_end, initrd_end);
	return true;
}
#else
static inline bool kexec_free_initrd(void)
{
	return false;
}
#endif /* CONFIG_KEXEC_CORE */

#ifdef CONFIG_BLK_DEV_RAM
static void __init populate_initrd_image(char *err)
{
	ssize_t written;
	struct file *file;
	loff_t pos = 0;

	printk(KERN_INFO "rootfs image is not initramfs (%s); looks like an initrd\n",
			err);
	file = filp_open("/initrd.image", O_WRONLY|O_CREAT|O_LARGEFILE, 0700);
	if (IS_ERR(file))
		return;

	written = xwrite(file, (char *)initrd_start, initrd_end - initrd_start,
			&pos);
	if (written != initrd_end - initrd_start)
		pr_err("/initrd.image: incomplete write (%zd != %ld)\n",
		       written, initrd_end - initrd_start);
	fput(file);
}
#endif /* CONFIG_BLK_DEV_RAM */

/*
 * 异步执行体：先解内建 cpio（CONFIG_INITRAMFS_SOURCE），再解
 * bootloader 传来的外部 initrd，最后把 initrd 内存还给 buddy。
 */
static void __init do_populate_rootfs(void *unused, async_cookie_t cookie)
{
	/* Load the built in initramfs */
	printk(KERN_INFO "[latte] do_populate_rootfs: builtin cpio\n");
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	/* 内建归档解不开就没法开机，只能 panic */
	if (err)
		panic_show_mem("%s", err); /* Failed to decompress INTERNAL initramfs */

	/* 没有外部 initrd（或强制只用内建）就直接收尾 */
	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
		goto done;

	if (IS_ENABLED(CONFIG_BLK_DEV_RAM))
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
	else
		/* 日志里那句 Unpacking initramfs... 就出自这里 */
		printk(KERN_INFO "Unpacking initramfs...\n");

	printk(KERN_INFO "[latte] do_populate_rootfs: external initrd %lu bytes\n",
	       initrd_end - initrd_start);
	err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start);
	/* 外部归档坏了只报错不 panic，可能还能换别的 root */
	if (err) {
#ifdef CONFIG_BLK_DEV_RAM
		populate_initrd_image(err);
#else
		printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
#endif
	}

done:
	/* 给 LSM 一次检查已铺好的 rootfs 的机会 */
	security_initramfs_populated();

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	/* 默认把 initrd 占的内存还回去，retain/kexec 场景除外 */
	if (!do_retain_initrd && initrd_start && !kexec_free_initrd()) {
		free_initrd_mem(initrd_start, initrd_end);
	} else if (do_retain_initrd && initrd_start) {
		bin_attr_initrd.size = initrd_end - initrd_start;
		bin_attr_initrd.private = (void *)initrd_start;
		if (sysfs_create_bin_file(firmware_kobj, &bin_attr_initrd))
			pr_err("Failed to create initrd sysfs file");
	}
	/* 清掉指针，防止解包完成后还有人误用这块内存 */
	initrd_start = 0;
	initrd_end = 0;

	/* 把 init_*() 期间攒下的 fput 一起放掉 */
	init_flush_fput();
}

static ASYNC_DOMAIN_EXCLUSIVE(initramfs_domain);
static async_cookie_t initramfs_cookie;

/*
 * 给消费者用的同步点：kernel_init_freeable() 靠它等解包完成，
 * 否则到那时 /init 还不存在。
 */
void wait_for_initramfs(void)
{
	/* 解包都还没登记就来等，说明调用顺序有 bug */
	if (!initramfs_cookie) {
		/*
		 * Something before rootfs_initcall wants to access
		 * the filesystem/initramfs. Probably a bug. Make a
		 * note, avoid deadlocking the machine, and let the
		 * caller's access fail as it used to.
		 */
		pr_warn_once("wait_for_initramfs() called before rootfs_initcalls\n");
		return;
	}
	/* 等这次异步解包连同之后排队的任务全部跑完 */
	printk(KERN_INFO "[latte] wait_for_initramfs -> async_synchronize\n");
	async_synchronize_cookie_domain(initramfs_cookie + 1, &initramfs_domain);
}
EXPORT_SYMBOL_GPL(wait_for_initramfs);

/*
 * rootfs_initcall 段里的登记点：do_initcalls() 调到它时只把解包
 * 任务扔进异步域就返回，不等解包做完。
 */
static int __init populate_rootfs(void)
{
	/* 解包扔给异步域，initcall 不必等它跑完 */
	printk(KERN_INFO "[latte] populate_rootfs: schedule async\n");
	initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
						 &initramfs_domain);
	/* 允许执行用户态程序，等下要用它跑 /init */
	usermodehelper_enable();
	/* 关掉异步就在这儿直接等，效果与同步解包一致 */
	if (!initramfs_async)
		wait_for_initramfs();
	return 0;
}
rootfs_initcall(populate_rootfs);
