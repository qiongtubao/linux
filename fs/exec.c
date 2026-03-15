// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/exec.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Linux程序执行子系统 - execve实现
 *
 * 本文件实现了execve系统调用的核心逻辑：
 * - 程序参数和环境变量的处理
 * - 二进制格式识别和加载（ELF、脚本等）
 * - 进程地址空间的重建
 * - 执行权限检查
 * - 信号处理器的重置
 */

/*
 * #!-checking implemented by tytso.
 */
/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 *
 * Demand loading changed July 1993 by Eric Youngdale.   Use mmap instead,
 * current->executable is only used by the procfs.  This allows a dispatch
 * table to check for several different types  of binary formats.  We keep
 * trying until we recognize the file or we run out of supported binary
 * formats.
 */

#include <linux/kernel_read_file.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/vmacache.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/swap.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/sched/signal.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/task.h>
#include <linux/pagemap.h>
#include <linux/perf_event.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include <linux/key.h>
#include <linux/personality.h>
#include <linux/binfmts.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/audit.h>
#include <linux/tracehook.h>
#include <linux/kmod.h>
#include <linux/fsnotify.h>
#include <linux/fs_struct.h>
#include <linux/oom.h>
#include <linux/compat.h>
#include <linux/vmalloc.h>
#include <linux/io_uring.h>

#include <linux/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/tlb.h>

#include <trace/events/task.h>
#include "internal.h"

#include <trace/events/sched.h>

static int bprm_creds_from_file(struct linux_binprm *bprm);

int suid_dumpable = 0;

static LIST_HEAD(formats);
static DEFINE_RWLOCK(binfmt_lock);

/**
 * __register_binfmt - 注册二进制格式处理器
 * @fmt: 要注册的二进制格式结构
 * @insert: 是否插入到链表头部（1）还是尾部（0）
 *
 * 将二进制格式处理器添加到全局formats链表中。
 * insert为1时插入到链表头部（优先级高），为0时插入到尾部。
 */
void __register_binfmt(struct linux_binfmt * fmt, int insert)
{
	BUG_ON(!fmt);
	if (WARN_ON(!fmt->load_binary))
		return;
	write_lock(&binfmt_lock);
	insert ? list_add(&fmt->lh, &formats) :
		 list_add_tail(&fmt->lh, &formats);
	write_unlock(&binfmt_lock);
}

EXPORT_SYMBOL(__register_binfmt);

/**
 * unregister_binfmt - 注销二进制格式处理器
 * @fmt: 要注销的二进制格式结构
 *
 * 从全局formats链表中移除指定的二进制格式处理器。
 */
void unregister_binfmt(struct linux_binfmt * fmt)
{
	write_lock(&binfmt_lock);
	list_del(&fmt->lh);
	write_unlock(&binfmt_lock);
}

EXPORT_SYMBOL(unregister_binfmt);

static inline void put_binfmt(struct linux_binfmt * fmt)
{
	module_put(fmt->module);
}

/**
 * path_noexec - 检查路径是否禁止执行
 * @path: 要检查的路径
 *
 * 检查指定路径是否设置了noexec标志，禁止执行程序。
 * 返回值: true表示禁止执行，false表示允许执行
 */
bool path_noexec(const struct path *path)
{
	return (path->mnt->mnt_flags & MNT_NOEXEC) ||
	       (path->mnt->mnt_sb->s_iflags & SB_I_NOEXEC);
}

#ifdef CONFIG_USELIB
/*
 * Note that a shared library must be both readable and executable due to
 * security reasons.
 *
 * Also note that we take the address to load from from the file itself.
 */
/**
 * SYSCALL_DEFINE1(uselib) - uselib系统调用实现
 * @library: 要加载的库文件路径
 *
 * 加载动态链接库的系统调用（已弃用）。
 * 现在主要用于兼容性，返回-ENOSYS表示不支持。
 * 返回值: 成功返回0，失败返回负的错误码
 */
SYSCALL_DEFINE1(uselib, const char __user *, library)
{
	struct linux_binfmt *fmt;
	struct file *file;
	struct filename *tmp = getname(library);
	int error = PTR_ERR(tmp);
	static const struct open_flags uselib_flags = {
		.open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
		.acc_mode = MAY_READ | MAY_EXEC,
		.intent = LOOKUP_OPEN,
		.lookup_flags = LOOKUP_FOLLOW,
	};

	if (IS_ERR(tmp))
		goto out;

	file = do_filp_open(AT_FDCWD, tmp, &uselib_flags);
	putname(tmp);
	error = PTR_ERR(file);
	if (IS_ERR(file))
		goto out;

	/*
	 * may_open() has already checked for this, so it should be
	 * impossible to trip now. But we need to be extra cautious
	 * and check again at the very end too.
	 */
	error = -EACCES;
	if (WARN_ON_ONCE(!S_ISREG(file_inode(file)->i_mode) ||
			 path_noexec(&file->f_path)))
		goto exit;

	fsnotify_open(file);

	error = -ENOEXEC;

	read_lock(&binfmt_lock);
	list_for_each_entry(fmt, &formats, lh) {
		if (!fmt->load_shlib)
			continue;
		if (!try_module_get(fmt->module))
			continue;
		read_unlock(&binfmt_lock);
		error = fmt->load_shlib(file);
		read_lock(&binfmt_lock);
		put_binfmt(fmt);
		if (error != -ENOEXEC)
			break;
	}
	read_unlock(&binfmt_lock);
exit:
	fput(file);
out:
  	return error;
}
#endif /* #ifdef CONFIG_USELIB */

#ifdef CONFIG_MMU
/*
 * The nascent bprm->mm is not visible until exec_mmap() but it can
 * use a lot of memory, account these pages in current->mm temporary
 * for oom_badness()->get_mm_rss(). Once exec succeeds or fails, we
 * change the counter back via acct_arg_size(0).
 */
/**
 * acct_arg_size - 统计参数页面大小用于内存审计
 * @bprm: 二进制程序结构
 * @pages: 使用的页面数量
 *
 * 更新当前进程的参数页面使用量统计，用于内存审计。
 */
static void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
	struct mm_struct *mm = current->mm;
	long diff = (long)(pages - bprm->vma_pages);

	if (!mm || !diff)
		return;

	bprm->vma_pages = pages;
	add_mm_counter(mm, MM_ANONPAGES, diff);
}

static struct page *get_arg_page(struct linux_binprm *bprm, unsigned long pos,
		int write)
{
	struct page *page;
	int ret;
	unsigned int gup_flags = FOLL_FORCE;

#ifdef CONFIG_STACK_GROWSUP
	if (write) {
		ret = expand_downwards(bprm->vma, pos);
		if (ret < 0)
			return NULL;
	}
#endif

	if (write)
		gup_flags |= FOLL_WRITE;

	/*
	 * We are doing an exec().  'current' is the process
	 * doing the exec and bprm->mm is the new process's mm.
	 */
	ret = get_user_pages_remote(bprm->mm, pos, 1, gup_flags,
			&page, NULL, NULL);
	if (ret <= 0)
		return NULL;

	if (write)
		acct_arg_size(bprm, vma_pages(bprm->vma));

	return page;
}

/**
 * put_arg_page - 释放参数页面引用
 * @page: 要释放的页面
 *
 * 减少参数页面的引用计数，当引用计数为0时释放页面。
 */
static void put_arg_page(struct page *page)
{
	put_page(page);
}

static void free_arg_pages(struct linux_binprm *bprm)
{
}

static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos,
		struct page *page)
{
	flush_cache_page(bprm->vma, pos, page_to_pfn(page));
}

static int __bprm_mm_init(struct linux_binprm *bprm)
{
	int err;
	struct vm_area_struct *vma = NULL;
	struct mm_struct *mm = bprm->mm;

	bprm->vma = vma = vm_area_alloc(mm);
	if (!vma)
		return -ENOMEM;
	vma_set_anonymous(vma);

	if (mmap_write_lock_killable(mm)) {
		err = -EINTR;
		goto err_free;
	}

	/*
	 * Place the stack at the largest stack address the architecture
	 * supports. Later, we'll move this to an appropriate place. We don't
	 * use STACK_TOP because that can depend on attributes which aren't
	 * configured yet.
	 */
	BUILD_BUG_ON(VM_STACK_FLAGS & VM_STACK_INCOMPLETE_SETUP);
	vma->vm_end = STACK_TOP_MAX;
	vma->vm_start = vma->vm_end - PAGE_SIZE;
	vma->vm_flags = VM_SOFTDIRTY | VM_STACK_FLAGS | VM_STACK_INCOMPLETE_SETUP;
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	err = insert_vm_struct(mm, vma);
	if (err)
		goto err;

	mm->stack_vm = mm->total_vm = 1;
	mmap_write_unlock(mm);
	bprm->p = vma->vm_end - sizeof(void *);
	return 0;
err:
	mmap_write_unlock(mm);
err_free:
	bprm->vma = NULL;
	vm_area_free(vma);
	return err;
}

static bool valid_arg_len(struct linux_binprm *bprm, long len)
{
	return len <= MAX_ARG_STRLEN;
}

#else

static inline void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
}

static struct page *get_arg_page(struct linux_binprm *bprm, unsigned long pos,
		int write)
{
	struct page *page;

	page = bprm->page[pos / PAGE_SIZE];
	if (!page && write) {
		page = alloc_page(GFP_HIGHUSER|__GFP_ZERO);
		if (!page)
			return NULL;
		bprm->page[pos / PAGE_SIZE] = page;
	}

	return page;
}

/**
 * put_arg_page - 释放参数页面引用（非MMU版本）
 * @page: 要释放的页面
 *
 * 在非MMU系统中，这是一个空操作函数。
 */
static void put_arg_page(struct page *page)
{
}

/**
 * free_arg_page - 释放单个参数页面
 * @bprm: 二进制程序结构
 * @i: 页面索引
 *
 * 释放指定索引位置的参数页面。
 */
static void free_arg_page(struct linux_binprm *bprm, int i)
{
	if (bprm->page[i]) {
		__free_page(bprm->page[i]);
		bprm->page[i] = NULL;
	}
}

