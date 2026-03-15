// SPDX-License-Identifier: GPL-2.0
/*
 * SLUB: A slab allocator that limits cache line use instead of queuing
 * objects in per cpu and per node lists.
 *
 * The allocator synchronizes using per slab locks or atomic operatios
 * and only uses a centralized lock to manage a pool of partial slabs.
 *
 * (C) 2007 SGI, Christoph Lameter
 * (C) 2011 Linux Foundation, Christoph Lameter
 */

/*
 * SLUB内存分配器 - Linux内核对象缓存分配器
 *
 * SLUB是Linux内核默认的slab分配器，主要特性：
 * - 每CPU缓存：减少多处理器竞争
 * - 部分页面链表：高效管理半满slab
 * - 调试支持：红区、毒化、追踪等
 * - NUMA感知：优化NUMA系统上的内存分配
 */

#include <linux/mm.h>
#include <linux/swap.h> /* struct reclaim_state */
#include <linux/module.h>
#include <linux/bit_spinlock.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include "slab.h"
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kasan.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/mempolicy.h>
#include <linux/ctype.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/memory.h>
#include <linux/math64.h>
#include <linux/fault-inject.h>
#include <linux/stacktrace.h>
#include <linux/prefetch.h>
#include <linux/memcontrol.h>
#include <linux/random.h>

#include <trace/events/kmem.h>

#include "internal.h"

/*
 * Lock order:
 *   1. slab_mutex (Global Mutex)
 *   2. node->list_lock
 *   3. slab_lock(page) (Only on some arches and for debugging)
 *
 *   slab_mutex
 *
 *   The role of the slab_mutex is to protect the list of all the slabs
 *   and to synchronize major metadata changes to slab cache structures.
 *
 *   The slab_lock is only used for debugging and on arches that do not
 *   have the ability to do a cmpxchg_double. It only protects:
 *	A. page->freelist	-> List of object free in a page
 *	B. page->inuse		-> Number of objects in use
 *	C. page->objects	-> Number of objects in page
 *	D. page->frozen		-> frozen state
 *
 *   If a slab is frozen then it is exempt from list management. It is not
 *   on any list except per cpu partial list. The processor that froze the
 *   slab is the one who can perform list operations on the page. Other
 *   processors may put objects onto the freelist but the processor that
 *   froze the slab is the only one that can retrieve the objects from the
 *   page's freelist.
 *
 *   The list_lock protects the partial and full list on each node and
 *   the partial slab counter. If taken then no new slabs may be added or
 *   removed from the lists nor make the number of partial slabs be modified.
 *   (Note that the total number of slabs is an atomic value that may be
 *   modified without taking the list lock).
 *
 *   The list_lock is a centralized lock and thus we avoid taking it as
 *   much as possible. As long as SLUB does not have to handle partial
 *   slabs, operations can continue without any centralized lock. F.e.
 *   allocating a long series of objects that fill up slabs does not require
 *   the list lock.
 *   Interrupts are disabled during allocation and deallocation in order to
 *   make the slab allocator safe to use in the context of an irq. In addition
 *   interrupts are disabled to ensure that the processor does not change
 *   while handling per_cpu slabs, due to kernel preemption.
 *
 * SLUB assigns one slab for allocation to each processor.
 * Allocations only occur from these slabs called cpu slabs.
 *
 * Slabs with free elements are kept on a partial list and during regular
 * operations no list for full slabs is used. If an object in a full slab is
 * freed then the slab will show up again on the partial lists.
 * We track full slabs for debugging purposes though because otherwise we
 * cannot scan all objects.
 *
 * Slabs are freed when they become empty. Teardown and setup is
 * minimal so we rely on the page allocators per cpu caches for
 * fast frees and allocs.
 *
 * page->frozen		The slab is frozen and exempt from list processing.
 * 			This means that the slab is dedicated to a purpose
 * 			such as satisfying allocations for a specific
 * 			processor. Objects may be freed in the slab while
 * 			it is frozen but slab_free will then skip the usual
 * 			list operations. It is up to the processor holding
 * 			the slab to integrate the slab into the slab lists
 * 			when the slab is no longer needed.
 *
 * 			One use of this flag is to mark slabs that are
 * 			used for allocations. Then such a slab becomes a cpu
 * 			slab. The cpu slab may be equipped with an additional
 * 			freelist that allows lockless access to
 * 			free objects in addition to the regular freelist
 * 			that requires the slab lock.
 *
 * SLAB_DEBUG_FLAGS	Slab requires special handling due to debug
 * 			options set. This moves	slab handling out of
 * 			the fast path and disables lockless freelists.
 */

#ifdef CONFIG_SLUB_DEBUG
#ifdef CONFIG_SLUB_DEBUG_ON
DEFINE_STATIC_KEY_TRUE(slub_debug_enabled);
#else
DEFINE_STATIC_KEY_FALSE(slub_debug_enabled);
#endif
#endif

/**
 * kmem_cache_debug - 检查缓存是否启用了调试功能
 * @s: 要检查的kmem_cache结构
 *
 * 检查给定的slab缓存是否设置了任何调试标志
 * 返回值: 如果启用了调试功能返回true，否则返回false
 */
static inline bool kmem_cache_debug(struct kmem_cache *s)
{
	return kmem_cache_debug_flags(s, SLAB_DEBUG_FLAGS);
}

/**
 * fixup_red_left - 修正左侧红区指针位置
 * @s: slab缓存结构
 * @p: 原始指针
 *
 * 当启用红区调试时，调整指针位置以跳过左侧红区
 * 返回值: 修正后的指针位置
 */
void *fixup_red_left(struct kmem_cache *s, void *p)
{
	if (kmem_cache_debug_flags(s, SLAB_RED_ZONE))
		p += s->red_left_pad;

	return p;
}

/**
 * kmem_cache_has_cpu_partial - 检查缓存是否支持CPU部分对象列表
 * @s: slab缓存结构
 *
 * 判断给定的slab缓存是否可以使用CPU部分对象列表优化
 * CPU部分对象列表用于提高分配/释放性能，但在调试模式下会被禁用
 * 返回值: 支持CPU部分对象列表返回true，否则返回false
 */
static inline bool kmem_cache_has_cpu_partial(struct kmem_cache *s)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
	return !kmem_cache_debug(s);
#else
	return false;
#endif
}

/*
 * Issues still to be resolved:
 *
 * - Support PAGE_ALLOC_DEBUG. Should be easy to do.
 *
 * - Variable sizing of the per node arrays
 */

/* Enable to test recovery from slab corruption on boot */
#undef SLUB_RESILIENCY_TEST

/* Enable to log cmpxchg failures */
#undef SLUB_DEBUG_CMPXCHG

/*
 * Mininum number of partial slabs. These will be left on the partial
 * lists even if they are empty. kmem_cache_shrink may reclaim them.
 */
#define MIN_PARTIAL 5

/*
 * Maximum number of desirable partial slabs.
 * The existence of more partial slabs makes kmem_cache_shrink
 * sort the partial list by the number of objects in use.
 */
#define MAX_PARTIAL 10

#define DEBUG_DEFAULT_FLAGS (SLAB_CONSISTENCY_CHECKS | SLAB_RED_ZONE | \
				SLAB_POISON | SLAB_STORE_USER)

/*
 * These debug flags cannot use CMPXCHG because there might be consistency
 * issues when checking or reading debug information
 */
#define SLAB_NO_CMPXCHG (SLAB_CONSISTENCY_CHECKS | SLAB_STORE_USER | \
				SLAB_TRACE)


/*
 * Debugging flags that require metadata to be stored in the slab.  These get
 * disabled when slub_debug=O is used and a cache's min order increases with
 * metadata.
 */
#define DEBUG_METADATA_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER)

#define OO_SHIFT	16
#define OO_MASK		((1 << OO_SHIFT) - 1)
#define MAX_OBJS_PER_PAGE	32767 /* since page.objects is u15 */

/* Internal SLUB flags */
/* Poison object */
#define __OBJECT_POISON		((slab_flags_t __force)0x80000000U)
/* Use cmpxchg_double */
#define __CMPXCHG_DOUBLE	((slab_flags_t __force)0x40000000U)

/*
 * Tracking user of a slab.
 */
#define TRACK_ADDRS_COUNT 16
struct track {
	unsigned long addr;	/* Called from address */
#ifdef CONFIG_STACKTRACE
	unsigned long addrs[TRACK_ADDRS_COUNT];	/* Called from address */
#endif
	int cpu;		/* Was running on cpu */
	int pid;		/* Pid context */
	unsigned long when;	/* When did the operation occur */
};

enum track_item { TRACK_ALLOC, TRACK_FREE };

#ifdef CONFIG_SYSFS
static int sysfs_slab_add(struct kmem_cache *);
static int sysfs_slab_alias(struct kmem_cache *, const char *);
#else
static inline int sysfs_slab_add(struct kmem_cache *s) { return 0; }
static inline int sysfs_slab_alias(struct kmem_cache *s, const char *p)
							{ return 0; }
#endif

/**
 * stat - 更新slab缓存统计计数器
 * @s: slab缓存结构
 * @si: 统计项目类型
 *
 * 在启用统计功能时，原子性地增加指定统计计数器
 * 使用raw_cpu_inc避免中断禁用的开销，但在可抢占内核中可能存在竞争
 */
static inline void stat(const struct kmem_cache *s, enum stat_item si)
{
#ifdef CONFIG_SLUB_STATS
	/*
	 * The rmw is racy on a preemptible kernel but this is acceptable, so
	 * avoid this_cpu_add()'s irq-disable overhead.
	 */
	raw_cpu_inc(s->cpu_slab->stat[si]);
#endif
}

/********************************************************************
 * 			Core slab cache functions
 *******************************************************************/

/**
 * freelist_ptr - 计算混淆后的freelist指针
 * @s: slab缓存结构
 * @ptr: 原始指针
 * @ptr_addr: 指针存储地址
 *
 * 当启用freelist强化功能时，使用XOR和随机数对指针进行混淆
 * 这有助于防止堆溢出攻击中的freelist篡改
 * 返回值: 混淆后的指针值
 */
static inline void *freelist_ptr(const struct kmem_cache *s, void *ptr,
				 unsigned long ptr_addr)
{
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	/*
	 * When CONFIG_KASAN_SW_TAGS is enabled, ptr_addr might be tagged.
	 * Normally, this doesn't cause any issues, as both set_freepointer()
	 * and get_freepointer() are called with a pointer with the same tag.
	 * However, there are some issues with CONFIG_SLUB_DEBUG code. For
	 * example, when __free_slub() iterates over objects in a cache, it
	 * passes untagged pointers to check_object(). check_object() in turns
	 * calls get_freepointer() with an untagged pointer, which causes the
	 * freepointer to be restored incorrectly.
	 */
	return (void *)((unsigned long)ptr ^ s->random ^
			swab((unsigned long)kasan_reset_tag((void *)ptr_addr)));
#else
	return ptr;
#endif
}

/**
 * freelist_dereference - 解引用freelist指针
 * @s: slab缓存结构
 * @ptr_addr: 指针存储地址
 *
 * 从指定地址读取并解混淆freelist指针
 * 返回值: 解混淆后的真实指针
 */
static inline void *freelist_dereference(const struct kmem_cache *s,
					 void *ptr_addr)
{
	return freelist_ptr(s, (void *)*(unsigned long *)(ptr_addr),
			    (unsigned long)ptr_addr);
}

/**
 * get_freepointer - 获取对象的freelist指针
 * @s: slab缓存结构
 * @object: 对象地址
 *
 * 从对象中获取下一个空闲对象的指针
 * 返回值: 下一个空闲对象的地址
 */
static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
	return freelist_dereference(s, object + s->offset);
}

/**
 * prefetch_freepointer - 预取freelist指针
 * @s: slab缓存结构
 * @object: 对象地址
 *
 * 预取对象中的freelist指针到CPU缓存，提高后续访问性能
 */
static void prefetch_freepointer(const struct kmem_cache *s, void *object)
{
	prefetch(object + s->offset);
}

/**
 * get_freepointer_safe - 安全获取freelist指针
 * @s: slab缓存结构
 * @object: 对象地址
 *
 * 在启用debug_pagealloc的情况下，安全地获取freelist指针
 * 使用copy_from_kernel_nofault避免访问被取消映射的页面时崩溃
 * 返回值: freelist指针，出错时返回NULL
 */
static inline void *get_freepointer_safe(struct kmem_cache *s, void *object)
{
	unsigned long freepointer_addr;
	void *p;

	if (!debug_pagealloc_enabled_static())
		return get_freepointer(s, object);

	freepointer_addr = (unsigned long)object + s->offset;
	copy_from_kernel_nofault(&p, (void **)freepointer_addr, sizeof(p));
	return freelist_ptr(s, p, freepointer_addr);
}

/**
 * set_freepointer - 设置对象的freelist指针
 * @s: slab缓存结构
 * @object: 对象地址
 * @fp: 要设置的freelist指针
 *
 * 在对象中设置指向下一个空闲对象的指针
 * 在强化模式下会检查双重释放或损坏
 */
static inline void set_freepointer(struct kmem_cache *s, void *object, void *fp)
{
	unsigned long freeptr_addr = (unsigned long)object + s->offset;

#ifdef CONFIG_SLAB_FREELIST_HARDENED
	BUG_ON(object == fp); /* naive detection of double free or corruption */
#endif

	*(void **)freeptr_addr = freelist_ptr(s, fp, freeptr_addr);
}

/* Loop over all objects in a slab */
#define for_each_object(__p, __s, __addr, __objects) \
	for (__p = fixup_red_left(__s, __addr); \
		__p < (__addr) + (__objects) * (__s)->size; \
		__p += (__s)->size)

/**
 * order_objects - 计算指定页面顺序下可容纳的对象数量
 * @order: 页面分配顺序（2^order个页面）
 * @size: 单个对象大小
 *
 * 计算给定页面顺序和对象大小下，一个slab可以容纳多少个对象
 * 返回值: 可容纳的对象数量
 */
static inline unsigned int order_objects(unsigned int order, unsigned int size)
{
	return ((unsigned int)PAGE_SIZE << order) / size;
}

/**
 * oo_make - 创建order_objects结构
 * @order: 页面分配顺序
 * @size: 对象大小
 *
 * 将页面顺序和对象数量编码到一个结构中
 * 高位存储order，低位存储objects数量
 * 返回值: 编码后的order_objects结构
 */
static inline struct kmem_cache_order_objects oo_make(unsigned int order,
		unsigned int size)
{
	struct kmem_cache_order_objects x = {
		(order << OO_SHIFT) + order_objects(order, size)
	};

	return x;
}

/**
 * oo_order - 从order_objects结构提取页面顺序
 * @x: order_objects结构
 *
 * 从编码的order_objects结构中提取页面分配顺序
 * 返回值: 页面分配顺序
 */
static inline unsigned int oo_order(struct kmem_cache_order_objects x)
{
	return x.x >> OO_SHIFT;
}

/**
 * oo_objects - 从order_objects结构提取对象数量
 * @x: order_objects结构
 *
 * 从编码的order_objects结构中提取对象数量
 * 返回值: slab中的对象数量
 */
static inline unsigned int oo_objects(struct kmem_cache_order_objects x)
{
	return x.x & OO_MASK;
}

/**
 * slab_lock - 锁定slab页面
 * @page: 要锁定的页面
 *
 * 使用页面的PG_locked位作为自旋锁来保护slab数据结构
 * 确保页面不是tail页面（复合页面的尾部）
 */
static __always_inline void slab_lock(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	bit_spin_lock(PG_locked, &page->flags);
}

/**
 * slab_unlock - 解锁slab页面
 * @page: 要解锁的页面
 *
 * 释放页面的PG_locked位自旋锁
 * 确保页面不是tail页面（复合页面的尾部）
 */
static __always_inline void slab_unlock(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	__bit_spin_unlock(PG_locked, &page->flags);
}

/**
 * __cmpxchg_double_slab - 原子双重比较交换操作（中断已禁用版本）
 * @s: slab缓存结构
 * @page: 目标页面
 * @freelist_old: 期望的旧freelist值
 * @counters_old: 期望的旧counters值
 * @freelist_new: 新freelist值
 * @counters_new: 新counters值
 * @n: 操作名称（用于调试）
 *
 * 原子性地更新页面的freelist和counters字段，要求中断已被禁用
 * 优先使用硬件cmpxchg_double指令，否则回退到加锁版本
 * 返回值: 成功返回true，失败返回false
 */
static inline bool __cmpxchg_double_slab(struct kmem_cache *s, struct page *page,
		void *freelist_old, unsigned long counters_old,
		void *freelist_new, unsigned long counters_new,
		const char *n)
{
	VM_BUG_ON(!irqs_disabled());
#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
    defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
	if (s->flags & __CMPXCHG_DOUBLE) {
		if (cmpxchg_double(&page->freelist, &page->counters,
				   freelist_old, counters_old,
				   freelist_new, counters_new))
			return true;
	} else
#endif
	{
		slab_lock(page);
		if (page->freelist == freelist_old &&
					page->counters == counters_old) {
			page->freelist = freelist_new;
			page->counters = counters_new;
			slab_unlock(page);
			return true;
		}
		slab_unlock(page);
	}

	cpu_relax();
	stat(s, CMPXCHG_DOUBLE_FAIL);

#ifdef SLUB_DEBUG_CMPXCHG
	pr_info("%s %s: cmpxchg double redo ", n, s->name);
#endif

	return false;
}

/**
 * cmpxchg_double_slab - 原子双重比较交换操作
 * @s: slab缓存结构
 * @page: 目标页面
 * @freelist_old: 期望的旧freelist值
 * @counters_old: 期望的旧counters值
 * @freelist_new: 新freelist值
 * @counters_new: 新counters值
 * @n: 操作名称（用于调试）
 *
 * 原子性地更新页面的freelist和counters字段
 * 优先使用硬件cmpxchg_double指令，否则禁用中断后加锁操作
 * 返回值: 成功返回true，失败返回false
 */
static inline bool cmpxchg_double_slab(struct kmem_cache *s, struct page *page,
		void *freelist_old, unsigned long counters_old,
		void *freelist_new, unsigned long counters_new,
		const char *n)
{
#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
    defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
	if (s->flags & __CMPXCHG_DOUBLE) {
		if (cmpxchg_double(&page->freelist, &page->counters,
				   freelist_old, counters_old,
				   freelist_new, counters_new))
			return true;
	} else
#endif
	{
		unsigned long flags;

		local_irq_save(flags);
		slab_lock(page);
		if (page->freelist == freelist_old &&
					page->counters == counters_old) {
			page->freelist = freelist_new;
			page->counters = counters_new;
			slab_unlock(page);
			local_irq_restore(flags);
			return true;
		}
		slab_unlock(page);
		local_irq_restore(flags);
	}

	cpu_relax();
	stat(s, CMPXCHG_DOUBLE_FAIL);

#ifdef SLUB_DEBUG_CMPXCHG
	pr_info("%s %s: cmpxchg double redo ", n, s->name);
#endif

	return false;
}

#ifdef CONFIG_SLUB_DEBUG
static unsigned long object_map[BITS_TO_LONGS(MAX_OBJS_PER_PAGE)];
static DEFINE_SPINLOCK(object_map_lock);

/**
 * get_map - 创建页面对象使用情况位图
 * @s: slab缓存结构
 * @page: 目标页面
 *
 * 生成页面中所有对象的使用情况位图，用于调试目的
 * 遍历freelist链表，标记所有空闲对象在位图中的位置
 * 必须在持有节点listlock的情况下调用，确保页面不会消失
 * 返回值: 指向对象位图的指针
 */
static unsigned long *get_map(struct kmem_cache *s, struct page *page)
	__acquires(&object_map_lock)
{
	void *p;
	void *addr = page_address(page);

	VM_BUG_ON(!irqs_disabled());

	spin_lock(&object_map_lock);

	bitmap_zero(object_map, page->objects);

	for (p = page->freelist; p; p = get_freepointer(s, p))
		set_bit(__obj_to_index(s, addr, p), object_map);

	return object_map;
}

/**
 * put_map - 释放对象位图锁
 * @map: 位图指针（必须是object_map）
 *
 * 释放get_map()获取的对象位图锁，验证传入的位图指针正确性
 */
static void put_map(unsigned long *map) __releases(&object_map_lock)
{
	VM_BUG_ON(map != object_map);
	spin_unlock(&object_map_lock);
}

/**
 * size_from_object - 计算对象的实际大小
 * @s: slab缓存结构
 *
 * 获取对象的实际大小，如果启用了红区调试则减去左侧红区填充
 * 返回值: 对象的实际可用大小
 */
static inline unsigned int size_from_object(struct kmem_cache *s)
{
	if (s->flags & SLAB_RED_ZONE)
		return s->size - s->red_left_pad;

	return s->size;
}

/**
 * restore_red_left - 恢复左侧红区前的指针位置
 * @s: slab缓存结构
 * @p: 当前指针
 *
 * 当启用红区调试时，将指针向左移动以跳过左侧红区填充
 * 与fixup_red_left()功能相反
 * 返回值: 恢复后的指针位置
 */
static inline void *restore_red_left(struct kmem_cache *s, void *p)
{
	if (s->flags & SLAB_RED_ZONE)
		p -= s->red_left_pad;

	return p;
}

/*
 * Debug settings:
 */
#if defined(CONFIG_SLUB_DEBUG_ON)
static slab_flags_t slub_debug = DEBUG_DEFAULT_FLAGS;
#else
static slab_flags_t slub_debug;
#endif

static char *slub_debug_string;
static int disable_higher_order_debug;

/**
 * metadata_access_enable - 启用元数据访问
 *
 * 告知KASAN这些对元数据的访问是合法的
 * slub即将操作位于分配对象范围外的内部对象元数据
 * 正常情况下访问这些内存会被KASAN报告为越界错误
 */
static inline void metadata_access_enable(void)
{
	kasan_disable_current();
}

/**
 * metadata_access_disable - 禁用元数据访问
 *
 * 重新启用KASAN对内存访问的检查
 * 在完成元数据操作后调用，恢复正常的边界检查
 */
static inline void metadata_access_disable(void)
{
	kasan_enable_current();
}

/*
 * Object debugging
 */

/**
 * check_valid_pointer - 验证指针是否在slab页面有效范围内
 * @s: slab缓存结构
 * @page: slab页面
 * @object: 要验证的对象指针
 *
 * 验证指针是否指向页面中的有效对象位置
 * 检查指针是否在页面范围内且正确对齐到对象边界
 * 返回值: 有效返回1，无效返回0
 */
static inline int check_valid_pointer(struct kmem_cache *s,
				struct page *page, void *object)
{
	void *base;

	if (!object)
		return 1;

	base = page_address(page);
	object = kasan_reset_tag(object);
	object = restore_red_left(s, object);
	if (object < base || object >= base + page->objects * s->size ||
		(object - base) % s->size) {
		return 0;
	}

	return 1;
}

/**
 * print_section - 打印内存区段的十六进制转储
 * @level: 打印级别（如KERN_ERR）
 * @text: 描述文本
 * @addr: 内存地址
 * @length: 打印长度
 *
 * 在启用元数据访问的情况下，安全地打印指定内存区段的内容
 * 主要用于调试时显示对象、红区、填充等区域的数据
 */
static void print_section(char *level, char *text, u8 *addr,
			  unsigned int length)
{
	metadata_access_enable();
	print_hex_dump(level, text, DUMP_PREFIX_ADDRESS, 16, 1, addr,
			length, 1);
	metadata_access_disable();
}

/**
 * freeptr_outside_object - 检查freelist指针是否在对象外部
 * @s: slab缓存结构
 *
 * 判断freelist指针是否存储在对象的使用区域之外
 * 当offset大于等于inuse时，freelist指针不会覆盖对象数据
 * 返回值: 在外部返回true，否则返回false
 */
static inline bool freeptr_outside_object(struct kmem_cache *s)
{
	return s->offset >= s->inuse;
}

/**
 * get_info_end - 获取信息块的结束偏移
 * @s: slab缓存结构
 *
 * 计算对象信息块的结束位置，包括使用区域和可能的freelist指针
 * 如果freelist指针不与对象重叠，则包含指针大小
 * 返回值: 信息块结束的字节偏移
 */
static inline unsigned int get_info_end(struct kmem_cache *s)
{
	if (freeptr_outside_object(s))
		return s->inuse + sizeof(void *);
	else
		return s->inuse;
}

/**
 * get_track - 获取对象的跟踪信息指针
 * @s: slab缓存结构
 * @object: 对象地址
 * @alloc: 跟踪类型（TRACK_ALLOC或TRACK_FREE）
 *
 * 计算对象中存储分配/释放跟踪信息的位置
 * 跟踪信息存储在对象的元数据区域中
 * 返回值: 指向跟踪结构的指针
 */
