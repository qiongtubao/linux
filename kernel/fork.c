// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/**
 * fork.c - Linux进程创建和管理模块
 *
 * 此文件包含Linux内核中进程创建的核心实现，主要负责：
 * 1. fork()系统调用的实现
 * 2. 进程结构体的复制和初始化
 * 3. 内核线程的创建
 * 4. 进程资源的管理和清理
 *
 * fork操作相对简单，但内存管理部分比较复杂
 * 详见 'mm/memory.c' 中的 'copy_page_range()' 函数
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_range()'
 */

#include <linux/anon_inodes.h>
#include <linux/slab.h>
#include <linux/sched/autogroup.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/sched/user.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/cputime.h>
#include <linux/seq_file.h>
#include <linux/rtmutex.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmacache.h>
#include <linux/nsproxy.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cgroup.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/seccomp.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/kthread.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/rcupdate.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/acct.h>
#include <linux/userfaultfd_k.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/freezer.h>
#include <linux/delayacct.h>
#include <linux/taskstats_kern.h>
#include <linux/random.h>
#include <linux/tty.h>
#include <linux/blkdev.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/perf_event.h>
#include <linux/posix-timers.h>
#include <linux/user-return-notifier.h>
#include <linux/oom.h>
#include <linux/khugepaged.h>
#include <linux/signalfd.h>
#include <linux/uprobes.h>
#include <linux/aio.h>
#include <linux/compiler.h>
#include <linux/sysctl.h>
#include <linux/kcov.h>
#include <linux/livepatch.h>
#include <linux/thread_info.h>
#include <linux/stackleak.h>
#include <linux/kasan.h>
#include <linux/scs.h>
#include <linux/io_uring.h>

#include <asm/pgalloc.h>
#include <linux/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include <trace/events/sched.h>

#define CREATE_TRACE_POINTS
#include <trace/events/task.h>

/*
 * Minimum number of threads to boot the kernel
 */
#define MIN_THREADS 20

/*
 * Maximum number of threads
 */
#define MAX_THREADS FUTEX_TID_MASK

/*
 * Protected counters by write_lock_irq(&tasklist_lock)
 */
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int nr_threads;			/* The idle threads do not count.. */

static int max_threads;		/* tunable limit on nr_threads */

#define NAMED_ARRAY_INDEX(x)	[x] = __stringify(x)

static const char * const resident_page_types[] = {
	NAMED_ARRAY_INDEX(MM_FILEPAGES),
	NAMED_ARRAY_INDEX(MM_ANONPAGES),
	NAMED_ARRAY_INDEX(MM_SWAPENTS),
	NAMED_ARRAY_INDEX(MM_SHMEMPAGES),
};

DEFINE_PER_CPU(unsigned long, process_counts) = 0;

__cacheline_aligned DEFINE_RWLOCK(tasklist_lock);  /* outer */

#ifdef CONFIG_PROVE_RCU
int lockdep_tasklist_lock_is_held(void)
{
	return lockdep_is_held(&tasklist_lock);
}
EXPORT_SYMBOL_GPL(lockdep_tasklist_lock_is_held);
#endif /* #ifdef CONFIG_PROVE_RCU */

int nr_processes(void)
{
	int cpu;
	int total = 0;

	for_each_possible_cpu(cpu)
		total += per_cpu(process_counts, cpu);

	return total;
}

void __weak arch_release_task_struct(struct task_struct *tsk)
{
}

#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
static struct kmem_cache *task_struct_cachep;

static inline struct task_struct *alloc_task_struct_node(int node)
{
	return kmem_cache_alloc_node(task_struct_cachep, GFP_KERNEL, node);
}

static inline void free_task_struct(struct task_struct *tsk)
{
	kmem_cache_free(task_struct_cachep, tsk);
}
#endif

#ifndef CONFIG_ARCH_THREAD_STACK_ALLOCATOR

/*
 * Allocate pages if THREAD_SIZE is >= PAGE_SIZE, otherwise use a
 * kmemcache based allocator.
 */
# if THREAD_SIZE >= PAGE_SIZE || defined(CONFIG_VMAP_STACK)

#ifdef CONFIG_VMAP_STACK
/*
 * vmalloc() is a bit slow, and calling vfree() enough times will force a TLB
 * flush.  Try to minimize the number of calls by caching stacks.
 */
#define NR_CACHED_STACKS 2
static DEFINE_PER_CPU(struct vm_struct *, cached_stacks[NR_CACHED_STACKS]);

static int free_vm_stack_cache(unsigned int cpu)
{
	struct vm_struct **cached_vm_stacks = per_cpu_ptr(cached_stacks, cpu);
	int i;

	for (i = 0; i < NR_CACHED_STACKS; i++) {
		struct vm_struct *vm_stack = cached_vm_stacks[i];

		if (!vm_stack)
			continue;

		vfree(vm_stack->addr);
		cached_vm_stacks[i] = NULL;
	}

	return 0;
}
#endif

static unsigned long *alloc_thread_stack_node(struct task_struct *tsk, int node)
{
#ifdef CONFIG_VMAP_STACK
	void *stack;
	int i;

	for (i = 0; i < NR_CACHED_STACKS; i++) {
		struct vm_struct *s;

		s = this_cpu_xchg(cached_stacks[i], NULL);

		if (!s)
			continue;

		/* Clear the KASAN shadow of the stack. */
		kasan_unpoison_shadow(s->addr, THREAD_SIZE);

		/* Clear stale pointers from reused stack. */
		memset(s->addr, 0, THREAD_SIZE);

		tsk->stack_vm_area = s;
		tsk->stack = s->addr;
		return s->addr;
	}

	/*
	 * Allocated stacks are cached and later reused by new threads,
	 * so memcg accounting is performed manually on assigning/releasing
	 * stacks to tasks. Drop __GFP_ACCOUNT.
	 */
	stack = __vmalloc_node_range(THREAD_SIZE, THREAD_ALIGN,
				     VMALLOC_START, VMALLOC_END,
				     THREADINFO_GFP & ~__GFP_ACCOUNT,
				     PAGE_KERNEL,
				     0, node, __builtin_return_address(0));

	/*
	 * We can't call find_vm_area() in interrupt context, and
	 * free_thread_stack() can be called in interrupt context,
	 * so cache the vm_struct.
	 */
	if (stack) {
		tsk->stack_vm_area = find_vm_area(stack);
		tsk->stack = stack;
	}
	return stack;
#else
	struct page *page = alloc_pages_node(node, THREADINFO_GFP,
					     THREAD_SIZE_ORDER);

	if (likely(page)) {
		tsk->stack = kasan_reset_tag(page_address(page));
		return tsk->stack;
	}
	return NULL;
#endif
}

static inline void free_thread_stack(struct task_struct *tsk)
{
#ifdef CONFIG_VMAP_STACK
	struct vm_struct *vm = task_stack_vm_area(tsk);

	if (vm) {
		int i;

		for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++)
			memcg_kmem_uncharge_page(vm->pages[i], 0);

		for (i = 0; i < NR_CACHED_STACKS; i++) {
			if (this_cpu_cmpxchg(cached_stacks[i],
					NULL, tsk->stack_vm_area) != NULL)
				continue;

			return;
		}

		vfree_atomic(tsk->stack);
		return;
	}
#endif

	__free_pages(virt_to_page(tsk->stack), THREAD_SIZE_ORDER);
}
# else
static struct kmem_cache *thread_stack_cache;

static unsigned long *alloc_thread_stack_node(struct task_struct *tsk,
						  int node)
{
	unsigned long *stack;
	stack = kmem_cache_alloc_node(thread_stack_cache, THREADINFO_GFP, node);
	stack = kasan_reset_tag(stack);
	tsk->stack = stack;
	return stack;
}

static void free_thread_stack(struct task_struct *tsk)
{
	kmem_cache_free(thread_stack_cache, tsk->stack);
}

void thread_stack_cache_init(void)
{
	thread_stack_cache = kmem_cache_create_usercopy("thread_stack",
					THREAD_SIZE, THREAD_SIZE, 0, 0,
					THREAD_SIZE, NULL);
	BUG_ON(thread_stack_cache == NULL);
}
# endif
#endif

/* SLAB cache for signal_struct structures (tsk->signal) */
static struct kmem_cache *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
struct kmem_cache *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
struct kmem_cache *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
struct kmem_cache *fs_cachep;

/* SLAB cache for vm_area_struct structures */
static struct kmem_cache *vm_area_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
static struct kmem_cache *mm_cachep;

struct vm_area_struct *vm_area_alloc(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	vma = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);
	if (vma)
		vma_init(vma, mm);
	return vma;
}

struct vm_area_struct *vm_area_dup(struct vm_area_struct *orig)
{
	struct vm_area_struct *new = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);

	if (new) {
		ASSERT_EXCLUSIVE_WRITER(orig->vm_flags);
		ASSERT_EXCLUSIVE_WRITER(orig->vm_file);
		/*
		 * orig->shared.rb may be modified concurrently, but the clone
		 * will be reinitialized.
		 */
		*new = data_race(*orig);
		INIT_LIST_HEAD(&new->anon_vma_chain);
		new->vm_next = new->vm_prev = NULL;
	}
	return new;
}

void vm_area_free(struct vm_area_struct *vma)
{
	kmem_cache_free(vm_area_cachep, vma);
}

static void account_kernel_stack(struct task_struct *tsk, int account)
{
	void *stack = task_stack_page(tsk);
	struct vm_struct *vm = task_stack_vm_area(tsk);


	/* All stack pages are in the same node. */
	if (vm)
		mod_lruvec_page_state(vm->pages[0], NR_KERNEL_STACK_KB,
				      account * (THREAD_SIZE / 1024));
	else
		mod_lruvec_slab_state(stack, NR_KERNEL_STACK_KB,
				      account * (THREAD_SIZE / 1024));
}

/**
 * memcg_charge_kernel_stack - 对内核栈进行内存控制组计费
 * @tsk: 目标任务结构体指针
 *
 * 为任务的内核栈页面计费到相应的内存控制组。
 * 在CONFIG_VMAP_STACK配置下，遍历栈的所有页面进行计费。
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
static int memcg_charge_kernel_stack(struct task_struct *tsk)
{
#ifdef CONFIG_VMAP_STACK
	struct vm_struct *vm = task_stack_vm_area(tsk);
	int ret;

	BUILD_BUG_ON(IS_ENABLED(CONFIG_VMAP_STACK) && PAGE_SIZE % 1024 != 0);

	if (vm) {
		int i;

		BUG_ON(vm->nr_pages != THREAD_SIZE / PAGE_SIZE);

		for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++) {
			/*
			 * If memcg_kmem_charge_page() fails, page->mem_cgroup
			 * pointer is NULL, and memcg_kmem_uncharge_page() in
			 * free_thread_stack() will ignore this page.
			 */
			/* 对栈的每个页面进行内存控制组计费 */
			ret = memcg_kmem_charge_page(vm->pages[i], GFP_KERNEL,
						     0);
			if (ret)
				return ret; /* 计费失败则返回错误 */
		}
	}
#endif
	return 0;
}

/**
 * release_task_stack - 释放任务栈内存
 * @tsk: 目标任务结构体指针
 *
 * 释放任务的内核栈内存，包括：
 * - 更新内核栈统计计数
 * - 释放栈物理内存
 * - 清理栈相关指针
 */
static void release_task_stack(struct task_struct *tsk)
{
	if (WARN_ON(tsk->state != TASK_DEAD))
		return;  /* Better to leak the stack than to free prematurely */

	account_kernel_stack(tsk, -1);	/* 更新内核栈统计 */
	free_thread_stack(tsk);		/* 释放线程栈内存 */
	tsk->stack = NULL;		/* 清空栈指针 */
#ifdef CONFIG_VMAP_STACK
	tsk->stack_vm_area = NULL;	/* 清空虚拟内存区域指针 */
#endif
}

#ifdef CONFIG_THREAD_INFO_IN_TASK
/**
 * put_task_stack - 减少任务栈引用计数
 * @tsk: 目标任务结构体指针
 *
 * 减少任务栈的引用计数，当计数降到0时释放栈内存。
 * 仅在CONFIG_THREAD_INFO_IN_TASK配置下使用。
 */
void put_task_stack(struct task_struct *tsk)
{
	if (refcount_dec_and_test(&tsk->stack_refcount))	/* 减少引用计数并检测是否为0 */
		release_task_stack(tsk);			/* 释放栈内存 */
}
#endif

/**
 * free_task - 释放任务结构体
 * @tsk: 要释放的任务结构体指针
 *
 * 释放task_struct结构体及其相关资源，包括：
 * - 影子调用栈(SCS)资源
 * - 任务栈内存
 * - 任务结构体本身的内存
 */
void free_task(struct task_struct *tsk)
{
	scs_release(tsk);	/* 释放影子调用栈 */

#ifndef CONFIG_THREAD_INFO_IN_TASK
	/*
	 * The task is finally done with both the stack and thread_info,
	 * so free both.
	 */
	release_task_stack(tsk);
#else
	/*
	 * If the task had a separate stack allocation, it should be gone
	 * by now.
	 */
	WARN_ON_ONCE(refcount_read(&tsk->stack_refcount) != 0);
#endif
	rt_mutex_debug_task_free(tsk);
	ftrace_graph_exit_task(tsk);
	arch_release_task_struct(tsk);
	if (tsk->flags & PF_KTHREAD)
		free_kthread_struct(tsk);
	free_task_struct(tsk);
}
EXPORT_SYMBOL(free_task);