/**
 * free_arg_pages - 释放所有参数页面
 * @bprm: 二进制程序结构
 *
 * 释放存储命令行参数和环境变量的所有页面。
 */
static void free_arg_pages(struct linux_binprm *bprm)
{
	int i;

	for (i = 0; i < MAX_ARG_PAGES; i++)
		free_arg_page(bprm, i);
}

/**
 * flush_arg_page - 刷新参数页面（非MMU版本）
 * @bprm: 二进制程序结构
 * @pos: 页面位置
 * @page: 要刷新的页面
 *
 * 在非MMU系统中，这是一个空操作函数。
 */
static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos,
		struct page *page)
{
}

/**
 * __bprm_mm_init - 初始化二进制程序内存管理（非MMU版本）
 * @bprm: 二进制程序结构
 *
 * 在非MMU系统中，简单设置参数指针位置。
 * 返回值: 总是返回0
 */
static int __bprm_mm_init(struct linux_binprm *bprm)
{
	bprm->p = PAGE_SIZE * MAX_ARG_PAGES - sizeof(void *);
	return 0;
}

/**
 * valid_arg_len - 验证参数长度是否有效
 * @bprm: 二进制程序结构
 * @len: 要验证的长度
 *
 * 检查参数长度是否在允许范围内。
 * 返回值: true表示长度有效，false表示长度无效
 */
static bool valid_arg_len(struct linux_binprm *bprm, long len)
{
	return len <= bprm->p;
}

#endif /* CONFIG_MMU */

/*
 * Create a new mm_struct and populate it with a temporary stack
 * vm_area_struct.  We don't have enough context at this point to set the stack
 * flags, permissions, and offset, so we use temporary values.  We'll update
 * them later in setup_arg_pages().
 */
/*
 * bprm_mm_init - 为新进程初始化内存管理结构
 * @bprm: 二进制程序执行参数结构
 *
 * 创建新的内存映射区域，为execve准备干净的地址空间
 * 这是execve过程中的关键步骤，建立新程序的虚拟内存布局
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int bprm_mm_init(struct linux_binprm *bprm)
{
	int err;
	struct mm_struct *mm = NULL;

	bprm->mm = mm = mm_alloc();
	err = -ENOMEM;
	if (!mm)
		goto err;

	/* Save current stack limit for all calculations made during exec. */
	task_lock(current->group_leader);
	bprm->rlim_stack = current->signal->rlim[RLIMIT_STACK];
	task_unlock(current->group_leader);

	err = __bprm_mm_init(bprm);
	if (err)
		goto err;

	return 0;

err:
	if (mm) {
		bprm->mm = NULL;
		mmdrop(mm);
	}

	return err;
}

struct user_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;
#endif
	union {
		const char __user *const __user *native;
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;
#endif
	} ptr;
};

/**
 * get_user_arg_ptr - 获取用户空间参数指针
 * @argv: 用户参数指针结构
 * @nr: 参数索引
 *
 * 从用户空间获取第nr个参数的指针，处理32位和64位兼容性。
 * 返回值: 成功返回参数指针，失败返回ERR_PTR错误码
 */
static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

#ifdef CONFIG_COMPAT
	if (unlikely(argv.is_compat)) {
		compat_uptr_t compat;

		if (get_user(compat, argv.ptr.compat + nr))
			return ERR_PTR(-EFAULT);

		return compat_ptr(compat);
	}
#endif

	if (get_user(native, argv.ptr.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}

/*
 * count() counts the number of strings in array ARGV.
 */
/*
 * count - 统计参数或环境变量的数量
 * @argv: 用户空间的参数指针结构
 * @max: 允许的最大参数数量
 *
 * 遍历argv数组，统计非NULL元素的个数
 * 返回值：成功时返回参数数量，出错时返回负值
 */
static int count(struct user_arg_ptr argv, int max)
{
	int i = 0;

	if (argv.ptr.native != NULL) {
		for (;;) {
			const char __user *p = get_user_arg_ptr(argv, i);

			if (!p)
				break;

			if (IS_ERR(p))
				return -EFAULT;

			if (i >= max)
				return -E2BIG;
			++i;

			if (fatal_signal_pending(current))
				return -ERESTARTNOHAND;
			cond_resched();
		}
	}
	return i;
}

/*
 * count_strings_kernel - 统计内核空间参数数组的数量
 * @argv: 内核空间的参数数组指针
 *
 * 遍历内核空间的NULL结尾的字符串数组，统计元素个数
 * 用于内核模块或内核线程调用execve时计算参数数量
 * 返回值：成功时返回参数数量，失败时返回负错误码
 */
static int count_strings_kernel(const char *const *argv)
{
	int i;

	if (!argv)
		return 0;

	for (i = 0; argv[i]; ++i) {
		if (i >= MAX_ARG_STRINGS)
			return -E2BIG;
		if (fatal_signal_pending(current))
			return -ERESTARTNOHAND;
		cond_resched();
	}
	return i;
}

/*
 * bprm_stack_limits - 计算并设置栈参数的大小限制
 * @bprm: 二进制程序执行参数结构
 *
 * 计算argv和envp字符串可以使用的最大栈空间：
 * - 限制为栈大小的1/4或_STK_LIM的3/4（取较小值）
 * - 确保二进制格式代码有足够的栈空间运行
 * - 为程序留下合理的栈空间
 * - 考虑历史兼容性，至少支持ARG_MAX大小
 * 返回值：成功时返回0，空间不足时返回-E2BIG
 */
static int bprm_stack_limits(struct linux_binprm *bprm)
{
	unsigned long limit, ptr_size;

	/*
	 * Limit to 1/4 of the max stack size or 3/4 of _STK_LIM
	 * (whichever is smaller) for the argv+env strings.
	 * This ensures that:
	 *  - the remaining binfmt code will not run out of stack space,
	 *  - the program will have a reasonable amount of stack left
	 *    to work from.
	 */
	limit = _STK_LIM / 4 * 3;
	limit = min(limit, bprm->rlim_stack.rlim_cur / 4);
	/*
	 * We've historically supported up to 32 pages (ARG_MAX)
	 * of argument strings even with small stacks
	 */
	limit = max_t(unsigned long, limit, ARG_MAX);
	/*
	 * We must account for the size of all the argv and envp pointers to
	 * the argv and envp strings, since they will also take up space in
	 * the stack. They aren't stored until much later when we can't
	 * signal to the parent that the child has run out of stack space.
	 * Instead, calculate it here so it's possible to fail gracefully.
	 */
	ptr_size = (bprm->argc + bprm->envc) * sizeof(void *);
	if (limit <= ptr_size)
		return -E2BIG;
	limit -= ptr_size;

	bprm->argmin = bprm->p - limit;
	return 0;
}

/*
 * 'copy_strings()' copies argument/environment strings from the old
 * processes's memory to the new process's stack.  The call to get_user_pages()
 * ensures the destination page is created and not swapped out.
 */
/*
 * copy_strings - 从用户空间复制参数或环境变量到内核栈
 * @argc: 参数数量
 * @argv: 用户空间参数指针结构
 * @bprm: 二进制程序执行参数结构
 *
 * 将用户空间的参数/环境变量字符串数组复制到内核为新进程准备的栈中
 * 支持32位兼容模式和64位原生模式
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int copy_strings(int argc, struct user_arg_ptr argv,
			struct linux_binprm *bprm)
{
	struct page *kmapped_page = NULL;
	char *kaddr = NULL;
	unsigned long kpos = 0;
	int ret;

	while (argc-- > 0) {
		const char __user *str;
		int len;
		unsigned long pos;

		ret = -EFAULT;
		str = get_user_arg_ptr(argv, argc);
		if (IS_ERR(str))
			goto out;

		len = strnlen_user(str, MAX_ARG_STRLEN);
		if (!len)
			goto out;

		ret = -E2BIG;
		if (!valid_arg_len(bprm, len))
			goto out;

		/* We're going to work our way backwords. */
		pos = bprm->p;
		str += len;
		bprm->p -= len;
#ifdef CONFIG_MMU
		if (bprm->p < bprm->argmin)
			goto out;
#endif

		while (len > 0) {
			int offset, bytes_to_copy;

			if (fatal_signal_pending(current)) {
				ret = -ERESTARTNOHAND;
				goto out;
			}
			cond_resched();

			offset = pos % PAGE_SIZE;
			if (offset == 0)
				offset = PAGE_SIZE;

			bytes_to_copy = offset;
			if (bytes_to_copy > len)
				bytes_to_copy = len;

			offset -= bytes_to_copy;
			pos -= bytes_to_copy;
			str -= bytes_to_copy;
			len -= bytes_to_copy;

			if (!kmapped_page || kpos != (pos & PAGE_MASK)) {
				struct page *page;

				page = get_arg_page(bprm, pos, 1);
				if (!page) {
					ret = -E2BIG;
					goto out;
				}

				if (kmapped_page) {
					flush_kernel_dcache_page(kmapped_page);
					kunmap(kmapped_page);
					put_arg_page(kmapped_page);
				}
				kmapped_page = page;
				kaddr = kmap(kmapped_page);
				kpos = pos & PAGE_MASK;
				flush_arg_page(bprm, kpos, kmapped_page);
			}
			if (copy_from_user(kaddr+offset, str, bytes_to_copy)) {
				ret = -EFAULT;
				goto out;
			}
		}
	}
	ret = 0;
out:
	if (kmapped_page) {
		flush_kernel_dcache_page(kmapped_page);
		kunmap(kmapped_page);
		put_arg_page(kmapped_page);
	}
	return ret;
}

/*
 * Copy and argument/environment string from the kernel to the processes stack.
 */
/*
 * copy_string_kernel - 从内核空间复制单个字符串到用户栈
 * @arg: 内核空间的字符串指针
 * @bprm: 二进制程序执行参数结构
 *
 * 将内核空间的字符串复制到用户进程的参数栈中
 * 主要用于内核内部调用execve时传递参数
 * 返回值：成功时返回0，失败时返回负错误码
 */