static struct track *get_track(struct kmem_cache *s, void *object,
	enum track_item alloc)
{
	struct track *p;

	p = object + get_info_end(s);

	return p + alloc;
}

/**
 * set_track - 设置对象的跟踪信息
 * @s: slab缓存结构
 * @object: 对象地址
 * @alloc: 跟踪类型（TRACK_ALLOC或TRACK_FREE）
 * @addr: 调用地址
 *
 * 在对象中记录分配或释放的跟踪信息，包括调用地址、
 * 堆栈跟踪、CPU编号、进程ID和时间戳
 * 如果addr为0则清空跟踪信息
 */
static void set_track(struct kmem_cache *s, void *object,
			enum track_item alloc, unsigned long addr)
{
	struct track *p = get_track(s, object, alloc);

	if (addr) {
#ifdef CONFIG_STACKTRACE
		unsigned int nr_entries;

		metadata_access_enable();
		nr_entries = stack_trace_save(p->addrs, TRACK_ADDRS_COUNT, 3);
		metadata_access_disable();

		if (nr_entries < TRACK_ADDRS_COUNT)
			p->addrs[nr_entries] = 0;
#endif
		p->addr = addr;
		p->cpu = smp_processor_id();
		p->pid = current->pid;
		p->when = jiffies;
	} else {
		memset(p, 0, sizeof(struct track));
	}
}

/**
 * init_tracking - 初始化对象的跟踪信息
 * @s: slab缓存结构
 * @object: 对象地址
 *
 * 初始化对象的分配和释放跟踪信息，将两个跟踪记录都清零
 * 仅在启用SLAB_STORE_USER标志时执行
 */
static void init_tracking(struct kmem_cache *s, void *object)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	set_track(s, object, TRACK_FREE, 0UL);
	set_track(s, object, TRACK_ALLOC, 0UL);
}

/**
 * print_track - 打印跟踪信息
 * @s: 描述字符串
 * @t: 跟踪结构指针
 * @pr_time: 当前时间
 *
 * 打印对象分配或释放的详细跟踪信息，包括调用地址、时间差、
 * CPU编号、进程ID以及完整的调用堆栈
 */
static void print_track(const char *s, struct track *t, unsigned long pr_time)
{
	if (!t->addr)
		return;

	pr_err("INFO: %s in %pS age=%lu cpu=%u pid=%d\n",
	       s, (void *)t->addr, pr_time - t->when, t->cpu, t->pid);
#ifdef CONFIG_STACKTRACE
	{
		int i;
		for (i = 0; i < TRACK_ADDRS_COUNT; i++)
			if (t->addrs[i])
				pr_err("\t%pS\n", (void *)t->addrs[i]);
			else
				break;
	}
#endif
}

/**
 * print_tracking - 打印对象的完整跟踪信息
 * @s: slab缓存结构
 * @object: 对象地址
 *
 * 打印对象的分配和释放跟踪信息，用于调试内存泄漏和双重释放问题
 * 仅在启用SLAB_STORE_USER标志时有效
 */
void print_tracking(struct kmem_cache *s, void *object)
{
	unsigned long pr_time = jiffies;
	if (!(s->flags & SLAB_STORE_USER))
		return;

	print_track("Allocated", get_track(s, object, TRACK_ALLOC), pr_time);
	print_track("Freed", get_track(s, object, TRACK_FREE), pr_time);
}

/**
 * print_page_info - 打印slab页面信息
 * @page: slab页面
 *
 * 打印页面的基本统计信息，包括对象总数、已使用数量、
 * freelist指针和页面标志位，用于调试slab状态
 */
static void print_page_info(struct page *page)
{
	pr_err("INFO: Slab 0x%p objects=%u used=%u fp=0x%p flags=0x%04lx\n",
	       page, page->objects, page->inuse, page->freelist, page->flags);

}

/**
 * slab_bug - 报告slab子系统错误
 * @s: 出错的slab缓存
 * @fmt: 格式化字符串
 * @...: 可变参数
 *
 * 格式化并打印slab子系统的严重错误信息，包括缓存名称、
 * 内核污染状态等，并设置BAD_PAGE污染标记
 */
static void slab_bug(struct kmem_cache *s, char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_err("=============================================================================\n");
	pr_err("BUG %s (%s): %pV\n", s->name, print_tainted(), &vaf);
	pr_err("-----------------------------------------------------------------------------\n\n");

	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
	va_end(args);
}

/**
 * slab_fix - 报告slab修复操作
 * @s: 被修复的slab缓存
 * @fmt: 格式化字符串
 * @...: 可变参数
 *
 * 打印slab子系统的修复操作信息，说明为了恢复一致性
 * 而采取的自动修复措施
 */
static void slab_fix(struct kmem_cache *s, char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_err("FIX %s: %pV\n", s->name, &vaf);
	va_end(args);
}

/**
 * freelist_corrupted - 检测freelist链表损坏
 * @s: slab缓存结构
 * @page: slab页面
 * @freelist: freelist指针的指针
 * @nextfree: 下一个空闲对象指针
 *
 * 检查freelist链表的完整性，如果发现损坏则隔离损坏的链表
 * 启用一致性检查时验证nextfree指针的有效性
 * 返回值: 发现损坏返回true，否则返回false
 */
static bool freelist_corrupted(struct kmem_cache *s, struct page *page,
			       void **freelist, void *nextfree)
{
	if ((s->flags & SLAB_CONSISTENCY_CHECKS) &&
	    !check_valid_pointer(s, page, nextfree) && freelist) {
		object_err(s, page, *freelist, "Freechain corrupt");
		*freelist = NULL;
		slab_fix(s, "Isolate corrupted freechain");
		return true;
	}

	return false;
}

static void print_trailer(struct kmem_cache *s, struct page *page, u8 *p)
{
	unsigned int off;	/* Offset of last byte */
	u8 *addr = page_address(page);

	print_tracking(s, p);

	print_page_info(page);

	pr_err("INFO: Object 0x%p @offset=%tu fp=0x%p\n\n",
	       p, p - addr, get_freepointer(s, p));

	if (s->flags & SLAB_RED_ZONE)
		print_section(KERN_ERR, "Redzone ", p - s->red_left_pad,
			      s->red_left_pad);
	else if (p > addr + 16)
		print_section(KERN_ERR, "Bytes b4 ", p - 16, 16);

	print_section(KERN_ERR, "Object ", p,
		      min_t(unsigned int, s->object_size, PAGE_SIZE));
	if (s->flags & SLAB_RED_ZONE)
		print_section(KERN_ERR, "Redzone ", p + s->object_size,
			s->inuse - s->object_size);

	off = get_info_end(s);

	if (s->flags & SLAB_STORE_USER)
		off += 2 * sizeof(struct track);

	off += kasan_metadata_size(s);

	if (off != size_from_object(s))
		/* Beginning of the filler is the free pointer */
		print_section(KERN_ERR, "Padding ", p + off,
			      size_from_object(s) - off);

	dump_stack();
}

void object_err(struct kmem_cache *s, struct page *page,
			u8 *object, char *reason)
{
	slab_bug(s, "%s", reason);
	print_trailer(s, page, object);
}

static __printf(3, 4) void slab_err(struct kmem_cache *s, struct page *page,
			const char *fmt, ...)
{
	va_list args;
	char buf[100];

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	slab_bug(s, "%s", buf);
	print_page_info(page);
	dump_stack();
}

static void init_object(struct kmem_cache *s, void *object, u8 val)
{
	u8 *p = object;

	if (s->flags & SLAB_RED_ZONE)
		memset(p - s->red_left_pad, val, s->red_left_pad);

	if (s->flags & __OBJECT_POISON) {
		memset(p, POISON_FREE, s->object_size - 1);
		p[s->object_size - 1] = POISON_END;
	}

	if (s->flags & SLAB_RED_ZONE)
		memset(p + s->object_size, val, s->inuse - s->object_size);
}

static void restore_bytes(struct kmem_cache *s, char *message, u8 data,
						void *from, void *to)
{
	slab_fix(s, "Restoring 0x%p-0x%p=0x%x\n", from, to - 1, data);
	memset(from, data, to - from);
}

static int check_bytes_and_report(struct kmem_cache *s, struct page *page,
			u8 *object, char *what,
			u8 *start, unsigned int value, unsigned int bytes)
{
	u8 *fault;
	u8 *end;
	u8 *addr = page_address(page);

	metadata_access_enable();
	fault = memchr_inv(start, value, bytes);
	metadata_access_disable();
	if (!fault)
		return 1;

	end = start + bytes;
	while (end > fault && end[-1] == value)
		end--;

	slab_bug(s, "%s overwritten", what);
	pr_err("INFO: 0x%p-0x%p @offset=%tu. First byte 0x%x instead of 0x%x\n",
					fault, end - 1, fault - addr,
					fault[0], value);
	print_trailer(s, page, object);

	restore_bytes(s, what, value, fault, end);
	return 0;
}

/*
 * Object layout:
 *
 * object address
 * 	Bytes of the object to be managed.
 * 	If the freepointer may overlay the object then the free
 *	pointer is at the middle of the object.
 *
 * 	Poisoning uses 0x6b (POISON_FREE) and the last byte is
 * 	0xa5 (POISON_END)
 *
 * object + s->object_size
 * 	Padding to reach word boundary. This is also used for Redzoning.
 * 	Padding is extended by another word if Redzoning is enabled and
 * 	object_size == inuse.
 *
 * 	We fill with 0xbb (RED_INACTIVE) for inactive objects and with
 * 	0xcc (RED_ACTIVE) for objects in use.
 *
 * object + s->inuse
 * 	Meta data starts here.
 *
 * 	A. Free pointer (if we cannot overwrite object on free)
 * 	B. Tracking data for SLAB_STORE_USER
 * 	C. Padding to reach required alignment boundary or at mininum
 * 		one word if debugging is on to be able to detect writes
 * 		before the word boundary.
 *
 *	Padding is done using 0x5a (POISON_INUSE)
 *
 * object + s->size
 * 	Nothing is used beyond s->size.
 *
 * If slabcaches are merged then the object_size and inuse boundaries are mostly
 * ignored. And therefore no slab options that rely on these boundaries
 * may be used with merged slabcaches.
 */

static int check_pad_bytes(struct kmem_cache *s, struct page *page, u8 *p)
{
	unsigned long off = get_info_end(s);	/* The end of info */

	if (s->flags & SLAB_STORE_USER)
		/* We also have user information there */
		off += 2 * sizeof(struct track);

	off += kasan_metadata_size(s);

	if (size_from_object(s) == off)
		return 1;

	return check_bytes_and_report(s, page, p, "Object padding",
			p + off, POISON_INUSE, size_from_object(s) - off);
}

/* Check the pad bytes at the end of a slab page */
static int slab_pad_check(struct kmem_cache *s, struct page *page)
{
	u8 *start;
	u8 *fault;
	u8 *end;
	u8 *pad;
	int length;
	int remainder;

	if (!(s->flags & SLAB_POISON))
		return 1;

	start = page_address(page);
	length = page_size(page);
	end = start + length;
	remainder = length % s->size;
	if (!remainder)
		return 1;

	pad = end - remainder;
	metadata_access_enable();
	fault = memchr_inv(pad, POISON_INUSE, remainder);
	metadata_access_disable();
	if (!fault)
		return 1;
	while (end > fault && end[-1] == POISON_INUSE)
		end--;

	slab_err(s, page, "Padding overwritten. 0x%p-0x%p @offset=%tu",
			fault, end - 1, fault - start);
	print_section(KERN_ERR, "Padding ", pad, remainder);

	restore_bytes(s, "slab padding", POISON_INUSE, fault, end);
	return 0;
}

static int check_object(struct kmem_cache *s, struct page *page,
					void *object, u8 val)
{
	u8 *p = object;
	u8 *endobject = object + s->object_size;

	if (s->flags & SLAB_RED_ZONE) {
		if (!check_bytes_and_report(s, page, object, "Redzone",
			object - s->red_left_pad, val, s->red_left_pad))
			return 0;

		if (!check_bytes_and_report(s, page, object, "Redzone",
			endobject, val, s->inuse - s->object_size))
			return 0;
	} else {
		if ((s->flags & SLAB_POISON) && s->object_size < s->inuse) {
			check_bytes_and_report(s, page, p, "Alignment padding",
				endobject, POISON_INUSE,
				s->inuse - s->object_size);
		}
	}

	if (s->flags & SLAB_POISON) {
		if (val != SLUB_RED_ACTIVE && (s->flags & __OBJECT_POISON) &&
			(!check_bytes_and_report(s, page, p, "Poison", p,
					POISON_FREE, s->object_size - 1) ||
			 !check_bytes_and_report(s, page, p, "Poison",
				p + s->object_size - 1, POISON_END, 1)))
			return 0;
		/*
		 * check_pad_bytes cleans up on its own.
		 */
		check_pad_bytes(s, page, p);
	}

	if (!freeptr_outside_object(s) && val == SLUB_RED_ACTIVE)
		/*
		 * Object and freepointer overlap. Cannot check
		 * freepointer while object is allocated.
		 */
		return 1;

	/* Check free pointer validity */
	if (!check_valid_pointer(s, page, get_freepointer(s, p))) {
		object_err(s, page, p, "Freepointer corrupt");
		/*
		 * No choice but to zap it and thus lose the remainder
		 * of the free objects in this slab. May cause
		 * another error because the object count is now wrong.
		 */
		set_freepointer(s, p, NULL);
		return 0;
	}
	return 1;
}

static int check_slab(struct kmem_cache *s, struct page *page)
{
	int maxobj;

	VM_BUG_ON(!irqs_disabled());

	if (!PageSlab(page)) {
		slab_err(s, page, "Not a valid slab page");
		return 0;
	}

	maxobj = order_objects(compound_order(page), s->size);
	if (page->objects > maxobj) {
		slab_err(s, page, "objects %u > max %u",
			page->objects, maxobj);
		return 0;
	}
	if (page->inuse > page->objects) {
		slab_err(s, page, "inuse %u > max %u",
			page->inuse, page->objects);
		return 0;
	}
	/* Slab_pad_check fixes things up after itself */
	slab_pad_check(s, page);
	return 1;
}

/*
 * Determine if a certain object on a page is on the freelist. Must hold the
 * slab lock to guarantee that the chains are in a consistent state.
 */
static int on_freelist(struct kmem_cache *s, struct page *page, void *search)
{
	int nr = 0;
	void *fp;
	void *object = NULL;
	int max_objects;

	fp = page->freelist;
	while (fp && nr <= page->objects) {
		if (fp == search)
			return 1;
		if (!check_valid_pointer(s, page, fp)) {
			if (object) {
				object_err(s, page, object,
					"Freechain corrupt");
				set_freepointer(s, object, NULL);
			} else {
				slab_err(s, page, "Freepointer corrupt");
				page->freelist = NULL;
				page->inuse = page->objects;
				slab_fix(s, "Freelist cleared");
				return 0;
			}
			break;
		}
		object = fp;
		fp = get_freepointer(s, object);
		nr++;
	}

	max_objects = order_objects(compound_order(page), s->size);
	if (max_objects > MAX_OBJS_PER_PAGE)
		max_objects = MAX_OBJS_PER_PAGE;

	if (page->objects != max_objects) {
		slab_err(s, page, "Wrong number of objects. Found %d but should be %d",
			 page->objects, max_objects);
		page->objects = max_objects;
		slab_fix(s, "Number of objects adjusted.");
	}
	if (page->inuse != page->objects - nr) {
		slab_err(s, page, "Wrong object count. Counter is %d but counted were %d",
			 page->inuse, page->objects - nr);
		page->inuse = page->objects - nr;
		slab_fix(s, "Object count adjusted.");
	}
	return search == NULL;
}

static void trace(struct kmem_cache *s, struct page *page, void *object,
								int alloc)
{
	if (s->flags & SLAB_TRACE) {
		pr_info("TRACE %s %s 0x%p inuse=%d fp=0x%p\n",
			s->name,
			alloc ? "alloc" : "free",
			object, page->inuse,
			page->freelist);

		if (!alloc)
			print_section(KERN_INFO, "Object ", (void *)object,
					s->object_size);

		dump_stack();
	}
}

/*
 * Tracking of fully allocated slabs for debugging purposes.
 */
static void add_full(struct kmem_cache *s,
	struct kmem_cache_node *n, struct page *page)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	lockdep_assert_held(&n->list_lock);
	list_add(&page->slab_list, &n->full);
}

static void remove_full(struct kmem_cache *s, struct kmem_cache_node *n, struct page *page)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	lockdep_assert_held(&n->list_lock);
	list_del(&page->slab_list);
}

/* Tracking of the number of slabs for debugging purposes */
static inline unsigned long slabs_node(struct kmem_cache *s, int node)
{
	struct kmem_cache_node *n = get_node(s, node);

	return atomic_long_read(&n->nr_slabs);
}

static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
{
	return atomic_long_read(&n->nr_slabs);
}

static inline void inc_slabs_node(struct kmem_cache *s, int node, int objects)
{
	struct kmem_cache_node *n = get_node(s, node);

	/*
	 * May be called early in order to allocate a slab for the
	 * kmem_cache_node structure. Solve the chicken-egg
	 * dilemma by deferring the increment of the count during
	 * bootstrap (see early_kmem_cache_node_alloc).
	 */
	if (likely(n)) {
		atomic_long_inc(&n->nr_slabs);
		atomic_long_add(objects, &n->total_objects);
	}
}
static inline void dec_slabs_node(struct kmem_cache *s, int node, int objects)
{
	struct kmem_cache_node *n = get_node(s, node);

	atomic_long_dec(&n->nr_slabs);
	atomic_long_sub(objects, &n->total_objects);
}

/* Object debug checks for alloc/free paths */
static void setup_object_debug(struct kmem_cache *s, struct page *page,
								void *object)
{
	if (!kmem_cache_debug_flags(s, SLAB_STORE_USER|SLAB_RED_ZONE|__OBJECT_POISON))
		return;

	init_object(s, object, SLUB_RED_INACTIVE);
	init_tracking(s, object);
}

static
void setup_page_debug(struct kmem_cache *s, struct page *page, void *addr)
{
	if (!kmem_cache_debug_flags(s, SLAB_POISON))
		return;

	metadata_access_enable();
	memset(addr, POISON_INUSE, page_size(page));
	metadata_access_disable();
}

static inline int alloc_consistency_checks(struct kmem_cache *s,
					struct page *page, void *object)
{
	if (!check_slab(s, page))
		return 0;

	if (!check_valid_pointer(s, page, object)) {
		object_err(s, page, object, "Freelist Pointer check fails");
		return 0;
	}

	if (!check_object(s, page, object, SLUB_RED_INACTIVE))
		return 0;

	return 1;
}

static noinline int alloc_debug_processing(struct kmem_cache *s,
					struct page *page,
					void *object, unsigned long addr)
{
	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!alloc_consistency_checks(s, page, object))
			goto bad;
	}

	/* Success perform special debug activities for allocs */
	if (s->flags & SLAB_STORE_USER)
		set_track(s, object, TRACK_ALLOC, addr);
	trace(s, page, object, 1);
	init_object(s, object, SLUB_RED_ACTIVE);
	return 1;

bad:
	if (PageSlab(page)) {
		/*
		 * If this is a slab page then lets do the best we can
		 * to avoid issues in the future. Marking all objects
		 * as used avoids touching the remaining objects.
		 */
		slab_fix(s, "Marking all objects used");
		page->inuse = page->objects;
		page->freelist = NULL;
	}
	return 0;
}

static inline int free_consistency_checks(struct kmem_cache *s,
		struct page *page, void *object, unsigned long addr)
{
	if (!check_valid_pointer(s, page, object)) {
		slab_err(s, page, "Invalid object pointer 0x%p", object);
		return 0;
	}

	if (on_freelist(s, page, object)) {
		object_err(s, page, object, "Object already free");
		return 0;
	}

	if (!check_object(s, page, object, SLUB_RED_ACTIVE))
		return 0;

	if (unlikely(s != page->slab_cache)) {
		if (!PageSlab(page)) {
			slab_err(s, page, "Attempt to free object(0x%p) outside of slab",
				 object);
		} else if (!page->slab_cache) {
			pr_err("SLUB <none>: no slab for object 0x%p.\n",
			       object);
			dump_stack();
		} else
			object_err(s, page, object,
					"page slab pointer corrupt.");
		return 0;
	}
	return 1;
}

/* Supports checking bulk free of a constructed freelist */
static noinline int free_debug_processing(
	struct kmem_cache *s, struct page *page,
	void *head, void *tail, int bulk_cnt,
	unsigned long addr)
{
	struct kmem_cache_node *n = get_node(s, page_to_nid(page));
	void *object = head;
	int cnt = 0;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&n->list_lock, flags);
	slab_lock(page);

	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!check_slab(s, page))
			goto out;
	}

next_object:
	cnt++;

	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!free_consistency_checks(s, page, object, addr))
			goto out;
	}

	if (s->flags & SLAB_STORE_USER)
		set_track(s, object, TRACK_FREE, addr);
	trace(s, page, object, 0);
	/* Freepointer not overwritten by init_object(), SLAB_POISON moved it */
	init_object(s, object, SLUB_RED_INACTIVE);

	/* Reached end of constructed freelist yet? */
	if (object != tail) {
		object = get_freepointer(s, object);
		goto next_object;
	}
	ret = 1;

out:
	if (cnt != bulk_cnt)
		slab_err(s, page, "Bulk freelist count(%d) invalid(%d)\n",
			 bulk_cnt, cnt);

	slab_unlock(page);
	spin_unlock_irqrestore(&n->list_lock, flags);
	if (!ret)
		slab_fix(s, "Object at 0x%p not freed", object);
	return ret;
}

/**
 * parse_slub_debug_flags - 解析slub_debug选项块
 * @str: 选项块的起始位置
 * @flags: 返回解析的标志，如果未指定则返回DEBUG_DEFAULT_FLAGS
 * @slabs: 返回slab列表的起始位置，如果没有列表则为NULL
 * @init: 假定这是初始解析而不是per-kmem-create解析
 *
 * 解析由';'分隔的slub_debug选项块，支持标志字符：
 * f-一致性检查，z-红区，p-毒化，u-用户跟踪，t-跟踪，a-故障注入，o-避免高阶调试
 * 返回值: 如果还有下一个块则返回其起始位置，否则返回NULL
 */
static char *
parse_slub_debug_flags(char *str, slab_flags_t *flags, char **slabs, bool init)
{
	bool higher_order_disable = false;

	/* Skip any completely empty blocks */
	while (*str && *str == ';')
		str++;

	if (*str == ',') {
		/*
		 * No options but restriction on slabs. This means full
		 * debugging for slabs matching a pattern.
		 */
		*flags = DEBUG_DEFAULT_FLAGS;
		goto check_slabs;
	}
	*flags = 0;

	/* Determine which debug features should be switched on */
	for (; *str && *str != ',' && *str != ';'; str++) {
		switch (tolower(*str)) {
		case '-':
			*flags = 0;
			break;
		case 'f':
			*flags |= SLAB_CONSISTENCY_CHECKS;
			break;
		case 'z':
			*flags |= SLAB_RED_ZONE;
			break;
		case 'p':
			*flags |= SLAB_POISON;
			break;
		case 'u':
			*flags |= SLAB_STORE_USER;
			break;
		case 't':
			*flags |= SLAB_TRACE;
			break;
		case 'a':
			*flags |= SLAB_FAILSLAB;
			break;
		case 'o':
			/*
			 * Avoid enabling debugging on caches if its minimum
			 * order would increase as a result.
			 */
			higher_order_disable = true;
			break;
		default:
			if (init)
				pr_err("slub_debug option '%c' unknown. skipped\n", *str);
		}
	}
check_slabs:
	if (*str == ',')
		*slabs = ++str;
	else
		*slabs = NULL;

	/* Skip over the slab list */
	while (*str && *str != ';')
		str++;

	/* Skip any completely empty blocks */
	while (*str && *str == ';')
		str++;

	if (init && higher_order_disable)
		disable_higher_order_debug = 1;

	if (*str)
		return str;
	else
		return NULL;
}

/**
 * setup_slub_debug - 设置slub调试选项
 * @str: 内核命令行参数字符串
 *
 * 解析内核启动参数slub_debug的值，设置全局调试标志和特定slab的调试选项
 * 支持格式：slub_debug=FUZ,kmalloc-256 或 slub_debug=FUZ;P,kmalloc-512
 * 启用相应的调试功能静态分支
 * 返回值: 总是返回1（表示参数已处理）
 */