#ifdef CONFIG_MMU
/**
 * dup_mmap - 复制内存映射区域
 * @mm: 新进程的内存描述符
 * @oldmm: 父进程的内存描述符
 *
 * 复制父进程的所有VMA(虚拟内存区域)到子进程，包括：
 * - 遍历父进程的所有VMA
 * - 复制VMA结构和权限
 * - 处理文件映射和匿名映射
 * - 设置写时复制(COW)标志
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
static __latent_entropy int dup_mmap(struct mm_struct *mm,
					struct mm_struct *oldmm)
{
	struct vm_area_struct *mpnt, *tmp, *prev, **pprev;
	struct rb_node **rb_link, *rb_parent;
	int retval;
	unsigned long charge;
	LIST_HEAD(uf);

	uprobe_start_dup_mmap();
	if (mmap_write_lock_killable(oldmm)) {
		retval = -EINTR;
		goto fail_uprobe_end;
	}
	flush_cache_dup_mm(oldmm);
	uprobe_dup_mmap(oldmm, mm);
	/*
	 * Not linked in yet - no deadlock potential:
	 */
	mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING);

	/* No ordering required: file already has been exposed. */
	RCU_INIT_POINTER(mm->exe_file, get_mm_exe_file(oldmm));

	mm->total_vm = oldmm->total_vm;
	mm->data_vm = oldmm->data_vm;
	mm->exec_vm = oldmm->exec_vm;
	mm->stack_vm = oldmm->stack_vm;

	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	pprev = &mm->mmap;
	retval = ksm_fork(mm, oldmm);
	if (retval)
		goto out;
	retval = khugepaged_fork(mm, oldmm);
	if (retval)
		goto out;

	prev = NULL;
	for (mpnt = oldmm->mmap; mpnt; mpnt = mpnt->vm_next) {
		struct file *file;

		if (mpnt->vm_flags & VM_DONTCOPY) {
			vm_stat_account(mm, mpnt->vm_flags, -vma_pages(mpnt));
			continue;
		}
		charge = 0;
		/*
		 * Don't duplicate many vmas if we've been oom-killed (for
		 * example)
		 */
		if (fatal_signal_pending(current)) {
			retval = -EINTR;
			goto out;
		}
		if (mpnt->vm_flags & VM_ACCOUNT) {
			unsigned long len = vma_pages(mpnt);

			if (security_vm_enough_memory_mm(oldmm, len)) /* sic */
				goto fail_nomem;
			charge = len;
		}
		tmp = vm_area_dup(mpnt);
		if (!tmp)
			goto fail_nomem;
		retval = vma_dup_policy(mpnt, tmp);
		if (retval)
			goto fail_nomem_policy;
		tmp->vm_mm = mm;
		retval = dup_userfaultfd(tmp, &uf);
		if (retval)
			goto fail_nomem_anon_vma_fork;
		if (tmp->vm_flags & VM_WIPEONFORK) {
			/*
			 * VM_WIPEONFORK gets a clean slate in the child.
			 * Don't prepare anon_vma until fault since we don't
			 * copy page for current vma.
			 */
			/* VM_WIPEONFORK标志：子进程获得干净的内存区域，不复制页面内容 */
			tmp->anon_vma = NULL;
		} else if (anon_vma_fork(tmp, mpnt))
			goto fail_nomem_anon_vma_fork;
		tmp->vm_flags &= ~(VM_LOCKED | VM_LOCKONFAULT);	/* 清除锁定标志 */
		file = tmp->vm_file;
		if (file) {
			struct inode *inode = file_inode(file);
			struct address_space *mapping = file->f_mapping;

			get_file(file);
			if (tmp->vm_flags & VM_DENYWRITE)
				put_write_access(inode);
			i_mmap_lock_write(mapping);
			if (tmp->vm_flags & VM_SHARED)
				mapping_allow_writable(mapping);
			flush_dcache_mmap_lock(mapping);
			/* insert tmp into the share list, just after mpnt */
			vma_interval_tree_insert_after(tmp, mpnt,
					&mapping->i_mmap);
			flush_dcache_mmap_unlock(mapping);
			i_mmap_unlock_write(mapping);
		}

		/*
		 * Clear hugetlb-related page reserves for children. This only
		 * affects MAP_PRIVATE mappings. Faults generated by the child
		 * are not guaranteed to succeed, even if read-only
		 */
		/* 清除子进程的大页预留，仅影响MAP_PRIVATE映射 */
		if (is_vm_hugetlb_page(tmp))
			reset_vma_resv_huge_pages(tmp);

		/*
		 * Link in the new vma and copy the page table entries.
		 */
		/* 将新VMA链接到链表中，并设置红黑树节点 */
		*pprev = tmp;
		pprev = &tmp->vm_next;
		tmp->vm_prev = prev;
		prev = tmp;

		__vma_link_rb(mm, tmp, rb_link, rb_parent);	/* 插入红黑树 */
		rb_link = &tmp->vm_rb.rb_right;
		rb_parent = &tmp->vm_rb;

		mm->map_count++;					/* 增加映射计数 */
		if (!(tmp->vm_flags & VM_WIPEONFORK))
			retval = copy_page_range(tmp, mpnt);		/* 复制页表项 */

		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);				/* 调用VMA的open操作 */

		if (retval)
			goto out;
	}
	/* a new mm has just been created */
	retval = arch_dup_mmap(oldmm, mm);
out:
	mmap_write_unlock(mm);
	flush_tlb_mm(oldmm);
	mmap_write_unlock(oldmm);
	dup_userfaultfd_complete(&uf);
fail_uprobe_end:
	uprobe_end_dup_mmap();
	return retval;
fail_nomem_anon_vma_fork:
	mpol_put(vma_policy(tmp));
fail_nomem_policy:
	vm_area_free(tmp);
fail_nomem:
	retval = -ENOMEM;
	vm_unacct_memory(charge);
	goto out;
}

/**
 * mm_alloc_pgd - 为内存描述符分配页目录
 * @mm: 内存描述符指针
 *
 * 为进程分配页全局目录(PGD)，这是进程页表的根目录。
 *
 * 返回值: 成功返回0，失败返回-ENOMEM
 */
static inline int mm_alloc_pgd(struct mm_struct *mm)
{
	mm->pgd = pgd_alloc(mm);	/* 分配页全局目录 */
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

/**
 * mm_free_pgd - 释放内存描述符的页目录
 * @mm: 内存描述符指针
 *
 * 释放进程的页全局目录(PGD)内存。
 */
static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);	/* 释放页全局目录 */
}
#else
static int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	mmap_write_lock(oldmm);
	RCU_INIT_POINTER(mm->exe_file, get_mm_exe_file(oldmm));
	mmap_write_unlock(oldmm);
	return 0;
}
#define mm_alloc_pgd(mm)	(0)
#define mm_free_pgd(mm)
#endif /* CONFIG_MMU */

/**
 * check_mm - 检查内存描述符的一致性
 * @mm: 要检查的内存描述符指针
 *
 * 在释放内存描述符之前检查其状态，确保：
 * - RSS统计计数器为零
 * - 页表字节数为零
 * - 没有遗留的大页PMD条目
 */
static void check_mm(struct mm_struct *mm)
{
	int i;

	BUILD_BUG_ON_MSG(ARRAY_SIZE(resident_page_types) != NR_MM_COUNTERS,
			 "Please make sure 'struct resident_page_types[]' is updated as well");

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = atomic_long_read(&mm->rss_stat.count[i]);

		if (unlikely(x))
			pr_alert("BUG: Bad rss-counter state mm:%p type:%s val:%ld\n",
				 mm, resident_page_types[i], x);	/* 检查RSS计数器异常 */
	}

	if (mm_pgtables_bytes(mm))
		pr_alert("BUG: non-zero pgtables_bytes on freeing mm: %ld\n",
				mm_pgtables_bytes(mm));		/* 检查页表字节数 */

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !USE_SPLIT_PMD_PTLOCKS
	VM_BUG_ON_MM(mm->pmd_huge_pte, mm);
#endif
}

#define allocate_mm()	(kmem_cache_alloc(mm_cachep, GFP_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
/**
 * __mmdrop - 释放内存描述符
 * @mm: 要释放的内存描述符指针
 *
 * 当内存描述符的最后一个引用被删除时调用，负责：
 * - 释放页目录
 * - 销毁MMU上下文
 * - 清理MMU通知器
 * - 释放用户命名空间引用
 * - 释放内存描述符本身
 */
void __mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	WARN_ON_ONCE(mm == current->mm);
	WARN_ON_ONCE(mm == current->active_mm);
	mm_free_pgd(mm);			/* 释放页全局目录 */
	destroy_context(mm);			/* 销毁MMU上下文 */
	mmu_notifier_subscriptions_destroy(mm);	/* 销毁MMU通知器订阅 */
	check_mm(mm);				/* 检查内存描述符一致性 */
	put_user_ns(mm->user_ns);		/* 释放用户命名空间引用 */
	free_mm(mm);				/* 释放内存描述符 */
}
EXPORT_SYMBOL_GPL(__mmdrop);

/**
 * mmdrop_async_fn - 异步释放内存描述符的工作函数
 * @work: 工作队列条目
 *
 * 在工作队列上下文中异步执行__mmdrop，避免在软中断上下文中
 * 执行可能不安全的pgd_dtor操作。
 */
static void mmdrop_async_fn(struct work_struct *work)
{
	struct mm_struct *mm;

	mm = container_of(work, struct mm_struct, async_put_work);
	__mmdrop(mm);				/* 异步执行内存描述符释放 */
}

/**
 * mmdrop_async - 异步减少内存描述符引用计数
 * @mm: 内存描述符指针
 *
 * 减少内存描述符的引用计数，如果计数降到0，则调度异步工作
 * 来释放内存描述符。这避免了在某些上下文中直接调用__mmdrop
 * 可能导致的问题。
 */
static void mmdrop_async(struct mm_struct *mm)
{
	if (unlikely(atomic_dec_and_test(&mm->mm_count))) {	/* 减少引用计数并检测 */
		INIT_WORK(&mm->async_put_work, mmdrop_async_fn);
		schedule_work(&mm->async_put_work);		/* 调度异步工作 */
	}
}

/**
 * free_signal_struct - 释放信号结构体
 * @sig: 要释放的信号结构体指针
 *
 * 释放进程组的信号结构体，包括：
 * - 任务统计信息
 * - 调度器自动分组
 * - OOM相关的内存描述符
 * - 信号结构体本身的内存
 */
static inline void free_signal_struct(struct signal_struct *sig)
{
	taskstats_tgid_free(sig);		/* 释放任务统计 */
	sched_autogroup_exit(sig);		/* 退出调度自动分组 */
	/*
	 * __mmdrop is not safe to call from softirq context on x86 due to
	 * pgd_dtor so postpone it to the async context
	 */
	/* 在x86上由于pgd_dtor，不能在软中断上下文调用__mmdrop，需异步处理 */
	if (sig->oom_mm)
		mmdrop_async(sig->oom_mm);	/* 异步释放OOM内存描述符 */
	kmem_cache_free(signal_cachep, sig);	/* 释放信号结构体 */
}

/**
 * put_signal_struct - 减少信号结构体引用计数
 * @sig: 信号结构体指针
 *
 * 减少信号结构体的引用计数，当计数降到0时释放结构体。
 * 用于进程组信号结构体的引用计数管理。
 */
static inline void put_signal_struct(struct signal_struct *sig)
{
	if (refcount_dec_and_test(&sig->sigcnt))	/* 减少引用计数并检测 */
		free_signal_struct(sig);		/* 释放信号结构体 */
}

/**
 * __put_task_struct - 释放任务结构体
 * @tsk: 要释放的任务结构体指针
 *
 * 当任务结构体的引用计数降到0时调用，负责释放所有相关资源：
 * - io_uring资源
 * - cgroup资源
 * - NUMA相关数据
 * - 安全上下文
 * - 凭证信息
 * - 延迟计费
 * - 信号结构体
 * - 最终释放任务结构体本身
 */
void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(!tsk->exit_state);
	WARN_ON(refcount_read(&tsk->usage));
	WARN_ON(tsk == current);

	io_uring_free(tsk);			/* 释放io_uring资源 */
	cgroup_free(tsk);			/* 释放cgroup资源 */
	task_numa_free(tsk, true);		/* 释放NUMA相关数据 */
	security_task_free(tsk);		/* 释放安全上下文 */
	exit_creds(tsk);			/* 退出凭证系统 */
	delayacct_tsk_free(tsk);		/* 释放延迟计费 */
	put_signal_struct(tsk->signal);		/* 释放信号结构体 */

	if (!profile_handoff_task(tsk))
		free_task(tsk);			/* 释放任务结构体 */
}
EXPORT_SYMBOL_GPL(__put_task_struct);

void __init __weak arch_task_cache_init(void) { }

/*
 * set_max_threads
 */
/**
 * set_max_threads - 设置系统最大线程数
 * @max_threads_suggested: 建议的最大线程数
 *
 * 根据系统内存大小计算并设置系统能支持的最大线程数。
 * 限制线程结构体只能消耗可用内存的一小部分，确保系统稳定性。
 */
static void set_max_threads(unsigned int max_threads_suggested)
{
	u64 threads;
	unsigned long nr_pages = totalram_pages();		/* 获取系统总页面数 */

	/*
	 * The number of threads shall be limited such that the thread
	 * structures may only consume a small part of the available memory.
	 */
	/* 限制线程数量，使线程结构体只消耗可用内存的一小部分 */
	if (fls64(nr_pages) + fls64(PAGE_SIZE) > 64)
		threads = MAX_THREADS;			/* 防止溢出，使用最大值 */
	else
		threads = div64_u64((u64) nr_pages * (u64) PAGE_SIZE,
				    (u64) THREAD_SIZE * 8UL);	/* 计算可支持的线程数 */

	if (threads > max_threads_suggested)
		threads = max_threads_suggested;		/* 限制在建议值内 */

	max_threads = clamp_t(u64, threads, MIN_THREADS, MAX_THREADS); /* 设置最终值 */
}

#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
/* Initialized by the architecture: */
int arch_task_struct_size __read_mostly;
#endif

#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
/**
 * task_struct_whitelist - 获取任务结构体的白名单区域
 * @offset: 返回白名单区域的偏移量
 * @size: 返回白名单区域的大小
 *
 * 获取task_struct中thread_struct的白名单区域信息，用于SLAB分配器
 * 的安全检查。如果白名单为空，则将偏移量设置为0。
 */
static void task_struct_whitelist(unsigned long *offset, unsigned long *size)
{
	/* Fetch thread_struct whitelist for the architecture. */
	arch_thread_struct_whitelist(offset, size);

	/*
	 * Handle zero-sized whitelist or empty thread_struct, otherwise
	 * adjust offset to position of thread_struct in task_struct.
	 */
	if (unlikely(*size == 0))
		*offset = 0;
	else
		*offset += offsetof(struct task_struct, thread);
}
#endif /* CONFIG_ARCH_TASK_STRUCT_ALLOCATOR */

/**
 * fork_init - 初始化fork系统
 *
 * 系统启动时初始化进程创建相关的数据结构和缓存，包括：
 * - 创建task_struct的SLAB缓存
 * - 初始化架构特定的任务缓存
 * - 设置系统最大线程数
 * - 设置进程和信号相关的资源限制
 * - 初始化栈缓存和其他子系统
 */