int copy_string_kernel(const char *arg, struct linux_binprm *bprm)
{
	int len = strnlen(arg, MAX_ARG_STRLEN) + 1 /* terminating NUL */;
	unsigned long pos = bprm->p;

	if (len == 0)
		return -EFAULT;
	if (!valid_arg_len(bprm, len))
		return -E2BIG;

	/* We're going to work our way backwards. */
	arg += len;
	bprm->p -= len;
	if (IS_ENABLED(CONFIG_MMU) && bprm->p < bprm->argmin)
		return -E2BIG;

	while (len > 0) {
		unsigned int bytes_to_copy = min_t(unsigned int, len,
				min_not_zero(offset_in_page(pos), PAGE_SIZE));
		struct page *page;
		char *kaddr;

		pos -= bytes_to_copy;
		arg -= bytes_to_copy;
		len -= bytes_to_copy;

		page = get_arg_page(bprm, pos, 1);
		if (!page)
			return -E2BIG;
		kaddr = kmap_atomic(page);
		flush_arg_page(bprm, pos & PAGE_MASK, page);
		memcpy(kaddr + offset_in_page(pos), arg, bytes_to_copy);
		flush_kernel_dcache_page(page);
		kunmap_atomic(kaddr);
		put_arg_page(page);
	}

	return 0;
}
EXPORT_SYMBOL(copy_string_kernel);

/*
 * copy_strings_kernel - 从内核空间复制参数数组到用户栈
 * @argc: 参数数量
 * @argv: 内核空间的参数数组指针
 * @bprm: 二进制程序执行参数结构
 *
 * 遍历内核空间的参数数组，逐个复制字符串到用户栈中
 * 用于内核模块或内核线程执行用户程序时传递参数
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int copy_strings_kernel(int argc, const char *const *argv,
			       struct linux_binprm *bprm)
{
	while (argc-- > 0) {
		int ret = copy_string_kernel(argv[argc], bprm);
		if (ret < 0)
			return ret;
		if (fatal_signal_pending(current))
			return -ERESTARTNOHAND;
		cond_resched();
	}
	return 0;
}

#ifdef CONFIG_MMU

/*
 * During bprm_mm_init(), we create a temporary stack at STACK_TOP_MAX.  Once
 * the binfmt code determines where the new stack should reside, we shift it to
 * its final location.  The process proceeds as follows:
 *
 * 1) Use shift to calculate the new vma endpoints.
 * 2) Extend vma to cover both the old and new ranges.  This ensures the
 *    arguments passed to subsequent functions are consistent.
 * 3) Move vma's page tables to the new range.
 * 4) Free up any cleared pgd range.
 * 5) Shrink the vma to cover only the new range.
 */
/*
 * shift_arg_pages - 移动参数页面到最终位置
 * @vma: 参数栈的虚拟内存区域
 * @shift: 需要移动的偏移量
 *
 * 在execve过程中调整参数栈的位置，确保新程序的栈布局正确
 * 这个函数处理栈的物理内存重新映射和页表更新
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int shift_arg_pages(struct vm_area_struct *vma, unsigned long shift)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long old_start = vma->vm_start;
	unsigned long old_end = vma->vm_end;
	unsigned long length = old_end - old_start;
	unsigned long new_start = old_start - shift;
	unsigned long new_end = old_end - shift;
	struct mmu_gather tlb;

	BUG_ON(new_start > new_end);

	/*
	 * ensure there are no vmas between where we want to go
	 * and where we are
	 */
	if (vma != find_vma(mm, new_start))
		return -EFAULT;

	/*
	 * cover the whole range: [new_start, old_end)
	 */
	if (vma_adjust(vma, new_start, old_end, vma->vm_pgoff, NULL))
		return -ENOMEM;

	/*
	 * move the page tables downwards, on failure we rely on
	 * process cleanup to remove whatever mess we made.
	 */
	if (length != move_page_tables(vma, old_start,
				       vma, new_start, length, false))
		return -ENOMEM;

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm, old_start, old_end);
	if (new_end > old_start) {
		/*
		 * when the old and new regions overlap clear from new_end.
		 */
		free_pgd_range(&tlb, new_end, old_end, new_end,
			vma->vm_next ? vma->vm_next->vm_start : USER_PGTABLES_CEILING);
	} else {
		/*
		 * otherwise, clean from old_start; this is done to not touch
		 * the address space in [new_end, old_start) some architectures
		 * have constraints on va-space that make this illegal (IA64) -
		 * for the others its just a little faster.
		 */
		free_pgd_range(&tlb, old_start, old_end, new_end,
			vma->vm_next ? vma->vm_next->vm_start : USER_PGTABLES_CEILING);
	}
	tlb_finish_mmu(&tlb, old_start, old_end);

	/*
	 * Shrink the vma to just the new range.  Always succeeds.
	 */
	vma_adjust(vma, new_start, new_end, vma->vm_pgoff, NULL);

	return 0;
}

/*
 * Finalizes the stack vm_area_struct. The flags and permissions are updated,
 * the stack is optionally relocated, and some extra space is added.
 */
/*
 * setup_arg_pages - 设置新进程的参数和环境变量页面
 * @bprm: 二进制程序执行参数结构
 *
 * 为新进程创建并设置参数栈，包括：
 * - 计算栈的大小和位置
 * - 创建栈的VMA（虚拟内存区域）
 * - 设置栈的权限和属性
 * - 调整栈的最终位置
 * 返回值：成功时返回0，失败时返回负错误码
 */
int setup_arg_pages(struct linux_binprm *bprm,
		    unsigned long stack_top,
		    int executable_stack)
{
	unsigned long ret;
	unsigned long stack_shift;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = bprm->vma;
	struct vm_area_struct *prev = NULL;
	unsigned long vm_flags;
	unsigned long stack_base;
	unsigned long stack_size;
	unsigned long stack_expand;
	unsigned long rlim_stack;

#ifdef CONFIG_STACK_GROWSUP
	/* Limit stack size */
	stack_base = bprm->rlim_stack.rlim_max;
	if (stack_base > STACK_SIZE_MAX)
		stack_base = STACK_SIZE_MAX;

	/* Add space for stack randomization. */
	stack_base += (STACK_RND_MASK << PAGE_SHIFT);

	/* Make sure we didn't let the argument array grow too large. */
	if (vma->vm_end - vma->vm_start > stack_base)
		return -ENOMEM;

	stack_base = PAGE_ALIGN(stack_top - stack_base);

	stack_shift = vma->vm_start - stack_base;
	mm->arg_start = bprm->p - stack_shift;
	bprm->p = vma->vm_end - stack_shift;
#else
	stack_top = arch_align_stack(stack_top);
	stack_top = PAGE_ALIGN(stack_top);

	if (unlikely(stack_top < mmap_min_addr) ||
	    unlikely(vma->vm_end - vma->vm_start >= stack_top - mmap_min_addr))
		return -ENOMEM;

	stack_shift = vma->vm_end - stack_top;

	bprm->p -= stack_shift;
	mm->arg_start = bprm->p;
#endif

	if (bprm->loader)
		bprm->loader -= stack_shift;
	bprm->exec -= stack_shift;

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	vm_flags = VM_STACK_FLAGS;

	/*
	 * Adjust stack execute permissions; explicitly enable for
	 * EXSTACK_ENABLE_X, disable for EXSTACK_DISABLE_X and leave alone
	 * (arch default) otherwise.
	 */
	if (unlikely(executable_stack == EXSTACK_ENABLE_X))
		vm_flags |= VM_EXEC;
	else if (executable_stack == EXSTACK_DISABLE_X)
		vm_flags &= ~VM_EXEC;
	vm_flags |= mm->def_flags;
	vm_flags |= VM_STACK_INCOMPLETE_SETUP;

	ret = mprotect_fixup(vma, &prev, vma->vm_start, vma->vm_end,
			vm_flags);
	if (ret)
		goto out_unlock;
	BUG_ON(prev != vma);

	if (unlikely(vm_flags & VM_EXEC)) {
		pr_warn_once("process '%pD4' started with executable stack\n",
			     bprm->file);
	}

	/* Move stack pages down in memory. */
	if (stack_shift) {
		ret = shift_arg_pages(vma, stack_shift);
		if (ret)
			goto out_unlock;
	}

	/* mprotect_fixup is overkill to remove the temporary stack flags */
	vma->vm_flags &= ~VM_STACK_INCOMPLETE_SETUP;

	stack_expand = 131072UL; /* randomly 32*4k (or 2*64k) pages */
	stack_size = vma->vm_end - vma->vm_start;
	/*
	 * Align this down to a page boundary as expand_stack
	 * will align it up.
	 */
	rlim_stack = bprm->rlim_stack.rlim_cur & PAGE_MASK;
#ifdef CONFIG_STACK_GROWSUP
	if (stack_size + stack_expand > rlim_stack)
		stack_base = vma->vm_start + rlim_stack;
	else
		stack_base = vma->vm_end + stack_expand;
#else
	if (stack_size + stack_expand > rlim_stack)
		stack_base = vma->vm_end - rlim_stack;
	else
		stack_base = vma->vm_start - stack_expand;
#endif
	current->mm->start_stack = bprm->p;
	ret = expand_stack(vma, stack_base);
	if (ret)
		ret = -EFAULT;

out_unlock:
	mmap_write_unlock(mm);
	return ret;
}
EXPORT_SYMBOL(setup_arg_pages);

#else

/*
 * Transfer the program arguments and environment from the holding pages
 * onto the stack. The provided stack pointer is adjusted accordingly.
 */
/*
 * transfer_args_to_stack - 将参数从临时页面转移到用户栈
 * @bprm: 二进制程序执行参数结构
 * @sp_location: 栈指针位置的指针
 *
 * 在非MMU系统中，将保存的参数和环境变量从临时页面
 * 复制到用户程序的栈空间，并相应调整栈指针。
 * 返回值：成功时返回0，失败时返回负错误码
 */