static int __init setup_slub_debug(char *str)
{
	slab_flags_t flags;
	char *saved_str;
	char *slab_list;
	bool global_slub_debug_changed = false;
	bool slab_list_specified = false;

	slub_debug = DEBUG_DEFAULT_FLAGS;
	if (*str++ != '=' || !*str)
		/*
		 * No options specified. Switch on full debugging.
		 */
		goto out;

	saved_str = str;
	while (str) {
		str = parse_slub_debug_flags(str, &flags, &slab_list, true);

		if (!slab_list) {
			slub_debug = flags;
			global_slub_debug_changed = true;
		} else {
			slab_list_specified = true;
		}
	}

	/*
	 * For backwards compatibility, a single list of flags with list of
	 * slabs means debugging is only enabled for those slabs, so the global
	 * slub_debug should be 0. We can extended that to multiple lists as
	 * long as there is no option specifying flags without a slab list.
	 */
	if (slab_list_specified) {
		if (!global_slub_debug_changed)
			slub_debug = 0;
		slub_debug_string = saved_str;
	}
out:
	if (slub_debug != 0 || slub_debug_string)
		static_branch_enable(&slub_debug_enabled);
	if ((static_branch_unlikely(&init_on_alloc) ||
	     static_branch_unlikely(&init_on_free)) &&
	    (slub_debug & SLAB_POISON))
		pr_info("mem auto-init: SLAB_POISON will take precedence over init_on_alloc/init_on_free\n");
	return 1;
}

__setup("slub_debug", setup_slub_debug);

/**
 * kmem_cache_flags - 为缓存应用调试选项
 * @object_size: 不包含元数据的对象大小
 * @flags: 要设置的标志
 * @name: 缓存名称
 * @ctor: 构造函数
 *
 * 将调试选项应用到@flags。除了调试选项外，如果指定了slab名称
 * （即slub_debug=<Debug-Options>,<slab name1>,<slab name2> ...）
 * 则只有匹配的slab才会接收调试选项
 * 返回值: 应用调试选项后的标志
 */
slab_flags_t kmem_cache_flags(unsigned int object_size,
	slab_flags_t flags, const char *name,
	void (*ctor)(void *))
{
	char *iter;
	size_t len;
	char *next_block;
	slab_flags_t block_flags;

	len = strlen(name);
	next_block = slub_debug_string;
	/* Go through all blocks of debug options, see if any matches our slab's name */
	while (next_block) {
		next_block = parse_slub_debug_flags(next_block, &block_flags, &iter, false);
		if (!iter)
			continue;
		/* Found a block that has a slab list, search it */
		while (*iter) {
			char *end, *glob;
			size_t cmplen;

			end = strchrnul(iter, ',');
			if (next_block && next_block < end)
				end = next_block - 1;

			glob = strnchr(iter, end - iter, '*');
			if (glob)
				cmplen = glob - iter;
			else
				cmplen = max_t(size_t, len, (end - iter));

			if (!strncmp(name, iter, cmplen)) {
				flags |= block_flags;
				return flags;
			}

			if (!*end || *end == ';')
				break;
			iter = end + 1;
		}
	}

	return flags | slub_debug;
}
#else /* !CONFIG_SLUB_DEBUG */
static inline void setup_object_debug(struct kmem_cache *s,
			struct page *page, void *object) {}
static inline
void setup_page_debug(struct kmem_cache *s, struct page *page, void *addr) {}

static inline int alloc_debug_processing(struct kmem_cache *s,
	struct page *page, void *object, unsigned long addr) { return 0; }

static inline int free_debug_processing(
	struct kmem_cache *s, struct page *page,
	void *head, void *tail, int bulk_cnt,
	unsigned long addr) { return 0; }

static inline int slab_pad_check(struct kmem_cache *s, struct page *page)
			{ return 1; }
static inline int check_object(struct kmem_cache *s, struct page *page,
			void *object, u8 val) { return 1; }
static inline void add_full(struct kmem_cache *s, struct kmem_cache_node *n,
					struct page *page) {}
static inline void remove_full(struct kmem_cache *s, struct kmem_cache_node *n,
					struct page *page) {}
slab_flags_t kmem_cache_flags(unsigned int object_size,
	slab_flags_t flags, const char *name,
	void (*ctor)(void *))
{
	return flags;
}
#define slub_debug 0

#define disable_higher_order_debug 0

static inline unsigned long slabs_node(struct kmem_cache *s, int node)
							{ return 0; }
static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
							{ return 0; }
static inline void inc_slabs_node(struct kmem_cache *s, int node,
							int objects) {}
static inline void dec_slabs_node(struct kmem_cache *s, int node,
							int objects) {}

static bool freelist_corrupted(struct kmem_cache *s, struct page *page,
			       void **freelist, void *nextfree)
{
	return false;
}
#endif /* CONFIG_SLUB_DEBUG */

/**
 * kmalloc_large_node_hook - 大内存分配的子系统钩子
 * @ptr: 分配的内存指针
 * @size: 分配大小
 * @flags: 分配标志
 *
 * 在典型的生产配置中，这些钩子不应产生任何代码
 * 调用KASAN进行大对象标记，然后调用kmemleak进行泄漏检测
 * 返回值: 处理后的内存指针
 */
static inline void *kmalloc_large_node_hook(void *ptr, size_t size, gfp_t flags)
{
	ptr = kasan_kmalloc_large(ptr, size, flags);
	/* As ptr might get tagged, call kmemleak hook after KASAN. */
	kmemleak_alloc(ptr, size, 1, flags);
	return ptr;
}

/**
 * kfree_hook - 大内存释放的钩子函数
 * @x: 要释放的内存指针
 *
 * 在释放大内存块时调用各种子系统的清理函数
 * 通知kmemleak释放内存，调用KASAN进行大对象释放处理
 */
static __always_inline void kfree_hook(void *x)
{
	kmemleak_free(x);
	kasan_kfree_large(x, _RET_IP_);
}

/**
 * slab_free_hook - slab对象释放的钩子函数
 * @s: slab缓存结构
 * @x: 要释放的对象指针
 *
 * 在释放slab对象时执行各种检查和清理：
 * - kmemleak递归释放检测
 * - lockdep锁检查（需要临时禁用中断）
 * - debug对象释放检查
 * - KCSAN竞态检测（用于检测释放后使用）
 * - KASAN slab释放处理（可能将对象放入隔离区）
 * 返回值: KASAN允许立即重用时返回true，否则返回false
 */
static __always_inline bool slab_free_hook(struct kmem_cache *s, void *x)
{
	kmemleak_free_recursive(x, s->flags);

	/*
	 * Trouble is that we may no longer disable interrupts in the fast path
	 * So in order to make the debug calls that expect irqs to be
	 * disabled we need to disable interrupts temporarily.
	 */
#ifdef CONFIG_LOCKDEP
	{
		unsigned long flags;

		local_irq_save(flags);
		debug_check_no_locks_freed(x, s->object_size);
		local_irq_restore(flags);
	}
#endif
	if (!(s->flags & SLAB_DEBUG_OBJECTS))
		debug_check_no_obj_freed(x, s->object_size);

	/* Use KCSAN to help debug racy use-after-free. */
	if (!(s->flags & SLAB_TYPESAFE_BY_RCU))
		__kcsan_check_access(x, s->object_size,
				     KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ASSERT);

	/* KASAN might put x into memory quarantine, delaying its reuse */
	return kasan_slab_free(s, x, _RET_IP_);
}

/**
 * slab_free_freelist_hook - 批量释放freelist的钩子函数
 * @s: slab缓存结构
 * @head: freelist头部指针的指针
 * @tail: freelist尾部指针的指针
 *
 * 处理构造的freelist的批量释放检查，重构可以立即重用的对象链表：
 * - 遍历原始freelist中的每个对象
 * - 可选择性地清零对象内容（init_on_free）
 * - 对每个对象调用slab_free_hook进行检查
 * - 重新构造允许重用的对象的freelist
 * 返回值: 如果有对象可以重用返回true，否则返回false
 */
static inline bool slab_free_freelist_hook(struct kmem_cache *s,
					   void **head, void **tail)
{

	void *object;
	void *next = *head;
	void *old_tail = *tail ? *tail : *head;
	int rsize;

	/* Head and tail of the reconstructed freelist */
	*head = NULL;
	*tail = NULL;

	do {
		object = next;
		next = get_freepointer(s, object);

		if (slab_want_init_on_free(s)) {
			/*
			 * Clear the object and the metadata, but don't touch
			 * the redzone.
			 */
			memset(object, 0, s->object_size);
			rsize = (s->flags & SLAB_RED_ZONE) ? s->red_left_pad
							   : 0;
			memset((char *)object + s->inuse, 0,
			       s->size - s->inuse - rsize);

		}
		/* If object's reuse doesn't have to be delayed */
		if (!slab_free_hook(s, object)) {
			/* Move object to the new freelist */
			set_freepointer(s, object, *head);
			*head = object;
			if (!*tail)
				*tail = object;
		}
	} while (object != old_tail);

	if (*head == *tail)
		*tail = NULL;

	return *head != NULL;
}

/**
 * setup_object - 设置新分配的slab对象
 * @s: slab缓存结构
 * @page: 包含对象的页面
 * @object: 要设置的对象地址
 *
 * 初始化新分配的slab对象：
 * - 设置调试信息（如果启用调试）
 * - 初始化KASAN标记
 * - 调用用户定义的构造函数（如果存在）
 * - 管理KASAN对象数据的毒化/去毒化
 * 返回值: 设置完成的对象指针
 */
static void *setup_object(struct kmem_cache *s, struct page *page,
				void *object)
{
	setup_object_debug(s, page, object);
	object = kasan_init_slab_obj(s, object);
	if (unlikely(s->ctor)) {
		kasan_unpoison_object_data(s, object);
		s->ctor(object);
		kasan_poison_object_data(s, object);
	}
	return object;
}

/**
 * alloc_slab_page - 分配slab页面
 * @s: slab缓存结构
 * @flags: 分配标志（GFP_*）
 * @node: NUMA节点ID，NUMA_NO_NODE表示当前节点
 * @oo: 页面顺序和对象数量的编码
 *
 * 为slab分配页面，根据NUMA节点选择合适的分配函数
 * 分配成功后进行slab页面计费统计
 * 返回值: 分配的页面指针，失败时返回NULL
 */
static inline struct page *alloc_slab_page(struct kmem_cache *s,
		gfp_t flags, int node, struct kmem_cache_order_objects oo)
{
	struct page *page;
	unsigned int order = oo_order(oo);

	if (node == NUMA_NO_NODE)
		page = alloc_pages(flags, order);
	else
		page = __alloc_pages_node(node, flags, order);

	if (page)
		account_slab_page(page, order, s);

	return page;
}

#ifdef CONFIG_SLAB_FREELIST_RANDOM
/**
 * init_cache_random_seq - 初始化缓存的随机序列
 * @s: slab缓存结构
 *
 * 为指定的slab缓存预先初始化随机序列，用于freelist随机化
 * 创建包含对象数量个随机索引的序列，并转换为页面偏移量
 * 返回值: 成功返回0，失败返回错误码
 */
static int init_cache_random_seq(struct kmem_cache *s)
{
	unsigned int count = oo_objects(s->oo);
	int err;

	/* Bailout if already initialised */
	if (s->random_seq)
		return 0;

	err = cache_random_seq_create(s, count, GFP_KERNEL);
	if (err) {
		pr_err("SLUB: Unable to initialize free list for %s\n",
			s->name);
		return err;
	}

	/* Transform to an offset on the set of pages */
	if (s->random_seq) {
		unsigned int i;

		for (i = 0; i < count; i++)
			s->random_seq[i] *= s->size;
	}
	return 0;
}

/**
 * init_freelist_randomization - 初始化所有缓存的freelist随机化
 *
 * 遍历所有slab缓存，为每个缓存初始化随机序列freelist
 * 在系统初始化阶段调用，提高内存分配的安全性
 */
static void __init init_freelist_randomization(void)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);

	list_for_each_entry(s, &slab_caches, list)
		init_cache_random_seq(s);

	mutex_unlock(&slab_mutex);
}

/**
 * next_freelist_entry - 获取预计算随机freelist的下一个条目
 * @s: slab缓存结构
 * @page: slab页面
 * @pos: 当前位置指针（会被更新）
 * @start: freelist起始地址
 * @page_limit: 页面限制
 * @freelist_count: freelist条目总数
 *
 * 从预计算的随机序列中获取下一个有效的freelist条目
 * 如果目标页面分配失败，页面上的对象数量可能小于缓存定义的常规大小
 * 返回值: 下一个freelist条目的地址
 */
static void *next_freelist_entry(struct kmem_cache *s, struct page *page,
				unsigned long *pos, void *start,
				unsigned long page_limit,
				unsigned long freelist_count)
{
	unsigned int idx;

	/*
	 * If the target page allocation failed, the number of objects on the
	 * page might be smaller than the usual size defined by the cache.
	 */
	do {
		idx = s->random_seq[*pos];
		*pos += 1;
		if (*pos >= freelist_count)
			*pos = 0;
	} while (unlikely(idx >= page_limit));

	return (char *)start + idx;
}

/**
 * shuffle_freelist - 基于随机预计算序列重排单链表freelist
 * @s: slab缓存结构
 * @page: 要重排freelist的页面
 *
 * 使用预计算的随机序列对页面的freelist进行随机排列
 * 提高内存分配的安全性，防止可预测的分配模式被利用
 * 返回值: 成功重排返回true，否则返回false
 */
static bool shuffle_freelist(struct kmem_cache *s, struct page *page)
{
	void *start;
	void *cur;
	void *next;
	unsigned long idx, pos, page_limit, freelist_count;

	if (page->objects < 2 || !s->random_seq)
		return false;

	freelist_count = oo_objects(s->oo);
	pos = get_random_int() % freelist_count;

	page_limit = page->objects * s->size;
	start = fixup_red_left(s, page_address(page));

	/* First entry is used as the base of the freelist */
	cur = next_freelist_entry(s, page, &pos, start, page_limit,
				freelist_count);
	cur = setup_object(s, page, cur);
	page->freelist = cur;

	for (idx = 1; idx < page->objects; idx++) {
		next = next_freelist_entry(s, page, &pos, start, page_limit,
			freelist_count);
		next = setup_object(s, page, next);
		set_freepointer(s, cur, next);
		cur = next;
	}
	set_freepointer(s, cur, NULL);

	return true;
}
#else
static inline int init_cache_random_seq(struct kmem_cache *s)
{
	return 0;
}
static inline void init_freelist_randomization(void) { }
static inline bool shuffle_freelist(struct kmem_cache *s, struct page *page)
{
	return false;
}
#endif /* CONFIG_SLAB_FREELIST_RANDOM */

/**
 * allocate_slab - 分配并初始化一个新的slab页面
 * @s: slab缓存结构
 * @flags: 分配标志（GFP_*）
 * @node: NUMA节点ID
 *
 * 分配一个新的slab页面并完全初始化：
 * - 尝试最优大小分配，失败时回退到最小大小
 * - 设置页面属性和slab标记
 * - KASAN毒化整个页面
 * - 设置调试信息（如果启用）
 * - 随机化或顺序构建freelist链表
 * - 冻结页面并更新统计信息
 * 返回值: 成功返回初始化完成的页面，失败返回NULL
 */
static struct page *allocate_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	struct page *page;
	struct kmem_cache_order_objects oo = s->oo;
	gfp_t alloc_gfp;
	void *start, *p, *next;
	int idx;
	bool shuffle;

	flags &= gfp_allowed_mask;

	if (gfpflags_allow_blocking(flags))
		local_irq_enable();

	flags |= s->allocflags;

	/*
	 * Let the initial higher-order allocation fail under memory pressure
	 * so we fall-back to the minimum order allocation.
	 */
	alloc_gfp = (flags | __GFP_NOWARN | __GFP_NORETRY) & ~__GFP_NOFAIL;
	if ((alloc_gfp & __GFP_DIRECT_RECLAIM) && oo_order(oo) > oo_order(s->min))
		alloc_gfp = (alloc_gfp | __GFP_NOMEMALLOC) & ~(__GFP_RECLAIM|__GFP_NOFAIL);

	page = alloc_slab_page(s, alloc_gfp, node, oo);
	if (unlikely(!page)) {
		oo = s->min;
		alloc_gfp = flags;
		/*
		 * Allocation may have failed due to fragmentation.
		 * Try a lower order alloc if possible
		 */
		page = alloc_slab_page(s, alloc_gfp, node, oo);
		if (unlikely(!page))
			goto out;
		stat(s, ORDER_FALLBACK);
	}

	page->objects = oo_objects(oo);

	page->slab_cache = s;
	__SetPageSlab(page);
	if (page_is_pfmemalloc(page))
		SetPageSlabPfmemalloc(page);

	kasan_poison_slab(page);

	start = page_address(page);

	setup_page_debug(s, page, start);

	shuffle = shuffle_freelist(s, page);

	if (!shuffle) {
		start = fixup_red_left(s, start);
		start = setup_object(s, page, start);
		page->freelist = start;
		for (idx = 0, p = start; idx < page->objects - 1; idx++) {
			next = p + s->size;
			next = setup_object(s, page, next);
			set_freepointer(s, p, next);
			p = next;
		}
		set_freepointer(s, p, NULL);
	}

	page->inuse = page->objects;
	page->frozen = 1;

out:
	if (gfpflags_allow_blocking(flags))
		local_irq_disable();
	if (!page)
		return NULL;

	inc_slabs_node(s, page_to_nid(page), page->objects);

	return page;
}

/**
 * new_slab - 分配一个新的slab页面
 * @s: 要分配slab的缓存结构体指针
 * @flags: 分配标志，控制内存分配行为
 * @node: 首选的NUMA节点
 *
 * 返回值：成功返回新分配的页面指针，失败返回NULL
 *
 * 为指定缓存分配一个新的slab页面。会检查并修正不合法的
 * 分配标志，然后调用allocate_slab进行实际的页面分配。
 */
static struct page *new_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	if (unlikely(flags & GFP_SLAB_BUG_MASK))
		flags = kmalloc_fix_flags(flags);

	return allocate_slab(s,
		flags & (GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK), node);
}

/**
 * __free_slab - 释放slab页面的内部实现
 * @s: slab缓存结构
 * @page: 要释放的页面
 *
 * 执行slab页面释放的实际工作：
 * - 在调试模式下检查页面填充和所有对象状态
 * - 清除slab相关的页面标志
 * - 更新内存回收统计
 * - 取消slab页面计费
 * - 释放页面到页面分配器
 */
static void __free_slab(struct kmem_cache *s, struct page *page)
{
	int order = compound_order(page);
	int pages = 1 << order;

	if (kmem_cache_debug_flags(s, SLAB_CONSISTENCY_CHECKS)) {
		void *p;

		slab_pad_check(s, page);
		for_each_object(p, s, page_address(page),
						page->objects)
			check_object(s, page, p, SLUB_RED_INACTIVE);
	}

	__ClearPageSlabPfmemalloc(page);
	__ClearPageSlab(page);

	page->mapping = NULL;
	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += pages;
	unaccount_slab_page(page, order, s);
	__free_pages(page, order);
}

/**
 * rcu_free_slab - RCU回调函数用于释放slab页面
 * @h: RCU回调头
 *
 * RCU延迟释放的回调函数，确保所有RCU读取端完成后再释放页面
 * 从RCU头中获取页面指针，然后调用__free_slab进行实际释放
 */
static void rcu_free_slab(struct rcu_head *h)
{
	struct page *page = container_of(h, struct page, rcu_head);

	__free_slab(page->slab_cache, page);
}

/**
 * free_slab - 释放slab页面
 * @s: slab缓存结构
 * @page: 要释放的页面
 *
 * 根据缓存类型选择释放方式：
 * - 如果是SLAB_TYPESAFE_BY_RCU类型，使用RCU延迟释放确保读取端安全
 * - 否则立即释放页面
 */
static void free_slab(struct kmem_cache *s, struct page *page)
{
	if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU)) {
		call_rcu(&page->rcu_head, rcu_free_slab);
	} else
		__free_slab(s, page);
}

/**
 * discard_slab - 丢弃slab页面
 * @s: slab缓存结构
 * @page: 要丢弃的页面
 *
 * 丢弃一个slab页面，减少节点的slab统计计数并释放页面
 * 通常在页面变为空或需要回收时调用
 */
static void discard_slab(struct kmem_cache *s, struct page *page)
{
	dec_slabs_node(s, page_to_nid(page), page->objects);
	free_slab(s, page);
}

/**
 * __add_partial - 将页面添加到部分slab链表（内部函数）
 * @n: kmem_cache_node结构
 * @page: 要添加的页面
 * @tail: 添加位置标志
 *
 * 将页面添加到节点的partial链表中，根据tail参数决定添加到头部还是尾部
 * 增加partial计数，DEACTIVATE_TO_TAIL时添加到尾部，否则添加到头部
 */
static inline void
__add_partial(struct kmem_cache_node *n, struct page *page, int tail)
{
	n->nr_partial++;
	if (tail == DEACTIVATE_TO_TAIL)
		list_add_tail(&page->slab_list, &n->partial);
	else
		list_add(&page->slab_list, &n->partial);
}

/**
 * add_partial - 将页面添加到部分slab链表
 * @n: kmem_cache_node结构
 * @page: 要添加的页面
 * @tail: 添加位置标志
 *
 * 在持有list_lock的情况下将页面添加到partial链表
 * 这是__add_partial的安全包装函数
 */
static inline void add_partial(struct kmem_cache_node *n,
				struct page *page, int tail)
{
	lockdep_assert_held(&n->list_lock);
	__add_partial(n, page, tail);
}

/**
 * remove_partial - 从部分slab链表中移除页面
 * @n: kmem_cache_node结构
 * @page: 要移除的页面
 *
 * 在持有list_lock的情况下从partial链表中移除页面
 * 同时减少partial计数
 */
static inline void remove_partial(struct kmem_cache_node *n,
					struct page *page)
{
	lockdep_assert_held(&n->list_lock);
	list_del(&page->slab_list);
	n->nr_partial--;
}

/**
 * acquire_slab - 从partial链表获取slab并冻结
 * @s: slab缓存结构
 * @n: kmem_cache_node结构
 * @page: 要获取的页面
 * @mode: 获取模式（是否清空freelist）
 * @objects: 返回可用对象数量
 *
 * 从partial链表中移除slab页面并冻结它：
 * - 清空freelist并设置frozen位
 * - 旧的freelist成为per-cpu分配链表
 * - 使用原子操作确保并发安全
 * - 从partial链表中移除页面
 * 返回值: 成功返回freelist指针，失败返回NULL
 */
static inline void *acquire_slab(struct kmem_cache *s,
		struct kmem_cache_node *n, struct page *page,
		int mode, int *objects)
{
	void *freelist;
	unsigned long counters;
	struct page new;

	lockdep_assert_held(&n->list_lock);

	/*
	 * Zap the freelist and set the frozen bit.
	 * The old freelist is the list of objects for the
	 * per cpu allocation list.
	 */
	freelist = page->freelist;
	counters = page->counters;
	new.counters = counters;
	*objects = new.objects - new.inuse;
	if (mode) {
		new.inuse = page->objects;
		new.freelist = NULL;
	} else {
		new.freelist = freelist;
	}

	VM_BUG_ON(new.frozen);
	new.frozen = 1;

	if (!__cmpxchg_double_slab(s, page,
			freelist, counters,
			new.freelist, new.counters,
			"acquire_slab"))
		return NULL;

	remove_partial(n, page);
	WARN_ON(!freelist);
	return freelist;
}

static void put_cpu_partial(struct kmem_cache *s, struct page *page, int drain);
static inline bool pfmemalloc_match(struct page *page, gfp_t gfpflags);

/**
 * get_partial_node - 尝试从特定节点分配partial slab
 * @s: slab缓存结构
 * @n: 目标kmem_cache_node结构
 * @c: per-CPU缓存结构
 * @flags: 分配标志
 *
 * 从指定节点的partial链表中获取slab：
 * - 检查pfmemalloc匹配性
 * - 获取第一个slab作为CPU slab返回对象
 * - 将额外的slab添加到CPU partial链表以备后用
 * - 控制CPU partial链表的大小防止过度累积
 * 返回值: 成功返回对象指针，失败返回NULL
 */
static void *get_partial_node(struct kmem_cache *s, struct kmem_cache_node *n,
				struct kmem_cache_cpu *c, gfp_t flags)
{
	struct page *page, *page2;
	void *object = NULL;
	unsigned int available = 0;
	int objects;

	/*
	 * Racy check. If we mistakenly see no partial slabs then we
	 * just allocate an empty slab. If we mistakenly try to get a
	 * partial slab and there is none available then get_partial()
	 * will return NULL.
	 */
	if (!n || !n->nr_partial)
		return NULL;