void __init fork_init(void)
{
	int i;
#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
#ifndef ARCH_MIN_TASKALIGN
#define ARCH_MIN_TASKALIGN	0
#endif
	int align = max_t(int, L1_CACHE_BYTES, ARCH_MIN_TASKALIGN);
	unsigned long useroffset, usersize;

	/* create a slab on which task_structs can be allocated */
	/* 创建用于分配task_struct的SLAB缓存 */
	task_struct_whitelist(&useroffset, &usersize);
	task_struct_cachep = kmem_cache_create_usercopy("task_struct",
			arch_task_struct_size, align,
			SLAB_PANIC|SLAB_ACCOUNT,
			useroffset, usersize, NULL);
#endif

	/* do the arch specific task caches init */
	arch_task_cache_init();				/* 初始化架构特定的任务缓存 */

	set_max_threads(MAX_THREADS);			/* 设置最大线程数 */

	init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads/2;	/* 设置进程数限制 */
	init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
	init_task.signal->rlim[RLIMIT_SIGPENDING] =
		init_task.signal->rlim[RLIMIT_NPROC];		/* 设置挂起信号限制 */

	for (i = 0; i < UCOUNT_COUNTS; i++) {
		init_user_ns.ucount_max[i] = max_threads/2;	/* 设置用户命名空间计数限制 */
	}

#ifdef CONFIG_VMAP_STACK
	cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, "fork:vm_stack_cache",
			  NULL, free_vm_stack_cache);		/* 设置栈缓存的CPU热插拔处理 */
#endif

	scs_init();					/* 初始化影子调用栈 */

	lockdep_init_task(&init_task);			/* 初始化lockdep */
	uprobes_init();					/* 初始化用户空间探针 */
}

/**
 * arch_dup_task_struct - 架构特定的任务结构体复制
 * @dst: 目标任务结构体
 * @src: 源任务结构体
 *
 * 执行架构特定的任务结构体复制操作。默认实现是简单的内存拷贝。
 * 各架构可以重写此函数来处理特殊的复制需求。
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
int __weak arch_dup_task_struct(struct task_struct *dst,
					       struct task_struct *src)
{
	*dst = *src;	/* 默认进行简单的结构体拷贝 */
	return 0;
}

/**
 * set_task_stack_end_magic - 设置栈结束魔数
 * @tsk: 目标任务结构体指针
 *
 * 在任务栈的末端设置魔数，用于检测栈溢出。
 * 这是一个重要的安全措施，帮助检测内核栈溢出问题。
 */
void set_task_stack_end_magic(struct task_struct *tsk)
{
	unsigned long *stackend;

	stackend = end_of_stack(tsk);
	*stackend = STACK_END_MAGIC;	/* for overflow detection */
}

/**
 * dup_task_struct - 复制任务结构体
 * @orig: 原始任务结构体
 * @node: NUMA节点ID
 *
 * 创建一个新的任务结构体，复制原始任务的内容。
 * 包括分配新的任务结构体、栈空间，并进行架构相关的复制操作。
 *
 * 返回值：成功返回新的任务结构体指针，失败返回NULL
 */
static struct task_struct *dup_task_struct(struct task_struct *orig, int node)
{
	struct task_struct *tsk;
	unsigned long *stack;
	struct vm_struct *stack_vm_area __maybe_unused;
	int err;

	if (node == NUMA_NO_NODE)
		node = tsk_fork_get_node(orig);		/* 获取合适的NUMA节点 */
	tsk = alloc_task_struct_node(node);		/* 分配任务结构体 */
	if (!tsk)
		return NULL;

	stack = alloc_thread_stack_node(tsk, node);	/* 分配线程栈 */
	if (!stack)
		goto free_tsk;

	if (memcg_charge_kernel_stack(tsk))		/* 对内核栈进行内存控制组计费 */
		goto free_stack;

	stack_vm_area = task_stack_vm_area(tsk);

	err = arch_dup_task_struct(tsk, orig);		/* 执行架构特定的复制 */

	/*
	 * arch_dup_task_struct() clobbers the stack-related fields.  Make
	 * sure they're properly initialized before using any stack-related
	 * functions again.
	 */
	/* arch_dup_task_struct会破坏栈相关字段，需要重新初始化 */
	tsk->stack = stack;				/* 恢复栈指针 */
#ifdef CONFIG_VMAP_STACK
	tsk->stack_vm_area = stack_vm_area;		/* 恢复栈虚拟内存区域 */
#endif
#ifdef CONFIG_THREAD_INFO_IN_TASK
	refcount_set(&tsk->stack_refcount, 1);		/* 设置栈引用计数 */
#endif

	if (err)
		goto free_stack;

	err = scs_prepare(tsk, node);			/* 准备影子调用栈 */
	if (err)
		goto free_stack;

#ifdef CONFIG_SECCOMP
	/*
	 * We must handle setting up seccomp filters once we're under
	 * the sighand lock in case orig has changed between now and
	 * then. Until then, filter must be NULL to avoid messing up
	 * the usage counts on the error path calling free_task.
	 */
	/* 暂时将seccomp过滤器设为NULL，稍后在信号锁下设置 */
	tsk->seccomp.filter = NULL;
#endif

	setup_thread_stack(tsk, orig);			/* 设置线程栈 */
	clear_user_return_notifier(tsk);		/* 清除用户返回通知器 */
	clear_tsk_need_resched(tsk);			/* 清除重新调度标志 */
	set_task_stack_end_magic(tsk);			/* 设置栈结束魔数 */

#ifdef CONFIG_STACKPROTECTOR
	tsk->stack_canary = get_random_canary();		/* 设置栈保护金丝雀值 */
#endif
	if (orig->cpus_ptr == &orig->cpus_mask)
		tsk->cpus_ptr = &tsk->cpus_mask;		/* 设置CPU亲和性指针 */

	/*
	 * One for the user space visible state that goes away when reaped.
	 * One for the scheduler.
	 */
	/* 设置引用计数：一个用于用户空间可见状态，一个用于调度器 */
	refcount_set(&tsk->rcu_users, 2);
	/* One for the rcu users */
	refcount_set(&tsk->usage, 1);			/* 设置使用计数 */
#ifdef CONFIG_BLK_DEV_IO_TRACE
	tsk->btrace_seq = 0;				/* 初始化块设备IO跟踪序号 */
#endif
	tsk->splice_pipe = NULL;			/* 初始化splice管道 */
	tsk->task_frag.page = NULL;			/* 初始化任务片段页面 */
	tsk->wake_q.next = NULL;			/* 初始化唤醒队列 */

	account_kernel_stack(tsk, 1);			/* 增加内核栈统计 */

	kcov_task_init(tsk);				/* 初始化内核代码覆盖率 */

#ifdef CONFIG_FAULT_INJECTION
	tsk->fail_nth = 0;				/* 初始化故障注入计数 */
#endif

#ifdef CONFIG_BLK_CGROUP
	tsk->throttle_queue = NULL;			/* 初始化节流队列 */
	tsk->use_memdelay = 0;				/* 初始化内存延迟使用标志 */
#endif

#ifdef CONFIG_MEMCG
	tsk->active_memcg = NULL;			/* 初始化活动内存控制组 */
#endif
	return tsk;

free_stack:
	free_thread_stack(tsk);				/* 释放线程栈 */
free_tsk:
	free_task_struct(tsk);				/* 释放任务结构体 */
	return NULL;
}

__cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

static unsigned long default_dump_filter = MMF_DUMP_FILTER_DEFAULT;

/**
 * coredump_filter_setup - 设置核心转储过滤器
 * @s: 过滤器参数字符串
 *
 * 内核启动参数处理函数，用于设置默认的核心转储过滤器。
 *
 * 返回值: 始终返回1
 */
static int __init coredump_filter_setup(char *s)
{
	default_dump_filter =
		(simple_strtoul(s, NULL, 0) << MMF_DUMP_FILTER_SHIFT) &
		MMF_DUMP_FILTER_MASK;		/* 解析并设置过滤器掩码 */
	return 1;
}

__setup("coredump_filter=", coredump_filter_setup);

#include <linux/init_task.h>

/**
 * mm_init_aio - 初始化内存描述符的AIO结构
 * @mm: 内存描述符指针
 *
 * 初始化内存描述符中与异步I/O相关的字段。
 */
static void mm_init_aio(struct mm_struct *mm)
{
#ifdef CONFIG_AIO
	spin_lock_init(&mm->ioctx_lock);	/* 初始化IO上下文锁 */
	mm->ioctx_table = NULL;			/* 初始化IO上下文表 */
#endif
}

/**
 * mm_clear_owner - 清除内存描述符的所有者
 * @mm: 内存描述符指针
 * @p: 当前所有者任务指针
 *
 * 如果指定的任务是内存描述符的所有者，则清除所有者字段。
 * 用于内存控制组(memcg)管理。
 */
static __always_inline void mm_clear_owner(struct mm_struct *mm,
					   struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	if (mm->owner == p)
		WRITE_ONCE(mm->owner, NULL);	/* 原子性地清除所有者 */
#endif
}

/**
 * mm_init_owner - 初始化内存描述符的所有者
 * @mm: 内存描述符指针
 * @p: 新所有者任务指针
 *
 * 设置内存描述符的所有者，用于内存控制组(memcg)管理。
 */
static void mm_init_owner(struct mm_struct *mm, struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	mm->owner = p;		/* 设置内存描述符所有者 */
#endif
}

/**
 * mm_init_uprobes_state - 初始化内存描述符的uprobes状态
 * @mm: 内存描述符指针
 *
 * 初始化内存描述符中与用户空间探针(uprobes)相关的状态。
 */
static void mm_init_uprobes_state(struct mm_struct *mm)
{
#ifdef CONFIG_UPROBES
	mm->uprobes_state.xol_area = NULL;	/* 初始化执行外区域 */
#endif
}

/**
 * mm_init - 初始化内存描述符
 * @mm: 要初始化的内存描述符
 * @p: 关联的任务结构体
 * @user_ns: 用户命名空间
 *
 * 初始化内存描述符的各个字段，包括：
 * - 内存映射相关结构
 * - 引用计数
 * - 锁和列表
 * - RSS统计
 * - 页表相关
 * - 各种标志位
 *
 * 返回值: 成功返回初始化后的mm，失败返回NULL
 */
static struct mm_struct *mm_init(struct mm_struct *mm, struct task_struct *p,
	struct user_namespace *user_ns)
{
	mm->mmap = NULL;				/* 初始化内存映射链表 */
	mm->mm_rb = RB_ROOT;				/* 初始化红黑树根 */
	mm->vmacache_seqnum = 0;			/* 初始化VMA缓存序号 */
	atomic_set(&mm->mm_users, 1);			/* 设置用户引用计数 */
	atomic_set(&mm->mm_count, 1);			/* 设置内核引用计数 */
	mmap_init_lock(mm);				/* 初始化mmap读写锁 */
	INIT_LIST_HEAD(&mm->mmlist);			/* 初始化mm列表头 */
	mm->core_state = NULL;				/* 初始化核心转储状态 */
	mm_pgtables_bytes_init(mm);			/* 初始化页表字节数 */
	mm->map_count = 0;				/* 初始化映射计数 */
	mm->locked_vm = 0;				/* 初始化锁定虚拟内存 */
	atomic_set(&mm->has_pinned, 0);			/* 初始化固定页面标志 */
	atomic64_set(&mm->pinned_vm, 0);		/* 初始化固定虚拟内存 */
	memset(&mm->rss_stat, 0, sizeof(mm->rss_stat));	/* 清零RSS统计 */
	spin_lock_init(&mm->page_table_lock);		/* 初始化页表锁 */
	spin_lock_init(&mm->arg_lock);			/* 初始化参数锁 */
	mm_init_cpumask(mm);				/* 初始化CPU掩码 */
	mm_init_aio(mm);				/* 初始化AIO */
	mm_init_owner(mm, p);				/* 初始化所有者 */
	RCU_INIT_POINTER(mm->exe_file, NULL);		/* 初始化可执行文件指针 */
	mmu_notifier_subscriptions_init(mm);		/* 初始化MMU通知订阅 */
	init_tlb_flush_pending(mm);			/* 初始化TLB刷新挂起状态 */
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !USE_SPLIT_PMD_PTLOCKS
	mm->pmd_huge_pte = NULL;			/* 初始化PMD大页PTE */
#endif
	mm_init_uprobes_state(mm);			/* 初始化uprobes状态 */

	if (current->mm) {
		mm->flags = current->mm->flags & MMF_INIT_MASK;		/* 从父进程继承标志 */
		mm->def_flags = current->mm->def_flags & VM_INIT_DEF_MASK; /* 继承默认VMA标志 */
	} else {
		mm->flags = default_dump_filter;	/* 使用默认转储过滤器 */
		mm->def_flags = 0;			/* 默认VMA标志为0 */
	}

	if (mm_alloc_pgd(mm))				/* 分配页全局目录 */
		goto fail_nopgd;

	if (init_new_context(p, mm))			/* 初始化新的MMU上下文 */
		goto fail_nocontext;

	mm->user_ns = get_user_ns(user_ns);		/* 获取用户命名空间引用 */
	return mm;

fail_nocontext:
	mm_free_pgd(mm);				/* 释放页全局目录 */
fail_nopgd:
	free_mm(mm);					/* 释放内存描述符 */
	return NULL;
}

/*
 * Allocate and initialize an mm_struct.
 */
/**
 * mm_alloc - 分配并初始化内存描述符
 *
 * 分配一个新的mm_struct并进行初始化，使用当前进程和
 * 当前用户命名空间作为参数。
 *
 * 返回值: 成功返回新的内存描述符，失败返回NULL
 */
struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm;

	mm = allocate_mm();			/* 从SLAB缓存分配mm_struct */
	if (!mm)
		return NULL;

	memset(mm, 0, sizeof(*mm));		/* 清零内存描述符 */
	return mm_init(mm, current, current_user_ns()); /* 初始化并返回 */
}

/**
 * __mmput - 实际执行内存描述符的释放工作
 * @mm: 要释放的内存描述符
 *
 * 当mm_users计数降到0时调用，负责释放所有相关资源：
 * - 清理uprobes状态
 * - 退出AIO、KSM、大页等子系统
 * - 释放内存映射
 * - 清理可执行文件引用
 * - 从全局列表中移除
 * - 释放二进制格式模块引用
 */