int transfer_args_to_stack(struct linux_binprm *bprm,
			   unsigned long *sp_location)
{
	unsigned long index, stop, sp;
	int ret = 0;

	stop = bprm->p >> PAGE_SHIFT;
	sp = *sp_location;

	for (index = MAX_ARG_PAGES - 1; index >= stop; index--) {
		unsigned int offset = index == stop ? bprm->p & ~PAGE_MASK : 0;
		char *src = kmap(bprm->page[index]) + offset;
		sp -= PAGE_SIZE - offset;
		if (copy_to_user((void *) sp, src, PAGE_SIZE - offset) != 0)
			ret = -EFAULT;
		kunmap(bprm->page[index]);
		if (ret)
			goto out;
	}

	*sp_location = sp;

out:
	return ret;
}
EXPORT_SYMBOL(transfer_args_to_stack);

#endif /* CONFIG_MMU */

/*
 * do_open_execat - 在指定目录打开可执行文件
 * @fd: 目录文件描述符，AT_FDCWD表示当前工作目录
 * @name: 文件名结构
 * @flags: 打开标志（AT_SYMLINK_NOFOLLOW、AT_EMPTY_PATH等）
 *
 * 以执行权限打开文件，进行基本的安全检查：
 * - 验证是否为常规文件
 * - 检查执行权限
 * - 确保文件系统未设置noexec标志
 * 返回值：成功时返回文件指针，失败时返回ERR_PTR错误码
 */
static struct file *do_open_execat(int fd, struct filename *name, int flags)
{
	struct file *file;
	int err;
	struct open_flags open_exec_flags = {
		.open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
		.acc_mode = MAY_EXEC,
		.intent = LOOKUP_OPEN,
		.lookup_flags = LOOKUP_FOLLOW,
	};

	if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0)
		return ERR_PTR(-EINVAL);
	if (flags & AT_SYMLINK_NOFOLLOW)
		open_exec_flags.lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & AT_EMPTY_PATH)
		open_exec_flags.lookup_flags |= LOOKUP_EMPTY;

	file = do_filp_open(fd, name, &open_exec_flags);
	if (IS_ERR(file))
		goto out;

	/*
	 * may_open() has already checked for this, so it should be
	 * impossible to trip now. But we need to be extra cautious
	 * and check again at the very end too.
	 */
	err = -EACCES;
	if (WARN_ON_ONCE(!S_ISREG(file_inode(file)->i_mode) ||
			 path_noexec(&file->f_path)))
		goto exit;

	err = deny_write_access(file);
	if (err)
		goto exit;

	if (name->name[0] != '\0')
		fsnotify_open(file);

out:
	return file;

exit:
	fput(file);
	return ERR_PTR(err);
}

/*
 * open_exec - 打开要执行的二进制文件
 * @name: 文件路径名
 *
 * 以只读方式打开可执行文件，并进行基本的权限检查：
 * - 检查文件是否存在且可读
 * - 验证执行权限
 * - 确保不是目录
 * 返回值：成功时返回文件指针，失败时返回ERR_PTR错误码
 */
struct file *open_exec(const char *name)
{
	struct filename *filename = getname_kernel(name);
	struct file *f = ERR_CAST(filename);

	if (!IS_ERR(filename)) {
		f = do_open_execat(AT_FDCWD, filename, 0);
		putname(filename);
	}
	return f;
}
EXPORT_SYMBOL(open_exec);

#if defined(CONFIG_HAVE_AOUT) || defined(CONFIG_BINFMT_FLAT) || \
    defined(CONFIG_BINFMT_ELF_FDPIC)
/*
 * read_code - 读取可执行代码到指定地址
 * @file: 可执行文件指针
 * @addr: 目标用户空间地址
 * @pos: 文件中的起始位置
 * @len: 要读取的字节数
 *
 * 从可执行文件读取代码段到用户空间地址，并刷新指令缓存。
 * 用于某些二进制格式（如a.out、FLAT、ELF_FDPIC）的代码加载。
 * 返回值：成功时返回读取的字节数，失败时返回负错误码
 */
ssize_t read_code(struct file *file, unsigned long addr, loff_t pos, size_t len)
{
	ssize_t res = vfs_read(file, (void __user *)addr, len, &pos);
	if (res > 0)
		flush_icache_user_range(addr, addr + len);
	return res;
}
EXPORT_SYMBOL(read_code);
#endif

/*
 * Maps the mm_struct mm into the current task struct.
 * On success, this function returns with the mutex
 * exec_update_mutex locked.
 */
/*
 * exec_mmap - 将新的内存管理结构映射到当前任务
 * @mm: 新进程的内存管理结构
 *
 * 在execve过程中替换当前进程的内存映射：
 * - 通知父进程不再关注旧的VM
 * - 检查是否有正在进行的核心转储
 * - 原子性地切换到新的内存管理结构
 * - 清理旧的内存管理结构
 * 成功时持有exec_update_mutex锁
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int exec_mmap(struct mm_struct *mm)
{
	struct task_struct *tsk;
	struct mm_struct *old_mm, *active_mm;
	int ret;

	/* Notify parent that we're no longer interested in the old VM */
	tsk = current;
	old_mm = current->mm;
	exec_mm_release(tsk, old_mm);
	if (old_mm)
		sync_mm_rss(old_mm);

	ret = mutex_lock_killable(&tsk->signal->exec_update_mutex);
	if (ret)
		return ret;

	if (old_mm) {
		/*
		 * Make sure that if there is a core dump in progress
		 * for the old mm, we get out and die instead of going
		 * through with the exec.  We must hold mmap_lock around
		 * checking core_state and changing tsk->mm.
		 */
		mmap_read_lock(old_mm);
		if (unlikely(old_mm->core_state)) {
			mmap_read_unlock(old_mm);
			mutex_unlock(&tsk->signal->exec_update_mutex);
			return -EINTR;
		}
	}

	task_lock(tsk);
	membarrier_exec_mmap(mm);

	local_irq_disable();
	active_mm = tsk->active_mm;
	tsk->active_mm = mm;
	tsk->mm = mm;
	/*
	 * This prevents preemption while active_mm is being loaded and
	 * it and mm are being updated, which could cause problems for
	 * lazy tlb mm refcounting when these are updated by context
	 * switches. Not all architectures can handle irqs off over
	 * activate_mm yet.
	 */
	if (!IS_ENABLED(CONFIG_ARCH_WANT_IRQS_OFF_ACTIVATE_MM))
		local_irq_enable();
	activate_mm(active_mm, mm);
	if (IS_ENABLED(CONFIG_ARCH_WANT_IRQS_OFF_ACTIVATE_MM))
		local_irq_enable();
	tsk->mm->vmacache_seqnum = 0;
	vmacache_flush(tsk);
	task_unlock(tsk);
	if (old_mm) {
		mmap_read_unlock(old_mm);
		BUG_ON(active_mm != old_mm);
		setmax_mm_hiwater_rss(&tsk->signal->maxrss, old_mm);
		mm_update_next_owner(old_mm);
		mmput(old_mm);
		return 0;
	}
	mmdrop(active_mm);
	return 0;
}

/*
 * de_thread - 在exec过程中处理线程组解散
 * @tsk: 当前任务结构
 *
 * 在execve过程中清理线程组，确保只有当前线程继续执行：
 * - 杀死线程组中的所有其他线程
 * - 等待线程组领导者变为非活跃状态
 * - 如果当前不是线程组领导者，则继承其PID和身份
 * - 处理信号和进程组的重新配置
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int de_thread(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct sighand_struct *oldsighand = tsk->sighand;
	spinlock_t *lock = &oldsighand->siglock;

	if (thread_group_empty(tsk))
		goto no_thread_group;

	/*
	 * Kill all other threads in the thread group.
	 */
	spin_lock_irq(lock);
	if (signal_group_exit(sig)) {
		/*
		 * Another group action in progress, just
		 * return so that the signal is processed.
		 */
		spin_unlock_irq(lock);
		return -EAGAIN;
	}

	sig->group_exit_task = tsk;
	sig->notify_count = zap_other_threads(tsk);
	if (!thread_group_leader(tsk))
		sig->notify_count--;

	while (sig->notify_count) {
		__set_current_state(TASK_KILLABLE);
		spin_unlock_irq(lock);
		schedule();
		if (__fatal_signal_pending(tsk))
			goto killed;
		spin_lock_irq(lock);
	}
	spin_unlock_irq(lock);

	/*
	 * At this point all other threads have exited, all we have to
	 * do is to wait for the thread group leader to become inactive,
	 * and to assume its PID:
	 */
	if (!thread_group_leader(tsk)) {
		struct task_struct *leader = tsk->group_leader;

		for (;;) {
			cgroup_threadgroup_change_begin(tsk);
			write_lock_irq(&tasklist_lock);
			/*
			 * Do this under tasklist_lock to ensure that
			 * exit_notify() can't miss ->group_exit_task
			 */
			sig->notify_count = -1;
			if (likely(leader->exit_state))
				break;
			__set_current_state(TASK_KILLABLE);
			write_unlock_irq(&tasklist_lock);
			cgroup_threadgroup_change_end(tsk);
			schedule();
			if (__fatal_signal_pending(tsk))
				goto killed;
		}

		/*
		 * The only record we have of the real-time age of a
		 * process, regardless of execs it's done, is start_time.
		 * All the past CPU time is accumulated in signal_struct
		 * from sister threads now dead.  But in this non-leader
		 * exec, nothing survives from the original leader thread,
		 * whose birth marks the true age of this process now.
		 * When we take on its identity by switching to its PID, we
		 * also take its birthdate (always earlier than our own).
		 */
		tsk->start_time = leader->start_time;
		tsk->start_boottime = leader->start_boottime;

		BUG_ON(!same_thread_group(leader, tsk));
		/*
		 * An exec() starts a new thread group with the
		 * TGID of the previous thread group. Rehash the
		 * two threads with a switched PID, and release
		 * the former thread group leader:
		 */

		/* Become a process group leader with the old leader's pid.
		 * The old leader becomes a thread of the this thread group.
		 */
		exchange_tids(tsk, leader);
		transfer_pid(leader, tsk, PIDTYPE_TGID);
		transfer_pid(leader, tsk, PIDTYPE_PGID);
		transfer_pid(leader, tsk, PIDTYPE_SID);

		list_replace_rcu(&leader->tasks, &tsk->tasks);
		list_replace_init(&leader->sibling, &tsk->sibling);

		tsk->group_leader = tsk;
		leader->group_leader = tsk;

		tsk->exit_signal = SIGCHLD;
		leader->exit_signal = -1;

		BUG_ON(leader->exit_state != EXIT_ZOMBIE);
		leader->exit_state = EXIT_DEAD;

		/*
		 * We are going to release_task()->ptrace_unlink() silently,
		 * the tracer can sleep in do_wait(). EXIT_DEAD guarantees
		 * the tracer wont't block again waiting for this thread.
		 */
		if (unlikely(leader->ptrace))
			__wake_up_parent(leader, leader->parent);
		write_unlock_irq(&tasklist_lock);
		cgroup_threadgroup_change_end(tsk);

		release_task(leader);
	}

	sig->group_exit_task = NULL;
	sig->notify_count = 0;