	spin_lock(&n->list_lock);
	list_for_each_entry_safe(page, page2, &n->partial, slab_list) {
		void *t;

		if (!pfmemalloc_match(page, flags))
			continue;

		t = acquire_slab(s, n, page, object == NULL, &objects);
		if (!t)
			break;

		available += objects;
		if (!object) {
			c->page = page;
			stat(s, ALLOC_FROM_PARTIAL);
			object = t;
		} else {
			put_cpu_partial(s, page, 0);
			stat(s, CPU_PARTIAL_NODE);
		}
		if (!kmem_cache_has_cpu_partial(s)
			|| available > slub_cpu_partial(s) / 2)
			break;

	}
	spin_unlock(&n->list_lock);
	return object;
}

/**
 * get_any_partial - 从任意节点获取partial slab
 * @s: slab缓存结构
 * @flags: 分配标志
 * @c: per-CPU缓存结构
 *
 * 按NUMA距离递增的顺序搜索partial slab：
 * - 根据defrag_ratio控制跨节点碎片整理的频率
 * - 遍历zone列表寻找有足够partial slab的节点
 * - 只有当节点的partial数量超过min_partial时才尝试获取
 * - 支持cpuset内存策略限制
 * 返回值: 成功返回对象指针，失败返回NULL
 */
static void *get_any_partial(struct kmem_cache *s, gfp_t flags,
		struct kmem_cache_cpu *c)
{
#ifdef CONFIG_NUMA
	struct zonelist *zonelist;
	struct zoneref *z;
	struct zone *zone;
	enum zone_type highest_zoneidx = gfp_zone(flags);
	void *object;
	unsigned int cpuset_mems_cookie;

	/*
	 * The defrag ratio allows a configuration of the tradeoffs between
	 * inter node defragmentation and node local allocations. A lower
	 * defrag_ratio increases the tendency to do local allocations
	 * instead of attempting to obtain partial slabs from other nodes.
	 *
	 * If the defrag_ratio is set to 0 then kmalloc() always
	 * returns node local objects. If the ratio is higher then kmalloc()
	 * may return off node objects because partial slabs are obtained
	 * from other nodes and filled up.
	 *
	 * If /sys/kernel/slab/xx/remote_node_defrag_ratio is set to 100
	 * (which makes defrag_ratio = 1000) then every (well almost)
	 * allocation will first attempt to defrag slab caches on other nodes.
	 * This means scanning over all nodes to look for partial slabs which
	 * may be expensive if we do it every time we are trying to find a slab
	 * with available objects.
	 */
	if (!s->remote_node_defrag_ratio ||
			get_cycles() % 1024 > s->remote_node_defrag_ratio)
		return NULL;

	do {
		cpuset_mems_cookie = read_mems_allowed_begin();
		zonelist = node_zonelist(mempolicy_slab_node(), flags);
		for_each_zone_zonelist(zone, z, zonelist, highest_zoneidx) {
			struct kmem_cache_node *n;

			n = get_node(s, zone_to_nid(zone));

			if (n && cpuset_zone_allowed(zone, flags) &&
					n->nr_partial > s->min_partial) {
				object = get_partial_node(s, n, c, flags);
				if (object) {
					/*
					 * Don't check read_mems_allowed_retry()
					 * here - if mems_allowed was updated in
					 * parallel, that was a harmless race
					 * between allocation and the cpuset
					 * update
					 */
					return object;
				}
			}
		}
	} while (read_mems_allowed_retry(cpuset_mems_cookie));
#endif	/* CONFIG_NUMA */
	return NULL;
}

/*
 * Get a partial page, lock it and return it.
 */
/**
 * get_partial - 获取部分使用的slab页面
 * @s: 要获取slab的缓存结构体指针
 * @flags: 分配标志，控制内存分配行为
 * @node: 首选的NUMA节点
 * @c: per-CPU缓存结构体指针
 *
 * 返回值：成功返回可用对象指针，失败返回NULL
 *
 * 从部分使用的slab页面中获取对象。首先尝试从指定节点获取，
 * 如果失败且允许跨节点分配，则尝试从其他节点获取。
 */
static void *get_partial(struct kmem_cache *s, gfp_t flags, int node,
		struct kmem_cache_cpu *c)
{
	void *object;
	int searchnode = node;

	if (node == NUMA_NO_NODE)
		searchnode = numa_mem_id();

	object = get_partial_node(s, get_node(s, searchnode), c, flags);
	if (object || node != NUMA_NO_NODE)
		return object;

	return get_any_partial(s, flags, c);
}

#ifdef CONFIG_PREEMPTION
/*
 * Calculate the next globally unique transaction for disambiguation
 * during cmpxchg. The transactions start with the cpu number and are then
 * incremented by CONFIG_NR_CPUS.
 */
#define TID_STEP  roundup_pow_of_two(CONFIG_NR_CPUS)
#else
/*
 * No preemption supported therefore also no need to check for
 * different cpus.
 */
#define TID_STEP 1
#endif

/**
 * next_tid - 计算下一个全局唯一事务ID
 * @tid: 当前事务ID
 *
 * 为cmpxchg操作计算下一个全局唯一的事务ID用于消歧
 * 事务ID从CPU号开始，然后按CONFIG_NR_CPUS递增
 * 返回值: 下一个事务ID
 */
static inline unsigned long next_tid(unsigned long tid)
{
	return tid + TID_STEP;
}

/**
 * tid_to_cpu - 从事务ID提取CPU编号
 * @tid: 事务ID
 *
 * 从事务ID中提取CPU编号，用于调试cmpxchg失败
 * 返回值: CPU编号
 */
static inline unsigned int tid_to_cpu(unsigned long tid)
{
	return tid % TID_STEP;
}

/**
 * tid_to_event - 从事务ID提取事件序号
 * @tid: 事务ID
 *
 * 从事务ID中提取事件序号，用于调试cmpxchg失败
 * 返回值: 事件序号
 */
static inline unsigned long tid_to_event(unsigned long tid)
{
	return tid / TID_STEP;
}

/**
 * init_tid - 初始化CPU的事务ID
 * @cpu: CPU编号
 *
 * 为指定CPU初始化事务ID，初始值就是CPU编号
 * 返回值: 初始化的事务ID
 */
static inline unsigned int init_tid(int cpu)
{
	return cpu;
}

/**
 * note_cmpxchg_failure - 记录cmpxchg失败的调试信息
 * @n: 操作名称
 * @s: slab缓存结构
 * @tid: 期望的事务ID
 *
 * 在调试模式下记录cmpxchg失败的详细信息，包括CPU变化、
 * 事件变化等可能的原因，帮助诊断并发问题
 */
static inline void note_cmpxchg_failure(const char *n,
		const struct kmem_cache *s, unsigned long tid)
{
#ifdef SLUB_DEBUG_CMPXCHG
	unsigned long actual_tid = __this_cpu_read(s->cpu_slab->tid);

	pr_info("%s %s: cmpxchg redo ", n, s->name);

#ifdef CONFIG_PREEMPTION
	if (tid_to_cpu(tid) != tid_to_cpu(actual_tid))
		pr_warn("due to cpu change %d -> %d\n",
			tid_to_cpu(tid), tid_to_cpu(actual_tid));
	else
#endif
	if (tid_to_event(tid) != tid_to_event(actual_tid))
		pr_warn("due to cpu running other code. Event %ld->%ld\n",
			tid_to_event(tid), tid_to_event(actual_tid));
	else
		pr_warn("for unknown reason: actual=%lx was=%lx target=%lx\n",
			actual_tid, tid, next_tid(tid));
#endif
	stat(s, CMPXCHG_DOUBLE_CPU_FAIL);
}

/**
 * init_kmem_cache_cpus - 初始化slab缓存的所有CPU结构
 * @s: slab缓存结构
 *
 * 为缓存的所有可能CPU初始化per-CPU结构的事务ID
 * 每个CPU的初始事务ID设为其CPU编号
 */
static void init_kmem_cache_cpus(struct kmem_cache *s)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu_ptr(s->cpu_slab, cpu)->tid = init_tid(cpu);
}

/*
 * Remove the cpu slab
 */
/**
 * deactivate_slab - 停用per-CPU slab页面
 * @s: slab所属的缓存结构体指针
 * @page: 要停用的slab页面
 * @freelist: 页面的空闲对象链表
 * @c: per-CPU缓存结构体指针
 *
 * 将per-CPU slab页面停用并根据使用情况将其放入适当的链表。
 * 空页面会被释放，部分使用的页面放入partial链表，
 * 满页面放入full链表。包含统计和调试支持。
 */
static void deactivate_slab(struct kmem_cache *s, struct page *page,
				void *freelist, struct kmem_cache_cpu *c)
{
	enum slab_modes { M_NONE, M_PARTIAL, M_FULL, M_FREE };
	struct kmem_cache_node *n = get_node(s, page_to_nid(page));
	int lock = 0;
	enum slab_modes l = M_NONE, m = M_NONE;
	void *nextfree;
	int tail = DEACTIVATE_TO_HEAD;
	struct page new;
	struct page old;

	if (page->freelist) {
		stat(s, DEACTIVATE_REMOTE_FREES);
		tail = DEACTIVATE_TO_TAIL;
	}

	/*
	 * Stage one: Free all available per cpu objects back
	 * to the page freelist while it is still frozen. Leave the
	 * last one.
	 *
	 * There is no need to take the list->lock because the page
	 * is still frozen.
	 */
	while (freelist && (nextfree = get_freepointer(s, freelist))) {
		void *prior;
		unsigned long counters;

		/*
		 * If 'nextfree' is invalid, it is possible that the object at
		 * 'freelist' is already corrupted.  So isolate all objects
		 * starting at 'freelist'.
		 */
		if (freelist_corrupted(s, page, &freelist, nextfree))
			break;

		do {
			prior = page->freelist;
			counters = page->counters;
			set_freepointer(s, freelist, prior);
			new.counters = counters;
			new.inuse--;
			VM_BUG_ON(!new.frozen);

		} while (!__cmpxchg_double_slab(s, page,
			prior, counters,
			freelist, new.counters,
			"drain percpu freelist"));

		freelist = nextfree;
	}

	/*
	 * Stage two: Ensure that the page is unfrozen while the
	 * list presence reflects the actual number of objects
	 * during unfreeze.
	 *
	 * We setup the list membership and then perform a cmpxchg
	 * with the count. If there is a mismatch then the page
	 * is not unfrozen but the page is on the wrong list.
	 *
	 * Then we restart the process which may have to remove
	 * the page from the list that we just put it on again
	 * because the number of objects in the slab may have
	 * changed.
	 */
redo:

	old.freelist = page->freelist;
	old.counters = page->counters;
	VM_BUG_ON(!old.frozen);

	/* Determine target state of the slab */
	new.counters = old.counters;
	if (freelist) {
		new.inuse--;
		set_freepointer(s, freelist, old.freelist);
		new.freelist = freelist;
	} else
		new.freelist = old.freelist;

	new.frozen = 0;

	if (!new.inuse && n->nr_partial >= s->min_partial)
		m = M_FREE;
	else if (new.freelist) {
		m = M_PARTIAL;
		if (!lock) {
			lock = 1;
			/*
			 * Taking the spinlock removes the possibility
			 * that acquire_slab() will see a slab page that
			 * is frozen
			 */
			spin_lock(&n->list_lock);
		}
	} else {
		m = M_FULL;
#ifdef CONFIG_SLUB_DEBUG
		if ((s->flags & SLAB_STORE_USER) && !lock) {
			lock = 1;
			/*
			 * This also ensures that the scanning of full
			 * slabs from diagnostic functions will not see
			 * any frozen slabs.
			 */
			spin_lock(&n->list_lock);
		}
#endif
	}

	if (l != m) {
		if (l == M_PARTIAL)
			remove_partial(n, page);
		else if (l == M_FULL)
			remove_full(s, n, page);

		if (m == M_PARTIAL)
			add_partial(n, page, tail);
		else if (m == M_FULL)
			add_full(s, n, page);
	}

	l = m;
	if (!__cmpxchg_double_slab(s, page,
				old.freelist, old.counters,
				new.freelist, new.counters,
				"unfreezing slab"))
		goto redo;

	if (lock)
		spin_unlock(&n->list_lock);

	if (m == M_PARTIAL)
		stat(s, tail);
	else if (m == M_FULL)
		stat(s, DEACTIVATE_FULL);
	else if (m == M_FREE) {
		stat(s, DEACTIVATE_EMPTY);
		discard_slab(s, page);
		stat(s, FREE_SLAB);
	}

	c->page = NULL;
	c->freelist = NULL;
}

/**
 * unfreeze_partials - 解冻所有CPU部分slab
 * @s: slab缓存结构
 * @c: per-CPU缓存结构
 *
 * 必须在禁用中断的情况下调用此函数（或有其他保证确保不会并发访问）
 *
 * 将CPU部分slab链表中的所有页面解冻并移动到适当位置：
 * - 空页面加入丢弃列表稍后释放
 * - 非空页面加入节点的部分slab链表
 * - 按节点分组处理以减少锁竞争
 */
static void unfreeze_partials(struct kmem_cache *s,
		struct kmem_cache_cpu *c)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
	struct kmem_cache_node *n = NULL, *n2 = NULL;
	struct page *page, *discard_page = NULL;

	while ((page = slub_percpu_partial(c))) {
		struct page new;
		struct page old;

		slub_set_percpu_partial(c, page);

		n2 = get_node(s, page_to_nid(page));
		if (n != n2) {
			if (n)
				spin_unlock(&n->list_lock);

			n = n2;
			spin_lock(&n->list_lock);
		}

		do {

			old.freelist = page->freelist;
			old.counters = page->counters;
			VM_BUG_ON(!old.frozen);

			new.counters = old.counters;
			new.freelist = old.freelist;

			new.frozen = 0;

		} while (!__cmpxchg_double_slab(s, page,
				old.freelist, old.counters,
				new.freelist, new.counters,
				"unfreezing slab"));

		if (unlikely(!new.inuse && n->nr_partial >= s->min_partial)) {
			page->next = discard_page;
			discard_page = page;
		} else {
			add_partial(n, page, DEACTIVATE_TO_TAIL);
			stat(s, FREE_ADD_PARTIAL);
		}
	}

	if (n)
		spin_unlock(&n->list_lock);

	while (discard_page) {
		page = discard_page;
		discard_page = discard_page->next;

		stat(s, DEACTIVATE_EMPTY);
		discard_slab(s, page);
		stat(s, FREE_SLAB);
	}
#endif	/* CONFIG_SLUB_CPU_PARTIAL */
}

/**
 * put_cpu_partial - 将冻结的页面放入CPU部分slab插槽
 * @s: slab缓存结构
 * @page: 刚刚冻结的页面
 * @drain: 是否强制排空标志
 *
 * 将刚刚冻结的页面（在__slab_free或get_partial_node中）放入可用的
 * CPU部分slab插槽。如果没有找到插槽，则将所有部分slab移动到
 * per-node部分链表中。
 *
 * 维护CPU部分slab链表的页面数和对象数统计，当超过限制时
 * 自动将现有部分slab排空到节点链表
 */
static void put_cpu_partial(struct kmem_cache *s, struct page *page, int drain)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
	struct page *oldpage;
	int pages;
	int pobjects;

	preempt_disable();
	do {
		pages = 0;
		pobjects = 0;
		oldpage = this_cpu_read(s->cpu_slab->partial);

		if (oldpage) {
			pobjects = oldpage->pobjects;
			pages = oldpage->pages;
			if (drain && pobjects > slub_cpu_partial(s)) {
				unsigned long flags;
				/*
				 * partial array is full. Move the existing
				 * set to the per node partial list.
				 */
				local_irq_save(flags);
				unfreeze_partials(s, this_cpu_ptr(s->cpu_slab));
				local_irq_restore(flags);
				oldpage = NULL;
				pobjects = 0;
				pages = 0;
				stat(s, CPU_PARTIAL_DRAIN);
			}
		}

		pages++;
		pobjects += page->objects - page->inuse;

		page->pages = pages;
		page->pobjects = pobjects;
		page->next = oldpage;

	} while (this_cpu_cmpxchg(s->cpu_slab->partial, oldpage, page)
								!= oldpage);
	if (unlikely(!slub_cpu_partial(s))) {
		unsigned long flags;

		local_irq_save(flags);
		unfreeze_partials(s, this_cpu_ptr(s->cpu_slab));
		local_irq_restore(flags);
	}
	preempt_enable();
#endif	/* CONFIG_SLUB_CPU_PARTIAL */
}

/**
 * flush_slab - 刷新CPU slab
 * @s: slab缓存结构
 * @c: per-CPU缓存结构
 *
 * 停用当前CPU slab并更新事务ID，释放CPU slab资源
 * 更新统计计数器记录CPU slab刷新操作
 */
static inline void flush_slab(struct kmem_cache *s, struct kmem_cache_cpu *c)
{
	stat(s, CPUSLAB_FLUSH);
	deactivate_slab(s, c->page, c->freelist, c);

	c->tid = next_tid(c->tid);
}

/**
 * __flush_cpu_slab - 刷新指定CPU的slab（内部函数）
 * @s: slab缓存结构
 * @cpu: 目标CPU编号
 *
 * 从IPI处理器调用，此时中断已禁用
 * 刷新指定CPU的当前slab和所有部分slab
 */
static inline void __flush_cpu_slab(struct kmem_cache *s, int cpu)
{
	struct kmem_cache_cpu *c = per_cpu_ptr(s->cpu_slab, cpu);

	if (c->page)
		flush_slab(s, c);

	unfreeze_partials(s, c);
}

/**
 * flush_cpu_slab - CPU slab刷新回调函数
 * @d: 传递的数据（slab缓存结构）
 *
 * 用作IPI回调函数，刷新当前CPU的slab
 */
static void flush_cpu_slab(void *d)
{
	struct kmem_cache *s = d;

	__flush_cpu_slab(s, smp_processor_id());
}

/**
 * has_cpu_slab - 检查CPU是否有slab
 * @cpu: CPU编号
 * @info: 传递的信息（slab缓存结构）
 *
 * 检查指定CPU是否有当前slab或部分slab
 * 返回值: 有slab返回true，否则返回false
 */
static bool has_cpu_slab(int cpu, void *info)
{
	struct kmem_cache *s = info;
	struct kmem_cache_cpu *c = per_cpu_ptr(s->cpu_slab, cpu);

	return c->page || slub_percpu_partial(c);
}

/**
 * flush_all - 刷新所有CPU的slab
 * @s: slab缓存结构
 *
 * 对每个有CPU slab的CPU发送IPI，刷新其slab缓存
 * 使用条件IPI，只对实际有slab的CPU执行操作以提高效率
 */
static void flush_all(struct kmem_cache *s)
{
	on_each_cpu_cond(has_cpu_slab, flush_cpu_slab, s, 1);
}

/**
 * slub_cpu_dead - CPU下线回调函数
 * @cpu: 下线的CPU编号
 *
 * 使用CPU通知器确保在必要时刷新CPU slab
 * 当CPU下线时，将该CPU上的所有slab缓存刷新到节点链表
 * 防止CPU下线后丢失slab中的对象
 * 返回值: 总是返回0表示成功处理
 */
static int slub_cpu_dead(unsigned int cpu)
{
	struct kmem_cache *s;
	unsigned long flags;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		local_irq_save(flags);
		__flush_cpu_slab(s, cpu);
		local_irq_restore(flags);
	}
	mutex_unlock(&slab_mutex);
	return 0;
}

/**
 * node_match - 检查页面是否符合NUMA局部性期望
 * @page: 要检查的页面
 * @node: 期望的NUMA节点
 *
 * 检查per-CPU结构中的对象是否符合NUMA局部性期望
 * 在NUMA系统中，确保对象分配在预期的节点上
 * 返回值: 匹配返回1，不匹配返回0
 */
static inline int node_match(struct page *page, int node)
{
#ifdef CONFIG_NUMA
	if (node != NUMA_NO_NODE && page_to_nid(page) != node)
		return 0;
#endif
	return 1;
}

/**
 * count_free - 计算页面中空闲对象数量
 * @page: 要计算的页面
 *
 * 返回值: 页面中空闲对象的数量
 */
static int count_free(struct page *page)
{
	return page->objects - page->inuse;
}

/**
 * node_nr_objs - 获取节点中对象总数
 * @n: kmem_cache_node结构
 *
 * 原子性地读取节点中的对象总数
 * 返回值: 节点中的对象总数
 */
static inline unsigned long node_nr_objs(struct kmem_cache_node *n)
{
	return atomic_long_read(&n->total_objects);
}

/**
 * count_partial - 统计部分slab的指定信息
 * @n: kmem_cache_node结构
 * @get_count: 获取计数的回调函数
 *
 * 遍历节点的partial链表，对每个页面调用get_count函数并累加结果
 * 在持有自旋锁的情况下操作，确保链表遍历的原子性
 * 返回值: 累加的计数结果
 */
static unsigned long count_partial(struct kmem_cache_node *n,
					int (*get_count)(struct page *))
{
	unsigned long flags;
	unsigned long x = 0;
	struct page *page;

	spin_lock_irqsave(&n->list_lock, flags);
	list_for_each_entry(page, &n->partial, slab_list)
		x += get_count(page);
	spin_unlock_irqrestore(&n->list_lock, flags);
	return x;
}

/**
 * slab_out_of_memory - 报告slab分配器内存不足
 * @s: 无法分配的slab缓存
 * @gfpflags: 分配标志
 * @nid: 尝试分配的节点ID
 *
 * 当slab分配器无法分配内存时打印详细的调试信息：
 * - 使用速率限制避免日志泛洪
 * - 显示缓存信息（名称、对象大小、缓冲区大小、分配顺序）
 * - 显示每个节点的slab统计（slab数量、对象数量、空闲数量）
 * - 如果调试增加了最小顺序会给出提示
 */