static inline void __mmput(struct mm_struct *mm)
{
	VM_BUG_ON(atomic_read(&mm->mm_users));

	uprobe_clear_state(mm);			/* 清理uprobes状态 */
	exit_aio(mm);				/* 退出AIO子系统 */
	ksm_exit(mm);				/* 退出KSM */
	khugepaged_exit(mm); /* must run before exit_mmap */	/* 退出大页折叠守护进程 */
	exit_mmap(mm);				/* 释放所有内存映射 */
	mm_put_huge_zero_page(mm);		/* 释放大页零页 */
	set_mm_exe_file(mm, NULL);		/* 清除可执行文件引用 */
	if (!list_empty(&mm->mmlist)) {
		spin_lock(&mmlist_lock);
		list_del(&mm->mmlist);		/* 从全局mm列表中移除 */
		spin_unlock(&mmlist_lock);
	}
	if (mm->binfmt)
		module_put(mm->binfmt->module);	/* 释放二进制格式模块 */
	mmdrop(mm);				/* 减少mm_count计数 */
}

/*
 * Decrement the use count and release all resources for an mm.
 */
/**
 * mmput - 减少内存描述符的使用计数
 * @mm: 内存描述符指针
 *
 * 减少mm_users计数，当计数降到0时调用__mmput释放所有资源。
 * 这是内存描述符用户引用的主要释放函数。
 */
void mmput(struct mm_struct *mm)
{
	might_sleep();

	if (atomic_dec_and_test(&mm->mm_users))	/* 减少用户计数并检测 */
		__mmput(mm);			/* 执行实际的释放工作 */
}
EXPORT_SYMBOL_GPL(mmput);

#ifdef CONFIG_MMU
/**
 * mmput_async_fn - 异步执行mmput的工作函数
 * @work: 工作队列条目
 *
 * 在工作队列上下文中异步执行__mmput，避免在某些上下文中
 * 直接调用可能导致的问题。
 */
static void mmput_async_fn(struct work_struct *work)
{
	struct mm_struct *mm = container_of(work, struct mm_struct,
					    async_put_work);

	__mmput(mm);			/* 异步执行内存描述符释放 */
}

/**
 * mmput_async - 异步减少内存描述符使用计数
 * @mm: 内存描述符指针
 *
 * 减少mm_users计数，如果计数降到0，则调度异步工作来释放
 * 内存描述符。用于在不适合直接调用__mmput的上下文中使用。
 */
void mmput_async(struct mm_struct *mm)
{
	if (atomic_dec_and_test(&mm->mm_users)) {	/* 减少用户计数并检测 */
		INIT_WORK(&mm->async_put_work, mmput_async_fn);
		schedule_work(&mm->async_put_work);	/* 调度异步释放工作 */
	}
}
#endif

/**
 * set_mm_exe_file - change a reference to the mm's executable file
 *
 * This changes mm's executable file (shown as symlink /proc/[pid]/exe).
 *
 * Main users are mmput() and sys_execve(). Callers prevent concurrent
 * invocations: in mmput() nobody alive left, in execve task is single
 * threaded. sys_prctl(PR_SET_MM_MAP/EXE_FILE) also needs to set the
 * mm->exe_file, but does so without using set_mm_exe_file() in order
 * to do avoid the need for any locks.
 */
/**
 * set_mm_exe_file - 更改内存描述符的可执行文件引用
 * @mm: 内存描述符指针
 * @new_exe_file: 新的可执行文件指针
 *
 * 更改内存描述符的可执行文件引用（显示为/proc/[pid]/exe符号链接）。
 * 主要调用者是mmput()和sys_execve()。调用者确保不会并发调用。
 */
void set_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	struct file *old_exe_file;

	/*
	 * It is safe to dereference the exe_file without RCU as
	 * this function is only called if nobody else can access
	 * this mm -- see comment above for justification.
	 */
	/* 安全地解引用exe_file，因为此时没有其他人能访问这个mm */
	old_exe_file = rcu_dereference_raw(mm->exe_file);

	if (new_exe_file)
		get_file(new_exe_file);		/* 增加新文件的引用计数 */
	rcu_assign_pointer(mm->exe_file, new_exe_file); /* 原子性地设置新文件 */
	if (old_exe_file)
		fput(old_exe_file);		/* 释放旧文件的引用 */
}

/**
 * get_mm_exe_file - acquire a reference to the mm's executable file
 *
 * Returns %NULL if mm has no associated executable file.
 * User must release file via fput().
 */
/**
 * get_mm_exe_file - 获取内存描述符可执行文件的引用
 * @mm: 内存描述符指针
 *
 * 获取内存描述符关联的可执行文件引用。
 * 如果mm没有关联的可执行文件则返回NULL。
 * 用户必须通过fput()释放文件引用。
 *
 * 返回值: 可执行文件指针或NULL
 */
struct file *get_mm_exe_file(struct mm_struct *mm)
{
	struct file *exe_file;

	rcu_read_lock();			/* 进入RCU读临界区 */
	exe_file = rcu_dereference(mm->exe_file);
	if (exe_file && !get_file_rcu(exe_file))	/* 在RCU保护下获取文件引用 */
		exe_file = NULL;
	rcu_read_unlock();				/* 退出RCU读临界区 */
	return exe_file;
}
EXPORT_SYMBOL(get_mm_exe_file);

/**
 * get_task_exe_file - acquire a reference to the task's executable file
 *
 * Returns %NULL if task's mm (if any) has no associated executable file or
 * this is a kernel thread with borrowed mm (see the comment above get_task_mm).
 * User must release file via fput().
 */
/**
 * get_task_exe_file - 获取任务可执行文件的引用
 * @task: 目标任务指针
 *
 * 获取任务关联的可执行文件引用。如果任务没有mm或者是内核线程，
 * 则返回NULL。用户必须通过fput()释放文件引用。
 *
 * 返回值: 可执行文件指针或NULL
 */
struct file *get_task_exe_file(struct task_struct *task)
{
	struct file *exe_file = NULL;
	struct mm_struct *mm;

	task_lock(task);			/* 锁定任务 */
	mm = task->mm;
	if (mm) {
		if (!(task->flags & PF_KTHREAD))	/* 非内核线程 */
			exe_file = get_mm_exe_file(mm);	/* 获取可执行文件 */
	}
	task_unlock(task);			/* 解锁任务 */
	return exe_file;
}
EXPORT_SYMBOL(get_task_exe_file);

/**
 * get_task_mm - acquire a reference to the task's mm
 *
 * Returns %NULL if the task has no mm.  Checks PF_KTHREAD (meaning
 * this kernel workthread has transiently adopted a user mm with use_mm,
 * to do its AIO) is not set and if so returns a reference to it, after
 * bumping up the use count.  User must release the mm via mmput()
 * after use.  Typically used by /proc and ptrace.
 */
/**
 * get_task_mm - 获取任务内存描述符的引用
 * @task: 目标任务指针
 *
 * 获取任务的内存描述符引用。如果任务没有mm或者是内核线程则返回NULL。
 * 检查PF_KTHREAD标志，确保不是临时借用用户mm的内核工作线程。
 * 用户必须通过mmput()释放mm引用。通常被/proc和ptrace使用。
 *
 * 返回值: 内存描述符指针或NULL
 */
struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	task_lock(task);			/* 锁定任务 */
	mm = task->mm;
	if (mm) {
		if (task->flags & PF_KTHREAD)
			mm = NULL;		/* 内核线程返回NULL */
		else
			mmget(mm);		/* 增加mm使用计数 */
	}
	task_unlock(task);			/* 解锁任务 */
	return mm;
}
EXPORT_SYMBOL_GPL(get_task_mm);

/**
 * mm_access - 安全访问任务的内存描述符
 * @task: 目标任务指针
 * @mode: 访问模式
 *
 * 安全地获取任务的内存描述符，包括权限检查。
 * 使用exec_update_mutex防止并发的exec操作。
 * 检查ptrace权限确保访问合法性。
 *
 * 返回值: 内存描述符指针或错误码
 */
struct mm_struct *mm_access(struct task_struct *task, unsigned int mode)
{
	struct mm_struct *mm;
	int err;

	err =  mutex_lock_killable(&task->signal->exec_update_mutex); /* 可中断地获取exec更新锁 */
	if (err)
		return ERR_PTR(err);

	mm = get_task_mm(task);			/* 获取任务的内存描述符 */
	if (mm && mm != current->mm &&
			!ptrace_may_access(task, mode)) { /* 检查ptrace访问权限 */
		mmput(mm);
		mm = ERR_PTR(-EACCES);		/* 访问被拒绝 */
	}
	mutex_unlock(&task->signal->exec_update_mutex); /* 释放exec更新锁 */

	return mm;
}

/**
 * complete_vfork_done - 完成vfork操作
 * @tsk: 子进程任务结构体指针
 *
 * 当子进程准备就绪时，通知等待中的父进程vfork操作已完成。
 * 清理vfork_done完成量并发出完成信号。
 */
static void complete_vfork_done(struct task_struct *tsk)
{
	struct completion *vfork;

	task_lock(tsk);				/* 锁定任务 */
	vfork = tsk->vfork_done;
	if (likely(vfork)) {
		tsk->vfork_done = NULL;		/* 清空vfork完成量 */
		complete(vfork);		/* 发出完成信号 */
	}
	task_unlock(tsk);			/* 解锁任务 */
}

/**
 * wait_for_vfork_done - 等待vfork子进程完成
 * @child: 子进程任务结构体指针
 * @vfork: vfork完成量指针
 *
 * 父进程等待vfork子进程完成exec或exit操作。
 * 在等待期间处理冰冻状态和信号中断。
 * 如果被信号中断，清理子进程的vfork_done字段。
 *
 * 返回值: 0表示正常完成，非0表示被信号中断
 */
static int wait_for_vfork_done(struct task_struct *child,
				struct completion *vfork)
{
	int killed;

	freezer_do_not_count();			/* 不计入冰冻器计数 */
	cgroup_enter_frozen();			/* 进入cgroup冰冻状态 */
	killed = wait_for_completion_killable(vfork); /* 可被信号中断的等待 */
	cgroup_leave_frozen(false);		/* 离开cgroup冰冻状态 */
	freezer_count();			/* 恢复冰冻器计数 */

	if (killed) {
		task_lock(child);
		child->vfork_done = NULL;	/* 清空子进程的vfork完成量 */
		task_unlock(child);
	}

	put_task_struct(child);			/* 释放子进程引用 */
	return killed;
}

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
/**
 * mm_release - 释放进程与内存描述符的关联
 * @tsk: 任务结构体指针
 * @mm: 内存描述符指针
 *
 * 当内存描述符从当前进程中移除后调用。与mmput不同，
 * mm_release专门处理进程退出时的清理工作：
 * - 释放uprobes任务
 * - 停用内存管理
 * - 处理child_tid清理
 * - 完成vfork操作
 */
static void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	uprobe_free_utask(tsk);			/* 释放uprobes用户任务 */

	/* Get rid of any cached register state */
	deactivate_mm(tsk, mm);			/* 停用内存管理，清除缓存的寄存器状态 */

	/*
	 * Signal userspace if we're not exiting with a core dump
	 * because we want to leave the value intact for debugging
	 * purposes.
	 */
	/* 如果不是因为核心转储退出，则通知用户空间 */
	if (tsk->clear_child_tid) {
		if (!(tsk->signal->flags & SIGNAL_GROUP_COREDUMP) &&
		    atomic_read(&mm->mm_users) > 1) {
			/*
			 * We don't check the error code - if userspace has
			 * not set up a proper pointer then tough luck.
			 */
			/* 清零child_tid并唤醒等待的进程 */
			put_user(0, tsk->clear_child_tid);
			do_futex(tsk->clear_child_tid, FUTEX_WAKE,
					1, NULL, NULL, 0, 0);
		}
		tsk->clear_child_tid = NULL;		/* 清空child_tid指针 */
	}

	/*
	 * All done, finally we can wake up parent and return this mm to him.
	 * Also kthread_stop() uses this completion for synchronization.
	 */
	/* 完成所有工作，唤醒父进程。kthread_stop()也使用此完成量进行同步 */
	if (tsk->vfork_done)
		complete_vfork_done(tsk);		/* 完成vfork操作 */
}

/**
 * exit_mm_release - 退出时释放内存管理
 * @tsk: 任务结构体指针
 * @mm: 内存描述符指针
 *
 * 进程退出时调用，先处理futex退出释放，然后调用mm_release。
 */
void exit_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exit_release(tsk);		/* 释放futex退出资源 */
	mm_release(tsk, mm);			/* 执行内存释放 */
}

/**
 * exec_mm_release - 执行exec时释放内存管理
 * @tsk: 任务结构体指针
 * @mm: 内存描述符指针
 *
 * 执行exec系统调用时调用，先处理futex exec释放，然后调用mm_release。
 */
void exec_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exec_release(tsk);		/* 释放futex exec资源 */
	mm_release(tsk, mm);			/* 执行内存释放 */
}

/**
 * dup_mm() - duplicates an existing mm structure
 * @tsk: the task_struct with which the new mm will be associated.
 * @oldmm: the mm to duplicate.
 *
 * Allocates a new mm structure and duplicates the provided @oldmm structure
 * content into it.
 *
 * Return: the duplicated mm or NULL on failure.
 */
/**
 * dup_mm - 复制现有的内存描述符结构
 * @tsk: 与新mm关联的任务结构体
 * @oldmm: 要复制的内存描述符
 *
 * 分配新的内存描述符结构并复制提供的@oldmm结构内容。
 * 包括内存映射、页表等的完整复制。
 *
 * 返回值: 复制的内存描述符或失败时返回NULL
 */
static struct mm_struct *dup_mm(struct task_struct *tsk,
				struct mm_struct *oldmm)
{
	struct mm_struct *mm;
	int err;

	mm = allocate_mm();
	if (!mm)
		goto fail_nomem;

	memcpy(mm, oldmm, sizeof(*mm));

	if (!mm_init(mm, tsk, mm->user_ns))
		goto fail_nomem;

	err = dup_mmap(mm, oldmm);
	if (err)
		goto free_pt;

	mm->hiwater_rss = get_mm_rss(mm);
	mm->hiwater_vm = mm->total_vm;

	if (mm->binfmt && !try_module_get(mm->binfmt->module))
		goto free_pt;

	return mm;

free_pt:
	/* don't put binfmt in mmput, we haven't got module yet */
	mm->binfmt = NULL;
	mm_init_owner(mm, NULL);
	mmput(mm);

fail_nomem:
	return NULL;
}