no_thread_group:
	/* we have changed execution domain */
	tsk->exit_signal = SIGCHLD;

	BUG_ON(!thread_group_leader(tsk));
	return 0;

killed:
	/* protects against exit_notify() and __exit_signal() */
	read_lock(&tasklist_lock);
	sig->group_exit_task = NULL;
	sig->notify_count = 0;
	read_unlock(&tasklist_lock);
	return -EAGAIN;
}


/*
 * This function makes sure the current process has its own signal table,
 * so that flush_signal_handlers can later reset the handlers without
 * disturbing other processes.  (Other processes might share the signal
 * table via the CLONE_SIGHAND option to clone().)
 */
/*
 * unshare_sighand - 确保当前进程拥有独立的信号处理表
 * @me: 当前任务结构
 *
 * 确保当前进程有自己的信号处理表，这样flush_signal_handlers
 * 可以重置信号处理器而不影响其他进程。如果信号表被其他进程
 * 共享（通过CLONE_SIGHAND），则创建一个新的私有副本。
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int unshare_sighand(struct task_struct *me)
{
	struct sighand_struct *oldsighand = me->sighand;

	if (refcount_read(&oldsighand->count) != 1) {
		struct sighand_struct *newsighand;
		/*
		 * This ->sighand is shared with the CLONE_SIGHAND
		 * but not CLONE_THREAD task, switch to the new one.
		 */
		newsighand = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
		if (!newsighand)
			return -ENOMEM;

		refcount_set(&newsighand->count, 1);
		memcpy(newsighand->action, oldsighand->action,
		       sizeof(newsighand->action));

		write_lock_irq(&tasklist_lock);
		spin_lock(&oldsighand->siglock);
		rcu_assign_pointer(me->sighand, newsighand);
		spin_unlock(&oldsighand->siglock);
		write_unlock_irq(&tasklist_lock);

		__cleanup_sighand(oldsighand);
	}
	return 0;
}

/*
 * __get_task_comm - 获取任务的命令名称
 * @buf: 输出缓冲区
 * @buf_size: 缓冲区大小
 * @tsk: 目标任务结构
 *
 * 线程安全地获取指定任务的命令名称。
 * 使用task_lock确保在复制过程中命令名称不会被修改。
 * 返回值：返回缓冲区指针
 */
char *__get_task_comm(char *buf, size_t buf_size, struct task_struct *tsk)
{
	task_lock(tsk);
	strncpy(buf, tsk->comm, buf_size);
	task_unlock(tsk);
	return buf;
}
EXPORT_SYMBOL_GPL(__get_task_comm);

/*
 * These functions flushes out all traces of the currently running executable
 * so that a new one can be started
 */

/*
 * __set_task_comm - 设置任务的命令名称
 * @tsk: 目标任务结构
 * @buf: 新的命令名称
 * @exec: 是否由exec调用（用于性能事件）
 *
 * 线程安全地设置任务的命令名称，同时：
 * - 触发任务重命名跟踪事件
 * - 通知性能事件子系统
 * 使用task_lock确保原子性更新
 */
void __set_task_comm(struct task_struct *tsk, const char *buf, bool exec)
{
	task_lock(tsk);
	trace_task_rename(tsk, buf);
	strlcpy(tsk->comm, buf, sizeof(tsk->comm));
	task_unlock(tsk);
	perf_event_comm(tsk, exec);
}

/*
 * Calling this is the point of no return. None of the failures will be
 * seen by userspace since either the process is already taking a fatal
 * signal (via de_thread() or coredump), or will have SEGV raised
 * (after exec_mmap()) by search_binary_handler (see below).
 */
/*
 * begin_new_exec - 开始新程序执行，进入不可回退点
 * @bprm: 二进制程序执行参数结构
 *
 * 这是execve过程中的关键转折点，执行后无法回退到原进程：
 * - 释放旧的内存管理结构
 * - 设置新的进程名称和可执行文件路径
 * - 清理旧的信号处理器
 * - 重置各种进程属性
 * - 更新进程的执行上下文
 * 调用此函数后，即使后续步骤失败，也不能恢复原进程状态
 * 返回值：成功时返回0，失败时返回负错误码
 */
int begin_new_exec(struct linux_binprm * bprm)
{
	struct task_struct *me = current;
	int retval;

	/* Once we are committed compute the creds */
	retval = bprm_creds_from_file(bprm);
	if (retval)
		return retval;

	/*
	 * Ensure all future errors are fatal.
	 */
	bprm->point_of_no_return = true;

	/*
	 * Make this the only thread in the thread group.
	 */
	retval = de_thread(me);
	if (retval)
		goto out;

	/*
	 * Must be called _before_ exec_mmap() as bprm->mm is
	 * not visibile until then. This also enables the update
	 * to be lockless.
	 */
	set_mm_exe_file(bprm->mm, bprm->file);

	/* If the binary is not readable then enforce mm->dumpable=0 */
	would_dump(bprm, bprm->file);
	if (bprm->have_execfd)
		would_dump(bprm, bprm->executable);

	/*
	 * Release all of the old mmap stuff
	 */
	acct_arg_size(bprm, 0);
	retval = exec_mmap(bprm->mm);
	if (retval)
		goto out;

	bprm->mm = NULL;

#ifdef CONFIG_POSIX_TIMERS
	exit_itimers(me->signal);
	flush_itimer_signals();
#endif

	/*
	 * Make the signal table private.
	 */
	retval = unshare_sighand(me);
	if (retval)
		goto out_unlock;

	/*
	 * Ensure that the uaccess routines can actually operate on userspace
	 * pointers:
	 */
	force_uaccess_begin();

	me->flags &= ~(PF_RANDOMIZE | PF_FORKNOEXEC | PF_KTHREAD |
					PF_NOFREEZE | PF_NO_SETAFFINITY);
	flush_thread();
	me->personality &= ~bprm->per_clear;

	/*
	 * We have to apply CLOEXEC before we change whether the process is
	 * dumpable (in setup_new_exec) to avoid a race with a process in userspace
	 * trying to access the should-be-closed file descriptors of a process
	 * undergoing exec(2).
	 */
	do_close_on_exec(me->files);

	if (bprm->secureexec) {
		/* Make sure parent cannot signal privileged process. */
		me->pdeath_signal = 0;

		/*
		 * For secureexec, reset the stack limit to sane default to
		 * avoid bad behavior from the prior rlimits. This has to
		 * happen before arch_pick_mmap_layout(), which examines
		 * RLIMIT_STACK, but after the point of no return to avoid
		 * needing to clean up the change on failure.
		 */
		if (bprm->rlim_stack.rlim_cur > _STK_LIM)
			bprm->rlim_stack.rlim_cur = _STK_LIM;
	}

	me->sas_ss_sp = me->sas_ss_size = 0;

	/*
	 * Figure out dumpability. Note that this checking only of current
	 * is wrong, but userspace depends on it. This should be testing
	 * bprm->secureexec instead.
	 */
	if (bprm->interp_flags & BINPRM_FLAGS_ENFORCE_NONDUMP ||
	    !(uid_eq(current_euid(), current_uid()) &&
	      gid_eq(current_egid(), current_gid())))
		set_dumpable(current->mm, suid_dumpable);
	else
		set_dumpable(current->mm, SUID_DUMP_USER);

	perf_event_exec();
	__set_task_comm(me, kbasename(bprm->filename), true);

	/* An exec changes our domain. We are no longer part of the thread
	   group */
	WRITE_ONCE(me->self_exec_id, me->self_exec_id + 1);
	flush_signal_handlers(me, 0);

	/*
	 * install the new credentials for this executable
	 */
	security_bprm_committing_creds(bprm);

	commit_creds(bprm->cred);
	bprm->cred = NULL;

	/*
	 * Disable monitoring for regular users
	 * when executing setuid binaries. Must
	 * wait until new credentials are committed
	 * by commit_creds() above
	 */
	if (get_dumpable(me->mm) != SUID_DUMP_USER)
		perf_event_exit_task(me);
	/*
	 * cred_guard_mutex must be held at least to this point to prevent
	 * ptrace_attach() from altering our determination of the task's
	 * credentials; any time after this it may be unlocked.
	 */
	security_bprm_committed_creds(bprm);

	/* Pass the opened binary to the interpreter. */
	if (bprm->have_execfd) {
		retval = get_unused_fd_flags(0);
		if (retval < 0)
			goto out_unlock;
		fd_install(retval, bprm->executable);
		bprm->executable = NULL;
		bprm->execfd = retval;
	}
	return 0;

out_unlock:
	mutex_unlock(&me->signal->exec_update_mutex);
out:
	return retval;
}
EXPORT_SYMBOL(begin_new_exec);

/*
 * would_dump - 检查进程是否应该生成核心转储
 * @bprm: 二进制程序执行参数结构
 * @file: 可执行文件指针
 *
 * 根据文件权限和SUID/SGID位判断新进程是否有权限生成core dump
 * 考虑安全因素，SUID/SGID程序通常不允许生成core dump
 * 返回值：无返回值，直接设置bprm->secureexec标志
 */