static noinline void
slab_out_of_memory(struct kmem_cache *s, gfp_t gfpflags, int nid)
{
#ifdef CONFIG_SLUB_DEBUG
	static DEFINE_RATELIMIT_STATE(slub_oom_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	int node;
	struct kmem_cache_node *n;

	if ((gfpflags & __GFP_NOWARN) || !__ratelimit(&slub_oom_rs))
		return;

	pr_warn("SLUB: Unable to allocate memory on node %d, gfp=%#x(%pGg)\n",
		nid, gfpflags, &gfpflags);
	pr_warn("  cache: %s, object size: %u, buffer size: %u, default order: %u, min order: %u\n",
		s->name, s->object_size, s->size, oo_order(s->oo),
		oo_order(s->min));

	if (oo_order(s->min) > get_order(s->object_size))
		pr_warn("  %s debugging increased min order, use slub_debug=O to disable.\n",
			s->name);

	for_each_kmem_cache_node(s, node, n) {
		unsigned long nr_slabs;
		unsigned long nr_objs;
		unsigned long nr_free;

		nr_free  = count_partial(n, count_free);
		nr_slabs = node_nr_slabs(n);
		nr_objs  = node_nr_objs(n);

		pr_warn("  node %d: slabs: %ld, objs: %ld, free: %ld\n",
			node, nr_slabs, nr_objs, nr_free);
	}
#endif
}

/**
 * new_slab_objects - 获取新的slab对象
 * @s: slab缓存结构
 * @flags: 分配标志
 * @node: 首选NUMA节点
 * @pc: per-CPU缓存指针的指针
 *
 * 尝试获取新的slab对象，优先从partial链表获取，失败则分配新slab：
 * 1. 首先尝试从partial slab获取对象
 * 2. 如果失败则分配全新的slab页面
 * 3. 如果当前CPU已有页面则先刷新
 * 4. 设置新页面为当前CPU slab并返回freelist
 *
 * 警告：如果缓存有构造函数且要求零初始化，这是不兼容的
 * 返回值: 成功返回freelist，失败返回NULL
 */
static inline void *new_slab_objects(struct kmem_cache *s, gfp_t flags,
			int node, struct kmem_cache_cpu **pc)
{
	void *freelist;
	struct kmem_cache_cpu *c = *pc;
	struct page *page;

	WARN_ON_ONCE(s->ctor && (flags & __GFP_ZERO));

	freelist = get_partial(s, flags, node, c);

	if (freelist)
		return freelist;

	page = new_slab(s, flags, node);
	if (page) {
		c = raw_cpu_ptr(s->cpu_slab);
		if (c->page)
			flush_slab(s, c);

		/*
		 * No other reference to the page yet so we can
		 * muck around with it freely without cmpxchg
		 */
		freelist = page->freelist;
		page->freelist = NULL;

		stat(s, ALLOC_SLAB);
		c->page = page;
		*pc = c;
	}

	return freelist;
}

/**
 * pfmemalloc_match - 检查页面是否匹配pfmemalloc要求
 * @page: 要检查的页面
 * @gfpflags: 分配标志
 *
 * 检查页面的pfmemalloc状态是否与分配标志匹配：
 * - 如果页面标记为pfmemalloc，检查分配标志是否允许pfmemalloc
 * - 普通页面总是匹配
 * 返回值: 匹配返回true，否则返回false
 */
static inline bool pfmemalloc_match(struct page *page, gfp_t gfpflags)
{
	if (unlikely(PageSlabPfmemalloc(page)))
		return gfp_pfmemalloc_allowed(gfpflags);

	return true;
}

/**
 * get_freelist - 检查并获取页面的freelist
 * @s: slab缓存结构
 * @page: 要检查的页面
 *
 * 检查页面的freelist并将其传输到per-cpu freelist或停用页面
 * 必须在禁用中断的情况下调用此函数
 *
 * 如果返回值不为NULL，页面仍然是冻结的
 * 如果此函数返回NULL，页面已被解冻
 * 返回值: freelist指针或NULL
 */
static inline void *get_freelist(struct kmem_cache *s, struct page *page)
{
	struct page new;
	unsigned long counters;
	void *freelist;

	do {
		freelist = page->freelist;
		counters = page->counters;

		new.counters = counters;
		VM_BUG_ON(!new.frozen);

		new.inuse = page->objects;
		new.frozen = freelist != NULL;

	} while (!__cmpxchg_double_slab(s, page,
		freelist, counters,
		NULL, new.counters,
		"get_freelist"));

	return freelist;
}

/*
 * Slow path. The lockless freelist is empty or we need to perform
 * debugging duties.
 *
 * Processing is still very fast if new objects have been freed to the
 * regular freelist. In that case we simply take over the regular freelist
 * as the lockless freelist and zap the regular freelist.
 *
 * If that is not working then we fall back to the partial lists. We take the
 * first element of the freelist as the object to allocate now and move the
 * rest of the freelist to the lockless freelist.
 *
 * And if we were unable to get a new slab from the partial slab lists then
 * we need to allocate a new slab. This is the slowest path since it involves
 * a call to the page allocator and the setup of a new slab.
 *
 * Version of __slab_alloc to use when we know that interrupts are
 * already disabled (which is the case for bulk allocation).
 */
/**
 * ___slab_alloc - SLUB分配器的最慢路径（中断已禁用版本）
 * @s: 要分配对象的缓存结构体指针
 * @gfpflags: 分配标志，控制内存分配行为
 * @node: 首选的NUMA节点ID
 * @addr: 返回地址，用于调试和追踪分配调用
 * @c: per-CPU缓存结构体指针
 *
 * 返回值：成功返回分配的对象指针，失败返回NULL
 *
 * 这是在中断已禁用情况下的慢路径分配函数，主要用于批量分配。
 * 分配策略的优先级顺序：
 * 1. 尝试使用当前slab页面的常规空闲链表
 * 2. 回退到部分slab链表，获取第一个对象并移动其余到无锁链表
 * 3. 最慢路径：分配新的slab页面（涉及页面分配器调用）
 */
static void *___slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
			  unsigned long addr, struct kmem_cache_cpu *c)
{
	void *freelist;
	struct page *page;

	stat(s, ALLOC_SLOWPATH);

	page = c->page;
	if (!page) {
		/*
		 * if the node is not online or has no normal memory, just
		 * ignore the node constraint
		 */
		if (unlikely(node != NUMA_NO_NODE &&
			     !node_state(node, N_NORMAL_MEMORY)))
			node = NUMA_NO_NODE;
		goto new_slab;
	}
redo:

	if (unlikely(!node_match(page, node))) {
		/*
		 * same as above but node_match() being false already
		 * implies node != NUMA_NO_NODE
		 */
		if (!node_state(node, N_NORMAL_MEMORY)) {
			node = NUMA_NO_NODE;
			goto redo;
		} else {
			stat(s, ALLOC_NODE_MISMATCH);
			deactivate_slab(s, page, c->freelist, c);
			goto new_slab;
		}
	}

	/*
	 * By rights, we should be searching for a slab page that was
	 * PFMEMALLOC but right now, we are losing the pfmemalloc
	 * information when the page leaves the per-cpu allocator
	 */
	if (unlikely(!pfmemalloc_match(page, gfpflags))) {
		deactivate_slab(s, page, c->freelist, c);
		goto new_slab;
	}

	/* must check again c->freelist in case of cpu migration or IRQ */
	freelist = c->freelist;
	if (freelist)
		goto load_freelist;

	freelist = get_freelist(s, page);

	if (!freelist) {
		c->page = NULL;
		stat(s, DEACTIVATE_BYPASS);
		goto new_slab;
	}

	stat(s, ALLOC_REFILL);

load_freelist:
	/*
	 * freelist is pointing to the list of objects to be used.
	 * page is pointing to the page from which the objects are obtained.
	 * That page must be frozen for per cpu allocations to work.
	 */
	VM_BUG_ON(!c->page->frozen);
	c->freelist = get_freepointer(s, freelist);
	c->tid = next_tid(c->tid);
	return freelist;

new_slab:

	if (slub_percpu_partial(c)) {
		page = c->page = slub_percpu_partial(c);
		slub_set_percpu_partial(c, page);
		stat(s, CPU_PARTIAL_ALLOC);
		goto redo;
	}

	freelist = new_slab_objects(s, gfpflags, node, &c);

	if (unlikely(!freelist)) {
		slab_out_of_memory(s, gfpflags, node);
		return NULL;
	}

	page = c->page;
	if (likely(!kmem_cache_debug(s) && pfmemalloc_match(page, gfpflags)))
		goto load_freelist;

	/* Only entered in the debug case */
	if (kmem_cache_debug(s) &&
			!alloc_debug_processing(s, page, freelist, addr))
		goto new_slab;	/* Slab failed checks. Next slab needed */

	deactivate_slab(s, page, get_freepointer(s, freelist), c);
	return freelist;
}

/*
 * Another one that disabled interrupt and compensates for possible
 * cpu changes by refetching the per cpu area pointer.
 */
/**
 * __slab_alloc - SLUB分配器的慢路径分配函数
 * @s: 要分配对象的缓存结构体指针
 * @gfpflags: 分配标志，控制内存分配行为
 * @node: 首选的NUMA节点
 * @addr: 返回地址，用于调试和追踪
 * @c: per-CPU缓存结构体指针
 *
 * 返回值：成功返回分配的对象指针，失败返回NULL
 *
 * 当快路径分配失败时调用此函数。会禁用中断以避免竞争，
 * 然后调用___slab_alloc进行实际的慢路径分配处理。
 */
static void *__slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
			  unsigned long addr, struct kmem_cache_cpu *c)
{
	void *p;
	unsigned long flags;

	local_irq_save(flags);
#ifdef CONFIG_PREEMPTION
	/*
	 * We may have been preempted and rescheduled on a different
	 * cpu before disabling interrupts. Need to reload cpu area
	 * pointer.
	 */
	c = this_cpu_ptr(s->cpu_slab);
#endif

	p = ___slab_alloc(s, gfpflags, node, addr, c);
	local_irq_restore(flags);
	return p;
}

/**
 * maybe_wipe_obj_freeptr - 可能清除对象的freelist指针
 * @s: slab缓存结构
 * @obj: 要处理的对象
 *
 * 如果对象在释放时被清零，确保通过清零freelist指针来完全初始化
 * 当启用init_on_free时，对象内容会被清零，但freelist指针位置
 * 需要特别处理以确保分配时的一致性
 */
static __always_inline void maybe_wipe_obj_freeptr(struct kmem_cache *s,
						   void *obj)
{
	if (unlikely(slab_want_init_on_free(s)) && obj)
		memset((void *)((char *)obj + s->offset), 0, sizeof(void *));
}

/*
 * Inlined fastpath so that allocation functions (kmalloc, kmem_cache_alloc)
 * have the fastpath folded into their functions. So no function call
 * overhead for requests that can be satisfied on the fastpath.
 *
 * The fastpath works by first checking if the lockless freelist can be used.
 * If not then __slab_alloc is called for slow processing.
 *
 * Otherwise we can simply pick the next object from the lockless free list.
 */
/**
 * slab_alloc_node - 从指定NUMA节点的slab缓存中分配对象（内联快速路径）
 * @s: 要分配对象的缓存结构体指针
 * @gfpflags: 分配标志，控制内存分配行为（如__GFP_ZERO等）
 * @node: 首选的NUMA节点ID，NUMA_NO_NODE表示不指定节点
 * @addr: 返回地址，用于调试和追踪分配调用
 *
 * 返回值：成功返回分配的对象指针，失败返回NULL
 *
 * 这是SLUB分配器的核心快速路径函数，被内联到kmalloc等分配函数中。
 * 快速路径优化：
 * 1. 首先尝试使用per-CPU无锁空闲链表
 * 2. 如果快速路径失败，则调用__slab_alloc慢路径处理
 * 3. 支持NUMA节点亲和性和对象组控制
 */
static __always_inline void *slab_alloc_node(struct kmem_cache *s,
		gfp_t gfpflags, int node, unsigned long addr)
{
	void *object;
	struct kmem_cache_cpu *c;
	struct page *page;
	unsigned long tid;
	struct obj_cgroup *objcg = NULL;

	s = slab_pre_alloc_hook(s, &objcg, 1, gfpflags);
	if (!s)
		return NULL;
redo:
	/*
	 * Must read kmem_cache cpu data via this cpu ptr. Preemption is
	 * enabled. We may switch back and forth between cpus while
	 * reading from one cpu area. That does not matter as long
	 * as we end up on the original cpu again when doing the cmpxchg.
	 *
	 * We should guarantee that tid and kmem_cache are retrieved on
	 * the same cpu. It could be different if CONFIG_PREEMPTION so we need
	 * to check if it is matched or not.
	 */
	do {
		tid = this_cpu_read(s->cpu_slab->tid);
		c = raw_cpu_ptr(s->cpu_slab);
	} while (IS_ENABLED(CONFIG_PREEMPTION) &&
		 unlikely(tid != READ_ONCE(c->tid)));

	/*
	 * Irqless object alloc/free algorithm used here depends on sequence
	 * of fetching cpu_slab's data. tid should be fetched before anything
	 * on c to guarantee that object and page associated with previous tid
	 * won't be used with current tid. If we fetch tid first, object and
	 * page could be one associated with next tid and our alloc/free
	 * request will be failed. In this case, we will retry. So, no problem.
	 */
	barrier();

	/*
	 * The transaction ids are globally unique per cpu and per operation on
	 * a per cpu queue. Thus they can be guarantee that the cmpxchg_double
	 * occurs on the right processor and that there was no operation on the
	 * linked list in between.
	 */

	object = c->freelist;
	page = c->page;
	if (unlikely(!object || !page || !node_match(page, node))) {
		object = __slab_alloc(s, gfpflags, node, addr, c);
	} else {
		void *next_object = get_freepointer_safe(s, object);

		/*
		 * The cmpxchg will only match if there was no additional
		 * operation and if we are on the right processor.
		 *
		 * The cmpxchg does the following atomically (without lock
		 * semantics!)
		 * 1. Relocate first pointer to the current per cpu area.
		 * 2. Verify that tid and freelist have not been changed
		 * 3. If they were not changed replace tid and freelist
		 *
		 * Since this is without lock semantics the protection is only
		 * against code executing on this cpu *not* from access by
		 * other cpus.
		 */
		if (unlikely(!this_cpu_cmpxchg_double(
				s->cpu_slab->freelist, s->cpu_slab->tid,
				object, tid,
				next_object, next_tid(tid)))) {

			note_cmpxchg_failure("slab_alloc", s, tid);
			goto redo;
		}
		prefetch_freepointer(s, next_object);
		stat(s, ALLOC_FASTPATH);
	}

	maybe_wipe_obj_freeptr(s, object);

	if (unlikely(slab_want_init_on_alloc(gfpflags, s)) && object)
		memset(object, 0, s->object_size);

	slab_post_alloc_hook(s, objcg, gfpflags, 1, &object);

	return object;
}

/**
 * slab_alloc - 从slab缓存中分配对象（简化接口）
 * @s: 要分配对象的缓存结构体指针
 * @gfpflags: 分配标志，控制内存分配行为
 * @addr: 返回地址，用于调试和追踪分配调用
 *
 * 返回值：成功返回分配的对象指针，失败返回NULL
 *
 * 这是slab_alloc_node的简化版本，不指定NUMA节点，
 * 让内核自动选择合适的节点进行分配。
 */
static __always_inline void *slab_alloc(struct kmem_cache *s,
		gfp_t gfpflags, unsigned long addr)
{
	return slab_alloc_node(s, gfpflags, NUMA_NO_NODE, addr);
}

/**
 * kmem_cache_alloc - 从指定的对象缓存中分配对象
 * @s: 要分配对象的缓存结构体指针
 * @gfpflags: 分配标志，控制内存分配行为
 *
 * 返回值：成功返回分配的对象指针，失败返回NULL
 *
 * 这是SLUB分配器的主要分配函数，从指定缓存分配一个对象。
 * 会尝试从per-CPU缓存快速分配，失败时走慢路径。
 */
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfpflags)
{
	void *ret = slab_alloc(s, gfpflags, _RET_IP_);

	trace_kmem_cache_alloc(_RET_IP_, ret, s->object_size,
				s->size, gfpflags);

	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc);

/**
 * kmem_cache_alloc_trace - 从缓存分配对象并启用追踪
 * @s: 要分配对象的缓存结构
 * @gfpflags: 分配标志
 * @size: 请求的大小（用于追踪）
 *
 * 从指定缓存分配对象，启用kmalloc追踪并进行KASAN检查
 * 主要用于通用kmalloc接口的实现
 * 返回值: 成功返回对象指针，失败返回NULL
 */
void *kmem_cache_alloc_trace(struct kmem_cache *s, gfp_t gfpflags, size_t size)
{
	void *ret = slab_alloc(s, gfpflags, _RET_IP_);
	trace_kmalloc(_RET_IP_, ret, size, s->size, gfpflags);
	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}

/**
 * kmem_cache_alloc_node - 从指定NUMA节点的缓存分配对象
 * @s: 要分配对象的缓存结构
 * @gfpflags: 分配标志
 * @node: 首选的NUMA节点
 *
 * 从指定NUMA节点的缓存分配对象，优化NUMA系统的内存访问局部性
 * 包含追踪支持以便性能分析
 * 返回值: 成功返回对象指针，失败返回NULL
 */
void *kmem_cache_alloc_node(struct kmem_cache *s, gfp_t gfpflags, int node)
{
	void *ret = slab_alloc_node(s, gfpflags, node, _RET_IP_);

	trace_kmem_cache_alloc_node(_RET_IP_, ret,
				    s->object_size, s->size, gfpflags, node);

	return ret;
}

/**
 * kmem_cache_alloc_node_trace - 从指定节点分配对象并启用追踪
 * @s: 要分配对象的缓存结构
 * @gfpflags: 分配标志
 * @node: 首选的NUMA节点
 * @size: 请求的大小（用于追踪）
 *
 * 从指定NUMA节点分配对象，启用kmalloc追踪并进行KASAN检查
 * 结合了NUMA感知和调试追踪功能
 * 返回值: 成功返回对象指针，失败返回NULL
 */
void *kmem_cache_alloc_node_trace(struct kmem_cache *s,
				    gfp_t gfpflags,
				    int node, size_t size)
{
	void *ret = slab_alloc_node(s, gfpflags, node, _RET_IP_);

	trace_kmalloc_node(_RET_IP_, ret,
			   size, s->size, gfpflags, node);

	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}

/*
 * Slow path handling. This may still be called frequently since objects
 * have a longer lifetime than the cpu slabs in most processing loads.
 *
 * So we still attempt to reduce cache line usage. Just take the slab
 * lock and free the item. If there is no additional partial page
 * handling required then we can return immediately.
 */
/**
 * __slab_free - slab对象释放的慢路径处理
 * @s: 对象所属的缓存结构体指针
 * @page: 对象所在的slab页面
 * @head: 要释放的对象链表头指针
 * @tail: 要释放的对象链表尾指针
 * @cnt: 要释放的对象数量
 * @addr: 调用者地址，用于调试和追踪
 *
 * 处理无法通过快速路径释放的对象。尽管是慢路径，但在大多数工作负载中
 * 仍可能被频繁调用，因为对象的生命周期通常比CPU slab更长。
 *
 * 优化策略：
 * 1. 减少缓存行使用 - 仅获取slab锁并释放对象
 * 2. 无需额外部分页面处理时立即返回
 * 3. 处理页面状态转换（空闲、部分、满）
 */
static void __slab_free(struct kmem_cache *s, struct page *page,
			void *head, void *tail, int cnt,
			unsigned long addr)

{
	void *prior;
	int was_frozen;
	struct page new;
	unsigned long counters;
	struct kmem_cache_node *n = NULL;
	unsigned long flags;

	stat(s, FREE_SLOWPATH);

	if (kmem_cache_debug(s) &&
	    !free_debug_processing(s, page, head, tail, cnt, addr))
		return;

	do {
		if (unlikely(n)) {
			spin_unlock_irqrestore(&n->list_lock, flags);
			n = NULL;
		}
		prior = page->freelist;
		counters = page->counters;
		set_freepointer(s, tail, prior);
		new.counters = counters;
		was_frozen = new.frozen;
		new.inuse -= cnt;
		if ((!new.inuse || !prior) && !was_frozen) {

			if (kmem_cache_has_cpu_partial(s) && !prior) {

				/*
				 * Slab was on no list before and will be
				 * partially empty
				 * We can defer the list move and instead
				 * freeze it.
				 */
				new.frozen = 1;

			} else { /* Needs to be taken off a list */

				n = get_node(s, page_to_nid(page));
				/*
				 * Speculatively acquire the list_lock.
				 * If the cmpxchg does not succeed then we may
				 * drop the list_lock without any processing.
				 *
				 * Otherwise the list_lock will synchronize with
				 * other processors updating the list of slabs.
				 */
				spin_lock_irqsave(&n->list_lock, flags);

			}
		}

	} while (!cmpxchg_double_slab(s, page,
		prior, counters,
		head, new.counters,
		"__slab_free"));

	if (likely(!n)) {

		if (likely(was_frozen)) {
			/*
			 * The list lock was not taken therefore no list
			 * activity can be necessary.
			 */
			stat(s, FREE_FROZEN);
		} else if (new.frozen) {
			/*
			 * If we just froze the page then put it onto the
			 * per cpu partial list.
			 */
			put_cpu_partial(s, page, 1);
			stat(s, CPU_PARTIAL_FREE);
		}

		return;
	}

	if (unlikely(!new.inuse && n->nr_partial >= s->min_partial))
		goto slab_empty;

	/*
	 * Objects left in the slab. If it was not on the partial list before
	 * then add it.
	 */
	if (!kmem_cache_has_cpu_partial(s) && unlikely(!prior)) {
		remove_full(s, n, page);
		add_partial(n, page, DEACTIVATE_TO_TAIL);
		stat(s, FREE_ADD_PARTIAL);
	}
	spin_unlock_irqrestore(&n->list_lock, flags);
	return;

slab_empty:
	if (prior) {
		/*
		 * Slab on the partial list.
		 */
		remove_partial(n, page);
		stat(s, FREE_REMOVE_PARTIAL);
	} else {
		/* Slab must be on the full list */
		remove_full(s, n, page);
	}

	spin_unlock_irqrestore(&n->list_lock, flags);
	stat(s, FREE_SLAB);
	discard_slab(s, page);
}

/*
 * Fastpath with forced inlining to produce a kfree and kmem_cache_free that
 * can perform fastpath freeing without additional function calls.
 *
 * The fastpath is only possible if we are freeing to the current cpu slab
 * of this processor. This typically the case if we have just allocated
 * the item before.
 *
 * If fastpath is not possible then fall back to __slab_free where we deal
 * with all sorts of special processing.
 *
 * Bulk free of a freelist with several objects (all pointing to the
 * same page) possible by specifying head and tail ptr, plus objects
 * count (cnt). Bulk free indicated by tail pointer being set.
 */
/**
 * do_slab_free - 执行实际的slab对象释放操作（快速路径优化）
 * @s: 对象所属的缓存结构体指针
 * @page: 对象所在的slab页面
 * @head: 要释放的对象链表头指针
 * @tail: 要释放的对象链表尾指针（NULL表示单个对象）
 * @cnt: 要释放的对象数量
 * @addr: 调用者地址，用于调试和追踪
 *
 * 这是强制内联的快速释放路径函数，用于生成高效的kfree和kmem_cache_free。
 *
 * 快速路径条件：释放到当前CPU的slab页面（通常是刚分配的对象）
 * 快速路径失败时：回退到__slab_free进行慢路径处理
 *
 * 支持批量释放：通过head/tail指针指定空闲链表，所有对象必须在同一页面
 */
static __always_inline void do_slab_free(struct kmem_cache *s,
				struct page *page, void *head, void *tail,
				int cnt, unsigned long addr)
{
	void *tail_obj = tail ? : head;
	struct kmem_cache_cpu *c;
	unsigned long tid;

	memcg_slab_free_hook(s, &head, 1);
redo:
	/*
	 * Determine the currently cpus per cpu slab.
	 * The cpu may change afterward. However that does not matter since
	 * data is retrieved via this pointer. If we are on the same cpu
	 * during the cmpxchg then the free will succeed.
	 */
	do {
		tid = this_cpu_read(s->cpu_slab->tid);
		c = raw_cpu_ptr(s->cpu_slab);
	} while (IS_ENABLED(CONFIG_PREEMPTION) &&
		 unlikely(tid != READ_ONCE(c->tid)));

	/* Same with comment on barrier() in slab_alloc_node() */
	barrier();

	if (likely(page == c->page)) {
		void **freelist = READ_ONCE(c->freelist);

		set_freepointer(s, tail_obj, freelist);

		if (unlikely(!this_cpu_cmpxchg_double(
				s->cpu_slab->freelist, s->cpu_slab->tid,
				freelist, tid,
				head, next_tid(tid)))) {

			note_cmpxchg_failure("slab_free", s, tid);
			goto redo;
		}
		stat(s, FREE_FASTPATH);
	} else
		__slab_free(s, page, head, tail_obj, cnt, addr);

}

/**
 * slab_free - 释放对象到slab缓存（支持批量释放）
 * @s: 对象所属的缓存结构体指针
 * @page: 对象所在的slab页面
 * @head: 要释放的对象链表头指针
 * @tail: 要释放的对象链表尾指针（NULL表示单个对象）
 * @cnt: 要释放的对象数量
 * @addr: 调用者地址，用于调试和追踪
 *
 * 这是SLUB释放器的主要入口函数，支持单个或批量对象释放。
 * 在KASAN启用时，会先处理空闲链表钩子来延迟对象重用，
 * 然后调用do_slab_free进行实际的释放操作。
 */
static __always_inline void slab_free(struct kmem_cache *s, struct page *page,
				      void *head, void *tail, int cnt,
				      unsigned long addr)
{
	/*
	 * With KASAN enabled slab_free_freelist_hook modifies the freelist
	 * to remove objects, whose reuse must be delayed.
	 */
	if (slab_free_freelist_hook(s, &head, &tail))
		do_slab_free(s, page, head, tail, cnt, addr);
}

#ifdef CONFIG_KASAN_GENERIC
void ___cache_free(struct kmem_cache *cache, void *x, unsigned long addr)
{
	do_slab_free(cache, virt_to_head_page(x), x, NULL, 1, addr);
}
#endif

/**
 * kmem_cache_free - 释放对象回对象缓存
 * @s: 对象所属的缓存结构体指针
 * @x: 要释放的对象指针
 *
 * 将对象释放回SLUB缓存。会尝试快速释放到per-CPU缓存，
 * 失败时走慢路径处理。包含调试和追踪支持。
 */
void kmem_cache_free(struct kmem_cache *s, void *x)
{
	s = cache_from_obj(s, x);
	if (!s)
		return;
	slab_free(s, virt_to_head_page(x), x, NULL, 1, _RET_IP_);
	trace_kmem_cache_free(_RET_IP_, x);
}
EXPORT_SYMBOL(kmem_cache_free);

/**
 * detached_freelist - 分离的释放列表结构
 * @page: 对象所在的页面
 * @tail: 释放列表的尾指针
 * @freelist: 释放列表的头指针
 * @cnt: 列表中对象的数量
 * @s: 对象所属的slab缓存
 *
 * 用于批量释放操作的临时结构，将属于同一页面的对象
 * 组织成单独的释放列表以便高效批量处理
 */
struct detached_freelist {
	struct page *page;
	void *tail;
	void *freelist;
	int cnt;
	struct kmem_cache *s;
};