static int copy_mm(unsigned long clone_flags, struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm;
	int retval;

	tsk->min_flt = tsk->maj_flt = 0;
	tsk->nvcsw = tsk->nivcsw = 0;
#ifdef CONFIG_DETECT_HUNG_TASK
	tsk->last_switch_count = tsk->nvcsw + tsk->nivcsw;
	tsk->last_switch_time = 0;
#endif

	tsk->mm = NULL;
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 */
	oldmm = current->mm;
	if (!oldmm)
		return 0;

	/* initialize the new vmacache entries */
	vmacache_flush(tsk);

	if (clone_flags & CLONE_VM) {
		mmget(oldmm);
		mm = oldmm;
		goto good_mm;
	}

	retval = -ENOMEM;
	mm = dup_mm(tsk, current->mm);
	if (!mm)
		goto fail_nomem;

good_mm:
	tsk->mm = mm;
	tsk->active_mm = mm;
	return 0;

fail_nomem:
	return retval;
}

static int copy_fs(unsigned long clone_flags, struct task_struct *tsk)
{
	struct fs_struct *fs = current->fs;
	if (clone_flags & CLONE_FS) {
		/* tsk->fs is already what we want */
		spin_lock(&fs->lock);
		if (fs->in_exec) {
			spin_unlock(&fs->lock);
			return -EAGAIN;
		}
		fs->users++;
		spin_unlock(&fs->lock);
		return 0;
	}
	tsk->fs = copy_fs_struct(fs);
	if (!tsk->fs)
		return -ENOMEM;
	return 0;
}

static int copy_files(unsigned long clone_flags, struct task_struct *tsk)
{
	struct files_struct *oldf, *newf;
	int error = 0;

	/*
	 * A background process may not have any files ...
	 */
	oldf = current->files;
	if (!oldf)
		goto out;

	if (clone_flags & CLONE_FILES) {
		atomic_inc(&oldf->count);
		goto out;
	}

	newf = dup_fd(oldf, NR_OPEN_MAX, &error);
	if (!newf)
		goto out;

	tsk->files = newf;
	error = 0;
out:
	return error;
}

static int copy_io(unsigned long clone_flags, struct task_struct *tsk)
{
#ifdef CONFIG_BLOCK
	struct io_context *ioc = current->io_context;
	struct io_context *new_ioc;

	if (!ioc)
		return 0;
	/*
	 * Share io context with parent, if CLONE_IO is set
	 */
	if (clone_flags & CLONE_IO) {
		ioc_task_link(ioc);
		tsk->io_context = ioc;
	} else if (ioprio_valid(ioc->ioprio)) {
		new_ioc = get_task_io_context(tsk, GFP_KERNEL, NUMA_NO_NODE);
		if (unlikely(!new_ioc))
			return -ENOMEM;

		new_ioc->ioprio = ioc->ioprio;
		put_io_context(new_ioc);
	}
#endif
	return 0;
}

static int copy_sighand(unsigned long clone_flags, struct task_struct *tsk)
{
	struct sighand_struct *sig;

	if (clone_flags & CLONE_SIGHAND) {
		refcount_inc(&current->sighand->count);
		return 0;
	}
	sig = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
	RCU_INIT_POINTER(tsk->sighand, sig);
	if (!sig)
		return -ENOMEM;

	refcount_set(&sig->count, 1);
	spin_lock_irq(&current->sighand->siglock);
	memcpy(sig->action, current->sighand->action, sizeof(sig->action));
	spin_unlock_irq(&current->sighand->siglock);

	/* Reset all signal handler not set to SIG_IGN to SIG_DFL. */
	if (clone_flags & CLONE_CLEAR_SIGHAND)
		flush_signal_handlers(tsk, 0);

	return 0;
}

void __cleanup_sighand(struct sighand_struct *sighand)
{
	if (refcount_dec_and_test(&sighand->count)) {
		signalfd_cleanup(sighand);
		/*
		 * sighand_cachep is SLAB_TYPESAFE_BY_RCU so we can free it
		 * without an RCU grace period, see __lock_task_sighand().
		 */
		kmem_cache_free(sighand_cachep, sighand);
	}
}

/*
 * Initialize POSIX timer handling for a thread group.
 */
static void posix_cpu_timers_init_group(struct signal_struct *sig)
{
	struct posix_cputimers *pct = &sig->posix_cputimers;
	unsigned long cpu_limit;

	cpu_limit = READ_ONCE(sig->rlim[RLIMIT_CPU].rlim_cur);
	posix_cputimers_group_init(pct, cpu_limit);
}

static int copy_signal(unsigned long clone_flags, struct task_struct *tsk)
{
	struct signal_struct *sig;

	if (clone_flags & CLONE_THREAD)
		return 0;

	sig = kmem_cache_zalloc(signal_cachep, GFP_KERNEL);
	tsk->signal = sig;
	if (!sig)
		return -ENOMEM;

	sig->nr_threads = 1;
	atomic_set(&sig->live, 1);
	refcount_set(&sig->sigcnt, 1);

	/* list_add(thread_node, thread_head) without INIT_LIST_HEAD() */
	sig->thread_head = (struct list_head)LIST_HEAD_INIT(tsk->thread_node);
	tsk->thread_node = (struct list_head)LIST_HEAD_INIT(sig->thread_head);

	init_waitqueue_head(&sig->wait_chldexit);
	sig->curr_target = tsk;
	init_sigpending(&sig->shared_pending);
	INIT_HLIST_HEAD(&sig->multiprocess);
	seqlock_init(&sig->stats_lock);
	prev_cputime_init(&sig->prev_cputime);

#ifdef CONFIG_POSIX_TIMERS
	INIT_LIST_HEAD(&sig->posix_timers);
	hrtimer_init(&sig->real_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sig->real_timer.function = it_real_fn;
#endif

	task_lock(current->group_leader);
	memcpy(sig->rlim, current->signal->rlim, sizeof sig->rlim);
	task_unlock(current->group_leader);

	posix_cpu_timers_init_group(sig);

	tty_audit_fork(sig);
	sched_autogroup_fork(sig);

	sig->oom_score_adj = current->signal->oom_score_adj;
	sig->oom_score_adj_min = current->signal->oom_score_adj_min;

	mutex_init(&sig->cred_guard_mutex);
	mutex_init(&sig->exec_update_mutex);

	return 0;
}

static void copy_seccomp(struct task_struct *p)
{
#ifdef CONFIG_SECCOMP
	/*
	 * Must be called with sighand->lock held, which is common to
	 * all threads in the group. Holding cred_guard_mutex is not
	 * needed because this new task is not yet running and cannot
	 * be racing exec.
	 */
	assert_spin_locked(&current->sighand->siglock);

	/* Ref-count the new filter user, and assign it. */
	get_seccomp_filter(current);
	p->seccomp = current->seccomp;

	/*
	 * Explicitly enable no_new_privs here in case it got set
	 * between the task_struct being duplicated and holding the
	 * sighand lock. The seccomp state and nnp must be in sync.
	 */
	if (task_no_new_privs(current))
		task_set_no_new_privs(p);

	/*
	 * If the parent gained a seccomp mode after copying thread
	 * flags and between before we held the sighand lock, we have
	 * to manually enable the seccomp thread flag here.
	 */
	if (p->seccomp.mode != SECCOMP_MODE_DISABLED)
		set_tsk_thread_flag(p, TIF_SECCOMP);
#endif
}

SYSCALL_DEFINE1(set_tid_address, int __user *, tidptr)
{
	current->clear_child_tid = tidptr;

	return task_pid_vnr(current);
}

static void rt_mutex_init_task(struct task_struct *p)
{
	raw_spin_lock_init(&p->pi_lock);
#ifdef CONFIG_RT_MUTEXES
	p->pi_waiters = RB_ROOT_CACHED;
	p->pi_top_task = NULL;
	p->pi_blocked_on = NULL;
#endif
}

static inline void init_task_pid_links(struct task_struct *task)
{
	enum pid_type type;

	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type) {
		INIT_HLIST_NODE(&task->pid_links[type]);
	}
}

static inline void
init_task_pid(struct task_struct *task, enum pid_type type, struct pid *pid)
{
	if (type == PIDTYPE_PID)
		task->thread_pid = pid;
	else
		task->signal->pids[type] = pid;
}

static inline void rcu_copy_process(struct task_struct *p)
{
#ifdef CONFIG_PREEMPT_RCU
	p->rcu_read_lock_nesting = 0;
	p->rcu_read_unlock_special.s = 0;
	p->rcu_blocked_node = NULL;
	INIT_LIST_HEAD(&p->rcu_node_entry);
#endif /* #ifdef CONFIG_PREEMPT_RCU */
#ifdef CONFIG_TASKS_RCU
	p->rcu_tasks_holdout = false;
	INIT_LIST_HEAD(&p->rcu_tasks_holdout_list);
	p->rcu_tasks_idle_cpu = -1;
#endif /* #ifdef CONFIG_TASKS_RCU */
#ifdef CONFIG_TASKS_TRACE_RCU
	p->trc_reader_nesting = 0;
	p->trc_reader_special.s = 0;
	INIT_LIST_HEAD(&p->trc_holdout_list);
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */
}

struct pid *pidfd_pid(const struct file *file)
{
	if (file->f_op == &pidfd_fops)
		return file->private_data;

	return ERR_PTR(-EBADF);
}

static int pidfd_release(struct inode *inode, struct file *file)
{
	struct pid *pid = file->private_data;

	file->private_data = NULL;
	put_pid(pid);
	return 0;
}

#ifdef CONFIG_PROC_FS
/**
 * pidfd_show_fdinfo - print information about a pidfd
 * @m: proc fdinfo file
 * @f: file referencing a pidfd
 *
 * Pid:
 * This function will print the pid that a given pidfd refers to in the
 * pid namespace of the procfs instance.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its pid. This is
 * similar to calling getppid() on a process whose parent is outside of
 * its pid namespace.
 *
 * NSpid:
 * If pid namespaces are supported then this function will also print
 * the pid of a given pidfd refers to for all descendant pid namespaces
 * starting from the current pid namespace of the instance, i.e. the
 * Pid field and the first entry in the NSpid field will be identical.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its first NSpid
 * entry and no others will be shown.
 * Note that this differs from the Pid and NSpid fields in
 * /proc/<pid>/status where Pid and NSpid are always shown relative to
 * the  pid namespace of the procfs instance. The difference becomes
 * obvious when sending around a pidfd between pid namespaces from a
 * different branch of the tree, i.e. where no ancestoral relation is
 * present between the pid namespaces:
 * - create two new pid namespaces ns1 and ns2 in the initial pid
 *   namespace (also take care to create new mount namespaces in the
 *   new pid namespace and mount procfs)
 * - create a process with a pidfd in ns1
 * - send pidfd from ns1 to ns2
 * - read /proc/self/fdinfo/<pidfd> and observe that both Pid and NSpid
 *   have exactly one entry, which is 0
 */
static void pidfd_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct pid *pid = f->private_data;
	struct pid_namespace *ns;
	pid_t nr = -1;

	if (likely(pid_has_task(pid, PIDTYPE_PID))) {
		ns = proc_pid_ns(file_inode(m->file)->i_sb);
		nr = pid_nr_ns(pid, ns);
	}

	seq_put_decimal_ll(m, "Pid:\t", nr);

#ifdef CONFIG_PID_NS
	seq_put_decimal_ll(m, "\nNSpid:\t", nr);
	if (nr > 0) {
		int i;

		/* If nr is non-zero it means that 'pid' is valid and that
		 * ns, i.e. the pid namespace associated with the procfs
		 * instance, is in the pid namespace hierarchy of pid.
		 * Start at one below the already printed level.
		 */
		for (i = ns->level + 1; i <= pid->level; i++)
			seq_put_decimal_ll(m, "\t", pid->numbers[i].nr);
	}
#endif
	seq_putc(m, '\n');
}
#endif

/*
 * Poll support for process exit notification.
 */
static __poll_t pidfd_poll(struct file *file, struct poll_table_struct *pts)
{
	struct pid *pid = file->private_data;
	__poll_t poll_flags = 0;

	poll_wait(file, &pid->wait_pidfd, pts);

	/*
	 * Inform pollers only when the whole thread group exits.
	 * If the thread group leader exits before all other threads in the
	 * group, then poll(2) should block, similar to the wait(2) family.
	 */
	if (thread_group_exited(pid))
		poll_flags = EPOLLIN | EPOLLRDNORM;

	return poll_flags;
}

const struct file_operations pidfd_fops = {
	.release = pidfd_release,
	.poll = pidfd_poll,
#ifdef CONFIG_PROC_FS
	.show_fdinfo = pidfd_show_fdinfo,
#endif
};

static void __delayed_free_task(struct rcu_head *rhp)
{
	struct task_struct *tsk = container_of(rhp, struct task_struct, rcu);

	free_task(tsk);
}

static __always_inline void delayed_free_task(struct task_struct *tsk)
{
	if (IS_ENABLED(CONFIG_MEMCG))
		call_rcu(&tsk->rcu, __delayed_free_task);
	else
		free_task(tsk);
}

static void copy_oom_score_adj(u64 clone_flags, struct task_struct *tsk)
{
	/* Skip if kernel thread */
	if (!tsk->mm)
		return;

	/* Skip if spawning a thread or using vfork */
	if ((clone_flags & (CLONE_VM | CLONE_THREAD | CLONE_VFORK)) != CLONE_VM)
		return;

	/* We need to synchronize with __set_oom_adj */
	mutex_lock(&oom_adj_mutex);
	set_bit(MMF_MULTIPROCESS, &tsk->mm->flags);
	/* Update the values in case they were changed after copy_signal */
	tsk->signal->oom_score_adj = current->signal->oom_score_adj;
	tsk->signal->oom_score_adj_min = current->signal->oom_score_adj_min;
	mutex_unlock(&oom_adj_mutex);
}

/*
 * This creates a new process as a copy of the old one,
 * but does not actually start it yet.
 *
 * It copies the registers, and all the appropriate
 * parts of the process environment (as per the clone
 * flags). The actual kick-off is left to the caller.
 */
/**
 * copy_process - 复制进程的核心实现
 * @pid: 进程PID结构体
 * @trace: 跟踪标志
 * @node: NUMA节点ID
 * @args: 内核克隆参数结构体
 *
 * 创建一个新进程作为旧进程的副本，但不会实际启动它。
 * 复制寄存器和进程环境的所有适当部分（根据clone标志）。
 * 实际的启动由调用者负责。
 *
 * 这是fork/clone操作的核心函数，负责：
 * 1. 复制任务结构体
 * 2. 设置进程属性和资源
 * 3. 复制各种子系统状态（内存、文件、信号等）
 * 4. 初始化新进程的运行环境
 *
 * 返回值：成功返回新任务结构体指针，失败返回错误码
 */