void would_dump(struct linux_binprm *bprm, struct file *file)
{
	struct inode *inode = file_inode(file);
	if (inode_permission(inode, MAY_READ) < 0) {
		struct user_namespace *old, *user_ns;
		bprm->interp_flags |= BINPRM_FLAGS_ENFORCE_NONDUMP;

		/* Ensure mm->user_ns contains the executable */
		user_ns = old = bprm->mm->user_ns;
		while ((user_ns != &init_user_ns) &&
		       !privileged_wrt_inode_uidgid(user_ns, inode))
			user_ns = user_ns->parent;

		if (old != user_ns) {
			bprm->mm->user_ns = get_user_ns(user_ns);
			put_user_ns(old);
		}
	}
}
EXPORT_SYMBOL(would_dump);

/*
 * setup_new_exec - 设置新的执行环境
 * @bprm: 二进制程序执行参数结构
 *
 * 完成新程序执行环境的最终设置：
 * - 设置进程名称为新程序名
 * - 更新进程的dump权限标志
 * - 调用体系结构相关的执行环境设置
 * - 提交新的凭证信息
 * 这个函数在begin_new_exec之后调用，完成执行环境的最后配置
 */
void setup_new_exec(struct linux_binprm * bprm)
{
	/* Setup things that can depend upon the personality */
	struct task_struct *me = current;

	arch_pick_mmap_layout(me->mm, &bprm->rlim_stack);

	arch_setup_new_exec();

	/* Set the new mm task size. We have to do that late because it may
	 * depend on TIF_32BIT which is only updated in flush_thread() on
	 * some architectures like powerpc
	 */
	me->mm->task_size = TASK_SIZE;
	mutex_unlock(&me->signal->exec_update_mutex);
	mutex_unlock(&me->signal->cred_guard_mutex);
}
EXPORT_SYMBOL(setup_new_exec);

/**
 * finalize_exec - 完成执行环境的最终设置
 * @bprm: 二进制程序执行参数结构
 *
 * 在start_thread()接管执行前的最后设置步骤：
 * - 存储栈限制的变更
 * - 执行体系结构相关的最终化操作
 * 此函数在所有其他exec设置完成后运行
 */
void finalize_exec(struct linux_binprm *bprm)
{
	/* Store any stack rlimit changes before starting thread. */
	task_lock(current->group_leader);
	current->signal->rlim[RLIMIT_STACK] = bprm->rlim_stack;
	task_unlock(current->group_leader);
}
EXPORT_SYMBOL(finalize_exec);

/**
 * prepare_bprm_creds - 准备二进制程序的凭证信息
 * @bprm: 二进制程序执行参数结构
 *
 * 为exec过程准备新的凭证信息并锁定cred_guard_mutex：
 * - 获取cred_guard_mutex锁，防止并发访问凭证
 * - 为新程序准备执行凭证
 * setup_new_exec()会提交新凭证并释放锁，
 * 如果exec失败，free_bprm()会释放凭证并解锁。
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int prepare_bprm_creds(struct linux_binprm *bprm)
{
	if (mutex_lock_interruptible(&current->signal->cred_guard_mutex))
		return -ERESTARTNOINTR;

	bprm->cred = prepare_exec_creds();
	if (likely(bprm->cred))
		return 0;

	mutex_unlock(&current->signal->cred_guard_mutex);
	return -ENOMEM;
}

/**
 * free_bprm - 释放二进制程序执行参数结构
 * @bprm: 要释放的二进制程序执行参数结构
 *
 * 清理并释放bprm结构及其所有相关资源：
 * - 释放内存管理结构
 * - 释放参数页面
 * - 释放凭证信息并解锁cred_guard_mutex
 * - 关闭文件句柄
 * - 释放路径字符串
 * 通常在exec失败或完成后调用
 */
static void free_bprm(struct linux_binprm *bprm)
{
	if (bprm->mm) {
		acct_arg_size(bprm, 0);
		mmput(bprm->mm);
	}
	free_arg_pages(bprm);
	if (bprm->cred) {
		mutex_unlock(&current->signal->cred_guard_mutex);
		abort_creds(bprm->cred);
	}
	if (bprm->file) {
		allow_write_access(bprm->file);
		fput(bprm->file);
	}
	if (bprm->executable)
		fput(bprm->executable);
	/* If a binfmt changed the interp, free it. */
	if (bprm->interp != bprm->filename)
		kfree(bprm->interp);
	kfree(bprm->fdpath);
	kfree(bprm);
}

/**
 * alloc_bprm - 分配并初始化二进制程序执行参数结构
 * @fd: 文件描述符，AT_FDCWD表示当前工作目录
 * @filename: 文件名结构
 *
 * 分配并初始化新的bprm结构：
 * - 分配内存并清零
 * - 设置文件名和解释器路径
 * - 处理相对路径和文件描述符路径
 * - 初始化内存管理结构
 * 返回值: 成功返回bprm指针，失败返回ERR_PTR错误码
 */
static struct linux_binprm *alloc_bprm(int fd, struct filename *filename)
{
	struct linux_binprm *bprm = kzalloc(sizeof(*bprm), GFP_KERNEL);
	int retval = -ENOMEM;
	if (!bprm)
		goto out;

	if (fd == AT_FDCWD || filename->name[0] == '/') {
		bprm->filename = filename->name;
	} else {
		if (filename->name[0] == '\0')
			bprm->fdpath = kasprintf(GFP_KERNEL, "/dev/fd/%d", fd);
		else
			bprm->fdpath = kasprintf(GFP_KERNEL, "/dev/fd/%d/%s",
						  fd, filename->name);
		if (!bprm->fdpath)
			goto out_free;

		bprm->filename = bprm->fdpath;
	}
	bprm->interp = bprm->filename;

	retval = bprm_mm_init(bprm);
	if (retval)
		goto out_free;
	return bprm;

out_free:
	free_bprm(bprm);
out:
	return ERR_PTR(retval);
}

/**
 * bprm_change_interp - 更改二进制程序的解释器路径
 * @interp: 新的解释器路径
 * @bprm: 二进制程序执行参数结构
 *
 * 动态更改程序的解释器路径，用于脚本执行和动态链接：
 * - 如果已存在不同的解释器路径，先释放旧的
 * - 复制新的解释器路径字符串
 * 返回值: 成功返回0，内存分配失败返回-ENOMEM
 */
int bprm_change_interp(const char *interp, struct linux_binprm *bprm)
{
	/* If a binfmt changed the interp, free it first. */
	if (bprm->interp != bprm->filename)
		kfree(bprm->interp);
	bprm->interp = kstrdup(interp, GFP_KERNEL);
	if (!bprm->interp)
		return -ENOMEM;
	return 0;
}
EXPORT_SYMBOL(bprm_change_interp);

/**
 * check_unsafe_exec - 检查执行程序的安全性
 * @bprm: 二进制程序执行参数结构
 *
 * 检查执行新程序时的各种安全风险：
 * - 检查是否被ptrace跟踪
 * - 检查是否设置了no_new_privs标志
 * - 检查文件系统是否与其他进程共享
 * 调用者必须持有cred_guard_mutex锁以防止PTRACE_ATTACH或seccomp线程同步
 */
static void check_unsafe_exec(struct linux_binprm *bprm)
{
	struct task_struct *p = current, *t;
	unsigned n_fs;

	if (p->ptrace)
		bprm->unsafe |= LSM_UNSAFE_PTRACE;

	/*
	 * This isn't strictly necessary, but it makes it harder for LSMs to
	 * mess up.
	 */
	if (task_no_new_privs(current))
		bprm->unsafe |= LSM_UNSAFE_NO_NEW_PRIVS;

	t = p;
	n_fs = 1;
	spin_lock(&p->fs->lock);
	rcu_read_lock();
	while_each_thread(p, t) {
		if (t->fs == p->fs)
			n_fs++;
	}
	rcu_read_unlock();

	if (p->fs->users > n_fs)
		bprm->unsafe |= LSM_UNSAFE_SHARE;
	else
		p->fs->in_exec = 1;
	spin_unlock(&p->fs->lock);
}

/**
 * bprm_fill_uid - 处理可执行文件的SUID和SGID权限
 * @bprm: 二进制程序执行参数结构
 * @file: 可执行文件指针
 *
 * 检查并处理文件的SUID/SGID位设置：
 * - 验证挂载点是否允许SUID
 * - 检查当前进程是否禁用了新权限
 * - 如果设置了SUID/SGID位，更新bprm的用户ID和组ID
 * 用于实现特权程序的权限提升机制
 */
static void bprm_fill_uid(struct linux_binprm *bprm, struct file *file)
{
	/* Handle suid and sgid on files */
	struct inode *inode;
	unsigned int mode;
	kuid_t uid;
	kgid_t gid;

	if (!mnt_may_suid(file->f_path.mnt))
		return;

	if (task_no_new_privs(current))
		return;

	inode = file->f_path.dentry->d_inode;
	mode = READ_ONCE(inode->i_mode);
	if (!(mode & (S_ISUID|S_ISGID)))
		return;

	/* Be careful if suid/sgid is set */
	inode_lock(inode);

	/* reload atomically mode/uid/gid now that lock held */
	mode = inode->i_mode;
	uid = inode->i_uid;
	gid = inode->i_gid;
	inode_unlock(inode);

	/* We ignore suid/sgid if there are no mappings for them in the ns */
	if (!kuid_has_mapping(bprm->cred->user_ns, uid) ||
		 !kgid_has_mapping(bprm->cred->user_ns, gid))
		return;

	if (mode & S_ISUID) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->euid = uid;
	}

	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->egid = gid;
	}
}