/**
 * build_detached_freelist - 构建分离的释放列表
 * @s: slab缓存结构
 * @size: 对象数组大小
 * @p: 要释放的对象指针数组
 * @df: 输出的分离释放列表结构
 *
 * 渐进式扫描对象数组（有限前瞻），提取属于同一页面的对象
 * 在给定的页面/对象内直接构建分离的释放列表，无需同步
 * 因为对象属于运行进程所有。释放列表作为对象中的单链表构建
 *
 * 核心思想是这个分离的释放列表可以批量传输到真实的释放列表，
 * 只需要一个同步原语。由于性能考虑，数组中的前瞻是有限的
 * 返回值: 处理的对象数量
 */
static inline
int build_detached_freelist(struct kmem_cache *s, size_t size,
			    void **p, struct detached_freelist *df)
{
	size_t first_skipped_index = 0;
	int lookahead = 3;
	void *object;
	struct page *page;

	/* Always re-init detached_freelist */
	df->page = NULL;

	do {
		object = p[--size];
		/* Do we need !ZERO_OR_NULL_PTR(object) here? (for kfree) */
	} while (!object && size);

	if (!object)
		return 0;

	page = virt_to_head_page(object);
	if (!s) {
		/* Handle kalloc'ed objects */
		if (unlikely(!PageSlab(page))) {
			BUG_ON(!PageCompound(page));
			kfree_hook(object);
			__free_pages(page, compound_order(page));
			p[size] = NULL; /* mark object processed */
			return size;
		}
		/* Derive kmem_cache from object */
		df->s = page->slab_cache;
	} else {
		df->s = cache_from_obj(s, object); /* Support for memcg */
	}

	/* Start new detached freelist */
	df->page = page;
	set_freepointer(df->s, object, NULL);
	df->tail = object;
	df->freelist = object;
	p[size] = NULL; /* mark object processed */
	df->cnt = 1;

	while (size) {
		object = p[--size];
		if (!object)
			continue; /* Skip processed objects */

		/* df->page is always set at this point */
		if (df->page == virt_to_head_page(object)) {
			/* Opportunity build freelist */
			set_freepointer(df->s, object, df->freelist);
			df->freelist = object;
			df->cnt++;
			p[size] = NULL; /* mark object processed */

			continue;
		}

		/* Limit look ahead search */
		if (!--lookahead)
			break;

		if (!first_skipped_index)
			first_skipped_index = size + 1;
	}

	return first_skipped_index;
}

/**
 * kmem_cache_free_bulk - 批量释放对象到缓存
 * @s: 对象所属的缓存结构
 * @size: 要释放的对象数量
 * @p: 对象指针数组
 *
 * 批量释放对象回slab缓存以提高性能。通过构建分离的释放列表
 * 将属于同一页面的对象组织在一起，然后批量释放以减少锁开销
 *
 * 注意：调用此函数时必须启用中断
 */
void kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p)
{
	if (WARN_ON(!size))
		return;

	memcg_slab_free_hook(s, p, size);
	do {
		struct detached_freelist df;

		size = build_detached_freelist(s, size, p, &df);
		if (!df.page)
			continue;

		slab_free(df.s, df.page, df.freelist, df.tail, df.cnt,_RET_IP_);
	} while (likely(size));
}

/**
 * kmem_cache_alloc_bulk - 从缓存批量分配对象
 * @s: 要分配对象的缓存结构
 * @flags: 分配标志
 * @size: 要分配的对象数量
 * @p: 用于返回对象指针的数组
 *
 * 批量分配对象以提高性能。在禁用本地中断的情况下从per-CPU slab
 * 消耗对象，这样可以保护免受PREEMPT和中断处理器调用正常快速路径的影响
 *
 * 优化策略：
 * - 优先从per-CPU freelist快速分配
 * - freelist为空时调用慢路径重新填充
 * - 支持init_on_alloc的零初始化
 *
 * 注意：调用此函数时必须启用中断
 * 返回值: 成功分配的对象数量
 */
int kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t flags, size_t size,
			  void **p)
{
	struct kmem_cache_cpu *c;
	int i;
	struct obj_cgroup *objcg = NULL;

	/* memcg and kmem_cache debug support */
	s = slab_pre_alloc_hook(s, &objcg, size, flags);
	if (unlikely(!s))
		return false;
	/*
	 * Drain objects in the per cpu slab, while disabling local
	 * IRQs, which protects against PREEMPT and interrupts
	 * handlers invoking normal fastpath.
	 */
	local_irq_disable();
	c = this_cpu_ptr(s->cpu_slab);

	for (i = 0; i < size; i++) {
		void *object = c->freelist;

		if (unlikely(!object)) {
			/*
			 * We may have removed an object from c->freelist using
			 * the fastpath in the previous iteration; in that case,
			 * c->tid has not been bumped yet.
			 * Since ___slab_alloc() may reenable interrupts while
			 * allocating memory, we should bump c->tid now.
			 */
			c->tid = next_tid(c->tid);

			/*
			 * Invoking slow path likely have side-effect
			 * of re-populating per CPU c->freelist
			 */
			p[i] = ___slab_alloc(s, flags, NUMA_NO_NODE,
					    _RET_IP_, c);
			if (unlikely(!p[i]))
				goto error;

			c = this_cpu_ptr(s->cpu_slab);
			maybe_wipe_obj_freeptr(s, p[i]);

			continue; /* goto for-loop */
		}
		c->freelist = get_freepointer(s, object);
		p[i] = object;
		maybe_wipe_obj_freeptr(s, p[i]);
	}
	c->tid = next_tid(c->tid);
	local_irq_enable();

	/* Clear memory outside IRQ disabled fastpath loop */
	if (unlikely(slab_want_init_on_alloc(flags, s))) {
		int j;

		for (j = 0; j < i; j++)
			memset(p[j], 0, s->object_size);
	}

	/* memcg and kmem_cache debug support */
	slab_post_alloc_hook(s, objcg, flags, size, p);
	return i;
error:
	local_irq_enable();
	slab_post_alloc_hook(s, objcg, flags, i, p);
	__kmem_cache_free_bulk(s, i, p);
	return 0;
}


/*
 * Object placement in a slab is made very easy because we always start at
 * offset 0. If we tune the size of the object to the alignment then we can
 * get the required alignment by putting one properly sized object after
 * another.
 *
 * Notice that the allocation order determines the sizes of the per cpu
 * caches. Each processor has always one slab available for allocations.
 * Increasing the allocation order reduces the number of times that slabs
 * must be moved on and off the partial lists and is therefore a factor in
 * locking overhead.
 */

/*
 * Mininum / Maximum order of slab pages. This influences locking overhead
 * and slab fragmentation. A higher order reduces the number of partial slabs
 * and increases the number of allocations possible without having to
 * take the list_lock.
 */
static unsigned int slub_min_order;
static unsigned int slub_max_order = PAGE_ALLOC_COSTLY_ORDER;
static unsigned int slub_min_objects;

/**
 * slab_order - 计算给定参数下的slab分配顺序
 * @size: 对象大小
 * @min_objects: 最少对象数量
 * @max_order: 最大分配顺序
 * @fract_leftover: 允许的剩余空间分数
 *
 * 计算分配顺序对性能和系统组件有重大影响。一般来说应该优先选择
 * 0阶分配，因为0阶不会在页面分配器中造成碎片。较大的对象放入
 * 0阶slab可能有问题，因为可能会留下太多未使用空间。如果slab
 * 浪费超过1/16，我们会使用更高的顺序
 *
 * 返回值: 计算出的分配顺序
 */
static inline unsigned int slab_order(unsigned int size,
		unsigned int min_objects, unsigned int max_order,
		unsigned int fract_leftover)
{
	unsigned int min_order = slub_min_order;
	unsigned int order;

	if (order_objects(min_order, size) > MAX_OBJS_PER_PAGE)
		return get_order(size * MAX_OBJS_PER_PAGE) - 1;

	for (order = max(min_order, (unsigned int)get_order(min_objects * size));
			order <= max_order; order++) {

		unsigned int slab_size = (unsigned int)PAGE_SIZE << order;
		unsigned int rem;

		rem = slab_size % size;

		if (rem <= slab_size / fract_leftover)
			break;
	}

	return order;
}

/**
 * calculate_order - 计算slab的最佳分配顺序
 * @size: 对象大小
 *
 * 尝试为slab找到最佳配置，通过首先尝试生成最佳配置的布局
 * 然后逐渐回退的方式工作
 *
 * 算法策略：
 * 1. 首先增加slab中可接受的浪费空间
 * 2. 然后减少slab中所需的最少对象数量
 * 3. 如果无法放置多个对象，尝试放置单个对象
 * 4. 最后使用MAX_ORDER进行强制分配
 *
 * 返回值: 计算出的分配顺序，失败时返回-ENOSYS
 */
static inline int calculate_order(unsigned int size)
{
	unsigned int order;
	unsigned int min_objects;
	unsigned int max_objects;

	/*
	 * Attempt to find best configuration for a slab. This
	 * works by first attempting to generate a layout with
	 * the best configuration and backing off gradually.
	 *
	 * First we increase the acceptable waste in a slab. Then
	 * we reduce the minimum objects required in a slab.
	 */
	min_objects = slub_min_objects;
	if (!min_objects)
		min_objects = 4 * (fls(nr_cpu_ids) + 1);
	max_objects = order_objects(slub_max_order, size);
	min_objects = min(min_objects, max_objects);

	while (min_objects > 1) {
		unsigned int fraction;

		fraction = 16;
		while (fraction >= 4) {
			order = slab_order(size, min_objects,
					slub_max_order, fraction);
			if (order <= slub_max_order)
				return order;
			fraction /= 2;
		}
		min_objects--;
	}

	/*
	 * We were unable to place multiple objects in a slab. Now
	 * lets see if we can place a single object there.
	 */
	order = slab_order(size, 1, slub_max_order, 1);
	if (order <= slub_max_order)
		return order;

	/*
	 * Doh this slab cannot be placed using slub_max_order.
	 */
	order = slab_order(size, 1, MAX_ORDER, 1);
	if (order < MAX_ORDER)
		return order;
	return -ENOSYS;
}

/**
 * init_kmem_cache_node - 初始化kmem_cache_node结构
 * @n: 要初始化的节点结构
 *
 * 初始化slab缓存节点的各种字段：
 * - 设置partial slab计数为0
 * - 初始化列表锁和partial链表
 * - 在调试模式下初始化slab和对象计数器以及full链表
 */
static void
init_kmem_cache_node(struct kmem_cache_node *n)
{
	n->nr_partial = 0;
	spin_lock_init(&n->list_lock);
	INIT_LIST_HEAD(&n->partial);
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_set(&n->nr_slabs, 0);
	atomic_long_set(&n->total_objects, 0);
	INIT_LIST_HEAD(&n->full);
#endif
}

/**
 * alloc_kmem_cache_cpus - 为slab缓存分配per-CPU结构
 * @s: slab缓存结构
 *
 * 分配并初始化slab缓存的per-CPU结构：
 * - 必须对齐到双字边界以支持双重cmpxchg指令
 * - 分配per-CPU内存并初始化所有CPU的结构
 * 返回值: 成功返回1，失败返回0
 */
static inline int alloc_kmem_cache_cpus(struct kmem_cache *s)
{
	BUILD_BUG_ON(PERCPU_DYNAMIC_EARLY_SIZE <
			KMALLOC_SHIFT_HIGH * sizeof(struct kmem_cache_cpu));

	/*
	 * Must align to double word boundary for the double cmpxchg
	 * instructions to work; see __pcpu_double_call_return_bool().
	 */
	s->cpu_slab = __alloc_percpu(sizeof(struct kmem_cache_cpu),
				     2 * sizeof(void *));

	if (!s->cpu_slab)
		return 0;

	init_kmem_cache_cpus(s);

	return 1;
}

static struct kmem_cache *kmem_cache_node;

/*
 * No kmalloc_node yet so do it by hand. We know that this is the first
 * slab on the node for this slabcache. There are no concurrent accesses
 * possible.
 *
 * Note that this function only works on the kmem_cache_node
 * when allocating for the kmem_cache_node. This is used for bootstrapping
 * memory on a fresh node that has no slab structures yet.
 */
static void early_kmem_cache_node_alloc(int node)
{
	struct page *page;
	struct kmem_cache_node *n;

	BUG_ON(kmem_cache_node->size < sizeof(struct kmem_cache_node));

	page = new_slab(kmem_cache_node, GFP_NOWAIT, node);

	BUG_ON(!page);
	if (page_to_nid(page) != node) {
		pr_err("SLUB: Unable to allocate memory from node %d\n", node);
		pr_err("SLUB: Allocating a useless per node structure in order to be able to continue\n");
	}

	n = page->freelist;
	BUG_ON(!n);
#ifdef CONFIG_SLUB_DEBUG
	init_object(kmem_cache_node, n, SLUB_RED_ACTIVE);
	init_tracking(kmem_cache_node, n);
#endif
	n = kasan_kmalloc(kmem_cache_node, n, sizeof(struct kmem_cache_node),
		      GFP_KERNEL);
	page->freelist = get_freepointer(kmem_cache_node, n);
	page->inuse = 1;
	page->frozen = 0;
	kmem_cache_node->node[node] = n;
	init_kmem_cache_node(n);
	inc_slabs_node(kmem_cache_node, node, page->objects);

	/*
	 * No locks need to be taken here as it has just been
	 * initialized and there is no concurrent access.
	 */
	__add_partial(n, page, DEACTIVATE_TO_HEAD);
}

static void free_kmem_cache_nodes(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n) {
		s->node[node] = NULL;
		kmem_cache_free(kmem_cache_node, n);
	}
}

void __kmem_cache_release(struct kmem_cache *s)
{
	cache_random_seq_destroy(s);
	free_percpu(s->cpu_slab);
	free_kmem_cache_nodes(s);
}

static int init_kmem_cache_nodes(struct kmem_cache *s)
{
	int node;

	for_each_node_state(node, N_NORMAL_MEMORY) {
		struct kmem_cache_node *n;

		if (slab_state == DOWN) {
			early_kmem_cache_node_alloc(node);
			continue;
		}
		n = kmem_cache_alloc_node(kmem_cache_node,
						GFP_KERNEL, node);

		if (!n) {
			free_kmem_cache_nodes(s);
			return 0;
		}

		init_kmem_cache_node(n);
		s->node[node] = n;
	}
	return 1;
}

static void set_min_partial(struct kmem_cache *s, unsigned long min)
{
	if (min < MIN_PARTIAL)
		min = MIN_PARTIAL;
	else if (min > MAX_PARTIAL)
		min = MAX_PARTIAL;
	s->min_partial = min;
}

static void set_cpu_partial(struct kmem_cache *s)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
	/*
	 * cpu_partial determined the maximum number of objects kept in the
	 * per cpu partial lists of a processor.
	 *
	 * Per cpu partial lists mainly contain slabs that just have one
	 * object freed. If they are used for allocation then they can be
	 * filled up again with minimal effort. The slab will never hit the
	 * per node partial lists and therefore no locking will be required.
	 *
	 * This setting also determines
	 *
	 * A) The number of objects from per cpu partial slabs dumped to the
	 *    per node list when we reach the limit.
	 * B) The number of objects in cpu partial slabs to extract from the
	 *    per node list when we run out of per cpu objects. We only fetch
	 *    50% to keep some capacity around for frees.
	 */
	if (!kmem_cache_has_cpu_partial(s))
		slub_set_cpu_partial(s, 0);
	else if (s->size >= PAGE_SIZE)
		slub_set_cpu_partial(s, 2);
	else if (s->size >= 1024)
		slub_set_cpu_partial(s, 6);
	else if (s->size >= 256)
		slub_set_cpu_partial(s, 13);
	else
		slub_set_cpu_partial(s, 30);
#endif
}

/*
 * calculate_sizes() determines the order and the distribution of data within
 * a slab object.
 */
static int calculate_sizes(struct kmem_cache *s, int forced_order)
{
	slab_flags_t flags = s->flags;
	unsigned int size = s->object_size;
	unsigned int freepointer_area;
	unsigned int order;

	/*
	 * Round up object size to the next word boundary. We can only
	 * place the free pointer at word boundaries and this determines
	 * the possible location of the free pointer.
	 */
	size = ALIGN(size, sizeof(void *));
	/*
	 * This is the area of the object where a freepointer can be
	 * safely written. If redzoning adds more to the inuse size, we
	 * can't use that portion for writing the freepointer, so
	 * s->offset must be limited within this for the general case.
	 */
	freepointer_area = size;

#ifdef CONFIG_SLUB_DEBUG
	/*
	 * Determine if we can poison the object itself. If the user of
	 * the slab may touch the object after free or before allocation
	 * then we should never poison the object itself.
	 */
	if ((flags & SLAB_POISON) && !(flags & SLAB_TYPESAFE_BY_RCU) &&
			!s->ctor)
		s->flags |= __OBJECT_POISON;
	else
		s->flags &= ~__OBJECT_POISON;


	/*
	 * If we are Redzoning then check if there is some space between the
	 * end of the object and the free pointer. If not then add an
	 * additional word to have some bytes to store Redzone information.
	 */
	if ((flags & SLAB_RED_ZONE) && size == s->object_size)
		size += sizeof(void *);
#endif

	/*
	 * With that we have determined the number of bytes in actual use
	 * by the object. This is the potential offset to the free pointer.
	 */
	s->inuse = size;

	if (((flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON)) ||
		s->ctor)) {
		/*
		 * Relocate free pointer after the object if it is not
		 * permitted to overwrite the first word of the object on
		 * kmem_cache_free.
		 *
		 * This is the case if we do RCU, have a constructor or
		 * destructor or are poisoning the objects.
		 *
		 * The assumption that s->offset >= s->inuse means free
		 * pointer is outside of the object is used in the
		 * freeptr_outside_object() function. If that is no
		 * longer true, the function needs to be modified.
		 */
		s->offset = size;
		size += sizeof(void *);
	} else if (freepointer_area > sizeof(void *)) {
		/*
		 * Store freelist pointer near middle of object to keep
		 * it away from the edges of the object to avoid small
		 * sized over/underflows from neighboring allocations.
		 */
		s->offset = ALIGN(freepointer_area / 2, sizeof(void *));
	}

#ifdef CONFIG_SLUB_DEBUG
	if (flags & SLAB_STORE_USER)
		/*
		 * Need to store information about allocs and frees after
		 * the object.
		 */
		size += 2 * sizeof(struct track);
#endif

	kasan_cache_create(s, &size, &s->flags);
#ifdef CONFIG_SLUB_DEBUG
	if (flags & SLAB_RED_ZONE) {
		/*
		 * Add some empty padding so that we can catch
		 * overwrites from earlier objects rather than let
		 * tracking information or the free pointer be
		 * corrupted if a user writes before the start
		 * of the object.
		 */
		size += sizeof(void *);

		s->red_left_pad = sizeof(void *);
		s->red_left_pad = ALIGN(s->red_left_pad, s->align);
		size += s->red_left_pad;
	}
#endif

	/*
	 * SLUB stores one object immediately after another beginning from
	 * offset 0. In order to align the objects we have to simply size
	 * each object to conform to the alignment.
	 */
	size = ALIGN(size, s->align);
	s->size = size;
	s->reciprocal_size = reciprocal_value(size);
	if (forced_order >= 0)
		order = forced_order;
	else
		order = calculate_order(size);

	if ((int)order < 0)
		return 0;

	s->allocflags = 0;
	if (order)
		s->allocflags |= __GFP_COMP;

	if (s->flags & SLAB_CACHE_DMA)
		s->allocflags |= GFP_DMA;

	if (s->flags & SLAB_CACHE_DMA32)
		s->allocflags |= GFP_DMA32;

	if (s->flags & SLAB_RECLAIM_ACCOUNT)
		s->allocflags |= __GFP_RECLAIMABLE;

	/*
	 * Determine the number of objects per slab
	 */
	s->oo = oo_make(order, size);
	s->min = oo_make(get_order(size), size);
	if (oo_objects(s->oo) > oo_objects(s->max))
		s->max = s->oo;

	return !!oo_objects(s->oo);
}

static int kmem_cache_open(struct kmem_cache *s, slab_flags_t flags)
{
	s->flags = kmem_cache_flags(s->size, flags, s->name, s->ctor);
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	s->random = get_random_long();
#endif

	if (!calculate_sizes(s, -1))
		goto error;
	if (disable_higher_order_debug) {
		/*
		 * Disable debugging flags that store metadata if the min slab
		 * order increased.
		 */
		if (get_order(s->size) > get_order(s->object_size)) {
			s->flags &= ~DEBUG_METADATA_FLAGS;
			s->offset = 0;
			if (!calculate_sizes(s, -1))
				goto error;
		}
	}

#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
    defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
	if (system_has_cmpxchg_double() && (s->flags & SLAB_NO_CMPXCHG) == 0)
		/* Enable fast mode */
		s->flags |= __CMPXCHG_DOUBLE;
#endif

	/*
	 * The larger the object size is, the more pages we want on the partial
	 * list to avoid pounding the page allocator excessively.
	 */
	set_min_partial(s, ilog2(s->size) / 2);

	set_cpu_partial(s);

#ifdef CONFIG_NUMA
	s->remote_node_defrag_ratio = 1000;
#endif

	/* Initialize the pre-computed randomized freelist if slab is up */
	if (slab_state >= UP) {
		if (init_cache_random_seq(s))
			goto error;
	}

	if (!init_kmem_cache_nodes(s))
		goto error;

	if (alloc_kmem_cache_cpus(s))
		return 0;

	free_kmem_cache_nodes(s);
error:
	return -EINVAL;
}

static void list_slab_objects(struct kmem_cache *s, struct page *page,
			      const char *text)
{
#ifdef CONFIG_SLUB_DEBUG
	void *addr = page_address(page);
	unsigned long *map;
	void *p;

	slab_err(s, page, text, s->name);
	slab_lock(page);

	map = get_map(s, page);
	for_each_object(p, s, addr, page->objects) {

		if (!test_bit(__obj_to_index(s, addr, p), map)) {
			pr_err("INFO: Object 0x%p @offset=%tu\n", p, p - addr);
			print_tracking(s, p);
		}
	}
	put_map(map);
	slab_unlock(page);
#endif
}

/*
 * Attempt to free all partial slabs on a node.
 * This is called from __kmem_cache_shutdown(). We must take list_lock
 * because sysfs file might still access partial list after the shutdowning.
 */
static void free_partial(struct kmem_cache *s, struct kmem_cache_node *n)
{
	LIST_HEAD(discard);
	struct page *page, *h;

	BUG_ON(irqs_disabled());
	spin_lock_irq(&n->list_lock);
	list_for_each_entry_safe(page, h, &n->partial, slab_list) {
		if (!page->inuse) {
			remove_partial(n, page);
			list_add(&page->slab_list, &discard);
		} else {
			list_slab_objects(s, page,
			  "Objects remaining in %s on __kmem_cache_shutdown()");
		}
	}
	spin_unlock_irq(&n->list_lock);

	list_for_each_entry_safe(page, h, &discard, slab_list)
		discard_slab(s, page);
}

bool __kmem_cache_empty(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n)
		if (n->nr_partial || slabs_node(s, node))
			return false;
	return true;
}

/*
 * Release all resources used by a slab cache.
 */
int __kmem_cache_shutdown(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	flush_all(s);
	/* Attempt to free all objects */
	for_each_kmem_cache_node(s, node, n) {
		free_partial(s, n);
		if (n->nr_partial || slabs_node(s, node))
			return 1;
	}
	return 0;
}

/********************************************************************
 *		Kmalloc subsystem
 *******************************************************************/

static int __init setup_slub_min_order(char *str)
{
	get_option(&str, (int *)&slub_min_order);

	return 1;
}

__setup("slub_min_order=", setup_slub_min_order);

static int __init setup_slub_max_order(char *str)
{
	get_option(&str, (int *)&slub_max_order);
	slub_max_order = min(slub_max_order, (unsigned int)MAX_ORDER - 1);

	return 1;
}

__setup("slub_max_order=", setup_slub_max_order);

static int __init setup_slub_min_objects(char *str)
{
	get_option(&str, (int *)&slub_min_objects);

	return 1;
}

__setup("slub_min_objects=", setup_slub_min_objects);

void *__kmalloc(size_t size, gfp_t flags)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE))
		return kmalloc_large(size, flags);

	s = kmalloc_slab(size, flags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc(s, flags, _RET_IP_);

	trace_kmalloc(_RET_IP_, ret, size, s->size, flags);

	ret = kasan_kmalloc(s, ret, size, flags);

	return ret;
}
EXPORT_SYMBOL(__kmalloc);