static __latent_entropy struct task_struct *copy_process(
					struct pid *pid,
					int trace,
					int node,
					struct kernel_clone_args *args)
{
	int pidfd = -1, retval;
	struct task_struct *p;
	struct multiprocess_signals delayed;
	struct file *pidfile = NULL;
	u64 clone_flags = args->flags;
	struct nsproxy *nsp = current->nsproxy;

	/*
	 * Don't allow sharing the root directory with processes in a different
	 * namespace
	 */
	if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
		return ERR_PTR(-EINVAL);

	if ((clone_flags & (CLONE_NEWUSER|CLONE_FS)) == (CLONE_NEWUSER|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);

	/*
	 * Siblings of global init remain as zombies on exit since they are
	 * not reaped by their parent (swapper). To solve this and to avoid
	 * multi-rooted process trees, prevent global and container-inits
	 * from creating siblings.
	 */
	if ((clone_flags & CLONE_PARENT) &&
				current->signal->flags & SIGNAL_UNKILLABLE)
		return ERR_PTR(-EINVAL);

	/*
	 * If the new process will be in a different pid or user namespace
	 * do not allow it to share a thread group with the forking task.
	 */
	if (clone_flags & CLONE_THREAD) {
		if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWPID)) ||
		    (task_active_pid_ns(current) != nsp->pid_ns_for_children))
			return ERR_PTR(-EINVAL);
	}

	/*
	 * If the new process will be in a different time namespace
	 * do not allow it to share VM or a thread group with the forking task.
	 */
	if (clone_flags & (CLONE_THREAD | CLONE_VM)) {
		if (nsp->time_ns != nsp->time_ns_for_children)
			return ERR_PTR(-EINVAL);
	}

	if (clone_flags & CLONE_PIDFD) {
		/*
		 * - CLONE_DETACHED is blocked so that we can potentially
		 *   reuse it later for CLONE_PIDFD.
		 * - CLONE_THREAD is blocked until someone really needs it.
		 */
		if (clone_flags & (CLONE_DETACHED | CLONE_THREAD))
			return ERR_PTR(-EINVAL);
	}

	/*
	 * Force any signals received before this point to be delivered
	 * before the fork happens.  Collect up signals sent to multiple
	 * processes that happen during the fork and delay them so that
	 * they appear to happen after the fork.
	 */
	sigemptyset(&delayed.signal);
	INIT_HLIST_NODE(&delayed.node);

	spin_lock_irq(&current->sighand->siglock);
	if (!(clone_flags & CLONE_THREAD))
		hlist_add_head(&delayed.node, &current->signal->multiprocess);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	retval = -ERESTARTNOINTR;
	if (signal_pending(current))
		goto fork_out;

	retval = -ENOMEM;
	p = dup_task_struct(current, node);
	if (!p)
		goto fork_out;

	/*
	 * This _must_ happen before we call free_task(), i.e. before we jump
	 * to any of the bad_fork_* labels. This is to avoid freeing
	 * p->set_child_tid which is (ab)used as a kthread's data pointer for
	 * kernel threads (PF_KTHREAD).
	 */
	p->set_child_tid = (clone_flags & CLONE_CHILD_SETTID) ? args->child_tid : NULL;
	/*
	 * Clear TID on mm_release()?
	 */
	p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? args->child_tid : NULL;

	ftrace_graph_init_task(p);

	rt_mutex_init_task(p);

	lockdep_assert_irqs_enabled();
#ifdef CONFIG_PROVE_LOCKING
	DEBUG_LOCKS_WARN_ON(!p->softirqs_enabled);
#endif
	retval = -EAGAIN;
	if (atomic_read(&p->real_cred->user->processes) >=
			task_rlimit(p, RLIMIT_NPROC)) {
		if (p->real_cred->user != INIT_USER &&
		    !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN))
			goto bad_fork_free;
	}
	current->flags &= ~PF_NPROC_EXCEEDED;

	retval = copy_creds(p, clone_flags);
	if (retval < 0)
		goto bad_fork_free;

	/*
	 * If multiple threads are within copy_process(), then this check
	 * triggers too late. This doesn't hurt, the check is only there
	 * to stop root fork bombs.
	 */
	retval = -EAGAIN;
	if (data_race(nr_threads >= max_threads))
		goto bad_fork_cleanup_count;

	delayacct_tsk_init(p);	/* Must remain after dup_task_struct() */
	p->flags &= ~(PF_SUPERPRIV | PF_WQ_WORKER | PF_IDLE);
	p->flags |= PF_FORKNOEXEC;
	INIT_LIST_HEAD(&p->children);
	INIT_LIST_HEAD(&p->sibling);
	rcu_copy_process(p);
	p->vfork_done = NULL;
	spin_lock_init(&p->alloc_lock);

	init_sigpending(&p->pending);

	p->utime = p->stime = p->gtime = 0;
#ifdef CONFIG_ARCH_HAS_SCALED_CPUTIME
	p->utimescaled = p->stimescaled = 0;
#endif
	prev_cputime_init(&p->prev_cputime);

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	seqcount_init(&p->vtime.seqcount);
	p->vtime.starttime = 0;
	p->vtime.state = VTIME_INACTIVE;
#endif

#ifdef CONFIG_IO_URING
	p->io_uring = NULL;
#endif

#if defined(SPLIT_RSS_COUNTING)
	memset(&p->rss_stat, 0, sizeof(p->rss_stat));
#endif

	p->default_timer_slack_ns = current->timer_slack_ns;

#ifdef CONFIG_PSI
	p->psi_flags = 0;
#endif

	task_io_accounting_init(&p->ioac);
	acct_clear_integrals(p);

	posix_cputimers_init(&p->posix_cputimers);

	p->io_context = NULL;
	audit_set_context(p, NULL);
	cgroup_fork(p);
#ifdef CONFIG_NUMA
	p->mempolicy = mpol_dup(p->mempolicy);
	if (IS_ERR(p->mempolicy)) {
		retval = PTR_ERR(p->mempolicy);
		p->mempolicy = NULL;
		goto bad_fork_cleanup_threadgroup_lock;
	}
#endif
#ifdef CONFIG_CPUSETS
	p->cpuset_mem_spread_rotor = NUMA_NO_NODE;
	p->cpuset_slab_spread_rotor = NUMA_NO_NODE;
	seqcount_spinlock_init(&p->mems_allowed_seq, &p->alloc_lock);
#endif
#ifdef CONFIG_TRACE_IRQFLAGS
	memset(&p->irqtrace, 0, sizeof(p->irqtrace));
	p->irqtrace.hardirq_disable_ip	= _THIS_IP_;
	p->irqtrace.softirq_enable_ip	= _THIS_IP_;
	p->softirqs_enabled		= 1;
	p->softirq_context		= 0;
#endif

	p->pagefault_disabled = 0;

#ifdef CONFIG_LOCKDEP
	lockdep_init_task(p);
#endif

#ifdef CONFIG_DEBUG_MUTEXES
	p->blocked_on = NULL; /* not blocked yet */
#endif
#ifdef CONFIG_BCACHE
	p->sequential_io	= 0;
	p->sequential_io_avg	= 0;
#endif

	/* Perform scheduler related setup. Assign this task to a CPU. */
	retval = sched_fork(clone_flags, p);	/* 调度器相关设置，分配CPU */
	if (retval)
		goto bad_fork_cleanup_policy;

	retval = perf_event_init_task(p);	/* 初始化性能事件 */
	if (retval)
		goto bad_fork_cleanup_policy;
	retval = audit_alloc(p);	/* 分配审计结构 */
	if (retval)
		goto bad_fork_cleanup_perf;
	/* copy all the process information */
	shm_init_task(p);	/* 初始化共享内存 */
	retval = security_task_alloc(p, clone_flags);	/* 安全模块任务分配 */
	if (retval)
		goto bad_fork_cleanup_audit;
	retval = copy_semundo(clone_flags, p);	/* 复制信号量撤销信息 */
	if (retval)
		goto bad_fork_cleanup_security;
	retval = copy_files(clone_flags, p);	/* 复制文件描述符表 */
	if (retval)
		goto bad_fork_cleanup_semundo;
	retval = copy_fs(clone_flags, p);	/* 复制文件系统信息 */
	if (retval)
		goto bad_fork_cleanup_files;
	retval = copy_sighand(clone_flags, p);	/* 复制信号处理函数 */
	if (retval)
		goto bad_fork_cleanup_fs;
	retval = copy_signal(clone_flags, p);	/* 复制信号结构 */
	if (retval)
		goto bad_fork_cleanup_sighand;
	retval = copy_mm(clone_flags, p);	/* 复制内存管理结构 */
	if (retval)
		goto bad_fork_cleanup_signal;
	retval = copy_namespaces(clone_flags, p);	/* 复制命名空间 */
	if (retval)
		goto bad_fork_cleanup_mm;
	retval = copy_io(clone_flags, p);	/* 复制IO上下文 */
	if (retval)
		goto bad_fork_cleanup_namespaces;
	retval = copy_thread(clone_flags, args->stack, args->stack_size, p, args->tls);	/* 复制线程特定信息 */
	if (retval)
		goto bad_fork_cleanup_io;

	stackleak_task_init(p);

	if (pid != &init_struct_pid) {
		pid = alloc_pid(p->nsproxy->pid_ns_for_children, args->set_tid,
				args->set_tid_size);
		if (IS_ERR(pid)) {
			retval = PTR_ERR(pid);
			goto bad_fork_cleanup_thread;
		}
	}

	/*
	 * This has to happen after we've potentially unshared the file
	 * descriptor table (so that the pidfd doesn't leak into the child
	 * if the fd table isn't shared).
	 */
	if (clone_flags & CLONE_PIDFD) {
		retval = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
		if (retval < 0)
			goto bad_fork_free_pid;

		pidfd = retval;

		pidfile = anon_inode_getfile("[pidfd]", &pidfd_fops, pid,
					      O_RDWR | O_CLOEXEC);
		if (IS_ERR(pidfile)) {
			put_unused_fd(pidfd);
			retval = PTR_ERR(pidfile);
			goto bad_fork_free_pid;
		}
		get_pid(pid);	/* held by pidfile now */

		retval = put_user(pidfd, args->pidfd);
		if (retval)
			goto bad_fork_put_pidfd;
	}

#ifdef CONFIG_BLOCK
	p->plug = NULL;
#endif
	futex_init_task(p);

	/*
	 * sigaltstack should be cleared when sharing the same VM
	 */
	if ((clone_flags & (CLONE_VM|CLONE_VFORK)) == CLONE_VM)
		sas_ss_reset(p);	/* 共享VM时清除信号栈 */

	/*
	 * Syscall tracing and stepping should be turned off in the
	 * child regardless of CLONE_PTRACE.
	 */
	user_disable_single_step(p);	/* 禁用单步调试 */
	clear_tsk_thread_flag(p, TIF_SYSCALL_TRACE);	/* 清除系统调用跟踪标志 */
#ifdef TIF_SYSCALL_EMU
	clear_tsk_thread_flag(p, TIF_SYSCALL_EMU);	/* 清除系统调用模拟标志 */
#endif
	clear_tsk_latency_tracing(p);	/* 清除延迟跟踪 */

	/* ok, now we should be set up.. */
	p->pid = pid_nr(pid);	/* 设置进程ID */
	if (clone_flags & CLONE_THREAD) {
		p->group_leader = current->group_leader;	/* 设置线程组组长 */
		p->tgid = current->tgid;	/* 设置线程组ID */
	} else {
		p->group_leader = p;	/* 进程自己是组长 */
		p->tgid = p->pid;	/* 进程组ID等于进程ID */
	}

	p->nr_dirtied = 0;
	p->nr_dirtied_pause = 128 >> (PAGE_SHIFT - 10);
	p->dirty_paused_when = 0;

	p->pdeath_signal = 0;
	INIT_LIST_HEAD(&p->thread_group);
	p->task_works = NULL;

	/*
	 * Ensure that the cgroup subsystem policies allow the new process to be
	 * forked. It should be noted that the new process's css_set can be changed
	 * between here and cgroup_post_fork() if an organisation operation is in
	 * progress.
	 */
	retval = cgroup_can_fork(p, args);
	if (retval)
		goto bad_fork_put_pidfd;

	/*
	 * From this point on we must avoid any synchronous user-space
	 * communication until we take the tasklist-lock. In particular, we do
	 * not want user-space to be able to predict the process start-time by
	 * stalling fork(2) after we recorded the start_time but before it is
	 * visible to the system.
	 */

	p->start_time = ktime_get_ns();
	p->start_boottime = ktime_get_boottime_ns();

	/*
	 * Make it visible to the rest of the system, but dont wake it up yet.
	 * Need tasklist lock for parent etc handling!
	 */
	write_lock_irq(&tasklist_lock);	/* 获取任务列表写锁 */

	/* CLONE_PARENT re-uses the old parent */
	if (clone_flags & (CLONE_PARENT|CLONE_THREAD)) {
		p->real_parent = current->real_parent;	/* 使用当前进程的父进程 */
		p->parent_exec_id = current->parent_exec_id;	/* 继承父进程执行ID */
		if (clone_flags & CLONE_THREAD)
			p->exit_signal = -1;	/* 线程不发送退出信号 */
		else
			p->exit_signal = current->group_leader->exit_signal;	/* 使用组长的退出信号 */
	} else {
		p->real_parent = current;	/* 当前进程是父进程 */
		p->parent_exec_id = current->self_exec_id;	/* 使用当前进程的执行ID */
		p->exit_signal = args->exit_signal;	/* 使用指定的退出信号 */
	}

	klp_copy_process(p);

	spin_lock(&current->sighand->siglock);

	/*
	 * Copy seccomp details explicitly here, in case they were changed
	 * before holding sighand lock.
	 */
	copy_seccomp(p);

	rseq_fork(p, clone_flags);

	/* Don't start children in a dying pid namespace */
	if (unlikely(!(ns_of_pid(pid)->pid_allocated & PIDNS_ADDING))) {
		retval = -ENOMEM;
		goto bad_fork_cancel_cgroup;
	}

	/* Let kill terminate clone/fork in the middle */
	if (fatal_signal_pending(current)) {
		retval = -EINTR;
		goto bad_fork_cancel_cgroup;
	}

	/* past the last point of failure */
	if (pidfile)
		fd_install(pidfd, pidfile);

	init_task_pid_links(p);
	if (likely(p->pid)) {
		ptrace_init_task(p, (clone_flags & CLONE_PTRACE) || trace);

		init_task_pid(p, PIDTYPE_PID, pid);
		if (thread_group_leader(p)) {
			init_task_pid(p, PIDTYPE_TGID, pid);
			init_task_pid(p, PIDTYPE_PGID, task_pgrp(current));
			init_task_pid(p, PIDTYPE_SID, task_session(current));

			if (is_child_reaper(pid)) {
				ns_of_pid(pid)->child_reaper = p;
				p->signal->flags |= SIGNAL_UNKILLABLE;
			}
			p->signal->shared_pending.signal = delayed.signal;
			p->signal->tty = tty_kref_get(current->signal->tty);
			/*
			 * Inherit has_child_subreaper flag under the same
			 * tasklist_lock with adding child to the process tree
			 * for propagate_has_child_subreaper optimization.
			 */
			p->signal->has_child_subreaper = p->real_parent->signal->has_child_subreaper ||
							 p->real_parent->signal->is_child_subreaper;
			list_add_tail(&p->sibling, &p->real_parent->children);
			list_add_tail_rcu(&p->tasks, &init_task.tasks);
			attach_pid(p, PIDTYPE_TGID);
			attach_pid(p, PIDTYPE_PGID);
			attach_pid(p, PIDTYPE_SID);
			__this_cpu_inc(process_counts);
		} else {
			current->signal->nr_threads++;
			atomic_inc(&current->signal->live);
			refcount_inc(&current->signal->sigcnt);
			task_join_group_stop(p);
			list_add_tail_rcu(&p->thread_group,
					  &p->group_leader->thread_group);
			list_add_tail_rcu(&p->thread_node,
					  &p->signal->thread_head);
		}
		attach_pid(p, PIDTYPE_PID);
		nr_threads++;
	}
	total_forks++;
	hlist_del_init(&delayed.node);
	spin_unlock(&current->sighand->siglock);
	syscall_tracepoint_update(p);
	write_unlock_irq(&tasklist_lock);

	proc_fork_connector(p);
	sched_post_fork(p);
	cgroup_post_fork(p, args);
	perf_event_fork(p);

	trace_task_newtask(p, clone_flags);
	uprobe_copy_process(p, clone_flags);

	copy_oom_score_adj(clone_flags, p);

	return p;