/**
 * bprm_creds_from_file - 根据最终二进制文件计算凭证
 * @bprm: 二进制程序执行参数结构
 *
 * 基于要执行的文件计算进程凭证信息：
 * - 根据execfd_creds标志选择使用executable还是file
 * - 处理SUID/SGID权限设置
 * - 调用安全子系统进行权限验证
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int bprm_creds_from_file(struct linux_binprm *bprm)
{
	/* Compute creds based on which file? */
	struct file *file = bprm->execfd_creds ? bprm->executable : bprm->file;

	bprm_fill_uid(bprm, file);
	return security_bprm_creds_from_file(bprm, file);
}

/*
 * Fill the binprm structure from the inode.
 * Read the first BINPRM_BUF_SIZE bytes
 *
 * This may be called multiple times for binary chains (scripts for example).
 */
static /*
 * prepare_binprm - 准备二进制程序执行结构
 * @bprm: 二进制程序执行参数结构
 *
 * 初始化执行环境，包括：
 * - 读取可执行文件的前128字节用于格式识别
 * - 处理SUID/SGID权限设置
 * - 设置有效用户ID和组ID
 * - 检查安全策略和权限
 * 返回值：成功时返回0，失败时返回负错误码
 */
int prepare_binprm(struct linux_binprm *bprm)
{
	loff_t pos = 0;

	memset(bprm->buf, 0, BINPRM_BUF_SIZE);
	return kernel_read(bprm->file, bprm->buf, BINPRM_BUF_SIZE, &pos);
}

/**
 * remove_arg_zero - 移除参数列表的第一个参数
 * @bprm: 二进制程序执行参数结构
 *
 * 移除argv[0]参数，用于脚本执行等场景：
 * - 参数以'\0'分隔存储在bprm->p指向的位置
 * - 通过重定位bprm->p到第一个'\0'后面来移除第一个参数
 * - 相应减少argc计数
 * 返回值: 成功返回0，失败返回负的错误码
 */
int remove_arg_zero(struct linux_binprm *bprm)
{
	int ret = 0;
	unsigned long offset;
	char *kaddr;
	struct page *page;

	if (!bprm->argc)
		return 0;

	do {
		offset = bprm->p & ~PAGE_MASK;
		page = get_arg_page(bprm, bprm->p, 0);
		if (!page) {
			ret = -EFAULT;
			goto out;
		}
		kaddr = kmap_atomic(page);

		for (; offset < PAGE_SIZE && kaddr[offset];
				offset++, bprm->p++)
			;

		kunmap_atomic(kaddr);
		put_arg_page(page);
	} while (offset == PAGE_SIZE);

	bprm->p++;
	bprm->argc--;
	ret = 0;

out:
	return ret;
}
EXPORT_SYMBOL(remove_arg_zero);

#define printable(c) (((c)=='\t') || ((c)=='\n') || (0x20<=(c) && (c)<=0x7e))
/*
 * cycle the list of binary formats handler, until one recognizes the image
 */
static /*
 * search_binary_handler - 搜索和调用适当的二进制格式处理器
 * @bprm: 二进制程序执行参数结构
 *
 * 遍历已注册的二进制格式处理器链表，找到能够处理当前文件格式的处理器：
 * - 尝试ELF格式处理器
 * - 尝试脚本格式处理器(#!)
 * - 尝试其他注册的格式处理器
 * 每个处理器根据文件头部特征判断是否能处理该文件
 * 返回值：成功时返回0，失败时返回负错误码
 */
int search_binary_handler(struct linux_binprm *bprm)
{
	bool need_retry = IS_ENABLED(CONFIG_MODULES);
	struct linux_binfmt *fmt;
	int retval;

	retval = prepare_binprm(bprm);
	if (retval < 0)
		return retval;

	retval = security_bprm_check(bprm);
	if (retval)
		return retval;

	retval = -ENOENT;
 retry:
	read_lock(&binfmt_lock);
	list_for_each_entry(fmt, &formats, lh) {
		if (!try_module_get(fmt->module))
			continue;
		read_unlock(&binfmt_lock);

		retval = fmt->load_binary(bprm);

		read_lock(&binfmt_lock);
		put_binfmt(fmt);
		if (bprm->point_of_no_return || (retval != -ENOEXEC)) {
			read_unlock(&binfmt_lock);
			return retval;
		}
	}
	read_unlock(&binfmt_lock);

	if (need_retry) {
		if (printable(bprm->buf[0]) && printable(bprm->buf[1]) &&
		    printable(bprm->buf[2]) && printable(bprm->buf[3]))
			return retval;
		if (request_module("binfmt-%04x", *(ushort *)(bprm->buf + 2)) < 0)
			return retval;
		need_retry = false;
		goto retry;
	}

	return retval;
}

/*
 * exec_binprm - 执行二进制程序格式处理
 * @bprm: 二进制程序执行参数结构
 *
 * execve系统调用的核心执行函数，完成以下关键步骤：
 * - 调用search_binary_handler查找合适的格式处理器
 * - 处理递归执行（如脚本调用解释器）
 * - 管理执行深度限制防止无限递归
 * - 协调各个格式处理器的调用
 * 返回值：成功时返回0，失败时返回负错误码
 */
static int exec_binprm(struct linux_binprm *bprm)
{
	pid_t old_pid, old_vpid;
	int ret, depth;

	/* Need to fetch pid before load_binary changes it */
	old_pid = current->pid;
	rcu_read_lock();
	old_vpid = task_pid_nr_ns(current, task_active_pid_ns(current->parent));
	rcu_read_unlock();

	/* This allows 4 levels of binfmt rewrites before failing hard. */
	for (depth = 0;; depth++) {
		struct file *exec;
		if (depth > 5)
			return -ELOOP;

		ret = search_binary_handler(bprm);
		if (ret < 0)
			return ret;
		if (!bprm->interpreter)
			break;

		exec = bprm->file;
		bprm->file = bprm->interpreter;
		bprm->interpreter = NULL;

		allow_write_access(exec);
		if (unlikely(bprm->have_execfd)) {
			if (bprm->executable) {
				fput(exec);
				return -ENOEXEC;
			}
			bprm->executable = exec;
		} else
			fput(exec);
	}

	audit_bprm(bprm);
	trace_sched_process_exec(current, old_pid, bprm);
	ptrace_event(PTRACE_EVENT_EXEC, old_vpid);
	proc_exec_connector(current);
	return 0;
}

/**
 * bprm_execve - 执行二进制程序的主要函数
 * @bprm: 二进制程序执行参数结构
 * @fd: 文件描述符
 * @filename: 文件名结构
 * @flags: 执行标志
 *
 * execve系统调用的核心实现函数：
 * - 取消任何io_uring活动
 * - 准备执行凭证和安全检查
 * - 打开并验证可执行文件
 * - 调用exec_binprm执行二进制程序
 * - 处理执行成功和失败的清理工作
 * - 在不可回退点确保进程终止而不返回用户空间
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int bprm_execve(struct linux_binprm *bprm,
		       int fd, struct filename *filename, int flags)
{
	struct file *file;
	struct files_struct *displaced;
	int retval;

	/*
	 * Cancel any io_uring activity across execve
	 */
	io_uring_task_cancel();

	retval = unshare_files(&displaced);
	if (retval)
		return retval;

	retval = prepare_bprm_creds(bprm);
	if (retval)
		goto out_files;

	check_unsafe_exec(bprm);
	current->in_execve = 1;

	file = do_open_execat(fd, filename, flags);
	retval = PTR_ERR(file);
	if (IS_ERR(file))
		goto out_unmark;

	sched_exec();

	bprm->file = file;
	/*
	 * Record that a name derived from an O_CLOEXEC fd will be
	 * inaccessible after exec. Relies on having exclusive access to
	 * current->files (due to unshare_files above).
	 */
	if (bprm->fdpath &&
	    close_on_exec(fd, rcu_dereference_raw(current->files->fdt)))
		bprm->interp_flags |= BINPRM_FLAGS_PATH_INACCESSIBLE;

	/* Set the unchanging part of bprm->cred */
	retval = security_bprm_creds_for_exec(bprm);
	if (retval)
		goto out;

	retval = exec_binprm(bprm);
	if (retval < 0)
		goto out;

	/* execve succeeded */
	current->fs->in_exec = 0;
	current->in_execve = 0;
	rseq_execve(current);
	acct_update_integrals(current);
	task_numa_free(current, false);
	if (displaced)
		put_files_struct(displaced);
	return retval;

out:
	/*
	 * If past the point of no return ensure the the code never
	 * returns to the userspace process.  Use an existing fatal
	 * signal if present otherwise terminate the process with
	 * SIGSEGV.
	 */
	if (bprm->point_of_no_return && !fatal_signal_pending(current))
		force_sigsegv(SIGSEGV);

out_unmark:
	current->fs->in_exec = 0;
	current->in_execve = 0;

out_files:
	if (displaced)
		reset_files_struct(displaced);

	return retval;
}

/**
 * do_execveat_common - execveat系统调用的通用实现
 * @fd: 文件描述符，AT_FDCWD表示当前工作目录
 * @filename: 要执行的文件名
 * @argv: 命令行参数数组
 * @envp: 环境变量数组
 * @flags: 执行标志
 *
 * execve和execveat系统调用的核心实现：
 * - 检查进程数量限制(RLIMIT_NPROC)
 * - 分配并初始化bprm结构
 * - 统计和复制参数、环境变量
 * - 调用bprm_execve执行程序
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int do_execveat_common(int fd, struct filename *filename,
			      struct user_arg_ptr argv,
			      struct user_arg_ptr envp,
			      int flags)
{
	struct linux_binprm *bprm;
	int retval;

	if (IS_ERR(filename))
		return PTR_ERR(filename);

	/*
	 * We move the actual failure in case of RLIMIT_NPROC excess from
	 * set*uid() to execve() because too many poorly written programs
	 * don't check setuid() return code.  Here we additionally recheck
	 * whether NPROC limit is still exceeded.
	 */
	if ((current->flags & PF_NPROC_EXCEEDED) &&
	    atomic_read(&current_user()->processes) > rlimit(RLIMIT_NPROC)) {
		retval = -EAGAIN;
		goto out_ret;
	}

	/* We're below the limit (still or again), so we don't want to make
	 * further execve() calls fail. */
	current->flags &= ~PF_NPROC_EXCEEDED;

	bprm = alloc_bprm(fd, filename);
	if (IS_ERR(bprm)) {
		retval = PTR_ERR(bprm);
		goto out_ret;
	}

	retval = count(argv, MAX_ARG_STRINGS);
	if (retval < 0)
		goto out_free;
	bprm->argc = retval;

	retval = count(envp, MAX_ARG_STRINGS);
	if (retval < 0)
		goto out_free;
	bprm->envc = retval;

	retval = bprm_stack_limits(bprm);
	if (retval < 0)
		goto out_free;

	retval = copy_string_kernel(bprm->filename, bprm);
	if (retval < 0)
		goto out_free;
	bprm->exec = bprm->p;

	retval = copy_strings(bprm->envc, envp, bprm);
	if (retval < 0)
		goto out_free;

	retval = copy_strings(bprm->argc, argv, bprm);
	if (retval < 0)
		goto out_free;

	retval = bprm_execve(bprm, fd, filename, flags);