#ifdef CONFIG_NUMA
static void *kmalloc_large_node(size_t size, gfp_t flags, int node)
{
	struct page *page;
	void *ptr = NULL;
	unsigned int order = get_order(size);

	flags |= __GFP_COMP;
	page = alloc_pages_node(node, flags, order);
	if (page) {
		ptr = page_address(page);
		mod_node_page_state(page_pgdat(page), NR_SLAB_UNRECLAIMABLE_B,
				    PAGE_SIZE << order);
	}

	return kmalloc_large_node_hook(ptr, size, flags);
}

void *__kmalloc_node(size_t size, gfp_t flags, int node)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
		ret = kmalloc_large_node(size, flags, node);

		trace_kmalloc_node(_RET_IP_, ret,
				   size, PAGE_SIZE << get_order(size),
				   flags, node);

		return ret;
	}

	s = kmalloc_slab(size, flags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc_node(s, flags, node, _RET_IP_);

	trace_kmalloc_node(_RET_IP_, ret, size, s->size, flags, node);

	ret = kasan_kmalloc(s, ret, size, flags);

	return ret;
}
EXPORT_SYMBOL(__kmalloc_node);
#endif	/* CONFIG_NUMA */

#ifdef CONFIG_HARDENED_USERCOPY
/*
 * Rejects incorrectly sized objects and objects that are to be copied
 * to/from userspace but do not fall entirely within the containing slab
 * cache's usercopy region.
 *
 * Returns NULL if check passes, otherwise const char * to name of cache
 * to indicate an error.
 */
void __check_heap_object(const void *ptr, unsigned long n, struct page *page,
			 bool to_user)
{
	struct kmem_cache *s;
	unsigned int offset;
	size_t object_size;

	ptr = kasan_reset_tag(ptr);

	/* Find object and usable object size. */
	s = page->slab_cache;

	/* Reject impossible pointers. */
	if (ptr < page_address(page))
		usercopy_abort("SLUB object not in SLUB page?!", NULL,
			       to_user, 0, n);

	/* Find offset within object. */
	offset = (ptr - page_address(page)) % s->size;

	/* Adjust for redzone and reject if within the redzone. */
	if (kmem_cache_debug_flags(s, SLAB_RED_ZONE)) {
		if (offset < s->red_left_pad)
			usercopy_abort("SLUB object in left red zone",
				       s->name, to_user, offset, n);
		offset -= s->red_left_pad;
	}

	/* Allow address range falling entirely within usercopy region. */
	if (offset >= s->useroffset &&
	    offset - s->useroffset <= s->usersize &&
	    n <= s->useroffset - offset + s->usersize)
		return;

	/*
	 * If the copy is still within the allocated object, produce
	 * a warning instead of rejecting the copy. This is intended
	 * to be a temporary method to find any missing usercopy
	 * whitelists.
	 */
	object_size = slab_ksize(s);
	if (usercopy_fallback &&
	    offset <= object_size && n <= object_size - offset) {
		usercopy_warn("SLUB object", s->name, to_user, offset, n);
		return;
	}

	usercopy_abort("SLUB object", s->name, to_user, offset, n);
}
#endif /* CONFIG_HARDENED_USERCOPY */

size_t __ksize(const void *object)
{
	struct page *page;

	if (unlikely(object == ZERO_SIZE_PTR))
		return 0;

	page = virt_to_head_page(object);

	if (unlikely(!PageSlab(page))) {
		WARN_ON(!PageCompound(page));
		return page_size(page);
	}

	return slab_ksize(page->slab_cache);
}
EXPORT_SYMBOL(__ksize);

void kfree(const void *x)
{
	struct page *page;
	void *object = (void *)x;

	trace_kfree(_RET_IP_, x);

	if (unlikely(ZERO_OR_NULL_PTR(x)))
		return;

	page = virt_to_head_page(x);
	if (unlikely(!PageSlab(page))) {
		unsigned int order = compound_order(page);

		BUG_ON(!PageCompound(page));
		kfree_hook(object);
		mod_node_page_state(page_pgdat(page), NR_SLAB_UNRECLAIMABLE_B,
				    -(PAGE_SIZE << order));
		__free_pages(page, order);
		return;
	}
	slab_free(page->slab_cache, page, object, NULL, 1, _RET_IP_);
}
EXPORT_SYMBOL(kfree);

#define SHRINK_PROMOTE_MAX 32

/*
 * kmem_cache_shrink discards empty slabs and promotes the slabs filled
 * up most to the head of the partial lists. New allocations will then
 * fill those up and thus they can be removed from the partial lists.
 *
 * The slabs with the least items are placed last. This results in them
 * being allocated from last increasing the chance that the last objects
 * are freed in them.
 */
int __kmem_cache_shrink(struct kmem_cache *s)
{
	int node;
	int i;
	struct kmem_cache_node *n;
	struct page *page;
	struct page *t;
	struct list_head discard;
	struct list_head promote[SHRINK_PROMOTE_MAX];
	unsigned long flags;
	int ret = 0;

	flush_all(s);
	for_each_kmem_cache_node(s, node, n) {
		INIT_LIST_HEAD(&discard);
		for (i = 0; i < SHRINK_PROMOTE_MAX; i++)
			INIT_LIST_HEAD(promote + i);

		spin_lock_irqsave(&n->list_lock, flags);

		/*
		 * Build lists of slabs to discard or promote.
		 *
		 * Note that concurrent frees may occur while we hold the
		 * list_lock. page->inuse here is the upper limit.
		 */
		list_for_each_entry_safe(page, t, &n->partial, slab_list) {
			int free = page->objects - page->inuse;

			/* Do not reread page->inuse */
			barrier();

			/* We do not keep full slabs on the list */
			BUG_ON(free <= 0);

			if (free == page->objects) {
				list_move(&page->slab_list, &discard);
				n->nr_partial--;
			} else if (free <= SHRINK_PROMOTE_MAX)
				list_move(&page->slab_list, promote + free - 1);
		}

		/*
		 * Promote the slabs filled up most to the head of the
		 * partial list.
		 */
		for (i = SHRINK_PROMOTE_MAX - 1; i >= 0; i--)
			list_splice(promote + i, &n->partial);

		spin_unlock_irqrestore(&n->list_lock, flags);

		/* Release empty slabs */
		list_for_each_entry_safe(page, t, &discard, slab_list)
			discard_slab(s, page);

		if (slabs_node(s, node))
			ret = 1;
	}

	return ret;
}

static int slab_mem_going_offline_callback(void *arg)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list)
		__kmem_cache_shrink(s);
	mutex_unlock(&slab_mutex);

	return 0;
}

static void slab_mem_offline_callback(void *arg)
{
	struct kmem_cache_node *n;
	struct kmem_cache *s;
	struct memory_notify *marg = arg;
	int offline_node;

	offline_node = marg->status_change_nid_normal;

	/*
	 * If the node still has available memory. we need kmem_cache_node
	 * for it yet.
	 */
	if (offline_node < 0)
		return;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		n = get_node(s, offline_node);
		if (n) {
			/*
			 * if n->nr_slabs > 0, slabs still exist on the node
			 * that is going down. We were unable to free them,
			 * and offline_pages() function shouldn't call this
			 * callback. So, we must fail.
			 */
			BUG_ON(slabs_node(s, offline_node));

			s->node[offline_node] = NULL;
			kmem_cache_free(kmem_cache_node, n);
		}
	}
	mutex_unlock(&slab_mutex);
}

static int slab_mem_going_online_callback(void *arg)
{
	struct kmem_cache_node *n;
	struct kmem_cache *s;
	struct memory_notify *marg = arg;
	int nid = marg->status_change_nid_normal;
	int ret = 0;

	/*
	 * If the node's memory is already available, then kmem_cache_node is
	 * already created. Nothing to do.
	 */
	if (nid < 0)
		return 0;

	/*
	 * We are bringing a node online. No memory is available yet. We must
	 * allocate a kmem_cache_node structure in order to bring the node
	 * online.
	 */
	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		/*
		 * XXX: kmem_cache_alloc_node will fallback to other nodes
		 *      since memory is not yet available from the node that
		 *      is brought up.
		 */
		n = kmem_cache_alloc(kmem_cache_node, GFP_KERNEL);
		if (!n) {
			ret = -ENOMEM;
			goto out;
		}
		init_kmem_cache_node(n);
		s->node[nid] = n;
	}
out:
	mutex_unlock(&slab_mutex);
	return ret;
}

static int slab_memory_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	int ret = 0;

	switch (action) {
	case MEM_GOING_ONLINE:
		ret = slab_mem_going_online_callback(arg);
		break;
	case MEM_GOING_OFFLINE:
		ret = slab_mem_going_offline_callback(arg);
		break;
	case MEM_OFFLINE:
	case MEM_CANCEL_ONLINE:
		slab_mem_offline_callback(arg);
		break;
	case MEM_ONLINE:
	case MEM_CANCEL_OFFLINE:
		break;
	}
	if (ret)
		ret = notifier_from_errno(ret);
	else
		ret = NOTIFY_OK;
	return ret;
}

static struct notifier_block slab_memory_callback_nb = {
	.notifier_call = slab_memory_callback,
	.priority = SLAB_CALLBACK_PRI,
};

/********************************************************************
 *			Basic setup of slabs
 *******************************************************************/

/*
 * Used for early kmem_cache structures that were allocated using
 * the page allocator. Allocate them properly then fix up the pointers
 * that may be pointing to the wrong kmem_cache structure.
 */

static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
	int node;
	struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
	struct kmem_cache_node *n;

	memcpy(s, static_cache, kmem_cache->object_size);

	/*
	 * This runs very early, and only the boot processor is supposed to be
	 * up.  Even if it weren't true, IRQs are not up so we couldn't fire
	 * IPIs around.
	 */
	__flush_cpu_slab(s, smp_processor_id());
	for_each_kmem_cache_node(s, node, n) {
		struct page *p;

		list_for_each_entry(p, &n->partial, slab_list)
			p->slab_cache = s;

#ifdef CONFIG_SLUB_DEBUG
		list_for_each_entry(p, &n->full, slab_list)
			p->slab_cache = s;
#endif
	}
	list_add(&s->list, &slab_caches);
	return s;
}

void __init kmem_cache_init(void)
{
	static __initdata struct kmem_cache boot_kmem_cache,
		boot_kmem_cache_node;

	if (debug_guardpage_minorder())
		slub_max_order = 0;

	kmem_cache_node = &boot_kmem_cache_node;
	kmem_cache = &boot_kmem_cache;

	create_boot_cache(kmem_cache_node, "kmem_cache_node",
		sizeof(struct kmem_cache_node), SLAB_HWCACHE_ALIGN, 0, 0);

	register_hotmemory_notifier(&slab_memory_callback_nb);

	/* Able to allocate the per node structures */
	slab_state = PARTIAL;

	create_boot_cache(kmem_cache, "kmem_cache",
			offsetof(struct kmem_cache, node) +
				nr_node_ids * sizeof(struct kmem_cache_node *),
		       SLAB_HWCACHE_ALIGN, 0, 0);

	kmem_cache = bootstrap(&boot_kmem_cache);
	kmem_cache_node = bootstrap(&boot_kmem_cache_node);

	/* Now we can use the kmem_cache to allocate kmalloc slabs */
	setup_kmalloc_cache_index_table();
	create_kmalloc_caches(0);

	/* Setup random freelists for each cache */
	init_freelist_randomization();

	cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", NULL,
				  slub_cpu_dead);

	pr_info("SLUB: HWalign=%d, Order=%u-%u, MinObjects=%u, CPUs=%u, Nodes=%u\n",
		cache_line_size(),
		slub_min_order, slub_max_order, slub_min_objects,
		nr_cpu_ids, nr_node_ids);
}

void __init kmem_cache_init_late(void)
{
}

struct kmem_cache *
__kmem_cache_alias(const char *name, unsigned int size, unsigned int align,
		   slab_flags_t flags, void (*ctor)(void *))
{
	struct kmem_cache *s;

	s = find_mergeable(size, align, flags, name, ctor);
	if (s) {
		s->refcount++;

		/*
		 * Adjust the object sizes so that we clear
		 * the complete object on kzalloc.
		 */
		s->object_size = max(s->object_size, size);
		s->inuse = max(s->inuse, ALIGN(size, sizeof(void *)));

		if (sysfs_slab_alias(s, name)) {
			s->refcount--;
			s = NULL;
		}
	}

	return s;
}

/**
 * __kmem_cache_create - 创建新的对象缓存
 * @s: 要初始化的缓存结构体指针
 * @flags: 缓存创建标志
 *
 * 返回值：成功返回0，失败返回错误码
 *
 * SLUB分配器的缓存创建函数。初始化缓存结构，设置对象大小、
 * 对齐方式等参数，并在系统启动完成后注册到sysfs。
 */
int __kmem_cache_create(struct kmem_cache *s, slab_flags_t flags)
{
	int err;

	err = kmem_cache_open(s, flags);
	if (err)
		return err;

	/* Mutex is not taken during early boot */
	if (slab_state <= UP)
		return 0;

	err = sysfs_slab_add(s);
	if (err)
		__kmem_cache_release(s);

	return err;
}

void *__kmalloc_track_caller(size_t size, gfp_t gfpflags, unsigned long caller)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE))
		return kmalloc_large(size, gfpflags);

	s = kmalloc_slab(size, gfpflags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc(s, gfpflags, caller);

	/* Honor the call site pointer we received. */
	trace_kmalloc(caller, ret, size, s->size, gfpflags);

	return ret;
}
EXPORT_SYMBOL(__kmalloc_track_caller);

#ifdef CONFIG_NUMA
void *__kmalloc_node_track_caller(size_t size, gfp_t gfpflags,
					int node, unsigned long caller)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
		ret = kmalloc_large_node(size, gfpflags, node);

		trace_kmalloc_node(caller, ret,
				   size, PAGE_SIZE << get_order(size),
				   gfpflags, node);

		return ret;
	}

	s = kmalloc_slab(size, gfpflags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc_node(s, gfpflags, node, caller);

	/* Honor the call site pointer we received. */
	trace_kmalloc_node(caller, ret, size, s->size, gfpflags, node);

	return ret;
}
EXPORT_SYMBOL(__kmalloc_node_track_caller);
#endif

#ifdef CONFIG_SYSFS
static int count_inuse(struct page *page)
{
	return page->inuse;
}

static int count_total(struct page *page)
{
	return page->objects;
}
#endif

#ifdef CONFIG_SLUB_DEBUG
static void validate_slab(struct kmem_cache *s, struct page *page)
{
	void *p;
	void *addr = page_address(page);
	unsigned long *map;

	slab_lock(page);

	if (!check_slab(s, page) || !on_freelist(s, page, NULL))
		goto unlock;

	/* Now we know that a valid freelist exists */
	map = get_map(s, page);
	for_each_object(p, s, addr, page->objects) {
		u8 val = test_bit(__obj_to_index(s, addr, p), map) ?
			 SLUB_RED_INACTIVE : SLUB_RED_ACTIVE;

		if (!check_object(s, page, p, val))
			break;
	}
	put_map(map);
unlock:
	slab_unlock(page);
}

static int validate_slab_node(struct kmem_cache *s,
		struct kmem_cache_node *n)
{
	unsigned long count = 0;
	struct page *page;
	unsigned long flags;

	spin_lock_irqsave(&n->list_lock, flags);

	list_for_each_entry(page, &n->partial, slab_list) {
		validate_slab(s, page);
		count++;
	}
	if (count != n->nr_partial)
		pr_err("SLUB %s: %ld partial slabs counted but counter=%ld\n",
		       s->name, count, n->nr_partial);

	if (!(s->flags & SLAB_STORE_USER))
		goto out;

	list_for_each_entry(page, &n->full, slab_list) {
		validate_slab(s, page);
		count++;
	}
	if (count != atomic_long_read(&n->nr_slabs))
		pr_err("SLUB: %s %ld slabs counted but counter=%ld\n",
		       s->name, count, atomic_long_read(&n->nr_slabs));

out:
	spin_unlock_irqrestore(&n->list_lock, flags);
	return count;
}

static long validate_slab_cache(struct kmem_cache *s)
{
	int node;
	unsigned long count = 0;
	struct kmem_cache_node *n;

	flush_all(s);
	for_each_kmem_cache_node(s, node, n)
		count += validate_slab_node(s, n);

	return count;
}
/*
 * Generate lists of code addresses where slabcache objects are allocated
 * and freed.
 */

struct location {
	unsigned long count;
	unsigned long addr;
	long long sum_time;
	long min_time;
	long max_time;
	long min_pid;
	long max_pid;
	DECLARE_BITMAP(cpus, NR_CPUS);
	nodemask_t nodes;
};

struct loc_track {
	unsigned long max;
	unsigned long count;
	struct location *loc;
};

static void free_loc_track(struct loc_track *t)
{
	if (t->max)
		free_pages((unsigned long)t->loc,
			get_order(sizeof(struct location) * t->max));
}

static int alloc_loc_track(struct loc_track *t, unsigned long max, gfp_t flags)
{
	struct location *l;
	int order;

	order = get_order(sizeof(struct location) * max);

	l = (void *)__get_free_pages(flags, order);
	if (!l)
		return 0;

	if (t->count) {
		memcpy(l, t->loc, sizeof(struct location) * t->count);
		free_loc_track(t);
	}
	t->max = max;
	t->loc = l;
	return 1;
}

static int add_location(struct loc_track *t, struct kmem_cache *s,
				const struct track *track)
{
	long start, end, pos;
	struct location *l;
	unsigned long caddr;
	unsigned long age = jiffies - track->when;

	start = -1;
	end = t->count;

	for ( ; ; ) {
		pos = start + (end - start + 1) / 2;

		/*
		 * There is nothing at "end". If we end up there
		 * we need to add something to before end.
		 */
		if (pos == end)
			break;

		caddr = t->loc[pos].addr;
		if (track->addr == caddr) {

			l = &t->loc[pos];
			l->count++;
			if (track->when) {
				l->sum_time += age;
				if (age < l->min_time)
					l->min_time = age;
				if (age > l->max_time)
					l->max_time = age;

				if (track->pid < l->min_pid)
					l->min_pid = track->pid;
				if (track->pid > l->max_pid)
					l->max_pid = track->pid;

				cpumask_set_cpu(track->cpu,
						to_cpumask(l->cpus));
			}
			node_set(page_to_nid(virt_to_page(track)), l->nodes);
			return 1;
		}

		if (track->addr < caddr)
			end = pos;
		else
			start = pos;
	}

	/*
	 * Not found. Insert new tracking element.
	 */
	if (t->count >= t->max && !alloc_loc_track(t, 2 * t->max, GFP_ATOMIC))
		return 0;

	l = t->loc + pos;
	if (pos < t->count)
		memmove(l + 1, l,
			(t->count - pos) * sizeof(struct location));
	t->count++;
	l->count = 1;
	l->addr = track->addr;
	l->sum_time = age;
	l->min_time = age;
	l->max_time = age;
	l->min_pid = track->pid;
	l->max_pid = track->pid;
	cpumask_clear(to_cpumask(l->cpus));
	cpumask_set_cpu(track->cpu, to_cpumask(l->cpus));
	nodes_clear(l->nodes);
	node_set(page_to_nid(virt_to_page(track)), l->nodes);
	return 1;
}

static void process_slab(struct loc_track *t, struct kmem_cache *s,
		struct page *page, enum track_item alloc)
{
	void *addr = page_address(page);
	void *p;
	unsigned long *map;

	map = get_map(s, page);
	for_each_object(p, s, addr, page->objects)
		if (!test_bit(__obj_to_index(s, addr, p), map))
			add_location(t, s, get_track(s, p, alloc));
	put_map(map);
}

static int list_locations(struct kmem_cache *s, char *buf,
					enum track_item alloc)
{
	int len = 0;
	unsigned long i;
	struct loc_track t = { 0, 0, NULL };
	int node;
	struct kmem_cache_node *n;

	if (!alloc_loc_track(&t, PAGE_SIZE / sizeof(struct location),
			     GFP_KERNEL)) {
		return sprintf(buf, "Out of memory\n");
	}
	/* Push back cpu slabs */
	flush_all(s);

	for_each_kmem_cache_node(s, node, n) {
		unsigned long flags;
		struct page *page;

		if (!atomic_long_read(&n->nr_slabs))
			continue;

		spin_lock_irqsave(&n->list_lock, flags);
		list_for_each_entry(page, &n->partial, slab_list)
			process_slab(&t, s, page, alloc);
		list_for_each_entry(page, &n->full, slab_list)
			process_slab(&t, s, page, alloc);
		spin_unlock_irqrestore(&n->list_lock, flags);
	}

	for (i = 0; i < t.count; i++) {
		struct location *l = &t.loc[i];

		if (len > PAGE_SIZE - KSYM_SYMBOL_LEN - 100)
			break;
		len += sprintf(buf + len, "%7ld ", l->count);

		if (l->addr)
			len += sprintf(buf + len, "%pS", (void *)l->addr);
		else
			len += sprintf(buf + len, "<not-available>");

		if (l->sum_time != l->min_time) {
			len += sprintf(buf + len, " age=%ld/%ld/%ld",
				l->min_time,
				(long)div_u64(l->sum_time, l->count),
				l->max_time);
		} else
			len += sprintf(buf + len, " age=%ld",
				l->min_time);

		if (l->min_pid != l->max_pid)
			len += sprintf(buf + len, " pid=%ld-%ld",
				l->min_pid, l->max_pid);
		else
			len += sprintf(buf + len, " pid=%ld",
				l->min_pid);

		if (num_online_cpus() > 1 &&
				!cpumask_empty(to_cpumask(l->cpus)) &&
				len < PAGE_SIZE - 60)
			len += scnprintf(buf + len, PAGE_SIZE - len - 50,
					 " cpus=%*pbl",
					 cpumask_pr_args(to_cpumask(l->cpus)));

		if (nr_online_nodes > 1 && !nodes_empty(l->nodes) &&
				len < PAGE_SIZE - 60)
			len += scnprintf(buf + len, PAGE_SIZE - len - 50,
					 " nodes=%*pbl",
					 nodemask_pr_args(&l->nodes));

		len += sprintf(buf + len, "\n");
	}

	free_loc_track(&t);
	if (!t.count)
		len += sprintf(buf, "No data\n");
	return len;
}
#endif	/* CONFIG_SLUB_DEBUG */

#ifdef SLUB_RESILIENCY_TEST
static void __init resiliency_test(void)
{
	u8 *p;
	int type = KMALLOC_NORMAL;

	BUILD_BUG_ON(KMALLOC_MIN_SIZE > 16 || KMALLOC_SHIFT_HIGH < 10);

	pr_err("SLUB resiliency testing\n");
	pr_err("-----------------------\n");
	pr_err("A. Corruption after allocation\n");

	p = kzalloc(16, GFP_KERNEL);
	p[16] = 0x12;
	pr_err("\n1. kmalloc-16: Clobber Redzone/next pointer 0x12->0x%p\n\n",
	       p + 16);

	validate_slab_cache(kmalloc_caches[type][4]);

	/* Hmmm... The next two are dangerous */
	p = kzalloc(32, GFP_KERNEL);
	p[32 + sizeof(void *)] = 0x34;
	pr_err("\n2. kmalloc-32: Clobber next pointer/next slab 0x34 -> -0x%p\n",
	       p);
	pr_err("If allocated object is overwritten then not detectable\n\n");

	validate_slab_cache(kmalloc_caches[type][5]);
	p = kzalloc(64, GFP_KERNEL);
	p += 64 + (get_cycles() & 0xff) * sizeof(void *);
	*p = 0x56;
	pr_err("\n3. kmalloc-64: corrupting random byte 0x56->0x%p\n",
	       p);
	pr_err("If allocated object is overwritten then not detectable\n\n");
	validate_slab_cache(kmalloc_caches[type][6]);

	pr_err("\nB. Corruption after free\n");
	p = kzalloc(128, GFP_KERNEL);
	kfree(p);
	*p = 0x78;
	pr_err("1. kmalloc-128: Clobber first word 0x78->0x%p\n\n", p);
	validate_slab_cache(kmalloc_caches[type][7]);

	p = kzalloc(256, GFP_KERNEL);
	kfree(p);
	p[50] = 0x9a;
	pr_err("\n2. kmalloc-256: Clobber 50th byte 0x9a->0x%p\n\n", p);
	validate_slab_cache(kmalloc_caches[type][8]);

	p = kzalloc(512, GFP_KERNEL);
	kfree(p);
	p[512] = 0xab;
	pr_err("\n3. kmalloc-512: Clobber redzone 0xab->0x%p\n\n", p);
	validate_slab_cache(kmalloc_caches[type][9]);
}
#else
#ifdef CONFIG_SYSFS
static void resiliency_test(void) {};
#endif
#endif	/* SLUB_RESILIENCY_TEST */