bad_fork_cancel_cgroup:
	spin_unlock(&current->sighand->siglock);
	write_unlock_irq(&tasklist_lock);
	cgroup_cancel_fork(p, args);
bad_fork_put_pidfd:
	if (clone_flags & CLONE_PIDFD) {
		fput(pidfile);
		put_unused_fd(pidfd);
	}
bad_fork_free_pid:
	if (pid != &init_struct_pid)
		free_pid(pid);
bad_fork_cleanup_thread:
	exit_thread(p);
bad_fork_cleanup_io:
	if (p->io_context)
		exit_io_context(p);
bad_fork_cleanup_namespaces:
	exit_task_namespaces(p);
bad_fork_cleanup_mm:
	if (p->mm) {
		mm_clear_owner(p->mm, p);
		mmput(p->mm);
	}
bad_fork_cleanup_signal:
	if (!(clone_flags & CLONE_THREAD))
		free_signal_struct(p->signal);
bad_fork_cleanup_sighand:
	__cleanup_sighand(p->sighand);
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */
bad_fork_cleanup_semundo:
	exit_sem(p);
bad_fork_cleanup_security:
	security_task_free(p);
bad_fork_cleanup_audit:
	audit_free(p);
bad_fork_cleanup_perf:
	perf_event_free_task(p);
bad_fork_cleanup_policy:
	lockdep_free_task(p);
#ifdef CONFIG_NUMA
	mpol_put(p->mempolicy);
bad_fork_cleanup_threadgroup_lock:
#endif
	delayacct_tsk_free(p);
bad_fork_cleanup_count:
	atomic_dec(&p->cred->user->processes);
	exit_creds(p);
bad_fork_free:
	p->state = TASK_DEAD;
	put_task_stack(p);
	delayed_free_task(p);
fork_out:
	spin_lock_irq(&current->sighand->siglock);
	hlist_del_init(&delayed.node);
	spin_unlock_irq(&current->sighand->siglock);
	return ERR_PTR(retval);
}

static inline void init_idle_pids(struct task_struct *idle)
{
	enum pid_type type;

	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type) {
		INIT_HLIST_NODE(&idle->pid_links[type]); /* not really needed */
		init_task_pid(idle, type, &init_struct_pid);
	}
}

/**
 * fork_idle - 为指定CPU创建idle进程
 * @cpu: 目标CPU编号
 *
 * 为指定的CPU创建一个idle进程。idle进程是每个CPU的特殊进程，
 * 当没有其他进程需要运行时，CPU会运行这个进程。
 *
 * 返回值：成功返回新的idle任务结构体指针，失败返回错误指针
 */
struct task_struct *fork_idle(int cpu)
{
	struct task_struct *task;
	struct kernel_clone_args args = {
		.flags = CLONE_VM,
	};

	task = copy_process(&init_struct_pid, 0, cpu_to_node(cpu), &args);
	if (!IS_ERR(task)) {
		init_idle_pids(task);
		init_idle(task, cpu);
	}

	return task;
}

struct mm_struct *copy_init_mm(void)
{
	return dup_mm(NULL, &init_mm);
}

/*
 *  Ok, this is the main fork-routine.
 *
 * It copies the process, and if successful kick-starts
 * it and waits for it to finish using the VM if required.
 *
 * args->exit_signal is expected to be checked for sanity by the caller.
 */
/**
 * kernel_clone - 内核克隆函数，fork系统调用的主要实现
 * @args: 克隆参数结构体
 *
 * 这是主要的fork例程。复制进程，如果成功则启动它，
 * 并在需要时等待其完成VM的使用。
 *
 * 此函数处理所有类型的进程创建：
 * - fork(): 创建完整的新进程
 * - vfork(): 创建共享内存的进程
 * - clone(): 创建线程或自定义共享的进程
 * - clone3(): 扩展的clone调用
 *
 * 调用者应该已经检查了args->exit_signal的合理性。
 *
 * 返回值：成功返回新进程的PID，失败返回负的错误码
 */
pid_t kernel_clone(struct kernel_clone_args *args)
{
	u64 clone_flags = args->flags;
	struct completion vfork;
	struct pid *pid;
	struct task_struct *p;
	int trace = 0;
	pid_t nr;

	/*
	 * For legacy clone() calls, CLONE_PIDFD uses the parent_tid argument
	 * to return the pidfd. Hence, CLONE_PIDFD and CLONE_PARENT_SETTID are
	 * mutually exclusive. With clone3() CLONE_PIDFD has grown a separate
	 * field in struct clone_args and it still doesn't make sense to have
	 * them both point at the same memory location. Performing this check
	 * here has the advantage that we don't need to have a separate helper
	 * to check for legacy clone().
	 */
	if ((args->flags & CLONE_PIDFD) &&
	    (args->flags & CLONE_PARENT_SETTID) &&
	    (args->pidfd == args->parent_tid))
		return -EINVAL;

	/*
	 * Determine whether and which event to report to ptracer.  When
	 * called from kernel_thread or CLONE_UNTRACED is explicitly
	 * requested, no event is reported; otherwise, report if the event
	 * for the type of forking is enabled.
	 */
	if (!(clone_flags & CLONE_UNTRACED)) {
		if (clone_flags & CLONE_VFORK)
			trace = PTRACE_EVENT_VFORK;	/* vfork事件 */
		else if (args->exit_signal != SIGCHLD)
			trace = PTRACE_EVENT_CLONE;	/* clone事件 */
		else
			trace = PTRACE_EVENT_FORK;	/* fork事件 */

		if (likely(!ptrace_event_enabled(current, trace)))
			trace = 0;	/* 跟踪未启用 */
	}

	p = copy_process(NULL, trace, NUMA_NO_NODE, args);	/* 复制进程 */
	add_latent_entropy();	/* 添加熵值 */

	if (IS_ERR(p))
		return PTR_ERR(p);

	/*
	 * Do this prior waking up the new thread - the thread pointer
	 * might get invalid after that point, if the thread exits quickly.
	 */
	trace_sched_process_fork(current, p);	/* 调度跟踪 */

	pid = get_task_pid(p, PIDTYPE_PID);	/* 获取进程PID */
	nr = pid_vnr(pid);	/* 获取虚拟PID号 */

	if (clone_flags & CLONE_PARENT_SETTID)
		put_user(nr, args->parent_tid);	/* 设置父进程TID */

	if (clone_flags & CLONE_VFORK) {
		p->vfork_done = &vfork;	/* 设置vfork完成标志 */
		init_completion(&vfork);	/* 初始化完成量 */
		get_task_struct(p);	/* 增加任务引用计数 */
	}

	wake_up_new_task(p);	/* 唤醒新任务 */

	/* forking complete and child started to run, tell ptracer */
	if (unlikely(trace))
		ptrace_event_pid(trace, pid);	/* 通知跟踪器 */

	if (clone_flags & CLONE_VFORK) {
		if (!wait_for_vfork_done(p, &vfork))	/* 等待vfork完成 */
			ptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
	}

	put_pid(pid);	/* 释放PID引用 */
	return nr;	/* 返回进程号 */
}

/*
 * Create a kernel thread.
 */
/**
 * kernel_thread - 创建内核线程
 * @fn: 线程函数指针
 * @arg: 传递给线程函数的参数
 * @flags: 创建标志
 *
 * 创建一个在内核空间运行的线程。内核线程与用户进程不同：
 * - 没有用户空间内存映射
 * - 共享内核虚拟内存空间
 * - 通常用于内核后台任务
 *
 * 返回值：成功返回新线程的PID，失败返回负的错误码
 */
pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
	struct kernel_clone_args args = {
		.flags		= ((lower_32_bits(flags) | CLONE_VM |
				    CLONE_UNTRACED) & ~CSIGNAL),
		.exit_signal	= (lower_32_bits(flags) & CSIGNAL),
		.stack		= (unsigned long)fn,
		.stack_size	= (unsigned long)arg,
	};

	return kernel_clone(&args);
}

#ifdef __ARCH_WANT_SYS_FORK
/**
 * sys_fork - fork系统调用
 *
 * 创建一个新进程，新进程是当前进程的完整副本。
 * 子进程继承父进程的所有资源，但拥有独立的地址空间。
 *
 * 返回值：
 * - 在父进程中返回子进程的PID
 * - 在子进程中返回0
 * - 失败时返回负的错误码
 */
SYSCALL_DEFINE0(fork)
{
#ifdef CONFIG_MMU
	struct kernel_clone_args args = {
		.exit_signal = SIGCHLD,
	};

	return kernel_clone(&args);
#else
	/* can not support in nommu mode */
	return -EINVAL;
#endif
}
#endif

#ifdef __ARCH_WANT_SYS_VFORK
/**
 * sys_vfork - vfork系统调用
 *
 * 创建一个新进程，但与fork不同的是：
 * 1. 子进程与父进程共享内存空间
 * 2. 父进程被挂起，直到子进程调用exec或exit
 * 3. 主要用于立即执行新程序的场景，避免不必要的内存复制
 *
 * 返回值：
 * - 在父进程中返回子进程的PID
 * - 在子进程中返回0
 * - 失败时返回负的错误码
 */
SYSCALL_DEFINE0(vfork)
{
	struct kernel_clone_args args = {
		.flags		= CLONE_VFORK | CLONE_VM,
		.exit_signal	= SIGCHLD,
	};

	return kernel_clone(&args);
}
#endif

#ifdef __ARCH_WANT_SYS_CLONE
#ifdef CONFIG_CLONE_BACKWARDS
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 unsigned long, tls,
		 int __user *, child_tidptr)