out_free:
	free_bprm(bprm);

out_ret:
	putname(filename);
	return retval;
}

/**
 * kernel_execve - 内核空间执行用户程序
 * @kernel_filename: 要执行的程序文件名（内核空间字符串）
 * @argv: 参数数组（内核空间指针）
 * @envp: 环境变量数组（内核空间指针）
 *
 * 供内核模块和内核线程调用的execve接口：
 * - 使用内核空间的文件名和参数
 * - 分配并初始化bprm结构
 * - 统计和复制内核空间的参数、环境变量
 * - 调用bprm_execve执行用户程序
 * 返回值: 成功返回0，失败返回负的错误码
 */
int kernel_execve(const char *kernel_filename,
		  const char *const *argv, const char *const *envp)
{
	struct filename *filename;
	struct linux_binprm *bprm;
	int fd = AT_FDCWD;
	int retval;

	filename = getname_kernel(kernel_filename);
	if (IS_ERR(filename))
		return PTR_ERR(filename);

	bprm = alloc_bprm(fd, filename);
	if (IS_ERR(bprm)) {
		retval = PTR_ERR(bprm);
		goto out_ret;
	}

	retval = count_strings_kernel(argv);
	if (retval < 0)
		goto out_free;
	bprm->argc = retval;

	retval = count_strings_kernel(envp);
	if (retval < 0)
		goto out_free;
	bprm->envc = retval;

	retval = bprm_stack_limits(bprm);
	if (retval < 0)
		goto out_free;

	retval = copy_string_kernel(bprm->filename, bprm);
	if (retval < 0)
		goto out_free;
	bprm->exec = bprm->p;

	retval = copy_strings_kernel(bprm->envc, envp, bprm);
	if (retval < 0)
		goto out_free;

	retval = copy_strings_kernel(bprm->argc, argv, bprm);
	if (retval < 0)
		goto out_free;

	retval = bprm_execve(bprm, fd, filename, 0);
out_free:
	free_bprm(bprm);
out_ret:
	putname(filename);
	return retval;
}

/**
 * do_execve - execve系统调用的实现
 * @filename: 要执行的文件名
 * @__argv: 用户空间参数数组指针
 * @__envp: 用户空间环境变量数组指针
 *
 * execve系统调用的内部实现，将用户空间指针包装后调用通用函数。
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int do_execve(struct filename *filename,
	const char __user *const __user *__argv,
	const char __user *const __user *__envp)
{
	struct user_arg_ptr argv = { .ptr.native = __argv };
	struct user_arg_ptr envp = { .ptr.native = __envp };
	return do_execveat_common(AT_FDCWD, filename, argv, envp, 0);
}

/**
 * do_execveat - execveat系统调用的实现
 * @fd: 目录文件描述符
 * @filename: 要执行的文件名
 * @__argv: 用户空间参数数组指针
 * @__envp: 用户空间环境变量数组指针
 * @flags: 执行标志
 *
 * execveat系统调用的内部实现，支持相对于指定目录执行程序。
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int do_execveat(int fd, struct filename *filename,
		const char __user *const __user *__argv,
		const char __user *const __user *__envp,
		int flags)
{
	struct user_arg_ptr argv = { .ptr.native = __argv };
	struct user_arg_ptr envp = { .ptr.native = __envp };

	return do_execveat_common(fd, filename, argv, envp, flags);
}

#ifdef CONFIG_COMPAT
/**
 * compat_do_execve - 32位兼容模式的execve实现
 * @filename: 要执行的文件名
 * @__argv: 32位用户空间参数数组指针
 * @__envp: 32位用户空间环境变量数组指针
 *
 * 为32位程序在64位系统上提供的execve兼容接口。
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int compat_do_execve(struct filename *filename,
	const compat_uptr_t __user *__argv,
	const compat_uptr_t __user *__envp)
{
	struct user_arg_ptr argv = {
		.is_compat = true,
		.ptr.compat = __argv,
	};
	struct user_arg_ptr envp = {
		.is_compat = true,
		.ptr.compat = __envp,
	};
	return do_execveat_common(AT_FDCWD, filename, argv, envp, 0);
}

/**
 * compat_do_execveat - 32位兼容模式的execveat实现
 * @fd: 目录文件描述符
 * @filename: 要执行的文件名
 * @__argv: 32位用户空间参数数组指针
 * @__envp: 32位用户空间环境变量数组指针
 * @flags: 执行标志
 *
 * 为32位程序在64位系统上提供的execveat兼容接口。
 * 返回值: 成功返回0，失败返回负的错误码
 */
static int compat_do_execveat(int fd, struct filename *filename,
			      const compat_uptr_t __user *__argv,
			      const compat_uptr_t __user *__envp,
			      int flags)
{
	struct user_arg_ptr argv = {
		.is_compat = true,
		.ptr.compat = __argv,
	};
	struct user_arg_ptr envp = {
		.is_compat = true,
		.ptr.compat = __envp,
	};
	return do_execveat_common(fd, filename, argv, envp, flags);
}
#endif

/**
 * set_binfmt - 设置当前进程的二进制格式处理器
 * @new: 新的二进制格式处理器
 *
 * 更新当前进程内存管理结构中的二进制格式处理器：
 * - 释放旧的格式处理器模块引用
 * - 设置新的格式处理器并增加模块引用计数
 * 用于在execve过程中记录加载程序的格式类型
 */
void set_binfmt(struct linux_binfmt *new)
{
	struct mm_struct *mm = current->mm;

	if (mm->binfmt)
		module_put(mm->binfmt->module);

	mm->binfmt = new;
	if (new)
		__module_get(new->module);
}
EXPORT_SYMBOL(set_binfmt);

/**
 * set_dumpable - 设置进程的core dump权限标志
 * @mm: 内存管理结构
 * @value: dump权限值 (SUID_DUMP_*)
 *
 * 将三值SUID_DUMP_*标志存储到mm->flags中：
 * - SUID_DUMP_DISABLE: 禁用core dump
 * - SUID_DUMP_USER: 允许用户dump
 * - SUID_DUMP_ROOT: 允许root dump
 */
void set_dumpable(struct mm_struct *mm, int value)
{
	if (WARN_ON((unsigned)value > SUID_DUMP_ROOT))
		return;

	set_mask_bits(&mm->flags, MMF_DUMPABLE_MASK, value);
}

/**
 * SYSCALL_DEFINE3(execve) - execve系统调用入口
 * @filename: 要执行的程序文件名
 * @argv: 命令行参数数组
 * @envp: 环境变量数组
 *
 * execve系统调用的入口点，用新程序替换当前进程映像。
 * 返回值: 成功时不返回，失败时返回负的错误码
 */
SYSCALL_DEFINE3(execve,
		const char __user *, filename,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp)
{
	return do_execve(getname(filename), argv, envp);
}

/**
 * SYSCALL_DEFINE5(execveat) - execveat系统调用入口
 * @fd: 目录文件描述符，AT_FDCWD表示当前工作目录
 * @filename: 要执行的程序文件名
 * @argv: 命令行参数数组
 * @envp: 环境变量数组
 * @flags: 执行标志（AT_EMPTY_PATH等）
 *
 * execveat系统调用的入口点，支持相对于指定目录执行程序。
 * 返回值: 成功时不返回，失败时返回负的错误码
 */
SYSCALL_DEFINE5(execveat,
		int, fd, const char __user *, filename,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp,
		int, flags)
{
	int lookup_flags = (flags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;

	return do_execveat(fd,
			   getname_flags(filename, lookup_flags, NULL),
			   argv, envp, flags);
}

#ifdef CONFIG_COMPAT
/**
 * COMPAT_SYSCALL_DEFINE3(execve) - 32位兼容模式execve系统调用
 * @filename: 要执行的程序文件名
 * @argv: 32位兼容模式参数数组
 * @envp: 32位兼容模式环境变量数组
 *
 * 为32位程序在64位系统上提供的execve系统调用入口。
 * 返回值: 成功时不返回，失败时返回负的错误码
 */
COMPAT_SYSCALL_DEFINE3(execve, const char __user *, filename,
	const compat_uptr_t __user *, argv,
	const compat_uptr_t __user *, envp)
{
	return compat_do_execve(getname(filename), argv, envp);
}

/**
 * COMPAT_SYSCALL_DEFINE5(execveat) - 32位兼容模式execveat系统调用
 * @fd: 目录文件描述符
 * @filename: 要执行的程序文件名
 * @argv: 32位兼容模式参数数组
 * @envp: 32位兼容模式环境变量数组
 * @flags: 执行标志
 *
 * 为32位程序在64位系统上提供的execveat系统调用入口。
 * 返回值: 成功时不返回，失败时返回负的错误码
 */
COMPAT_SYSCALL_DEFINE5(execveat, int, fd,
		       const char __user *, filename,
		       const compat_uptr_t __user *, argv,
		       const compat_uptr_t __user *, envp,
		       int,  flags)
{
	int lookup_flags = (flags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;

	return compat_do_execveat(fd,
				  getname_flags(filename, lookup_flags, NULL),
				  argv, envp, flags);
}
#endif