#ifdef CONFIG_SYSFS
enum slab_stat_type {
	SL_ALL,			/* All slabs */
	SL_PARTIAL,		/* Only partially allocated slabs */
	SL_CPU,			/* Only slabs used for cpu caches */
	SL_OBJECTS,		/* Determine allocated objects not slabs */
	SL_TOTAL		/* Determine object capacity not slabs */
};

#define SO_ALL		(1 << SL_ALL)
#define SO_PARTIAL	(1 << SL_PARTIAL)
#define SO_CPU		(1 << SL_CPU)
#define SO_OBJECTS	(1 << SL_OBJECTS)
#define SO_TOTAL	(1 << SL_TOTAL)

#ifdef CONFIG_MEMCG
static bool memcg_sysfs_enabled = IS_ENABLED(CONFIG_SLUB_MEMCG_SYSFS_ON);

static int __init setup_slub_memcg_sysfs(char *str)
{
	int v;

	if (get_option(&str, &v) > 0)
		memcg_sysfs_enabled = v;

	return 1;
}

__setup("slub_memcg_sysfs=", setup_slub_memcg_sysfs);
#endif

static ssize_t show_slab_objects(struct kmem_cache *s,
			    char *buf, unsigned long flags)
{
	unsigned long total = 0;
	int node;
	int x;
	unsigned long *nodes;

	nodes = kcalloc(nr_node_ids, sizeof(unsigned long), GFP_KERNEL);
	if (!nodes)
		return -ENOMEM;

	if (flags & SO_CPU) {
		int cpu;

		for_each_possible_cpu(cpu) {
			struct kmem_cache_cpu *c = per_cpu_ptr(s->cpu_slab,
							       cpu);
			int node;
			struct page *page;

			page = READ_ONCE(c->page);
			if (!page)
				continue;

			node = page_to_nid(page);
			if (flags & SO_TOTAL)
				x = page->objects;
			else if (flags & SO_OBJECTS)
				x = page->inuse;
			else
				x = 1;

			total += x;
			nodes[node] += x;

			page = slub_percpu_partial_read_once(c);
			if (page) {
				node = page_to_nid(page);
				if (flags & SO_TOTAL)
					WARN_ON_ONCE(1);
				else if (flags & SO_OBJECTS)
					WARN_ON_ONCE(1);
				else
					x = page->pages;
				total += x;
				nodes[node] += x;
			}
		}
	}

	/*
	 * It is impossible to take "mem_hotplug_lock" here with "kernfs_mutex"
	 * already held which will conflict with an existing lock order:
	 *
	 * mem_hotplug_lock->slab_mutex->kernfs_mutex
	 *
	 * We don't really need mem_hotplug_lock (to hold off
	 * slab_mem_going_offline_callback) here because slab's memory hot
	 * unplug code doesn't destroy the kmem_cache->node[] data.
	 */

#ifdef CONFIG_SLUB_DEBUG
	if (flags & SO_ALL) {
		struct kmem_cache_node *n;

		for_each_kmem_cache_node(s, node, n) {

			if (flags & SO_TOTAL)
				x = atomic_long_read(&n->total_objects);
			else if (flags & SO_OBJECTS)
				x = atomic_long_read(&n->total_objects) -
					count_partial(n, count_free);
			else
				x = atomic_long_read(&n->nr_slabs);
			total += x;
			nodes[node] += x;
		}

	} else
#endif
	if (flags & SO_PARTIAL) {
		struct kmem_cache_node *n;

		for_each_kmem_cache_node(s, node, n) {
			if (flags & SO_TOTAL)
				x = count_partial(n, count_total);
			else if (flags & SO_OBJECTS)
				x = count_partial(n, count_inuse);
			else
				x = n->nr_partial;
			total += x;
			nodes[node] += x;
		}
	}
	x = sprintf(buf, "%lu", total);
#ifdef CONFIG_NUMA
	for (node = 0; node < nr_node_ids; node++)
		if (nodes[node])
			x += sprintf(buf + x, " N%d=%lu",
					node, nodes[node]);
#endif
	kfree(nodes);
	return x + sprintf(buf + x, "\n");
}

#define to_slab_attr(n) container_of(n, struct slab_attribute, attr)
#define to_slab(n) container_of(n, struct kmem_cache, kobj)

struct slab_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kmem_cache *s, char *buf);
	ssize_t (*store)(struct kmem_cache *s, const char *x, size_t count);
};

#define SLAB_ATTR_RO(_name) \
	static struct slab_attribute _name##_attr = \
	__ATTR(_name, 0400, _name##_show, NULL)

#define SLAB_ATTR(_name) \
	static struct slab_attribute _name##_attr =  \
	__ATTR(_name, 0600, _name##_show, _name##_store)

static ssize_t slab_size_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", s->size);
}
SLAB_ATTR_RO(slab_size);

static ssize_t align_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", s->align);
}
SLAB_ATTR_RO(align);

static ssize_t object_size_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", s->object_size);
}
SLAB_ATTR_RO(object_size);

static ssize_t objs_per_slab_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", oo_objects(s->oo));
}
SLAB_ATTR_RO(objs_per_slab);

static ssize_t order_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", oo_order(s->oo));
}
SLAB_ATTR_RO(order);

static ssize_t min_partial_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%lu\n", s->min_partial);
}

static ssize_t min_partial_store(struct kmem_cache *s, const char *buf,
				 size_t length)
{
	unsigned long min;
	int err;

	err = kstrtoul(buf, 10, &min);
	if (err)
		return err;

	set_min_partial(s, min);
	return length;
}
SLAB_ATTR(min_partial);

static ssize_t cpu_partial_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", slub_cpu_partial(s));
}

static ssize_t cpu_partial_store(struct kmem_cache *s, const char *buf,
				 size_t length)
{
	unsigned int objects;
	int err;

	err = kstrtouint(buf, 10, &objects);
	if (err)
		return err;
	if (objects && !kmem_cache_has_cpu_partial(s))
		return -EINVAL;

	slub_set_cpu_partial(s, objects);
	flush_all(s);
	return length;
}
SLAB_ATTR(cpu_partial);

static ssize_t ctor_show(struct kmem_cache *s, char *buf)
{
	if (!s->ctor)
		return 0;
	return sprintf(buf, "%pS\n", s->ctor);
}
SLAB_ATTR_RO(ctor);

static ssize_t aliases_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", s->refcount < 0 ? 0 : s->refcount - 1);
}
SLAB_ATTR_RO(aliases);

static ssize_t partial_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_PARTIAL);
}
SLAB_ATTR_RO(partial);

static ssize_t cpu_slabs_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_CPU);
}
SLAB_ATTR_RO(cpu_slabs);

static ssize_t objects_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL|SO_OBJECTS);
}
SLAB_ATTR_RO(objects);

static ssize_t objects_partial_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_PARTIAL|SO_OBJECTS);
}
SLAB_ATTR_RO(objects_partial);

static ssize_t slabs_cpu_partial_show(struct kmem_cache *s, char *buf)
{
	int objects = 0;
	int pages = 0;
	int cpu;
	int len;

	for_each_online_cpu(cpu) {
		struct page *page;

		page = slub_percpu_partial(per_cpu_ptr(s->cpu_slab, cpu));

		if (page) {
			pages += page->pages;
			objects += page->pobjects;
		}
	}

	len = sprintf(buf, "%d(%d)", objects, pages);

#ifdef CONFIG_SMP
	for_each_online_cpu(cpu) {
		struct page *page;

		page = slub_percpu_partial(per_cpu_ptr(s->cpu_slab, cpu));

		if (page && len < PAGE_SIZE - 20)
			len += sprintf(buf + len, " C%d=%d(%d)", cpu,
				page->pobjects, page->pages);
	}
#endif
	return len + sprintf(buf + len, "\n");
}
SLAB_ATTR_RO(slabs_cpu_partial);

static ssize_t reclaim_account_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_RECLAIM_ACCOUNT));
}
SLAB_ATTR_RO(reclaim_account);

static ssize_t hwcache_align_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_HWCACHE_ALIGN));
}
SLAB_ATTR_RO(hwcache_align);

#ifdef CONFIG_ZONE_DMA
static ssize_t cache_dma_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_CACHE_DMA));
}
SLAB_ATTR_RO(cache_dma);
#endif

static ssize_t usersize_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", s->usersize);
}
SLAB_ATTR_RO(usersize);

static ssize_t destroy_by_rcu_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_TYPESAFE_BY_RCU));
}
SLAB_ATTR_RO(destroy_by_rcu);

#ifdef CONFIG_SLUB_DEBUG
static ssize_t slabs_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL);
}
SLAB_ATTR_RO(slabs);

static ssize_t total_objects_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL|SO_TOTAL);
}
SLAB_ATTR_RO(total_objects);

static ssize_t sanity_checks_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_CONSISTENCY_CHECKS));
}
SLAB_ATTR_RO(sanity_checks);

static ssize_t trace_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_TRACE));
}
SLAB_ATTR_RO(trace);

static ssize_t red_zone_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_RED_ZONE));
}

SLAB_ATTR_RO(red_zone);

static ssize_t poison_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_POISON));
}

SLAB_ATTR_RO(poison);

static ssize_t store_user_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_STORE_USER));
}

SLAB_ATTR_RO(store_user);

static ssize_t validate_show(struct kmem_cache *s, char *buf)
{
	return 0;
}

static ssize_t validate_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	int ret = -EINVAL;

	if (buf[0] == '1') {
		ret = validate_slab_cache(s);
		if (ret >= 0)
			ret = length;
	}
	return ret;
}
SLAB_ATTR(validate);

static ssize_t alloc_calls_show(struct kmem_cache *s, char *buf)
{
	if (!(s->flags & SLAB_STORE_USER))
		return -ENOSYS;
	return list_locations(s, buf, TRACK_ALLOC);
}
SLAB_ATTR_RO(alloc_calls);

static ssize_t free_calls_show(struct kmem_cache *s, char *buf)
{
	if (!(s->flags & SLAB_STORE_USER))
		return -ENOSYS;
	return list_locations(s, buf, TRACK_FREE);
}
SLAB_ATTR_RO(free_calls);
#endif /* CONFIG_SLUB_DEBUG */

#ifdef CONFIG_FAILSLAB
static ssize_t failslab_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%d\n", !!(s->flags & SLAB_FAILSLAB));
}
SLAB_ATTR_RO(failslab);
#endif

static ssize_t shrink_show(struct kmem_cache *s, char *buf)
{
	return 0;
}

static ssize_t shrink_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	if (buf[0] == '1')
		kmem_cache_shrink(s);
	else
		return -EINVAL;
	return length;
}
SLAB_ATTR(shrink);

#ifdef CONFIG_NUMA
static ssize_t remote_node_defrag_ratio_show(struct kmem_cache *s, char *buf)
{
	return sprintf(buf, "%u\n", s->remote_node_defrag_ratio / 10);
}

static ssize_t remote_node_defrag_ratio_store(struct kmem_cache *s,
				const char *buf, size_t length)
{
	unsigned int ratio;
	int err;

	err = kstrtouint(buf, 10, &ratio);
	if (err)
		return err;
	if (ratio > 100)
		return -ERANGE;

	s->remote_node_defrag_ratio = ratio * 10;

	return length;
}
SLAB_ATTR(remote_node_defrag_ratio);
#endif

#ifdef CONFIG_SLUB_STATS
static int show_stat(struct kmem_cache *s, char *buf, enum stat_item si)
{
	unsigned long sum  = 0;
	int cpu;
	int len;
	int *data = kmalloc_array(nr_cpu_ids, sizeof(int), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	for_each_online_cpu(cpu) {
		unsigned x = per_cpu_ptr(s->cpu_slab, cpu)->stat[si];

		data[cpu] = x;
		sum += x;
	}

	len = sprintf(buf, "%lu", sum);

#ifdef CONFIG_SMP
	for_each_online_cpu(cpu) {
		if (data[cpu] && len < PAGE_SIZE - 20)
			len += sprintf(buf + len, " C%d=%u", cpu, data[cpu]);
	}
#endif
	kfree(data);
	return len + sprintf(buf + len, "\n");
}

static void clear_stat(struct kmem_cache *s, enum stat_item si)
{
	int cpu;

	for_each_online_cpu(cpu)
		per_cpu_ptr(s->cpu_slab, cpu)->stat[si] = 0;
}

#define STAT_ATTR(si, text) 					\
static ssize_t text##_show(struct kmem_cache *s, char *buf)	\
{								\
	return show_stat(s, buf, si);				\
}								\
static ssize_t text##_store(struct kmem_cache *s,		\
				const char *buf, size_t length)	\
{								\
	if (buf[0] != '0')					\
		return -EINVAL;					\
	clear_stat(s, si);					\
	return length;						\
}								\
SLAB_ATTR(text);						\

STAT_ATTR(ALLOC_FASTPATH, alloc_fastpath);
STAT_ATTR(ALLOC_SLOWPATH, alloc_slowpath);
STAT_ATTR(FREE_FASTPATH, free_fastpath);
STAT_ATTR(FREE_SLOWPATH, free_slowpath);
STAT_ATTR(FREE_FROZEN, free_frozen);
STAT_ATTR(FREE_ADD_PARTIAL, free_add_partial);
STAT_ATTR(FREE_REMOVE_PARTIAL, free_remove_partial);
STAT_ATTR(ALLOC_FROM_PARTIAL, alloc_from_partial);
STAT_ATTR(ALLOC_SLAB, alloc_slab);
STAT_ATTR(ALLOC_REFILL, alloc_refill);
STAT_ATTR(ALLOC_NODE_MISMATCH, alloc_node_mismatch);
STAT_ATTR(FREE_SLAB, free_slab);
STAT_ATTR(CPUSLAB_FLUSH, cpuslab_flush);
STAT_ATTR(DEACTIVATE_FULL, deactivate_full);
STAT_ATTR(DEACTIVATE_EMPTY, deactivate_empty);
STAT_ATTR(DEACTIVATE_TO_HEAD, deactivate_to_head);
STAT_ATTR(DEACTIVATE_TO_TAIL, deactivate_to_tail);
STAT_ATTR(DEACTIVATE_REMOTE_FREES, deactivate_remote_frees);
STAT_ATTR(DEACTIVATE_BYPASS, deactivate_bypass);
STAT_ATTR(ORDER_FALLBACK, order_fallback);
STAT_ATTR(CMPXCHG_DOUBLE_CPU_FAIL, cmpxchg_double_cpu_fail);
STAT_ATTR(CMPXCHG_DOUBLE_FAIL, cmpxchg_double_fail);
STAT_ATTR(CPU_PARTIAL_ALLOC, cpu_partial_alloc);
STAT_ATTR(CPU_PARTIAL_FREE, cpu_partial_free);
STAT_ATTR(CPU_PARTIAL_NODE, cpu_partial_node);
STAT_ATTR(CPU_PARTIAL_DRAIN, cpu_partial_drain);
#endif	/* CONFIG_SLUB_STATS */

static struct attribute *slab_attrs[] = {
	&slab_size_attr.attr,
	&object_size_attr.attr,
	&objs_per_slab_attr.attr,
	&order_attr.attr,
	&min_partial_attr.attr,
	&cpu_partial_attr.attr,
	&objects_attr.attr,
	&objects_partial_attr.attr,
	&partial_attr.attr,
	&cpu_slabs_attr.attr,
	&ctor_attr.attr,
	&aliases_attr.attr,
	&align_attr.attr,
	&hwcache_align_attr.attr,
	&reclaim_account_attr.attr,
	&destroy_by_rcu_attr.attr,
	&shrink_attr.attr,
	&slabs_cpu_partial_attr.attr,
#ifdef CONFIG_SLUB_DEBUG
	&total_objects_attr.attr,
	&slabs_attr.attr,
	&sanity_checks_attr.attr,
	&trace_attr.attr,
	&red_zone_attr.attr,
	&poison_attr.attr,
	&store_user_attr.attr,
	&validate_attr.attr,
	&alloc_calls_attr.attr,
	&free_calls_attr.attr,
#endif
#ifdef CONFIG_ZONE_DMA
	&cache_dma_attr.attr,
#endif
#ifdef CONFIG_NUMA
	&remote_node_defrag_ratio_attr.attr,
#endif
#ifdef CONFIG_SLUB_STATS
	&alloc_fastpath_attr.attr,
	&alloc_slowpath_attr.attr,
	&free_fastpath_attr.attr,
	&free_slowpath_attr.attr,
	&free_frozen_attr.attr,
	&free_add_partial_attr.attr,
	&free_remove_partial_attr.attr,
	&alloc_from_partial_attr.attr,
	&alloc_slab_attr.attr,
	&alloc_refill_attr.attr,
	&alloc_node_mismatch_attr.attr,
	&free_slab_attr.attr,
	&cpuslab_flush_attr.attr,
	&deactivate_full_attr.attr,
	&deactivate_empty_attr.attr,
	&deactivate_to_head_attr.attr,
	&deactivate_to_tail_attr.attr,
	&deactivate_remote_frees_attr.attr,
	&deactivate_bypass_attr.attr,
	&order_fallback_attr.attr,
	&cmpxchg_double_fail_attr.attr,
	&cmpxchg_double_cpu_fail_attr.attr,
	&cpu_partial_alloc_attr.attr,
	&cpu_partial_free_attr.attr,
	&cpu_partial_node_attr.attr,
	&cpu_partial_drain_attr.attr,
#endif
#ifdef CONFIG_FAILSLAB
	&failslab_attr.attr,
#endif
	&usersize_attr.attr,

	NULL
};

static const struct attribute_group slab_attr_group = {
	.attrs = slab_attrs,
};

static ssize_t slab_attr_show(struct kobject *kobj,
				struct attribute *attr,
				char *buf)
{
	struct slab_attribute *attribute;
	struct kmem_cache *s;
	int err;

	attribute = to_slab_attr(attr);
	s = to_slab(kobj);

	if (!attribute->show)
		return -EIO;

	err = attribute->show(s, buf);

	return err;
}

static ssize_t slab_attr_store(struct kobject *kobj,
				struct attribute *attr,
				const char *buf, size_t len)
{
	struct slab_attribute *attribute;
	struct kmem_cache *s;
	int err;

	attribute = to_slab_attr(attr);
	s = to_slab(kobj);

	if (!attribute->store)
		return -EIO;

	err = attribute->store(s, buf, len);
	return err;
}

static void kmem_cache_release(struct kobject *k)
{
	slab_kmem_cache_release(to_slab(k));
}

static const struct sysfs_ops slab_sysfs_ops = {
	.show = slab_attr_show,
	.store = slab_attr_store,
};

static struct kobj_type slab_ktype = {
	.sysfs_ops = &slab_sysfs_ops,
	.release = kmem_cache_release,
};

static struct kset *slab_kset;

static inline struct kset *cache_kset(struct kmem_cache *s)
{
	return slab_kset;
}

#define ID_STR_LENGTH 64

/* Create a unique string id for a slab cache:
 *
 * Format	:[flags-]size
 */
static char *create_unique_id(struct kmem_cache *s)
{
	char *name = kmalloc(ID_STR_LENGTH, GFP_KERNEL);
	char *p = name;

	BUG_ON(!name);

	*p++ = ':';
	/*
	 * First flags affecting slabcache operations. We will only
	 * get here for aliasable slabs so we do not need to support
	 * too many flags. The flags here must cover all flags that
	 * are matched during merging to guarantee that the id is
	 * unique.
	 */
	if (s->flags & SLAB_CACHE_DMA)
		*p++ = 'd';
	if (s->flags & SLAB_CACHE_DMA32)
		*p++ = 'D';
	if (s->flags & SLAB_RECLAIM_ACCOUNT)
		*p++ = 'a';
	if (s->flags & SLAB_CONSISTENCY_CHECKS)
		*p++ = 'F';
	if (s->flags & SLAB_ACCOUNT)
		*p++ = 'A';
	if (p != name + 1)
		*p++ = '-';
	p += sprintf(p, "%07u", s->size);

	BUG_ON(p > name + ID_STR_LENGTH - 1);
	return name;
}

static int sysfs_slab_add(struct kmem_cache *s)
{
	int err;
	const char *name;
	struct kset *kset = cache_kset(s);
	int unmergeable = slab_unmergeable(s);

	if (!kset) {
		kobject_init(&s->kobj, &slab_ktype);
		return 0;
	}

	if (!unmergeable && disable_higher_order_debug &&
			(slub_debug & DEBUG_METADATA_FLAGS))
		unmergeable = 1;

	if (unmergeable) {
		/*
		 * Slabcache can never be merged so we can use the name proper.
		 * This is typically the case for debug situations. In that
		 * case we can catch duplicate names easily.
		 */
		sysfs_remove_link(&slab_kset->kobj, s->name);
		name = s->name;
	} else {
		/*
		 * Create a unique name for the slab as a target
		 * for the symlinks.
		 */
		name = create_unique_id(s);
	}

	s->kobj.kset = kset;
	err = kobject_init_and_add(&s->kobj, &slab_ktype, NULL, "%s", name);
	if (err) {
		kobject_put(&s->kobj);
		goto out;
	}

	err = sysfs_create_group(&s->kobj, &slab_attr_group);
	if (err)
		goto out_del_kobj;

	if (!unmergeable) {
		/* Setup first alias */
		sysfs_slab_alias(s, s->name);
	}
out:
	if (!unmergeable)
		kfree(name);
	return err;
out_del_kobj:
	kobject_del(&s->kobj);
	goto out;
}

void sysfs_slab_unlink(struct kmem_cache *s)
{
	if (slab_state >= FULL)
		kobject_del(&s->kobj);
}

void sysfs_slab_release(struct kmem_cache *s)
{
	if (slab_state >= FULL)
		kobject_put(&s->kobj);
}

/*
 * Need to buffer aliases during bootup until sysfs becomes
 * available lest we lose that information.
 */
struct saved_alias {
	struct kmem_cache *s;
	const char *name;
	struct saved_alias *next;
};

static struct saved_alias *alias_list;

static int sysfs_slab_alias(struct kmem_cache *s, const char *name)
{
	struct saved_alias *al;

	if (slab_state == FULL) {
		/*
		 * If we have a leftover link then remove it.
		 */
		sysfs_remove_link(&slab_kset->kobj, name);
		return sysfs_create_link(&slab_kset->kobj, &s->kobj, name);
	}

	al = kmalloc(sizeof(struct saved_alias), GFP_KERNEL);
	if (!al)
		return -ENOMEM;

	al->s = s;
	al->name = name;
	al->next = alias_list;
	alias_list = al;
	return 0;
}

static int __init slab_sysfs_init(void)
{
	struct kmem_cache *s;
	int err;

	mutex_lock(&slab_mutex);

	slab_kset = kset_create_and_add("slab", NULL, kernel_kobj);
	if (!slab_kset) {
		mutex_unlock(&slab_mutex);
		pr_err("Cannot register slab subsystem.\n");
		return -ENOSYS;
	}

	slab_state = FULL;

	list_for_each_entry(s, &slab_caches, list) {
		err = sysfs_slab_add(s);
		if (err)
			pr_err("SLUB: Unable to add boot slab %s to sysfs\n",
			       s->name);
	}

	while (alias_list) {
		struct saved_alias *al = alias_list;

		alias_list = alias_list->next;
		err = sysfs_slab_alias(al->s, al->name);
		if (err)
			pr_err("SLUB: Unable to add boot slab alias %s to sysfs\n",
			       al->name);
		kfree(al);
	}

	mutex_unlock(&slab_mutex);
	resiliency_test();
	return 0;
}

__initcall(slab_sysfs_init);
#endif /* CONFIG_SYSFS */

/*
 * The /proc/slabinfo ABI
 */
#ifdef CONFIG_SLUB_DEBUG
void get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo)
{
	unsigned long nr_slabs = 0;
	unsigned long nr_objs = 0;
	unsigned long nr_free = 0;
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n) {
		nr_slabs += node_nr_slabs(n);
		nr_objs += node_nr_objs(n);
		nr_free += count_partial(n, count_free);
	}

	sinfo->active_objs = nr_objs - nr_free;
	sinfo->num_objs = nr_objs;
	sinfo->active_slabs = nr_slabs;
	sinfo->num_slabs = nr_slabs;
	sinfo->objects_per_slab = oo_objects(s->oo);
	sinfo->cache_order = oo_order(s->oo);
}

void slabinfo_show_stats(struct seq_file *m, struct kmem_cache *s)
{
}

ssize_t slabinfo_write(struct file *file, const char __user *buffer,
		       size_t count, loff_t *ppos)
{
	return -EIO;
}
#endif /* CONFIG_SLUB_DEBUG */