#elif defined(CONFIG_CLONE_BACKWARDS2)
SYSCALL_DEFINE5(clone, unsigned long, newsp, unsigned long, clone_flags,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#elif defined(CONFIG_CLONE_BACKWARDS3)
SYSCALL_DEFINE6(clone, unsigned long, clone_flags, unsigned long, newsp,
		int, stack_size,
		int __user *, parent_tidptr,
		int __user *, child_tidptr,
		unsigned long, tls)
#else
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#endif
{
	struct kernel_clone_args args = {
		.flags		= (lower_32_bits(clone_flags) & ~CSIGNAL),
		.pidfd		= parent_tidptr,
		.child_tid	= child_tidptr,
		.parent_tid	= parent_tidptr,
		.exit_signal	= (lower_32_bits(clone_flags) & CSIGNAL),
		.stack		= newsp,
		.tls		= tls,
	};

	return kernel_clone(&args);
}
#endif

#ifdef __ARCH_WANT_SYS_CLONE3

noinline static int copy_clone_args_from_user(struct kernel_clone_args *kargs,
					      struct clone_args __user *uargs,
					      size_t usize)
{
	int err;
	struct clone_args args;
	pid_t *kset_tid = kargs->set_tid;

	BUILD_BUG_ON(offsetofend(struct clone_args, tls) !=
		     CLONE_ARGS_SIZE_VER0);
	BUILD_BUG_ON(offsetofend(struct clone_args, set_tid_size) !=
		     CLONE_ARGS_SIZE_VER1);
	BUILD_BUG_ON(offsetofend(struct clone_args, cgroup) !=
		     CLONE_ARGS_SIZE_VER2);
	BUILD_BUG_ON(sizeof(struct clone_args) != CLONE_ARGS_SIZE_VER2);

	if (unlikely(usize > PAGE_SIZE))
		return -E2BIG;
	if (unlikely(usize < CLONE_ARGS_SIZE_VER0))
		return -EINVAL;

	err = copy_struct_from_user(&args, sizeof(args), uargs, usize);
	if (err)
		return err;

	if (unlikely(args.set_tid_size > MAX_PID_NS_LEVEL))
		return -EINVAL;

	if (unlikely(!args.set_tid && args.set_tid_size > 0))
		return -EINVAL;

	if (unlikely(args.set_tid && args.set_tid_size == 0))
		return -EINVAL;

	/*
	 * Verify that higher 32bits of exit_signal are unset and that
	 * it is a valid signal
	 */
	if (unlikely((args.exit_signal & ~((u64)CSIGNAL)) ||
		     !valid_signal(args.exit_signal)))
		return -EINVAL;

	if ((args.flags & CLONE_INTO_CGROUP) &&
	    (args.cgroup > INT_MAX || usize < CLONE_ARGS_SIZE_VER2))
		return -EINVAL;

	*kargs = (struct kernel_clone_args){
		.flags		= args.flags,
		.pidfd		= u64_to_user_ptr(args.pidfd),
		.child_tid	= u64_to_user_ptr(args.child_tid),
		.parent_tid	= u64_to_user_ptr(args.parent_tid),
		.exit_signal	= args.exit_signal,
		.stack		= args.stack,
		.stack_size	= args.stack_size,
		.tls		= args.tls,
		.set_tid_size	= args.set_tid_size,
		.cgroup		= args.cgroup,
	};

	if (args.set_tid &&
		copy_from_user(kset_tid, u64_to_user_ptr(args.set_tid),
			(kargs->set_tid_size * sizeof(pid_t))))
		return -EFAULT;

	kargs->set_tid = kset_tid;

	return 0;
}

/**
 * clone3_stack_valid - check and prepare stack
 * @kargs: kernel clone args
 *
 * Verify that the stack arguments userspace gave us are sane.
 * In addition, set the stack direction for userspace since it's easy for us to
 * determine.
 */
static inline bool clone3_stack_valid(struct kernel_clone_args *kargs)
{
	if (kargs->stack == 0) {
		if (kargs->stack_size > 0)
			return false;
	} else {
		if (kargs->stack_size == 0)
			return false;

		if (!access_ok((void __user *)kargs->stack, kargs->stack_size))
			return false;

#if !defined(CONFIG_STACK_GROWSUP) && !defined(CONFIG_IA64)
		kargs->stack += kargs->stack_size;
#endif
	}

	return true;
}

static bool clone3_args_valid(struct kernel_clone_args *kargs)
{
	/* Verify that no unknown flags are passed along. */
	if (kargs->flags &
	    ~(CLONE_LEGACY_FLAGS | CLONE_CLEAR_SIGHAND | CLONE_INTO_CGROUP))
		return false;

	/*
	 * - make the CLONE_DETACHED bit reuseable for clone3
	 * - make the CSIGNAL bits reuseable for clone3
	 */
	if (kargs->flags & (CLONE_DETACHED | CSIGNAL))
		return false;

	if ((kargs->flags & (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) ==
	    (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND))
		return false;

	if ((kargs->flags & (CLONE_THREAD | CLONE_PARENT)) &&
	    kargs->exit_signal)
		return false;

	if (!clone3_stack_valid(kargs))
		return false;

	return true;
}

/**
 * clone3 - create a new process with specific properties
 * @uargs: argument structure
 * @size:  size of @uargs
 *
 * clone3() is the extensible successor to clone()/clone2().
 * It takes a struct as argument that is versioned by its size.
 *
 * Return: On success, a positive PID for the child process.
 *         On error, a negative errno number.
 */
SYSCALL_DEFINE2(clone3, struct clone_args __user *, uargs, size_t, size)
{
	int err;

	struct kernel_clone_args kargs;
	pid_t set_tid[MAX_PID_NS_LEVEL];

	kargs.set_tid = set_tid;

	err = copy_clone_args_from_user(&kargs, uargs, size);
	if (err)
		return err;

	if (!clone3_args_valid(&kargs))
		return -EINVAL;

	return kernel_clone(&kargs);
}
#endif

/**
 * walk_process_tree - 遍历进程树
 * @top: 起始进程
 * @visitor: 访问者函数指针
 * @data: 传递给访问者函数的数据
 *
 * 从指定的顶层进程开始，递归遍历整个进程树。
 * 对每个遇到的进程调用visitor函数。
 * 遍历顺序是深度优先的。
 */
void walk_process_tree(struct task_struct *top, proc_visitor visitor, void *data)
{
	struct task_struct *leader, *parent, *child;
	int res;

	read_lock(&tasklist_lock);
	leader = top = top->group_leader;
down:
	for_each_thread(leader, parent) {
		list_for_each_entry(child, &parent->children, sibling) {
			res = visitor(child, data);
			if (res) {
				if (res < 0)
					goto out;
				leader = child;
				goto down;
			}
up:
			;
		}
	}

	if (leader != top) {
		child = leader;
		parent = child->real_parent;
		leader = parent->group_leader;
		goto up;
	}
out:
	read_unlock(&tasklist_lock);
}

#ifndef ARCH_MIN_MMSTRUCT_ALIGN
#define ARCH_MIN_MMSTRUCT_ALIGN 0
#endif

/**
 * sighand_ctor - 信号处理结构体构造函数
 * @data: 指向sighand_struct的指针
 *
 * 初始化信号处理结构体，设置信号锁和等待队列。
 * 这是slab缓存的构造函数，在分配新的sighand_struct时调用。
 */
static void sighand_ctor(void *data)
{
	struct sighand_struct *sighand = data;

	spin_lock_init(&sighand->siglock);	/* 初始化信号锁 */
	init_waitqueue_head(&sighand->signalfd_wqh);	/* 初始化signalfd等待队列 */
}

/**
 * proc_caches_init - 初始化进程相关的slab缓存
 *
 * 在系统启动时创建各种进程相关数据结构的slab缓存，包括：
 * - sighand_cachep: 信号处理结构体缓存
 * - signal_cachep: 信号结构体缓存
 * - files_cachep: 文件结构体缓存
 * - fs_cachep: 文件系统结构体缓存
 * - mm_cachep: 内存管理结构体缓存
 * - vm_area_cachep: 虚拟内存区域缓存
 */
void __init proc_caches_init(void)
{
	unsigned int mm_size;

	sighand_cachep = kmem_cache_create("sighand_cache",
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_TYPESAFE_BY_RCU|
			SLAB_ACCOUNT, sighand_ctor);
	signal_cachep = kmem_cache_create("signal_cache",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	files_cachep = kmem_cache_create("files_cache",
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	fs_cachep = kmem_cache_create("fs_cache",
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);

	/*
	 * The mm_cpumask is located at the end of mm_struct, and is
	 * dynamically sized based on the maximum CPU number this system
	 * can have, taking hotplug into account (nr_cpu_ids).
	 */
	mm_size = sizeof(struct mm_struct) + cpumask_size();

	mm_cachep = kmem_cache_create_usercopy("mm_struct",
			mm_size, ARCH_MIN_MMSTRUCT_ALIGN,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			offsetof(struct mm_struct, saved_auxv),
			sizeof_field(struct mm_struct, saved_auxv),
			NULL);
	vm_area_cachep = KMEM_CACHE(vm_area_struct, SLAB_PANIC|SLAB_ACCOUNT);
	mmap_init();
	nsproxy_cache_init();
}

/*
 * Check constraints on flags passed to the unshare system call.
 */
static int check_unshare_flags(unsigned long unshare_flags)
{
	if (unshare_flags & ~(CLONE_THREAD|CLONE_FS|CLONE_NEWNS|CLONE_SIGHAND|
				CLONE_VM|CLONE_FILES|CLONE_SYSVSEM|
				CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWNET|
				CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWCGROUP|
				CLONE_NEWTIME))
		return -EINVAL;
	/*
	 * Not implemented, but pretend it works if there is nothing
	 * to unshare.  Note that unsharing the address space or the
	 * signal handlers also need to unshare the signal queues (aka
	 * CLONE_THREAD).
	 */
	if (unshare_flags & (CLONE_THREAD | CLONE_SIGHAND | CLONE_VM)) {
		if (!thread_group_empty(current))
			return -EINVAL;
	}
	if (unshare_flags & (CLONE_SIGHAND | CLONE_VM)) {
		if (refcount_read(&current->sighand->count) > 1)
			return -EINVAL;
	}
	if (unshare_flags & CLONE_VM) {
		if (!current_is_single_threaded())
			return -EINVAL;
	}

	return 0;
}

/*
 * Unshare the filesystem structure if it is being shared
 */
static int unshare_fs(unsigned long unshare_flags, struct fs_struct **new_fsp)
{
	struct fs_struct *fs = current->fs;

	if (!(unshare_flags & CLONE_FS) || !fs)
		return 0;

	/* don't need lock here; in the worst case we'll do useless copy */
	if (fs->users == 1)
		return 0;

	*new_fsp = copy_fs_struct(fs);
	if (!*new_fsp)
		return -ENOMEM;

	return 0;
}

/*
 * Unshare file descriptor table if it is being shared
 */
int unshare_fd(unsigned long unshare_flags, unsigned int max_fds,
	       struct files_struct **new_fdp)
{
	struct files_struct *fd = current->files;
	int error = 0;

	if ((unshare_flags & CLONE_FILES) &&
	    (fd && atomic_read(&fd->count) > 1)) {
		*new_fdp = dup_fd(fd, max_fds, &error);
		if (!*new_fdp)
			return error;
	}

	return 0;
}

/*
 * unshare allows a process to 'unshare' part of the process
 * context which was originally shared using clone.  copy_*
 * functions used by kernel_clone() cannot be used here directly
 * because they modify an inactive task_struct that is being
 * constructed. Here we are modifying the current, active,
 * task_struct.
 */
/**
 * ksys_unshare - 取消共享进程资源
 * @unshare_flags: 指定要取消共享的资源类型
 *
 * 允许进程"取消共享"原本通过clone共享的进程上下文部分。
 * 不能直接使用kernel_clone()中的copy_*函数，因为那些函数
 * 修改的是正在构造的非活动task_struct。这里修改的是当前活动的task_struct。
 *
 * 支持取消共享的资源包括：
 * - CLONE_FS: 文件系统信息
 * - CLONE_FILES: 文件描述符表
 * - CLONE_NEWNS: 挂载命名空间
 * - CLONE_SYSVSEM: System V信号量
 * - CLONE_NEWUTS: UTS命名空间
 * - CLONE_NEWIPC: IPC命名空间
 * - 等等
 *
 * 返回值：成功返回0，失败返回负的错误码
 */
int ksys_unshare(unsigned long unshare_flags)
{
	struct fs_struct *fs, *new_fs = NULL;
	struct files_struct *fd, *new_fd = NULL;
	struct cred *new_cred = NULL;
	struct nsproxy *new_nsproxy = NULL;
	int do_sysvsem = 0;
	int err;

	/*
	 * If unsharing a user namespace must also unshare the thread group
	 * and unshare the filesystem root and working directories.
	 */
	if (unshare_flags & CLONE_NEWUSER)
		unshare_flags |= CLONE_THREAD | CLONE_FS;
	/*
	 * If unsharing vm, must also unshare signal handlers.
	 */
	if (unshare_flags & CLONE_VM)
		unshare_flags |= CLONE_SIGHAND;
	/*
	 * If unsharing a signal handlers, must also unshare the signal queues.
	 */
	if (unshare_flags & CLONE_SIGHAND)
		unshare_flags |= CLONE_THREAD;
	/*
	 * If unsharing namespace, must also unshare filesystem information.
	 */
	if (unshare_flags & CLONE_NEWNS)
		unshare_flags |= CLONE_FS;

	err = check_unshare_flags(unshare_flags);
	if (err)
		goto bad_unshare_out;
	/*
	 * CLONE_NEWIPC must also detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.
	 */
	if (unshare_flags & (CLONE_NEWIPC|CLONE_SYSVSEM))
		do_sysvsem = 1;
	err = unshare_fs(unshare_flags, &new_fs);
	if (err)
		goto bad_unshare_out;
	err = unshare_fd(unshare_flags, NR_OPEN_MAX, &new_fd);
	if (err)
		goto bad_unshare_cleanup_fs;
	err = unshare_userns(unshare_flags, &new_cred);
	if (err)
		goto bad_unshare_cleanup_fd;
	err = unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy,
					 new_cred, new_fs);
	if (err)
		goto bad_unshare_cleanup_cred;

	if (new_fs || new_fd || do_sysvsem || new_cred || new_nsproxy) {
		if (do_sysvsem) {
			/*
			 * CLONE_SYSVSEM is equivalent to sys_exit().
			 */
			exit_sem(current);
		}
		if (unshare_flags & CLONE_NEWIPC) {
			/* Orphan segments in old ns (see sem above). */
			exit_shm(current);
			shm_init_task(current);
		}

		if (new_nsproxy)
			switch_task_namespaces(current, new_nsproxy);

		task_lock(current);

		if (new_fs) {
			fs = current->fs;
			spin_lock(&fs->lock);
			current->fs = new_fs;
			if (--fs->users)
				new_fs = NULL;
			else
				new_fs = fs;
			spin_unlock(&fs->lock);
		}

		if (new_fd) {
			fd = current->files;
			current->files = new_fd;
			new_fd = fd;
		}

		task_unlock(current);

		if (new_cred) {
			/* Install the new user namespace */
			commit_creds(new_cred);
			new_cred = NULL;
		}
	}

	perf_event_namespaces(current);

bad_unshare_cleanup_cred:
	if (new_cred)
		put_cred(new_cred);
bad_unshare_cleanup_fd:
	if (new_fd)
		put_files_struct(new_fd);

bad_unshare_cleanup_fs:
	if (new_fs)
		free_fs_struct(new_fs);

bad_unshare_out:
	return err;
}

/**
 * sys_unshare - unshare系统调用
 * @unshare_flags: 指定要取消共享的资源标志
 *
 * 系统调用接口，用于取消共享进程的某些资源。
 * 允许进程创建资源的私有副本，而不是与其他进程共享。
 *
 * 返回值：成功返回0，失败返回负的错误码
 */
SYSCALL_DEFINE1(unshare, unsigned long, unshare_flags)
{
	return ksys_unshare(unshare_flags);
}

/*
 *	Helper to unshare the files of the current task.
 *	We don't want to expose copy_files internals to
 *	the exec layer of the kernel.
 */

/**
 * unshare_files - 取消共享当前任务的文件描述符表
 * @displaced: 输出参数，返回被替换的原文件结构体
 *
 * 辅助函数，用于取消共享当前任务的文件描述符表。
 * 我们不想向内核的exec层暴露copy_files的内部实现。
 *
 * 返回值：成功返回0，失败返回错误码
 */
int unshare_files(struct files_struct **displaced)
{
	struct task_struct *task = current;
	struct files_struct *copy = NULL;
	int error;

	error = unshare_fd(CLONE_FILES, NR_OPEN_MAX, &copy);
	if (error || !copy) {
		*displaced = NULL;
		return error;
	}
	*displaced = task->files;
	task_lock(task);
	task->files = copy;
	task_unlock(task);
	return 0;
}

int sysctl_max_threads(struct ctl_table *table, int write,
		       void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int ret;
	int threads = max_threads;
	int min = 1;
	int max = MAX_THREADS;

	t = *table;
	t.data = &threads;
	t.extra1 = &min;
	t.extra2 = &max;

	ret = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	max_threads = threads;

	return 0;
}
