// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/workqueue.c - generic async execution with shared worker pool
 *
 * Copyright (C) 2002		Ingo Molnar
 *
 *   Derived from the taskqueue/keventd code by:
 *     David Woodhouse <dwmw2@infradead.org>
 *     Andrew Morton
 *     Kai Petzke <wpp@marie.physik.tu-berlin.de>
 *     Theodore Ts'o <tytso@mit.edu>
 *
 * Made to use alloc_percpu by Christoph Lameter.
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This is the generic async execution mechanism.  Work items as are
 * executed in process context.  The worker pool is shared and
 * automatically managed.  There are two worker pools for each CPU (one for
 * normal work items and the other for high priority ones) and some extra
 * pools for workqueues which are not bound to any specific CPU - the
 * number of these backing pools is dynamic.
 *
 * Please read Documentation/core-api/workqueue.rst for details.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/hardirq.h>
#include <linux/mempolicy.h>
#include <linux/freezer.h>
#include <linux/debug_locks.h>
#include <linux/lockdep.h>
#include <linux/idr.h>
#include <linux/jhash.h>
#include <linux/hashtable.h>
#include <linux/rculist.h>
#include <linux/nodemask.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/sched/isolation.h>
#include <linux/nmi.h>

#include "workqueue_internal.h"

enum {
	/*
	 * worker_pool flags
	 *
	 * A bound pool is either associated or disassociated with its CPU.
	 * While associated (!DISASSOCIATED), all workers are bound to the
	 * CPU and none has %WORKER_UNBOUND set and concurrency management
	 * is in effect.
	 *
	 * While DISASSOCIATED, the cpu may be offline and all workers have
	 * %WORKER_UNBOUND set and concurrency management disabled, and may
	 * be executing on any CPU.  The pool behaves as an unbound one.
	 *
	 * Note that DISASSOCIATED should be flipped only while holding
	 * wq_pool_attach_mutex to avoid changing binding state while
	 * worker_attach_to_pool() is in progress.
	 */
	POOL_MANAGER_ACTIVE	= 1 << 0,	/* being managed */
	POOL_DISASSOCIATED	= 1 << 2,	/* cpu can't serve workers */

	/* worker flags */
	WORKER_DIE		= 1 << 1,	/* die die die */
	WORKER_IDLE		= 1 << 2,	/* is idle */
	WORKER_PREP		= 1 << 3,	/* preparing to run works */
	WORKER_CPU_INTENSIVE	= 1 << 6,	/* cpu intensive */
	WORKER_UNBOUND		= 1 << 7,	/* worker is unbound */
	WORKER_REBOUND		= 1 << 8,	/* worker was rebound */

	WORKER_NOT_RUNNING	= WORKER_PREP | WORKER_CPU_INTENSIVE |
				  WORKER_UNBOUND | WORKER_REBOUND,

	NR_STD_WORKER_POOLS	= 2,		/* # standard pools per cpu */

	UNBOUND_POOL_HASH_ORDER	= 6,		/* hashed by pool->attrs */
	BUSY_WORKER_HASH_ORDER	= 6,		/* 64 pointers */

	MAX_IDLE_WORKERS_RATIO	= 4,		/* 1/4 of busy can be idle */
	IDLE_WORKER_TIMEOUT	= 300 * HZ,	/* keep idle ones for 5 mins */

	MAYDAY_INITIAL_TIMEOUT  = HZ / 100 >= 2 ? HZ / 100 : 2,
						/* call for help after 10ms
						   (min two ticks) */
	MAYDAY_INTERVAL		= HZ / 10,	/* and then every 100ms */
	CREATE_COOLDOWN		= HZ,		/* time to breath after fail */

	/*
	 * Rescue workers are used only on emergencies and shared by
	 * all cpus.  Give MIN_NICE.
	 */
	RESCUER_NICE_LEVEL	= MIN_NICE,
	HIGHPRI_NICE_LEVEL	= MIN_NICE,

	WQ_NAME_LEN		= 24,
};

/*
 * Structure fields follow one of the following exclusion rules.
 *
 * I: Modifiable by initialization/destruction paths and read-only for
 *    everyone else.
 *
 * P: Preemption protected.  Disabling preemption is enough and should
 *    only be modified and accessed from the local cpu.
 *
 * L: pool->lock protected.  Access with pool->lock held.
 *
 * X: During normal operation, modification requires pool->lock and should
 *    be done only from local cpu.  Either disabling preemption on local
 *    cpu or grabbing pool->lock is enough for read access.  If
 *    POOL_DISASSOCIATED is set, it's identical to L.
 *
 * A: wq_pool_attach_mutex protected.
 *
 * PL: wq_pool_mutex protected.
 *
 * PR: wq_pool_mutex protected for writes.  RCU protected for reads.
 *
 * PW: wq_pool_mutex and wq->mutex protected for writes.  Either for reads.
 *
 * PWR: wq_pool_mutex and wq->mutex protected for writes.  Either or
 *      RCU for reads.
 *
 * WQ: wq->mutex protected.
 *
 * WR: wq->mutex protected for writes.  RCU protected for reads.
 *
 * MD: wq_mayday_lock protected.
 */

/* struct worker is defined in workqueue_internal.h */

struct worker_pool {
	raw_spinlock_t		lock;		/* the pool lock */
	int			cpu;		/* I: the associated cpu */
	int			node;		/* I: the associated node ID */
	int			id;		/* I: pool ID */
	unsigned int		flags;		/* X: flags */

	unsigned long		watchdog_ts;	/* L: watchdog timestamp */

	struct list_head	worklist;	/* L: list of pending works */

	int			nr_workers;	/* L: total number of workers */
	int			nr_idle;	/* L: currently idle workers */

	struct list_head	idle_list;	/* X: list of idle workers */
	struct timer_list	idle_timer;	/* L: worker idle timeout */
	struct timer_list	mayday_timer;	/* L: SOS timer for workers */

	/* a workers is either on busy_hash or idle_list, or the manager */
	DECLARE_HASHTABLE(busy_hash, BUSY_WORKER_HASH_ORDER);
						/* L: hash of busy workers */

	struct worker		*manager;	/* L: purely informational */
	struct list_head	workers;	/* A: attached workers */
	struct completion	*detach_completion; /* all workers detached */

	struct ida		worker_ida;	/* worker IDs for task name */

	struct workqueue_attrs	*attrs;		/* I: worker attributes */
	struct hlist_node	hash_node;	/* PL: unbound_pool_hash node */
	int			refcnt;		/* PL: refcnt for unbound pools */

	/*
	 * The current concurrency level.  As it's likely to be accessed
	 * from other CPUs during try_to_wake_up(), put it in a separate
	 * cacheline.
	 */
	atomic_t		nr_running ____cacheline_aligned_in_smp;

	/*
	 * Destruction of pool is RCU protected to allow dereferences
	 * from get_work_pool().
	 */
	struct rcu_head		rcu;
} ____cacheline_aligned_in_smp;

/*
 * The per-pool workqueue.  While queued, the lower WORK_STRUCT_FLAG_BITS
 * of work_struct->data are used for flags and the remaining high bits
 * point to the pwq; thus, pwqs need to be aligned at two's power of the
 * number of flag bits.
 */
struct pool_workqueue {
	struct worker_pool	*pool;		/* I: the associated pool */
	struct workqueue_struct *wq;		/* I: the owning workqueue */
	int			work_color;	/* L: current color */
	int			flush_color;	/* L: flushing color */
	int			refcnt;		/* L: reference count */
	int			nr_in_flight[WORK_NR_COLORS];
						/* L: nr of in_flight works */
	int			nr_active;	/* L: nr of active works */
	int			max_active;	/* L: max active works */
	struct list_head	delayed_works;	/* L: delayed works */
	struct list_head	pwqs_node;	/* WR: node on wq->pwqs */
	struct list_head	mayday_node;	/* MD: node on wq->maydays */

	/*
	 * Release of unbound pwq is punted to system_wq.  See put_pwq()
	 * and pwq_unbound_release_workfn() for details.  pool_workqueue
	 * itself is also RCU protected so that the first pwq can be
	 * determined without grabbing wq->mutex.
	 */
	struct work_struct	unbound_release_work;
	struct rcu_head		rcu;
} __aligned(1 << WORK_STRUCT_FLAG_BITS);

/*
 * Structure used to wait for workqueue flush.
 */
struct wq_flusher {
	struct list_head	list;		/* WQ: list of flushers */
	int			flush_color;	/* WQ: flush color waiting for */
	struct completion	done;		/* flush completion */
};

struct wq_device;

/*
 * The externally visible workqueue.  It relays the issued work items to
 * the appropriate worker_pool through its pool_workqueues.
 */
struct workqueue_struct {
	struct list_head	pwqs;		/* WR: all pwqs of this wq */
	struct list_head	list;		/* PR: list of all workqueues */

	struct mutex		mutex;		/* protects this wq */
	int			work_color;	/* WQ: current work color */
	int			flush_color;	/* WQ: current flush color */
	atomic_t		nr_pwqs_to_flush; /* flush in progress */
	struct wq_flusher	*first_flusher;	/* WQ: first flusher */
	struct list_head	flusher_queue;	/* WQ: flush waiters */
	struct list_head	flusher_overflow; /* WQ: flush overflow list */

	struct list_head	maydays;	/* MD: pwqs requesting rescue */
	struct worker		*rescuer;	/* MD: rescue worker */

	int			nr_drainers;	/* WQ: drain in progress */
	int			saved_max_active; /* WQ: saved pwq max_active */

	struct workqueue_attrs	*unbound_attrs;	/* PW: only for unbound wqs */
	struct pool_workqueue	*dfl_pwq;	/* PW: only for unbound wqs */

#ifdef CONFIG_SYSFS
	struct wq_device	*wq_dev;	/* I: for sysfs interface */
#endif
#ifdef CONFIG_LOCKDEP
	char			*lock_name;
	struct lock_class_key	key;
	struct lockdep_map	lockdep_map;
#endif
	char			name[WQ_NAME_LEN]; /* I: workqueue name */

	/*
	 * Destruction of workqueue_struct is RCU protected to allow walking
	 * the workqueues list without grabbing wq_pool_mutex.
	 * This is used to dump all workqueues from sysrq.
	 */
	struct rcu_head		rcu;

	/* hot fields used during command issue, aligned to cacheline */
	unsigned int		flags ____cacheline_aligned; /* WQ: WQ_* flags */
	struct pool_workqueue __percpu *cpu_pwqs; /* I: per-cpu pwqs */
	struct pool_workqueue __rcu *numa_pwq_tbl[]; /* PWR: unbound pwqs indexed by node */
};

static struct kmem_cache *pwq_cache;

static cpumask_var_t *wq_numa_possible_cpumask;
					/* possible CPUs of each node */

static bool wq_disable_numa;
module_param_named(disable_numa, wq_disable_numa, bool, 0444);

/* see the comment above the definition of WQ_POWER_EFFICIENT */
static bool wq_power_efficient = IS_ENABLED(CONFIG_WQ_POWER_EFFICIENT_DEFAULT);
module_param_named(power_efficient, wq_power_efficient, bool, 0444);

static bool wq_online;			/* can kworkers be created yet? */

static bool wq_numa_enabled;		/* unbound NUMA affinity enabled */

/* buf for wq_update_unbound_numa_attrs(), protected by CPU hotplug exclusion */
static struct workqueue_attrs *wq_update_unbound_numa_attrs_buf;

static DEFINE_MUTEX(wq_pool_mutex);	/* protects pools and workqueues list */
static DEFINE_MUTEX(wq_pool_attach_mutex); /* protects worker attach/detach */
static DEFINE_RAW_SPINLOCK(wq_mayday_lock);	/* protects wq->maydays list */
/* wait for manager to go away */
static struct rcuwait manager_wait = __RCUWAIT_INITIALIZER(manager_wait);

static LIST_HEAD(workqueues);		/* PR: list of all workqueues */
static bool workqueue_freezing;		/* PL: have wqs started freezing? */

/* PL: allowable cpus for unbound wqs and work items */
static cpumask_var_t wq_unbound_cpumask;

/* CPU where unbound work was last round robin scheduled from this CPU */
static DEFINE_PER_CPU(int, wq_rr_cpu_last);

/*
 * Local execution of unbound work items is no longer guaranteed.  The
 * following always forces round-robin CPU selection on unbound work items
 * to uncover usages which depend on it.
 */
#ifdef CONFIG_DEBUG_WQ_FORCE_RR_CPU
static bool wq_debug_force_rr_cpu = true;
#else
static bool wq_debug_force_rr_cpu = false;
#endif
module_param_named(debug_force_rr_cpu, wq_debug_force_rr_cpu, bool, 0644);

/* the per-cpu worker pools */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [NR_STD_WORKER_POOLS], cpu_worker_pools);

static DEFINE_IDR(worker_pool_idr);	/* PR: idr of all pools */

/* PL: hash of all unbound pools keyed by pool->attrs */
static DEFINE_HASHTABLE(unbound_pool_hash, UNBOUND_POOL_HASH_ORDER);

/* I: attributes used when instantiating standard unbound pools on demand */
static struct workqueue_attrs *unbound_std_wq_attrs[NR_STD_WORKER_POOLS];

/* I: attributes used when instantiating ordered pools on demand */
static struct workqueue_attrs *ordered_wq_attrs[NR_STD_WORKER_POOLS];

struct workqueue_struct *system_wq __read_mostly;
EXPORT_SYMBOL(system_wq);
struct workqueue_struct *system_highpri_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_highpri_wq);
struct workqueue_struct *system_long_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_long_wq);
struct workqueue_struct *system_unbound_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_unbound_wq);
struct workqueue_struct *system_freezable_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_freezable_wq);
struct workqueue_struct *system_power_efficient_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_power_efficient_wq);
struct workqueue_struct *system_freezable_power_efficient_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_freezable_power_efficient_wq);

static int worker_thread(void *__worker);
static void workqueue_sysfs_unregister(struct workqueue_struct *wq);
static void show_pwq(struct pool_workqueue *pwq);

#define CREATE_TRACE_POINTS
#include <trace/events/workqueue.h>

#define assert_rcu_or_pool_mutex()					\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU or wq_pool_mutex should be held")

#define assert_rcu_or_wq_mutex_or_pool_mutex(wq)			\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq->mutex) &&		\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU, wq->mutex or wq_pool_mutex should be held")

#define for_each_cpu_worker_pool(pool, cpu)				\
	for ((pool) = &per_cpu(cpu_worker_pools, cpu)[0];		\
	     (pool) < &per_cpu(cpu_worker_pools, cpu)[NR_STD_WORKER_POOLS]; \
	     (pool)++)

/**
 * for_each_pool - iterate through all worker_pools in the system
 * @pool: iteration cursor
 * @pi: integer used for iteration
 *
 * This must be called either with wq_pool_mutex held or RCU read
 * locked.  If the pool needs to be used beyond the locking in effect, the
 * caller is responsible for guaranteeing that the pool stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
#define for_each_pool(pool, pi)						\
	idr_for_each_entry(&worker_pool_idr, pool, pi)			\
		if (({ assert_rcu_or_pool_mutex(); false; })) { }	\
		else

/**
 * for_each_pool_worker - iterate through all workers of a worker_pool
 * @worker: iteration cursor
 * @pool: worker_pool to iterate workers of
 *
 * This must be called with wq_pool_attach_mutex.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
#define for_each_pool_worker(worker, pool)				\
	list_for_each_entry((worker), &(pool)->workers, node)		\
		if (({ lockdep_assert_held(&wq_pool_attach_mutex); false; })) { } \
		else

/**
 * for_each_pwq - iterate through all pool_workqueues of the specified workqueue
 * @pwq: iteration cursor
 * @wq: the target workqueue
 *
 * This must be called either with wq->mutex held or RCU read locked.
 * If the pwq needs to be used beyond the locking in effect, the caller is
 * responsible for guaranteeing that the pwq stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
#define for_each_pwq(pwq, wq)						\
	list_for_each_entry_rcu((pwq), &(wq)->pwqs, pwqs_node,		\
				 lockdep_is_held(&(wq->mutex)))

#ifdef CONFIG_DEBUG_OBJECTS_WORK

static const struct debug_obj_descr work_debug_descr;

/**
 * work_debug_hint - 返回work对象的调试提示信息
 * @addr: work_struct指针
 *
 * 供debug_object框架调用，返回work->func作为调试时的识别信息。
 */
static void *work_debug_hint(void *addr)
{
	return ((struct work_struct *) addr)->func;
}

/**
 * work_is_static_object - 检查work是否为静态分配对象
 * @addr: work_struct指针
 *
 * 供debug_object框架调用，通过WORK_STRUCT_STATIC_BIT标志判断
 * work是否为静态对象（不需要动态初始化检查）。
 */
static bool work_is_static_object(void *addr)
{
	struct work_struct *work = addr;

	return test_bit(WORK_STRUCT_STATIC_BIT, work_data_bits(work));
}

/**
 * work_fixup_init - 修复被错误初始化的活跃work对象
 * @addr: work_struct指针
 * @state: debug_object当前状态
 *
 * 当活跃状态的work被再次初始化时，先调用cancel_work_sync()等待其完成，
 * 再重新初始化debug对象，防止use-after-free。
 */
/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static bool work_fixup_init(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_init(work, &work_debug_descr);
		return true;
	default:
		return false;
	}
}

/**
 * work_fixup_free - 修复被错误释放的活跃work对象
 * @addr: work_struct指针
 * @state: debug_object当前状态
 *
 * 当活跃状态的work被释放时，先调用cancel_work_sync()等待其完成，
 * 再释放debug对象，防止释放正在执行的work。
 */
/*
 * fixup_free is called when:
 * - an active object is freed
 */
static bool work_fixup_free(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_free(work, &work_debug_descr);
		return true;
	default:
		return false;
	}
}

static const struct debug_obj_descr work_debug_descr = {
	.name		= "work_struct",
	.debug_hint	= work_debug_hint,
	.is_static_object = work_is_static_object,
	.fixup_init	= work_fixup_init,
	.fixup_free	= work_fixup_free,
};

/**
 * debug_work_activate - 标记work进入活跃状态（调试用）
 * @work: 目标work_struct
 *
 * 在DEBUG_OBJECTS_WORK模式下通知调试对象追踪系统work已激活，
 * 检测并发激活或未初始化使用等错误。
 */
static inline void debug_work_activate(struct work_struct *work)
{
	debug_object_activate(work, &work_debug_descr);
}

/**
 * debug_work_deactivate - 标记work退出活跃状态（调试用）
 * @work: 目标work_struct
 *
 * 在DEBUG_OBJECTS_WORK模式下通知调试对象追踪系统work已停止活跃，
 * 与debug_work_activate()配对使用，确保活跃状态计数正确。
 */
static inline void debug_work_deactivate(struct work_struct *work)
{
	debug_object_deactivate(work, &work_debug_descr);
}

/**
 * __init_work - 初始化work_struct调试对象
 *
 * 在DEBUG_OBJECTS_WORK模式下注册work到调试子系统。@onstack为true
 * 时使用栈上对象初始化路径，false时用堆分配路径。非调试构建下
 * 此函数为空操作。
 */
void __init_work(struct work_struct *work, int onstack)
{
	if (onstack)
		debug_object_init_on_stack(work, &work_debug_descr);
	else
		debug_object_init(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(__init_work);

/**
 * destroy_work_on_stack - 释放栈上work的调试对象
 *
 * 与DECLARE_WORK_ONSTACK配对使用，在栈上work离开作用域前
 * 注销其调试对象，避免debug_objects误报use-after-free。
 */
void destroy_work_on_stack(struct work_struct *work)
{
	debug_object_free(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_work_on_stack);

void destroy_delayed_work_on_stack(struct delayed_work *work)
{
	destroy_timer_on_stack(&work->timer);
	debug_object_free(&work->work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_delayed_work_on_stack);

#else
static inline void debug_work_activate(struct work_struct *work) { }
static inline void debug_work_deactivate(struct work_struct *work) { }
#endif

/**
 * worker_pool_assign_id - allocate ID and assing it to @pool
 * @pool: the pool pointer of interest
 *
 * Returns 0 if ID in [0, WORK_OFFQ_POOL_NONE) is allocated and assigned
 * successfully, -errno on failure.
 */
/**
 * worker_pool_assign_id - 为worker_pool分配唯一ID
 *
 * 通过worker_pool_idr分配一个数值ID并存入pool->id，范围限于
 * [0, WORK_OFFQ_POOL_NONE)。ID用于work->data中的OFFQ_POOL_ID
 * 字段，在work不关联pwq时仍能找到所属pool。
 */
static int worker_pool_assign_id(struct worker_pool *pool)
{
	int ret;

	lockdep_assert_held(&wq_pool_mutex);

	ret = idr_alloc(&worker_pool_idr, pool, 0, WORK_OFFQ_POOL_NONE,
			GFP_KERNEL);
	if (ret >= 0) {
		pool->id = ret;
		return 0;
	}
	return ret;
}

/**
 * unbound_pwq_by_node - return the unbound pool_workqueue for the given node
 * @wq: the target workqueue
 * @node: the node ID
 *
 * This must be called with any of wq_pool_mutex, wq->mutex or RCU
 * read locked.
 * If the pwq needs to be used beyond the locking in effect, the caller is
 * responsible for guaranteeing that the pwq stays online.
 *
 * Return: The unbound pool_workqueue for @node.
 */
/**
 * unbound_pwq_by_node - 获取unbound工作队列在指定NUMA节点上的pwq
 *
 * 返回@wq在@node对应的pool_workqueue。若node为NUMA_NO_NODE
 * （CPU离线时可能发生），则退回到默认pwq。调用者需持有RCU或
 * wq_mutex或pool_mutex之一。
 */
static struct pool_workqueue *unbound_pwq_by_node(struct workqueue_struct *wq,
						  int node)
{
	assert_rcu_or_wq_mutex_or_pool_mutex(wq);

	/*
	 * XXX: @node can be NUMA_NO_NODE if CPU goes offline while a
	 * delayed item is pending.  The plan is to keep CPU -> NODE
	 * mapping valid and stable across CPU on/offlines.  Once that
	 * happens, this workaround can be removed.
	 */
	if (unlikely(node == NUMA_NO_NODE))
		return wq->dfl_pwq;

	return rcu_dereference_raw(wq->numa_pwq_tbl[node]);
}

/**
 * work_color_to_flags - 将颜色值转换为work->data的标志位
 * @color: flush颜色值
 *
 * 将颜色值左移WORK_STRUCT_COLOR_SHIFT位，生成存入work->data的标志掩码，
 * 供set_work_pwq()等函数在设置work->data时编码颜色信息。
 */
static unsigned int work_color_to_flags(int color)
{
	return color << WORK_STRUCT_COLOR_SHIFT;
}

/**
 * get_work_color - 获取work当前颜色值
 *
 * work颜色（color）用于flush_workqueue()的"颜色翻转"机制：
 * 每轮flush给新入队的work打上不同颜色，等待所有当前颜色的
 * work执行完毕，实现精确的全队列同步。
 */
static int get_work_color(struct work_struct *work)
{
	return (*work_data_bits(work) >> WORK_STRUCT_COLOR_SHIFT) &
		((1 << WORK_STRUCT_COLOR_BITS) - 1);
}

static int work_next_color(int color)
{
	return (color + 1) % WORK_NR_COLORS;
}

/*
 * While queued, %WORK_STRUCT_PWQ is set and non flag bits of a work's data
 * contain the pointer to the queued pwq.  Once execution starts, the flag
 * is cleared and the high bits contain OFFQ flags and pool ID.
 *
 * set_work_pwq(), set_work_pool_and_clear_pending(), mark_work_canceling()
 * and clear_work_data() can be used to set the pwq, pool or clear
 * work->data.  These functions should only be called while the work is
 * owned - ie. while the PENDING bit is set.
 *
 * get_work_pool() and get_work_pwq() can be used to obtain the pool or pwq
 * corresponding to a work.  Pool is available once the work has been
 * queued anywhere after initialization until it is sync canceled.  pwq is
 * available only while the work item is queued.
 *
 * %WORK_OFFQ_CANCELING is used to mark a work item which is being
 * canceled.  While being canceled, a work item may have its PENDING set
 * but stay off timer and worklist for arbitrarily long and nobody should
 * try to steal the PENDING bit.
 */
/**
 * set_work_data - 原子设置work->data字段
 *
 * work->data低位编码PENDING/PWQ/LINKED等标志，高位存储pool ID
 * 或pwq指针。此函数在持有PENDING位（work被"owned"）时才能调用，
 * 保证对work状态的原子更新。
 */
static inline void set_work_data(struct work_struct *work, unsigned long data,
				 unsigned long flags)
{
	WARN_ON_ONCE(!work_pending(work));
	atomic_long_set(&work->data, data | flags | work_static(work));
}

/**
 * set_work_pwq - 将work关联到指定pool_workqueue
 *
 * 设置work->data指向@pwq，并置PENDING|PWQ标志，表示work已入队
 * 等待执行。extra_flags可附加LINKED等额外标志位。
 */
static void set_work_pwq(struct work_struct *work, struct pool_workqueue *pwq,
			 unsigned long extra_flags)
{
	set_work_data(work, (unsigned long)pwq,
		      WORK_STRUCT_PENDING | WORK_STRUCT_PWQ | extra_flags);
}

/**
 * set_work_pool_and_keep_pending - 记录pool ID并保留PENDING位
 *
 * 将work->data设置为@pool_id（OFFQ格式）同时保持PENDING位，
 * 用于work完成执行后但尚未清除PENDING时的过渡状态，防止
 * 执行期间的重复入队竞争。
 */
static void set_work_pool_and_keep_pending(struct work_struct *work,
					   int pool_id)
{
	set_work_data(work, (unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT,
		      WORK_STRUCT_PENDING);
}

/**
 * set_work_pool_and_clear_pending - 记录pool ID并清除PENDING位
 *
 * work执行完毕后调用：将work->data设置为@pool_id（OFFQ格式）
 * 同时清除PENDING标志，允许work重新入队。与
 * set_work_pool_and_keep_pending()相对，这是执行完成路径。
 */
static void set_work_pool_and_clear_pending(struct work_struct *work,
					    int pool_id)
{
	/*
	 * The following wmb is paired with the implied mb in
	 * test_and_set_bit(PENDING) and ensures all updates to @work made
	 * here are visible to and precede any updates by the next PENDING
	 * owner.
	 */
	smp_wmb();
	set_work_data(work, (unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT, 0);
	/*
	 * The following mb guarantees that previous clear of a PENDING bit
	 * will not be reordered with any speculative LOADS or STORES from
	 * work->current_func, which is executed afterwards.  This possible
	 * reordering can lead to a missed execution on attempt to queue
	 * the same @work.  E.g. consider this case:
	 *
	 *   CPU#0                         CPU#1
	 *   ----------------------------  --------------------------------
	 *
	 * 1  STORE event_indicated
	 * 2  queue_work_on() {
	 * 3    test_and_set_bit(PENDING)
	 * 4 }                             set_..._and_clear_pending() {
	 * 5                                 set_work_data() # clear bit
	 * 6                                 smp_mb()
	 * 7                               work->current_func() {
	 * 8				      LOAD event_indicated
	 *				   }
	 *
	 * Without an explicit full barrier speculative LOAD on line 8 can
	 * be executed before CPU#0 does STORE on line 1.  If that happens,
	 * CPU#0 observes the PENDING bit is still set and new execution of
	 * a @work is not queued in a hope, that CPU#1 will eventually
	 * finish the queued @work.  Meanwhile CPU#1 does not see
	 * event_indicated is set, because speculative LOAD was executed
	 * before actual STORE.
	 */
	smp_mb();
}

/**
 * clear_work_data - 清除work关联数据（标记为无pool）
 *
 * 写屏障后将work->data重置为WORK_STRUCT_NO_POOL，表示该work
 * 当前未关联任何pool或pwq，可安全重新入队。
 */
static void clear_work_data(struct work_struct *work)
{
	smp_wmb();	/* see set_work_pool_and_clear_pending() */
	set_work_data(work, WORK_STRUCT_NO_POOL, 0);
}

/**
 * get_work_pwq - 获取work当前关联的pool_workqueue
 *
 * 从work->data读取pool_workqueue指针（仅当WORK_STRUCT_PWQ标志
 * 已设置时有效）。若work处于OFFQ状态（已完成或无pwq），返回NULL。
 */
static struct pool_workqueue *get_work_pwq(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return (void *)(data & WORK_STRUCT_WQ_DATA_MASK);
	else
		return NULL;
}

/**
 * get_work_pool - return the worker_pool a given work was associated with
 * @work: the work item of interest
 *
 * Pools are created and destroyed under wq_pool_mutex, and allows read
 * access under RCU read lock.  As such, this function should be
 * called under wq_pool_mutex or inside of a rcu_read_lock() region.
 *
 * All fields of the returned pool are accessible as long as the above
 * mentioned locking is in effect.  If the returned pool needs to be used
 * beyond the critical section, the caller is responsible for ensuring the
 * returned pool is and stays online.
 *
 * Return: The worker_pool @work was last associated with.  %NULL if none.
 */
/**
 * get_work_pool - 获取work最近关联的worker_pool
 *
 * 从work->data读取pool ID或pwq指针，返回对应的worker_pool。
 * 若work处于空闲或canceling状态则可能返回NULL。
 * 调用者需持有RCU或pool_mutex保证pool不被销毁。
 */
static struct worker_pool *get_work_pool(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);
	int pool_id;

	assert_rcu_or_pool_mutex();

	if (data & WORK_STRUCT_PWQ)
		return ((struct pool_workqueue *)
			(data & WORK_STRUCT_WQ_DATA_MASK))->pool;

	pool_id = data >> WORK_OFFQ_POOL_SHIFT;
	if (pool_id == WORK_OFFQ_POOL_NONE)
		return NULL;

	return idr_find(&worker_pool_idr, pool_id);
}

/**
 * get_work_pool_id - return the worker pool ID a given work is associated with
 * @work: the work item of interest
 *
 * Return: The worker_pool ID @work was last associated with.
 * %WORK_OFFQ_POOL_NONE if none.
 */
/**
 * get_work_pool_id - 获取work最近关联的worker_pool ID
 *
 * 若work->data中WORK_STRUCT_PWQ标志已设，从pwq->pool->id取；
 * 否则从OFFQ字段直接读取pool_id。用于work执行完成后记录
 * 最后所属pool，供cancel/flush路径快速定位。
 */
static int get_work_pool_id(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return ((struct pool_workqueue *)
			(data & WORK_STRUCT_WQ_DATA_MASK))->pool->id;

	return data >> WORK_OFFQ_POOL_SHIFT;
}

/**
 * mark_work_canceling - 标记work正在被取消
 *
 * 在cancel_work_sync()流程中，将work->data设置为OFFQ_CANCELING
 * 状态，防止其他CPU重新入队或抢占PENDING位，确保取消操作的
 * 原子性。
 */
static void mark_work_canceling(struct work_struct *work)
{
	unsigned long pool_id = get_work_pool_id(work);

	pool_id <<= WORK_OFFQ_POOL_SHIFT;
	set_work_data(work, pool_id | WORK_OFFQ_CANCELING, WORK_STRUCT_PENDING);
}

/**
 * work_is_canceling - 检查work是否正在被取消
 *
 * 返回true表示work处于OFFQ（无pwq关联）且设置了OFFQ_CANCELING
 * 标志，即cancel_work_sync()正在进行中，此时不应再次入队。
 */
static bool work_is_canceling(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	return !(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_CANCELING);
}

/*
 * Policy functions.  These define the policies on how the global worker
 * pools are managed.  Unless noted otherwise, these functions assume that
 * they're being called with pool->lock held.
 */

/**
 * __need_more_worker - 检查pool是否需要更多worker运行
 *
 * 当pool->nr_running为0时返回true，表示没有正在运行的worker
 * 处理work，需要唤醒空闲worker或创建新worker。
 */
static bool __need_more_worker(struct worker_pool *pool)
{
	return !atomic_read(&pool->nr_running);
}

/*
 * Need to wake up a worker?  Called from anything but currently
 * running workers.
 *
 * Note that, because unbound workers never contribute to nr_running, this
 * function will always return %true for unbound pools as long as the
 * worklist isn't empty.
 */
/**
 * need_more_worker - 检查pool是否需要唤醒worker
 *
 * worklist非空且没有正在运行的worker时返回true。对于unbound
 * pool（nr_running始终为0），只要有work就总是返回true。
 * 是__need_more_worker()的外层封装，增加了worklist非空检查。
 */
static bool need_more_worker(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && __need_more_worker(pool);
}

/**
 * may_start_working - 检查pool中是否有空闲worker可以开始工作
 *
 * 返回pool->nr_idle是否非零。由忙碌但未运行的worker调用，
 * 判断是否有其他idle worker可以接管工作，从而决定自身是否
 * 需要保持运行状态或转入idle。
 */
/* Can I start working?  Called from busy but !running workers. */
static bool may_start_working(struct worker_pool *pool)
{
	return pool->nr_idle;
}

/**
 * keep_working - 检查worker是否需要继续处理work
 *
 * 由正在运行的worker调用：worklist非空且自己是唯一运行的
 * worker（nr_running<=1）时返回true，worker应继续循环取work。
 * 若有其他worker也在运行则不必再处理，可能进入idle。
 */
/* Do I need to keep working?  Called from currently running workers. */
static bool keep_working(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) &&
		atomic_read(&pool->nr_running) <= 1;
}

/**
 * need_to_create_worker - 判断是否需要创建新工作线程
 *
 * 由manager调用：当有待执行work但没有空闲worker时返回true，
 * 触发maybe_create_worker()创建新线程。
 */
/* Do we need a new worker?  Called from manager. */
static bool need_to_create_worker(struct worker_pool *pool)
{
	return need_more_worker(pool) && !may_start_working(pool);
}

/**
 * too_many_workers - 判断是否空闲worker过多
 *
 * 当空闲worker数超过2且 (idle-2)*MAX_IDLE_WORKERS_RATIO >= busy时
 * 返回true，触发idle_worker_timeout()逐步回收多余线程。
 */
/* Do we have too many workers and should some go away? */
static bool too_many_workers(struct worker_pool *pool)
{
	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
	int nr_idle = pool->nr_idle + managing; /* manager is considered idle */
	int nr_busy = pool->nr_workers - nr_idle;

	return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/*
 * Wake up functions.
 */

/**
 * first_idle_worker - 获取pool中第一个空闲worker
 *
 * 从pool->idle_list链表头取出第一个空闲worker。调用时需关闭
 * 抢占（或持有pool->lock），保证链表遍历的安全性。若无空闲
 * worker则返回NULL。
 */
/* Return the first idle worker.  Safe with preemption disabled */
static struct worker *first_idle_worker(struct worker_pool *pool)
{
	if (unlikely(list_empty(&pool->idle_list)))
		return NULL;

	return list_first_entry(&pool->idle_list, struct worker, entry);
}

/**
 * wake_up_worker - wake up an idle worker
 * @pool: worker pool to wake worker from
 *
 * Wake up the first idle worker of @pool.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * wake_up_worker - 唤醒pool中第一个空闲worker
 *
 * 在持有pool->lock时调用，取出idle_list首个worker并调用
 * wake_up_process()唤醒。用于insert_work()和wq_worker_sleeping()
 * 在需要更多并发时触发idle worker开始处理work。
 */
static void wake_up_worker(struct worker_pool *pool)
{
	struct worker *worker = first_idle_worker(pool);

	if (likely(worker))
		wake_up_process(worker->task);
}

/**
 * wq_worker_running - a worker is running again
 * @task: task waking up
 *
 * This function is called when a worker returns from schedule()
 */
/**
 * wq_worker_running - 工作线程从睡眠恢复运行
 *
 * 由调度器在worker任务被唤醒后调用（schedule()返回路径）。
 * 若worker此前处于sleeping状态且未被标记NOT_RUNNING，则递增
 * pool->nr_running，使并发管理感知到该线程已再次活跃。
 */
void wq_worker_running(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	if (!worker->sleeping)
		return;
	if (!(worker->flags & WORKER_NOT_RUNNING))
		atomic_inc(&worker->pool->nr_running);
	worker->sleeping = 0;
}

/**
 * wq_worker_sleeping - a worker is going to sleep
 * @task: task going to sleep
 *
 * This function is called from schedule() when a busy worker is
 * going to sleep. Preemption needs to be disabled to protect ->sleeping
 * assignment.
 */
/**
 * wq_worker_sleeping - 工作线程即将进入睡眠
 *
 * 由调度器在worker任务调用schedule()时（抢占前）调用。递减
 * pool->nr_running，若降至0且worklist非空则唤醒下一个空闲worker，
 * 维持并发度。rescuer线程（WORKER_NOT_RUNNING）不参与此计数。
 */
void wq_worker_sleeping(struct task_struct *task)
{
	struct worker *next, *worker = kthread_data(task);
	struct worker_pool *pool;

	/*
	 * Rescuers, which may not have all the fields set up like normal
	 * workers, also reach here, let's not access anything before
	 * checking NOT_RUNNING.
	 */
	if (worker->flags & WORKER_NOT_RUNNING)
		return;

	pool = worker->pool;

	/* Return if preempted before wq_worker_running() was reached */
	if (worker->sleeping)
		return;

	worker->sleeping = 1;
	raw_spin_lock_irq(&pool->lock);

	/*
	 * The counterpart of the following dec_and_test, implied mb,
	 * worklist not empty test sequence is in insert_work().
	 * Please read comment there.
	 *
	 * NOT_RUNNING is clear.  This means that we're bound to and
	 * running on the local cpu w/ rq lock held and preemption
	 * disabled, which in turn means that none else could be
	 * manipulating idle_list, so dereferencing idle_list without pool
	 * lock is safe.
	 */
	if (atomic_dec_and_test(&pool->nr_running) &&
	    !list_empty(&pool->worklist)) {
		next = first_idle_worker(pool);
		if (next)
			wake_up_process(next->task);
	}
	raw_spin_unlock_irq(&pool->lock);
}

/**
 * wq_worker_last_func - 获取worker最后执行的work函数
 * @task: 目标worker任务结构体
 *
 * 在schedule()路径中（kworker即将进入睡眠时）由调度器调用，
 * 用于PSI（Pressure Stall Information）识别聚合worker的身份，
 * 当该worker是系统或cgroup中最后进入睡眠的任务时可关闭周期性聚合。
 * 不涉及workqueue锁，仅在调度器的入/出队路径中返回稳定值。
 *
 * Return: worker最后执行的work函数指针，未执行过任何work时为NULL。
 */
work_func_t wq_worker_last_func(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	return worker->last_func;
}

/**
 * worker_set_flags - set worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to set
 *
 * Set @flags in @worker->flags and adjust nr_running accordingly.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock)
 */
/**
 * worker_set_flags - 设置worker标志位并维护nr_running计数
 *
 * 设置worker->flags中指定位。若设置了WORKER_NOT_RUNNING标志
 * （如等待flush、管理任务等），则同步递减pool->nr_running，
 * 通知并发管理该线程暂时不执行work。
 */
static inline void worker_set_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;

	WARN_ON_ONCE(worker->task != current);

	/* If transitioning into NOT_RUNNING, adjust nr_running. */
	if ((flags & WORKER_NOT_RUNNING) &&
	    !(worker->flags & WORKER_NOT_RUNNING)) {
		atomic_dec(&pool->nr_running);
	}

	worker->flags |= flags;
}

/**
 * worker_clr_flags - clear worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to clear
 *
 * Clear @flags in @worker->flags and adjust nr_running accordingly.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock)
 */
/**
 * worker_clr_flags - 清除worker标志位并维护nr_running计数
 *
 * 清除worker->flags中指定位。若清除了WORKER_NOT_RUNNING标志
 * （worker重新进入可运行状态），则递增pool->nr_running，
 * 通知并发管理该线程恢复活跃。
 */
static inline void worker_clr_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;
	unsigned int oflags = worker->flags;

	WARN_ON_ONCE(worker->task != current);

	worker->flags &= ~flags;

	/*
	 * If transitioning out of NOT_RUNNING, increment nr_running.  Note
	 * that the nested NOT_RUNNING is not a noop.  NOT_RUNNING is mask
	 * of multiple flags, not a single flag.
	 */
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(worker->flags & WORKER_NOT_RUNNING))
			atomic_inc(&pool->nr_running);
}

/**
 * find_worker_executing_work - find worker which is executing a work
 * @pool: pool of interest
 * @work: work to find worker for
 *
 * Find a worker which is executing @work on @pool by searching
 * @pool->busy_hash which is keyed by the address of @work.  For a worker
 * to match, its current execution should match the address of @work and
 * its work function.  This is to avoid unwanted dependency between
 * unrelated work executions through a work item being recycled while still
 * being executed.
 *
 * This is a bit tricky.  A work item may be freed once its execution
 * starts and nothing prevents the freed area from being recycled for
 * another work item.  If the same work item address ends up being reused
 * before the original execution finishes, workqueue will identify the
 * recycled work item as currently executing and make it wait until the
 * current execution finishes, introducing an unwanted dependency.
 *
 * This function checks the work item address and work function to avoid
 * false positives.  Note that this isn't complete as one may construct a
 * work function which can introduce dependency onto itself through a
 * recycled work item.  Well, if somebody wants to shoot oneself in the
 * foot that badly, there's only so much we can do, and if such deadlock
 * actually occurs, it should be easy to locate the culprit work function.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 *
 * Return:
 * Pointer to worker which is executing @work if found, %NULL
 * otherwise.
 */
/**
 * find_worker_executing_work - 查找正在执行指定work的worker
 *
 * 在pool->busy_hash哈希表中按work地址查找当前正在执行该work的
 * worker线程。用于检测work并发执行冲突，决定是否需要barrier或
 * 等待当前执行完毕再重新入队。
 */
static struct worker *find_worker_executing_work(struct worker_pool *pool,
						 struct work_struct *work)
{
	struct worker *worker;

	hash_for_each_possible(pool->busy_hash, worker, hentry,
			       (unsigned long)work)
		if (worker->current_work == work &&
		    worker->current_func == work->func)
			return worker;

	return NULL;
}

/**
 * move_linked_works - move linked works to a list
 * @work: start of series of works to be scheduled
 * @head: target list to append @work to
 * @nextp: out parameter for nested worklist walking
 *
 * Schedule linked works starting from @work to @head.  Work series to
 * be scheduled starts at @work and includes any consecutive work with
 * WORK_STRUCT_LINKED set in its predecessor.
 *
 * If @nextp is not NULL, it's updated to point to the next work of
 * the last scheduled work.  This allows move_linked_works() to be
 * nested inside outer list_for_each_entry_safe().
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * move_linked_works - 将链式work批量移动到目标链表头
 *
 * 将以@work起始的一组链式work（通过WORK_STRUCT_LINKED标志串联）
 * 整体移到@head链表尾部，用于process_one_work()在执行前将同一批
 * 关联work从pool->worklist搬移到本地列表，保证原子提交语义。
 */
static void move_linked_works(struct work_struct *work, struct list_head *head,
			      struct work_struct **nextp)
{
	struct work_struct *n;

	/*
	 * Linked worklist will always end before the end of the list,
	 * use NULL for list head.
	 */
	list_for_each_entry_safe_from(work, n, NULL, entry) {
		list_move_tail(&work->entry, head);
		if (!(*work_data_bits(work) & WORK_STRUCT_LINKED))
			break;
	}

	/*
	 * If we're already inside safe list traversal and have moved
	 * multiple works to the scheduled queue, the next position
	 * needs to be updated.
	 */
	if (nextp)
		*nextp = n;
}

/**
 * get_pwq - get an extra reference on the specified pool_workqueue
 * @pwq: pool_workqueue to get
 *
 * Obtain an extra reference on @pwq.  The caller should guarantee that
 * @pwq has positive refcnt and be holding the matching pool->lock.
 */
/**
 * get_pwq - 增加pool_workqueue引用计数
 *
 * 在持有pool->lock时对@pwq引用计数加一。调用者必须保证@pwq
 * 当前引用计数大于0，通常在将work入队到该pwq时调用。
 */
static void get_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	WARN_ON_ONCE(pwq->refcnt <= 0);
	pwq->refcnt++;
}

/**
 * put_pwq - put a pool_workqueue reference
 * @pwq: pool_workqueue to put
 *
 * Drop a reference of @pwq.  If its refcnt reaches zero, schedule its
 * destruction.  The caller should be holding the matching pool->lock.
 */
/**
 * put_pwq - 释放pool_workqueue引用计数
 *
 * 对@pwq引用计数减一。若降至0，调度pwq_unbound_release_workfn()
 * 异步释放（不能在pool->lock下直接释放），仅unbound队列才会
 * 真正触发销毁。
 */
static void put_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	if (likely(--pwq->refcnt))
		return;
	if (WARN_ON_ONCE(!(pwq->wq->flags & WQ_UNBOUND)))
		return;
	/*
	 * @pwq can't be released under pool->lock, bounce to
	 * pwq_unbound_release_workfn().  This never recurses on the same
	 * pool->lock as this path is taken only for unbound workqueues and
	 * the release work item is scheduled on a per-cpu workqueue.  To
	 * avoid lockdep warning, unbound pool->locks are given lockdep
	 * subclass of 1 in get_unbound_pool().
	 */
	schedule_work(&pwq->unbound_release_work);
}

/**
 * put_pwq_unlocked - put_pwq() with surrounding pool lock/unlock
 * @pwq: pool_workqueue to put (can be %NULL)
 *
 * put_pwq() with locking.  This function also allows %NULL @pwq.
 */
/**
 * put_pwq_unlocked - 不加锁版put_pwq，可接受NULL
 *
 * 自动获取pool->lock后调用put_pwq()，适用于调用者未持有
 * pool->lock的场景。安全处理@pwq为NULL的情况。
 */
static void put_pwq_unlocked(struct pool_workqueue *pwq)
{
	if (pwq) {
		/*
		 * As both pwqs and pools are RCU protected, the
		 * following lock operations are safe.
		 */
		raw_spin_lock_irq(&pwq->pool->lock);
		put_pwq(pwq);
		raw_spin_unlock_irq(&pwq->pool->lock);
	}
}

/**
 * pwq_activate_delayed_work - 将延迟work从delayed_works移入活跃队列
 *
 * 当pwq->nr_active未超过max_active时，将@work从pwq->delayed_works
 * 移到pool->worklist，清除DELAYED标志，递增nr_active，使work
 * 进入可被worker线程执行的状态。
 */
static void pwq_activate_delayed_work(struct work_struct *work)
{
	struct pool_workqueue *pwq = get_work_pwq(work);

	trace_workqueue_activate_work(work);
	if (list_empty(&pwq->pool->worklist))
		pwq->pool->watchdog_ts = jiffies;
	move_linked_works(work, &pwq->pool->worklist, NULL);
	__clear_bit(WORK_STRUCT_DELAYED_BIT, work_data_bits(work));
	pwq->nr_active++;
}

/**
 * pwq_activate_first_delayed - 激活delayed_works链表的第一个work
 *
 * 取出pwq->delayed_works链表头部第一个被延迟的work，调用
 * pwq_activate_delayed_work()将其移入pool->worklist，使其
 * 成为可被worker线程执行的活跃work。
 */
static void pwq_activate_first_delayed(struct pool_workqueue *pwq)
{
	struct work_struct *work = list_first_entry(&pwq->delayed_works,
						    struct work_struct, entry);

	pwq_activate_delayed_work(work);
}

/**
 * pwq_dec_nr_in_flight - decrement pwq's nr_in_flight
 * @pwq: pwq of interest
 * @color: color of work which left the queue
 *
 * A work either has completed or is removed from pending queue,
 * decrement nr_in_flight of its pwq and handle workqueue flushing.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * pwq_dec_nr_in_flight - work执行完成后更新pwq飞行中计数
 *
 * work完成后递减pwq->nr_in_flight[color]和nr_active，若有
 * delayed_works等待则激活首个延迟work。同时检查是否满足flush
 * 屏障条件，通知等待flush的调用者。
 */
static void pwq_dec_nr_in_flight(struct pool_workqueue *pwq, int color)
{
	/* uncolored work items don't participate in flushing or nr_active */
	if (color == WORK_NO_COLOR)
		goto out_put;

	pwq->nr_in_flight[color]--;

	pwq->nr_active--;
	if (!list_empty(&pwq->delayed_works)) {
		/* one down, submit a delayed one */
		if (pwq->nr_active < pwq->max_active)
			pwq_activate_first_delayed(pwq);
	}

	/* is flush in progress and are we at the flushing tip? */
	if (likely(pwq->flush_color != color))
		goto out_put;

	/* are there still in-flight works? */
	if (pwq->nr_in_flight[color])
		goto out_put;

	/* this pwq is done, clear flush_color */
	pwq->flush_color = -1;

	/*
	 * If this was the last pwq, wake up the first flusher.  It
	 * will handle the rest.
	 */
	if (atomic_dec_and_test(&pwq->wq->nr_pwqs_to_flush))
		complete(&pwq->wq->first_flusher->done);
out_put:
	put_pwq(pwq);
}

/**
 * try_to_grab_pending - steal work item from worklist and disable irq
 * @work: work item to steal
 * @is_dwork: @work is a delayed_work
 * @flags: place to store irq state
 *
 * Try to grab PENDING bit of @work.  This function can handle @work in any
 * stable state - idle, on timer or on worklist.
 *
 * Return:
 *
 *  ========	================================================================
 *  1		if @work was pending and we successfully stole PENDING
 *  0		if @work was idle and we claimed PENDING
 *  -EAGAIN	if PENDING couldn't be grabbed at the moment, safe to busy-retry
 *  -ENOENT	if someone else is canceling @work, this state may persist
 *		for arbitrarily long
 *  ========	================================================================
 *
 * Note:
 * On >= 0 return, the caller owns @work's PENDING bit.  To avoid getting
 * interrupted while holding PENDING and @work off queue, irq must be
 * disabled on entry.  This, combined with delayed_work->timer being
 * irqsafe, ensures that we return -EAGAIN for finite short period of time.
 *
 * On successful return, >= 0, irq is disabled and the caller is
 * responsible for releasing it using local_irq_restore(*@flags).
 *
 * This function is safe to call from any context including IRQ handler.
 */
/**
 * try_to_grab_pending - 尝试抢占work的PENDING位并从worklist移除
 *
 * 用于cancel_work()/cancel_delayed_work()流程。可处理work处于
 * 空闲、定时器或worklist上的任意状态：
 *   返回1：成功从worklist偷取；返回0：work空闲，直接置PENDING；
 *   返回-EAGAIN：暂时无法抢占，可重试；返回-ENOENT：已被其他人取消。
 * 成功时禁用IRQ并存入@flags，调用者需恢复。
 */
static int try_to_grab_pending(struct work_struct *work, bool is_dwork,
			       unsigned long *flags)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	local_irq_save(*flags);

	/* try to steal the timer if it exists */
	if (is_dwork) {
		struct delayed_work *dwork = to_delayed_work(work);

		/*
		 * dwork->timer is irqsafe.  If del_timer() fails, it's
		 * guaranteed that the timer is not queued anywhere and not
		 * running on the local CPU.
		 */
		if (likely(del_timer(&dwork->timer)))
			return 1;
	}

	/* try to claim PENDING the normal way */
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return 0;

	rcu_read_lock();
	/*
	 * The queueing is in progress, or it is already queued. Try to
	 * steal it from ->worklist without clearing WORK_STRUCT_PENDING.
	 */
	pool = get_work_pool(work);
	if (!pool)
		goto fail;

	raw_spin_lock(&pool->lock);
	/*
	 * work->data is guaranteed to point to pwq only while the work
	 * item is queued on pwq->wq, and both updating work->data to point
	 * to pwq on queueing and to pool on dequeueing are done under
	 * pwq->pool->lock.  This in turn guarantees that, if work->data
	 * points to pwq which is associated with a locked pool, the work
	 * item is currently queued on that pool.
	 */
	pwq = get_work_pwq(work);
	if (pwq && pwq->pool == pool) {
		debug_work_deactivate(work);

		/*
		 * A delayed work item cannot be grabbed directly because
		 * it might have linked NO_COLOR work items which, if left
		 * on the delayed_list, will confuse pwq->nr_active
		 * management later on and cause stall.  Make sure the work
		 * item is activated before grabbing.
		 */
		if (*work_data_bits(work) & WORK_STRUCT_DELAYED)
			pwq_activate_delayed_work(work);

		list_del_init(&work->entry);
		pwq_dec_nr_in_flight(pwq, get_work_color(work));

		/* work->data points to pwq iff queued, point to pool */
		set_work_pool_and_keep_pending(work, pool->id);

		raw_spin_unlock(&pool->lock);
		rcu_read_unlock();
		return 1;
	}
	raw_spin_unlock(&pool->lock);
fail:
	rcu_read_unlock();
	local_irq_restore(*flags);
	if (work_is_canceling(work))
		return -ENOENT;
	cpu_relax();
	return -EAGAIN;
}

/**
 * insert_work - insert a work into a pool
 * @pwq: pwq @work belongs to
 * @work: work to insert
 * @head: insertion point
 * @extra_flags: extra WORK_STRUCT_* flags to set
 *
 * Insert @work which belongs to @pwq after @head.  @extra_flags is or'd to
 * work_struct flags.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * insert_work - 将work item插入工作队列的执行链表
 *
 * 设置work的pwq关联（set_work_pwq）并将其添加到指定链表尾部，
 * 递增pwq引用计数。通过内存屏障确保work的数据对工作线程可见。
 * 若worker_pool处于运行状态但没有活跃worker，则唤醒一个工作线程。
 */
static void insert_work(struct pool_workqueue *pwq, struct work_struct *work,
			struct list_head *head, unsigned int extra_flags)
{
	struct worker_pool *pool = pwq->pool;

	/* we own @work, set data and link */
	set_work_pwq(work, pwq, extra_flags);
	list_add_tail(&work->entry, head);
	get_pwq(pwq);

	/*
	 * Ensure either wq_worker_sleeping() sees the above
	 * list_add_tail() or we see zero nr_running to avoid workers lying
	 * around lazily while there are works to be processed.
	 */
	smp_mb();

	if (__need_more_worker(pool))
		wake_up_worker(pool);
}

/*
 * Test whether @work is being queued from another work executing on the
 * same workqueue.
 */
/**
 * is_chained_work - 检查当前是否在同一wq的work回调中入队
 *
 * 若调用者本身就是正在执行@wq上某个work的worker线程，则返回true。
 * 用于__queue_work()检测链式入队（chained work），避免在
 * WQ_MEM_RECLAIM队列中因自我依赖导致死锁。
 */
static bool is_chained_work(struct workqueue_struct *wq)
{
	struct worker *worker;

	worker = current_wq_worker();
	/*
	 * Return %true iff I'm a worker executing a work item on @wq.  If
	 * I'm @worker, it's safe to dereference it without locking.
	 */
	return worker && worker->current_pwq->wq == wq;
}

/*
 * When queueing an unbound work item to a wq, prefer local CPU if allowed
 * by wq_unbound_cpumask.  Otherwise, round robin among the allowed ones to
 * avoid perturbing sensitive tasks.
 */
/**
 * wq_select_unbound_cpu - 为unbound work选择目标CPU
 *
 * 优先选择当前本地CPU（若在wq_unbound_cpumask内），否则轮询
 * 允许的CPU列表。若启用了wq_debug_force_rr_cpu则强制轮询以便
 * 调试。用于__queue_work()确定unbound work的目标pool。
 */
static int wq_select_unbound_cpu(int cpu)
{
	static bool printed_dbg_warning;
	int new_cpu;

	if (likely(!wq_debug_force_rr_cpu)) {
		if (cpumask_test_cpu(cpu, wq_unbound_cpumask))
			return cpu;
	} else if (!printed_dbg_warning) {
		pr_warn("workqueue: round-robin CPU selection forced, expect performance impact\n");
		printed_dbg_warning = true;
	}

	if (cpumask_empty(wq_unbound_cpumask))
		return cpu;

	new_cpu = __this_cpu_read(wq_rr_cpu_last);
	new_cpu = cpumask_next_and(new_cpu, wq_unbound_cpumask, cpu_online_mask);
	if (unlikely(new_cpu >= nr_cpu_ids)) {
		new_cpu = cpumask_first_and(wq_unbound_cpumask, cpu_online_mask);
		if (unlikely(new_cpu >= nr_cpu_ids))
			return cpu;
	}
	__this_cpu_write(wq_rr_cpu_last, new_cpu);

	return new_cpu;
}

/**
 * __queue_work - 将work item加入工作队列（核心实现）
 *
 * 选择合适的pool_workqueue（pwq）：优先使用与work上次执行的pool相同
 * 的pwq以利用缓存局部性（工作迁移优化）；若队列被限速则检查是否可执行。
 * 最终调用insert_work()将work加入待执行链表，并在必要时唤醒工作线程。
 * 处理unbound workqueue、NUMA感知放置和WQ_MEM_RECLAIM特殊情况。
 */
static void __queue_work(int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
{
	struct pool_workqueue *pwq;
	struct worker_pool *last_pool;
	struct list_head *worklist;
	unsigned int work_flags;
	unsigned int req_cpu = cpu;

	/*
	 * While a work item is PENDING && off queue, a task trying to
	 * steal the PENDING will busy-loop waiting for it to either get
	 * queued or lose PENDING.  Grabbing PENDING and queueing should
	 * happen with IRQ disabled.
	 */
	lockdep_assert_irqs_disabled();

	debug_work_activate(work);

	/* if draining, only works from the same workqueue are allowed */
	if (unlikely(wq->flags & __WQ_DRAINING) &&
	    WARN_ON_ONCE(!is_chained_work(wq)))
		return;
	rcu_read_lock();
retry:
	/* pwq which will be used unless @work is executing elsewhere */
	if (wq->flags & WQ_UNBOUND) {
		if (req_cpu == WORK_CPU_UNBOUND)
			cpu = wq_select_unbound_cpu(raw_smp_processor_id());
		pwq = unbound_pwq_by_node(wq, cpu_to_node(cpu));
	} else {
		if (req_cpu == WORK_CPU_UNBOUND)
			cpu = raw_smp_processor_id();
		pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	}

	/*
	 * If @work was previously on a different pool, it might still be
	 * running there, in which case the work needs to be queued on that
	 * pool to guarantee non-reentrancy.
	 */
	last_pool = get_work_pool(work);
	if (last_pool && last_pool != pwq->pool) {
		struct worker *worker;

		raw_spin_lock(&last_pool->lock);

		worker = find_worker_executing_work(last_pool, work);

		if (worker && worker->current_pwq->wq == wq) {
			pwq = worker->current_pwq;
		} else {
			/* meh... not running there, queue here */
			raw_spin_unlock(&last_pool->lock);
			raw_spin_lock(&pwq->pool->lock);
		}
	} else {
		raw_spin_lock(&pwq->pool->lock);
	}

	/*
	 * pwq is determined and locked.  For unbound pools, we could have
	 * raced with pwq release and it could already be dead.  If its
	 * refcnt is zero, repeat pwq selection.  Note that pwqs never die
	 * without another pwq replacing it in the numa_pwq_tbl or while
	 * work items are executing on it, so the retrying is guaranteed to
	 * make forward-progress.
	 */
	if (unlikely(!pwq->refcnt)) {
		if (wq->flags & WQ_UNBOUND) {
			raw_spin_unlock(&pwq->pool->lock);
			cpu_relax();
			goto retry;
		}
		/* oops */
		WARN_ONCE(true, "workqueue: per-cpu pwq for %s on cpu%d has 0 refcnt",
			  wq->name, cpu);
	}

	/* pwq determined, queue */
	trace_workqueue_queue_work(req_cpu, pwq, work);

	if (WARN_ON(!list_empty(&work->entry)))
		goto out;

	pwq->nr_in_flight[pwq->work_color]++;
	work_flags = work_color_to_flags(pwq->work_color);

	if (likely(pwq->nr_active < pwq->max_active)) {
		trace_workqueue_activate_work(work);
		pwq->nr_active++;
		worklist = &pwq->pool->worklist;
		if (list_empty(worklist))
			pwq->pool->watchdog_ts = jiffies;
	} else {
		work_flags |= WORK_STRUCT_DELAYED;
		worklist = &pwq->delayed_works;
	}

	insert_work(pwq, work, worklist, work_flags);

out:
	raw_spin_unlock(&pwq->pool->lock);
	rcu_read_unlock();
}

/**
 * queue_work_on - queue work on specific cpu
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a specific CPU, the caller must ensure it
 * can't go away.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
/**
 * queue_work_on - 将work item提交到指定CPU的工作队列
 *
 * 原子地设置WORK_STRUCT_PENDING_BIT标志（防止同一work被重复入队），
 * 成功后调用__queue_work()将work加入目标CPU的worker_pool队列。
 * 返回true表示成功入队，false表示work已在队列中（pending）。
 * 这是queue_work()/queue_work_on()系列函数的公共路径。
 */
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	bool ret = false;
	unsigned long flags;

	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL(queue_work_on);

/**
 * workqueue_select_cpu_near - Select a CPU based on NUMA node
 * @node: NUMA node ID that we want to select a CPU from
 *
 * This function will attempt to find a "random" cpu available on a given
 * node. If there are no CPUs available on the given node it will return
 * WORK_CPU_UNBOUND indicating that we should just schedule to any
 * available CPU if we need to schedule this work.
 */
/**
 * workqueue_select_cpu_near - 选择指定NUMA节点上的CPU
 *
 * 为unbound work选择一个靠近@node的CPU：若NUMA未启用或节点
 * 无效/离线，返回WORK_CPU_UNBOUND；否则优先返回当前CPU（若在
 * 同节点），否则返回节点上第一个在线CPU。
 */
static int workqueue_select_cpu_near(int node)
{
	int cpu;

	/* No point in doing this if NUMA isn't enabled for workqueues */
	if (!wq_numa_enabled)
		return WORK_CPU_UNBOUND;

	/* Delay binding to CPU if node is not valid or online */
	if (node < 0 || node >= MAX_NUMNODES || !node_online(node))
		return WORK_CPU_UNBOUND;

	/* Use local node/cpu if we are already there */
	cpu = raw_smp_processor_id();
	if (node == cpu_to_node(cpu))
		return cpu;

	/* Use "random" otherwise know as "first" online CPU of node */
	cpu = cpumask_any_and(cpumask_of_node(node), cpu_online_mask);

	/* If CPU is valid return that, otherwise just defer */
	return cpu < nr_cpu_ids ? cpu : WORK_CPU_UNBOUND;
}

/**
 * queue_work_node - queue work on a "random" cpu for a given NUMA node
 * @node: NUMA node that we are targeting the work for
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a "random" CPU within a given NUMA node. The basic
 * idea here is to provide a way to somehow associate work with a given
 * NUMA node.
 *
 * This function will only make a best effort attempt at getting this onto
 * the right NUMA node. If no node is requested or the requested node is
 * offline then we just fall back to standard queue_work behavior.
 *
 * Currently the "random" CPU ends up being the first available CPU in the
 * intersection of cpu_online_mask and the cpumask of the node, unless we
 * are running on the node. In that case we just use the current CPU.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
/**
 * queue_work_node - 将work提交到指定NUMA节点上的工作队列
 *
 * 尽力将@work调度到@node上的某个CPU执行，优先选取当前在该节点
 * 上运行的CPU，否则选取该节点第一个在线CPU。若节点无效或离线，
 * 退回到标准queue_work行为。返回false表示work已在队列中。
 */
bool queue_work_node(int node, struct workqueue_struct *wq,
		     struct work_struct *work)
{
	unsigned long flags;
	bool ret = false;

	/*
	 * This current implementation is specific to unbound workqueues.
	 * Specifically we only return the first available CPU for a given
	 * node instead of cycling through individual CPUs within the node.
	 *
	 * If this is used with a per-cpu workqueue then the logic in
	 * workqueue_select_cpu_near would need to be updated to allow for
	 * some round robin type logic.
	 */
	WARN_ON_ONCE(!(wq->flags & WQ_UNBOUND));

	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		int cpu = workqueue_select_cpu_near(node);

		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL_GPL(queue_work_node);

/**
 * delayed_work_timer_fn - delayed_work定时器到期回调
 *
 * 在IRQ关闭状态下由hrtimer/timer触发，从dwork->wq和dwork->cpu
 * 读取目标参数，直接调用__queue_work()将work立即入队执行。
 * delayed_work到期后进入普通work执行流程的入口点。
 */
void delayed_work_timer_fn(struct timer_list *t)
{
	struct delayed_work *dwork = from_timer(dwork, t, timer);

	/* should have been called from irqsafe timer with irq already off */
	__queue_work(dwork->cpu, dwork->wq, &dwork->work);
}
EXPORT_SYMBOL(delayed_work_timer_fn);

/**
 * __queue_delayed_work - 延迟work入队的内部实现
 *
 * 若delay为0则直接调用__queue_work()立即入队；否则设置
 * dwork->cpu和dwork->wq并启动高精度定时器，超时后由
 * delayed_work_timer_fn()回调触发实际入队操作。
 */
static void __queue_delayed_work(int cpu, struct workqueue_struct *wq,
				struct delayed_work *dwork, unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	WARN_ON_ONCE(!wq);
	WARN_ON_ONCE(timer->function != delayed_work_timer_fn);
	WARN_ON_ONCE(timer_pending(timer));
	WARN_ON_ONCE(!list_empty(&work->entry));

	/*
	 * If @delay is 0, queue @dwork->work immediately.  This is for
	 * both optimization and correctness.  The earliest @timer can
	 * expire is on the closest next tick and delayed_work users depend
	 * on that there's no such delay when @delay is 0.
	 */
	if (!delay) {
		__queue_work(cpu, wq, &dwork->work);
		return;
	}

	dwork->wq = wq;
	dwork->cpu = cpu;
	timer->expires = jiffies + delay;

	if (unlikely(cpu != WORK_CPU_UNBOUND))
		add_timer_on(timer, cpu);
	else
		add_timer(timer);
}

/**
 * queue_delayed_work_on - queue work on specific CPU after delay
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * Return: %false if @work was already on a queue, %true otherwise.  If
 * @delay is zero and @dwork is idle, it will be scheduled for immediate
 * execution.
 */
/**
 * queue_delayed_work_on - 将delayed_work提交到指定CPU（带延迟）
 *
 * 若work处于idle状态，设置PENDING位后启动定时器（或直接入队），
 * 延迟@delay jiffies后在@cpu上执行。返回true表示成功入队，
 * false表示work已在pending状态。
 */
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			   struct delayed_work *dwork, unsigned long delay)
{
	struct work_struct *work = &dwork->work;
	bool ret = false;
	unsigned long flags;

	/* read the comment in __queue_work() */
	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL(queue_delayed_work_on);

/**
 * mod_delayed_work_on - modify delay of or queue a delayed work on specific CPU
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * If @dwork is idle, equivalent to queue_delayed_work_on(); otherwise,
 * modify @dwork's timer so that it expires after @delay.  If @delay is
 * zero, @work is guaranteed to be scheduled immediately regardless of its
 * current state.
 *
 * Return: %false if @dwork was idle and queued, %true if @dwork was
 * pending and its timer was modified.
 *
 * This function is safe to call from any context including IRQ handler.
 * See try_to_grab_pending() for details.
 */
/**
 * mod_delayed_work_on - 修改delayed_work的延迟时间和目标CPU
 *
 * 取消当前pending的@dwork并以新的@delay和@cpu重新入队。若
 * work正在执行中则返回-EAGAIN并重试。可在任意上下文调用，
 * 参见try_to_grab_pending()。
 */
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
			 struct delayed_work *dwork, unsigned long delay)
{
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(&dwork->work, true, &flags);
	} while (unlikely(ret == -EAGAIN));

	if (likely(ret >= 0)) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		local_irq_restore(flags);
	}

	/* -ENOENT from try_to_grab_pending() becomes %true */
	return ret;
}
EXPORT_SYMBOL_GPL(mod_delayed_work_on);

/**
 * rcu_work_rcufn - RCU宽限期结束后将rcu_work入队
 *
 * 作为call_rcu()的回调，在RCU宽限期过后被调用，将rwork
 * 提交到WORK_CPU_UNBOUND池执行。禁用IRQ后调用__queue_work()
 * 保证在正确上下文中入队。
 */
static void rcu_work_rcufn(struct rcu_head *rcu)
{
	struct rcu_work *rwork = container_of(rcu, struct rcu_work, rcu);

	/* read the comment in __queue_work() */
	local_irq_disable();
	__queue_work(WORK_CPU_UNBOUND, rwork->wq, &rwork->work);
	local_irq_enable();
}

/**
 * queue_rcu_work - queue work after a RCU grace period
 * @wq: workqueue to use
 * @rwork: work to queue
 *
 * Return: %false if @rwork was already pending, %true otherwise.  Note
 * that a full RCU grace period is guaranteed only after a %true return.
 * While @rwork is guaranteed to be executed after a %false return, the
 * execution may happen before a full RCU grace period has passed.
 */
/**
 * queue_rcu_work - 等待RCU宽限期后将work入队执行
 *
 * 设置PENDING位后调用call_rcu()注册回调，宽限期结束后由
 * rcu_work_rcufn()将work提交到@wq。返回true表示成功注册，
 * false表示work已在pending状态。注意返回false时work仍会
 * 在宽限期结束后执行（并非保证不执行）。
 */
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork)
{
	struct work_struct *work = &rwork->work;

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		rwork->wq = wq;
		call_rcu(&rwork->rcu, rcu_work_rcufn);
		return true;
	}

	return false;
}
EXPORT_SYMBOL(queue_rcu_work);

/**
 * worker_enter_idle - enter idle state
 * @worker: worker which is entering idle state
 *
 * @worker is entering idle state.  Update stats and idle timer if
 * necessary.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * worker_enter_idle - worker线程进入空闲状态
 *
 * 设置WORKER_IDLE标志，将worker加入pool->idle_list，递减
 * nr_workers并（若需要）启动idle超时定时器。若pool->worklist
 * 仍有work则唤醒另一个worker继续处理。
 */
static void worker_enter_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(worker->flags & WORKER_IDLE) ||
	    WARN_ON_ONCE(!list_empty(&worker->entry) &&
			 (worker->hentry.next || worker->hentry.pprev)))
		return;

	/* can't use worker_set_flags(), also called from create_worker() */
	worker->flags |= WORKER_IDLE;
	pool->nr_idle++;
	worker->last_active = jiffies;

	/* idle_list is LIFO */
	list_add(&worker->entry, &pool->idle_list);

	if (too_many_workers(pool) && !timer_pending(&pool->idle_timer))
		mod_timer(&pool->idle_timer, jiffies + IDLE_WORKER_TIMEOUT);

	/*
	 * Sanity check nr_running.  Because unbind_workers() releases
	 * pool->lock between setting %WORKER_UNBOUND and zapping
	 * nr_running, the warning may trigger spuriously.  Check iff
	 * unbind is not in progress.
	 */
	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     pool->nr_workers == pool->nr_idle &&
		     atomic_read(&pool->nr_running));
}

/**
 * worker_leave_idle - leave idle state
 * @worker: worker which is leaving idle state
 *
 * @worker is leaving idle state.  Update stats.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * worker_leave_idle - worker线程离开空闲状态
 *
 * 清除WORKER_IDLE标志，从pool->idle_list移除，递减nr_idle。
 * 在worker被唤醒开始处理work时调用，与worker_enter_idle()
 * 配对，维护pool的idle计数和列表一致性。
 */
static void worker_leave_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(!(worker->flags & WORKER_IDLE)))
		return;
	worker_clr_flags(worker, WORKER_IDLE);
	pool->nr_idle--;
	list_del_init(&worker->entry);
}

/**
 * alloc_worker - 分配并初始化worker结构
 *
 * 在@node节点上分配worker结构，初始化entry/scheduled/node
 * 链表头，设置初始last_active时间戳。不绑定到pool，需后续
 * 调用worker_attach_to_pool()完成完整初始化。
 */
static struct worker *alloc_worker(int node)
{
	struct worker *worker;

	worker = kzalloc_node(sizeof(*worker), GFP_KERNEL, node);
	if (worker) {
		INIT_LIST_HEAD(&worker->entry);
		INIT_LIST_HEAD(&worker->scheduled);
		INIT_LIST_HEAD(&worker->node);
		/* on creation a worker is in !idle && prep state */
		worker->flags = WORKER_PREP;
	}
	return worker;
}

/**
 * worker_attach_to_pool() - attach a worker to a pool
 * @worker: worker to be attached
 * @pool: the target pool
 *
 * Attach @worker to @pool.  Once attached, the %WORKER_UNBOUND flag and
 * cpu-binding of @worker are kept coordinated with the pool across
 * cpu-[un]hotplugs.
 */
/**
 * worker_attach_to_pool - 将worker绑定到worker_pool
 *
 * 在持有wq_pool_attach_mutex的情况下将@worker加入
 * pool->workers列表，设置worker->pool，并根据pool状态
 * 设置WORKER_UNBOUND标志。确保CPU热插拔时能正确感知worker。
 */
static void worker_attach_to_pool(struct worker *worker,
				   struct worker_pool *pool)
{
	mutex_lock(&wq_pool_attach_mutex);

	/*
	 * set_cpus_allowed_ptr() will fail if the cpumask doesn't have any
	 * online CPUs.  It'll be re-applied when any of the CPUs come up.
	 */
	set_cpus_allowed_ptr(worker->task, pool->attrs->cpumask);

	/*
	 * The wq_pool_attach_mutex ensures %POOL_DISASSOCIATED remains
	 * stable across this function.  See the comments above the flag
	 * definition for details.
	 */
	if (pool->flags & POOL_DISASSOCIATED)
		worker->flags |= WORKER_UNBOUND;

	list_add_tail(&worker->node, &pool->workers);
	worker->pool = pool;

	mutex_unlock(&wq_pool_attach_mutex);
}

/**
 * worker_detach_from_pool() - detach a worker from its pool
 * @worker: worker which is attached to its pool
 *
 * Undo the attaching which had been done in worker_attach_to_pool().  The
 * caller worker shouldn't access to the pool after detached except it has
 * other reference to the pool.
 */
/**
 * worker_detach_from_pool - 将worker从pool解绑
 *
 * worker_attach_to_pool()的逆操作。从pool->workers移除worker，
 * 若pool->detach_completion已设（put_unbound_pool()在等待），
 * 则通知其所有worker已离开，可安全销毁pool。
 */
static void worker_detach_from_pool(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;
	struct completion *detach_completion = NULL;

	mutex_lock(&wq_pool_attach_mutex);

	list_del(&worker->node);
	worker->pool = NULL;

	if (list_empty(&pool->workers))
		detach_completion = pool->detach_completion;
	mutex_unlock(&wq_pool_attach_mutex);

	/* clear leftover flags without pool->lock after it is detached */
	worker->flags &= ~(WORKER_UNBOUND | WORKER_REBOUND);

	if (detach_completion)
		complete(detach_completion);
}

/**
 * create_worker - create a new workqueue worker
 * @pool: pool the new worker will belong to
 *
 * Create and start a new worker which is attached to @pool.
 *
 * CONTEXT:
 * Might sleep.  Does GFP_KERNEL allocations.
 *
 * Return:
 * Pointer to the newly created worker.
 */
/**
 * create_worker - 为worker_pool创建一个新工作线程
 *
 * 分配worker结构体，通过ida_simple_get()获取唯一ID用于命名kthread
 * （格式：kworker/u<cpu>:<id> 或 kworker/<cpu>:<id>），然后创建
 * 内核线程并绑定到pool的CPU（per-CPU pool）或设置为unbound。
 * 创建成功后将worker加入pool->workers链表并启动线程。
 */
static struct worker *create_worker(struct worker_pool *pool)
{
	struct worker *worker = NULL;
	int id = -1;
	char id_buf[16];

	/* ID is needed to determine kthread name */
	id = ida_simple_get(&pool->worker_ida, 0, 0, GFP_KERNEL);
	if (id < 0)
		goto fail;

	worker = alloc_worker(pool->node);
	if (!worker)
		goto fail;

	worker->id = id;

	if (pool->cpu >= 0)
		snprintf(id_buf, sizeof(id_buf), "%d:%d%s", pool->cpu, id,
			 pool->attrs->nice < 0  ? "H" : "");
	else
		snprintf(id_buf, sizeof(id_buf), "u%d:%d", pool->id, id);

	worker->task = kthread_create_on_node(worker_thread, worker, pool->node,
					      "kworker/%s", id_buf);
	if (IS_ERR(worker->task))
		goto fail;

	set_user_nice(worker->task, pool->attrs->nice);
	kthread_bind_mask(worker->task, pool->attrs->cpumask);

	/* successful, attach the worker to the pool */
	worker_attach_to_pool(worker, pool);

	/* start the newly created worker */
	raw_spin_lock_irq(&pool->lock);
	worker->pool->nr_workers++;
	worker_enter_idle(worker);
	wake_up_process(worker->task);
	raw_spin_unlock_irq(&pool->lock);

	return worker;

fail:
	if (id >= 0)
		ida_simple_remove(&pool->worker_ida, id);
	kfree(worker);
	return NULL;
}

/**
 * destroy_worker - destroy a workqueue worker
 * @worker: worker to be destroyed
 *
 * Destroy @worker and adjust @pool stats accordingly.  The worker should
 * be idle.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * destroy_worker - 销毁一个空闲工作线程
 *
 * 将worker从pool->idle_list和pool->workers链表中移除，
 * 释放IDA ID，并唤醒内核线程使其退出（设置WORKER_DIE标志）。
 * 只允许销毁处于IDLE状态且没有当前执行任务的worker。
 * 用于控制worker_pool的线程数量（空闲超时后收缩）。
 */
static void destroy_worker(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	lockdep_assert_held(&pool->lock);

	/* sanity check frenzy */
	if (WARN_ON(worker->current_work) ||
	    WARN_ON(!list_empty(&worker->scheduled)) ||
	    WARN_ON(!(worker->flags & WORKER_IDLE)))
		return;

	pool->nr_workers--;
	pool->nr_idle--;

	list_del_init(&worker->entry);
	worker->flags |= WORKER_DIE;
	wake_up_process(worker->task);
}

/**
 * idle_worker_timeout - 空闲worker超时定时器处理
 *
 * pool->idle_timer到期时触发：检查是否有过多空闲worker
 * （too_many_workers()），若有则销毁最老的idle worker。
 * 若仍有多余worker则重新设置定时器，逐步回收多余线程。
 */
static void idle_worker_timeout(struct timer_list *t)
{
	struct worker_pool *pool = from_timer(pool, t, idle_timer);

	raw_spin_lock_irq(&pool->lock);

	while (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		/* idle_list is kept in LIFO order, check the last one */
		worker = list_entry(pool->idle_list.prev, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;

		if (time_before(jiffies, expires)) {
			mod_timer(&pool->idle_timer, expires);
			break;
		}

		destroy_worker(worker);
	}

	raw_spin_unlock_irq(&pool->lock);
}

/**
 * send_mayday - 向WQ_MEM_RECLAIM队列的rescuer发送求救信号
 *
 * 当worker_pool无法创建新线程时调用。将@work所属wq加入
 * pool->maydays链表，并唤醒wq->rescuer线程，让rescuer接管
 * 执行该work，避免内存分配死锁。需持有wq_mayday_lock。
 */
static void send_mayday(struct work_struct *work)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq_mayday_lock);

	if (!wq->rescuer)
		return;

	/* mayday mayday mayday */
	if (list_empty(&pwq->mayday_node)) {
		/*
		 * If @pwq is for an unbound wq, its base ref may be put at
		 * any time due to an attribute change.  Pin @pwq until the
		 * rescuer is done with it.
		 */
		get_pwq(pwq);
		list_add_tail(&pwq->mayday_node, &wq->maydays);
		wake_up_process(wq->rescuer->task);
	}
}

/**
 * pool_mayday_timeout - mayday定时器超时处理
 *
 * pool->mayday_timer到期时触发：遍历pool->worklist中所有属于
 * WQ_MEM_RECLAIM队列的work，逐一调用send_mayday()发送求救信号，
 * 并重启定时器以持续通知直到内存分配成功。
 */
static void pool_mayday_timeout(struct timer_list *t)
{
	struct worker_pool *pool = from_timer(pool, t, mayday_timer);
	struct work_struct *work;

	raw_spin_lock_irq(&pool->lock);
	raw_spin_lock(&wq_mayday_lock);		/* for wq->maydays */

	if (need_to_create_worker(pool)) {
		/*
		 * We've been trying to create a new worker but
		 * haven't been successful.  We might be hitting an
		 * allocation deadlock.  Send distress signals to
		 * rescuers.
		 */
		list_for_each_entry(work, &pool->worklist, entry)
			send_mayday(work);
	}

	raw_spin_unlock(&wq_mayday_lock);
	raw_spin_unlock_irq(&pool->lock);

	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INTERVAL);
}

/**
 * maybe_create_worker - create a new worker if necessary
 * @pool: pool to create a new worker for
 *
 * Create a new worker for @pool if necessary.  @pool is guaranteed to
 * have at least one idle worker on return from this function.  If
 * creating a new worker takes longer than MAYDAY_INTERVAL, mayday is
 * sent to all rescuers with works scheduled on @pool to resolve
 * possible allocation deadlock.
 *
 * On return, need_to_create_worker() is guaranteed to be %false and
 * may_start_working() %true.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.  Called only from
 * manager.
 */
/**
 * maybe_create_worker - 按需创建工作线程（manager专用）
 *
 * 在持有pool->lock的情况下被manager调用。若need_to_create_worker()
 * 为真，循环调用create_worker()直到成功或不再需要。若创建耗时超过
 * MAYDAY_INITIAL_TIMEOUT，向所有rescuer发送mayday信号，避免内存
 * 分配死锁。函数返回时保证may_start_working()为真。
 */
static void maybe_create_worker(struct worker_pool *pool)
__releases(&pool->lock)
__acquires(&pool->lock)
{
restart:
	raw_spin_unlock_irq(&pool->lock);

	/* if we don't make progress in MAYDAY_INITIAL_TIMEOUT, call for help */
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INITIAL_TIMEOUT);

	while (true) {
		if (create_worker(pool) || !need_to_create_worker(pool))
			break;

		schedule_timeout_interruptible(CREATE_COOLDOWN);

		if (!need_to_create_worker(pool))
			break;
	}

	del_timer_sync(&pool->mayday_timer);
	raw_spin_lock_irq(&pool->lock);
	/*
	 * This is necessary even after a new worker was just successfully
	 * created as @pool->lock was dropped and the new worker might have
	 * already become busy.
	 */
	if (need_to_create_worker(pool))
		goto restart;
}

/**
 * manage_workers - manage worker pool
 * @worker: self
 *
 * Assume the manager role and manage the worker pool @worker belongs
 * to.  At any given time, there can be only zero or one manager per
 * pool.  The exclusion is handled automatically by this function.
 *
 * The caller can safely start processing works on false return.  On
 * true return, it's guaranteed that need_to_create_worker() is false
 * and may_start_working() is true.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.
 *
 * Return:
 * %false if the pool doesn't need management and the caller can safely
 * start processing works, %true if management function was performed and
 * the conditions that the caller verified before calling the function may
 * no longer be true.
 */
/**
 * manage_workers - 工作线程池管理入口
 *
 * 由worker_thread()在发现需要管理时调用。每个pool同一时刻只允许
 * 一个manager（POOL_MANAGER_ACTIVE标志互斥）。manager负责调用
 * maybe_create_worker()保证pool有足够worker可用。返回false表示
 * 无需管理，调用者可直接处理work；返回true表示执行了管理动作，
 * 调用者需重新检查条件。
 */
static bool manage_workers(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (pool->flags & POOL_MANAGER_ACTIVE)
		return false;

	pool->flags |= POOL_MANAGER_ACTIVE;
	pool->manager = worker;

	maybe_create_worker(pool);

	pool->manager = NULL;
	pool->flags &= ~POOL_MANAGER_ACTIVE;
	rcuwait_wake_up(&manager_wait);
	return true;
}

/**
 * process_one_work - process single work
 * @worker: self
 * @work: work to process
 *
 * Process @work.  This function contains all the logics necessary to
 * process a single work including synchronization against and
 * interaction with other workers on the same cpu, queueing and
 * flushing.  As long as context requirement is met, any worker can
 * call this function to process a work.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which is released and regrabbed.
 */
/**
 * process_one_work - 执行一个work item
 *
 * 工作线程的核心执行函数。在释放pool->lock后调用work->func()，
 * 执行完成后清除PENDING标志。处理WQ_CPU_INTENSIVE标志（将worker
 * 标记为CPU密集型，使concurrency管理将其排除在活跃worker计数外）。
 * 若发现同一work正被其他worker执行则等待（collision处理）。
 * 使用lockdep跟踪work的锁依赖关系。
 */
static void process_one_work(struct worker *worker, struct work_struct *work)
__releases(&pool->lock)
__acquires(&pool->lock)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct worker_pool *pool = worker->pool;
	bool cpu_intensive = pwq->wq->flags & WQ_CPU_INTENSIVE;
	int work_color;
	struct worker *collision;
#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the struct work_struct from
	 * inside the function that is called from it, this we need to
	 * take into account for lockdep too.  To avoid bogus "held
	 * lock freed" warnings as well as problems when looking into
	 * work->lockdep_map, make a copy and use that here.
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &work->lockdep_map);
#endif
	/* ensure we're on the correct CPU */
	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     raw_smp_processor_id() != pool->cpu);

	/*
	 * A single work shouldn't be executed concurrently by
	 * multiple workers on a single cpu.  Check whether anyone is
	 * already processing the work.  If so, defer the work to the
	 * currently executing one.
	 */
	collision = find_worker_executing_work(pool, work);
	if (unlikely(collision)) {
		move_linked_works(work, &collision->scheduled, NULL);
		return;
	}

	/* claim and dequeue */
	debug_work_deactivate(work);
	hash_add(pool->busy_hash, &worker->hentry, (unsigned long)work);
	worker->current_work = work;
	worker->current_func = work->func;
	worker->current_pwq = pwq;
	work_color = get_work_color(work);

	/*
	 * Record wq name for cmdline and debug reporting, may get
	 * overridden through set_worker_desc().
	 */
	strscpy(worker->desc, pwq->wq->name, WORKER_DESC_LEN);

	list_del_init(&work->entry);

	/*
	 * CPU intensive works don't participate in concurrency management.
	 * They're the scheduler's responsibility.  This takes @worker out
	 * of concurrency management and the next code block will chain
	 * execution of the pending work items.
	 */
	if (unlikely(cpu_intensive))
		worker_set_flags(worker, WORKER_CPU_INTENSIVE);

	/*
	 * Wake up another worker if necessary.  The condition is always
	 * false for normal per-cpu workers since nr_running would always
	 * be >= 1 at this point.  This is used to chain execution of the
	 * pending work items for WORKER_NOT_RUNNING workers such as the
	 * UNBOUND and CPU_INTENSIVE ones.
	 */
	if (need_more_worker(pool))
		wake_up_worker(pool);

	/*
	 * Record the last pool and clear PENDING which should be the last
	 * update to @work.  Also, do this inside @pool->lock so that
	 * PENDING and queued state changes happen together while IRQ is
	 * disabled.
	 */
	set_work_pool_and_clear_pending(work, pool->id);

	raw_spin_unlock_irq(&pool->lock);

	lock_map_acquire(&pwq->wq->lockdep_map);
	lock_map_acquire(&lockdep_map);
	/*
	 * Strictly speaking we should mark the invariant state without holding
	 * any locks, that is, before these two lock_map_acquire()'s.
	 *
	 * However, that would result in:
	 *
	 *   A(W1)
	 *   WFC(C)
	 *		A(W1)
	 *		C(C)
	 *
	 * Which would create W1->C->W1 dependencies, even though there is no
	 * actual deadlock possible. There are two solutions, using a
	 * read-recursive acquire on the work(queue) 'locks', but this will then
	 * hit the lockdep limitation on recursive locks, or simply discard
	 * these locks.
	 *
	 * AFAICT there is no possible deadlock scenario between the
	 * flush_work() and complete() primitives (except for single-threaded
	 * workqueues), so hiding them isn't a problem.
	 */
	lockdep_invariant_state(true);
	trace_workqueue_execute_start(work);
	worker->current_func(work);
	/*
	 * While we must be careful to not use "work" after this, the trace
	 * point will only record its address.
	 */
	trace_workqueue_execute_end(work, worker->current_func);
	lock_map_release(&lockdep_map);
	lock_map_release(&pwq->wq->lockdep_map);

	if (unlikely(in_atomic() || lockdep_depth(current) > 0)) {
		pr_err("BUG: workqueue leaked lock or atomic: %s/0x%08x/%d\n"
		       "     last function: %ps\n",
		       current->comm, preempt_count(), task_pid_nr(current),
		       worker->current_func);
		debug_show_held_locks(current);
		dump_stack();
	}

	/*
	 * The following prevents a kworker from hogging CPU on !PREEMPTION
	 * kernels, where a requeueing work item waiting for something to
	 * happen could deadlock with stop_machine as such work item could
	 * indefinitely requeue itself while all other CPUs are trapped in
	 * stop_machine. At the same time, report a quiescent RCU state so
	 * the same condition doesn't freeze RCU.
	 */
	cond_resched();

	raw_spin_lock_irq(&pool->lock);

	/* clear cpu intensive status */
	if (unlikely(cpu_intensive))
		worker_clr_flags(worker, WORKER_CPU_INTENSIVE);

	/* tag the worker for identification in schedule() */
	worker->last_func = worker->current_func;

	/* we're done with it, release */
	hash_del(&worker->hentry);
	worker->current_work = NULL;
	worker->current_func = NULL;
	worker->current_pwq = NULL;
	pwq_dec_nr_in_flight(pwq, work_color);
}

/**
 * process_scheduled_works - process scheduled works
 * @worker: self
 *
 * Process all scheduled works.  Please note that the scheduled list
 * may change while processing a work, so this function repeatedly
 * fetches a work from the top and executes it.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.
 */
/**
 * process_scheduled_works - 依次处理worker->scheduled链表上所有work
 *
 * 被worker_thread()和rescuer_thread()调用，循环取出并执行
 * worker->scheduled链表中的work。scheduled链表存放rescuer接管
 * 的work或通过schedule_work_on()指定worker的work。
 */
static void process_scheduled_works(struct worker *worker)
{
	while (!list_empty(&worker->scheduled)) {
		struct work_struct *work = list_first_entry(&worker->scheduled,
						struct work_struct, entry);
		process_one_work(worker, work);
	}
}

/**
 * set_pf_worker - 设置或清除当前任务的PF_WQ_WORKER标志
 *
 * 在持有wq_pool_attach_mutex的情况下设置/清除task->flags中的
 * PF_WQ_WORKER标志，用于worker线程在启动和退出时标识自身身份，
 * 供调度器和lockdep正确处理工作线程的特殊语义。
 */
static void set_pf_worker(bool val)
{
	mutex_lock(&wq_pool_attach_mutex);
	if (val)
		current->flags |= PF_WQ_WORKER;
	else
		current->flags &= ~PF_WQ_WORKER;
	mutex_unlock(&wq_pool_attach_mutex);
}

/**
 * worker_thread - the worker thread function
 * @__worker: self
 *
 * The worker thread function.  All workers belong to a worker_pool -
 * either a per-cpu one or dynamic unbound one.  These workers process all
 * work items regardless of their specific target workqueue.  The only
 * exception is work items which belong to workqueues with a rescuer which
 * will be explained in rescuer_thread().
 *
 * Return: 0
 */
/**
 * worker_thread - 工作线程主循环（kworker内核线程）
 *
 * 每个worker_pool有一组工作线程运行此函数。主循环：
 * 1. 检查是否需要退出（WORKER_DIE标志）
 * 2. 若有待执行的work则调用process_one_work()处理
 * 3. 若无work则进入睡眠等待唤醒（worklist为空时）
 * 4. 实现动态并发控制（concurrency management）：保持池中
 *    恰好有一个活跃worker，新work到来时唤醒更多worker
 */
static int worker_thread(void *__worker)
{
	struct worker *worker = __worker;
	struct worker_pool *pool = worker->pool;

	/* tell the scheduler that this is a workqueue worker */
	set_pf_worker(true);
woke_up:
	raw_spin_lock_irq(&pool->lock);

	/* am I supposed to die? */
	if (unlikely(worker->flags & WORKER_DIE)) {
		raw_spin_unlock_irq(&pool->lock);
		WARN_ON_ONCE(!list_empty(&worker->entry));
		set_pf_worker(false);

		set_task_comm(worker->task, "kworker/dying");
		ida_simple_remove(&pool->worker_ida, worker->id);
		worker_detach_from_pool(worker);
		kfree(worker);
		return 0;
	}

	worker_leave_idle(worker);
recheck:
	/* no more worker necessary? */
	if (!need_more_worker(pool))
		goto sleep;

	/* do we need to manage? */
	if (unlikely(!may_start_working(pool)) && manage_workers(worker))
		goto recheck;

	/*
	 * ->scheduled list can only be filled while a worker is
	 * preparing to process a work or actually processing it.
	 * Make sure nobody diddled with it while I was sleeping.
	 */
	WARN_ON_ONCE(!list_empty(&worker->scheduled));

	/*
	 * Finish PREP stage.  We're guaranteed to have at least one idle
	 * worker or that someone else has already assumed the manager
	 * role.  This is where @worker starts participating in concurrency
	 * management if applicable and concurrency management is restored
	 * after being rebound.  See rebind_workers() for details.
	 */
	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);

	do {
		struct work_struct *work =
			list_first_entry(&pool->worklist,
					 struct work_struct, entry);

		pool->watchdog_ts = jiffies;

		if (likely(!(*work_data_bits(work) & WORK_STRUCT_LINKED))) {
			/* optimization path, not strictly necessary */
			process_one_work(worker, work);
			if (unlikely(!list_empty(&worker->scheduled)))
				process_scheduled_works(worker);
		} else {
			move_linked_works(work, &worker->scheduled, NULL);
			process_scheduled_works(worker);
		}
	} while (keep_working(pool));

	worker_set_flags(worker, WORKER_PREP);
sleep:
	/*
	 * pool->lock is held and there's no work to process and no need to
	 * manage, sleep.  Workers are woken up only while holding
	 * pool->lock or from local cpu, so setting the current state
	 * before releasing pool->lock is enough to prevent losing any
	 * event.
	 */
	worker_enter_idle(worker);
	__set_current_state(TASK_IDLE);
	raw_spin_unlock_irq(&pool->lock);
	schedule();
	goto woke_up;
}

/**
 * rescuer_thread - the rescuer thread function
 * @__rescuer: self
 *
 * Workqueue rescuer thread function.  There's one rescuer for each
 * workqueue which has WQ_MEM_RECLAIM set.
 *
 * Regular work processing on a pool may block trying to create a new
 * worker which uses GFP_KERNEL allocation which has slight chance of
 * developing into deadlock if some works currently on the same queue
 * need to be processed to satisfy the GFP_KERNEL allocation.  This is
 * the problem rescuer solves.
 *
 * When such condition is possible, the pool summons rescuers of all
 * workqueues which have works queued on the pool and let them process
 * those works so that forward progress can be guaranteed.
 *
 * This should happen rarely.
 *
 * Return: 0
 */
/**
 * rescuer_thread - WQ_MEM_RECLAIM工作队列的救援线程
 *
 * 当worker_pool内存不足无法创建新worker时，救援线程接管并执行
 * 该工作队列的待处理work item，防止内存回收路径发生死锁。
 * 以RESCUER_NICE_LEVEL（-20，最高优先级）运行，处理完成后
 * 回到睡眠等待下次救援请求。WQ_MEM_RECLAIM标志触发此机制。
 */
static int rescuer_thread(void *__rescuer)
{
	struct worker *rescuer = __rescuer;
	struct workqueue_struct *wq = rescuer->rescue_wq;
	struct list_head *scheduled = &rescuer->scheduled;
	bool should_stop;

	set_user_nice(current, RESCUER_NICE_LEVEL);

	/*
	 * Mark rescuer as worker too.  As WORKER_PREP is never cleared, it
	 * doesn't participate in concurrency management.
	 */
	set_pf_worker(true);
repeat:
	set_current_state(TASK_IDLE);

	/*
	 * By the time the rescuer is requested to stop, the workqueue
	 * shouldn't have any work pending, but @wq->maydays may still have
	 * pwq(s) queued.  This can happen by non-rescuer workers consuming
	 * all the work items before the rescuer got to them.  Go through
	 * @wq->maydays processing before acting on should_stop so that the
	 * list is always empty on exit.
	 */
	should_stop = kthread_should_stop();

	/* see whether any pwq is asking for help */
	raw_spin_lock_irq(&wq_mayday_lock);

	while (!list_empty(&wq->maydays)) {
		struct pool_workqueue *pwq = list_first_entry(&wq->maydays,
					struct pool_workqueue, mayday_node);
		struct worker_pool *pool = pwq->pool;
		struct work_struct *work, *n;
		bool first = true;

		__set_current_state(TASK_RUNNING);
		list_del_init(&pwq->mayday_node);

		raw_spin_unlock_irq(&wq_mayday_lock);

		worker_attach_to_pool(rescuer, pool);

		raw_spin_lock_irq(&pool->lock);

		/*
		 * Slurp in all works issued via this workqueue and
		 * process'em.
		 */
		WARN_ON_ONCE(!list_empty(scheduled));
		list_for_each_entry_safe(work, n, &pool->worklist, entry) {
			if (get_work_pwq(work) == pwq) {
				if (first)
					pool->watchdog_ts = jiffies;
				move_linked_works(work, scheduled, &n);
			}
			first = false;
		}

		if (!list_empty(scheduled)) {
			process_scheduled_works(rescuer);

			/*
			 * The above execution of rescued work items could
			 * have created more to rescue through
			 * pwq_activate_first_delayed() or chained
			 * queueing.  Let's put @pwq back on mayday list so
			 * that such back-to-back work items, which may be
			 * being used to relieve memory pressure, don't
			 * incur MAYDAY_INTERVAL delay inbetween.
			 */
			if (pwq->nr_active && need_to_create_worker(pool)) {
				raw_spin_lock(&wq_mayday_lock);
				/*
				 * Queue iff we aren't racing destruction
				 * and somebody else hasn't queued it already.
				 */
				if (wq->rescuer && list_empty(&pwq->mayday_node)) {
					get_pwq(pwq);
					list_add_tail(&pwq->mayday_node, &wq->maydays);
				}
				raw_spin_unlock(&wq_mayday_lock);
			}
		}

		/*
		 * Put the reference grabbed by send_mayday().  @pool won't
		 * go away while we're still attached to it.
		 */
		put_pwq(pwq);

		/*
		 * Leave this pool.  If need_more_worker() is %true, notify a
		 * regular worker; otherwise, we end up with 0 concurrency
		 * and stalling the execution.
		 */
		if (need_more_worker(pool))
			wake_up_worker(pool);

		raw_spin_unlock_irq(&pool->lock);

		worker_detach_from_pool(rescuer);

		raw_spin_lock_irq(&wq_mayday_lock);
	}

	raw_spin_unlock_irq(&wq_mayday_lock);

	if (should_stop) {
		__set_current_state(TASK_RUNNING);
		set_pf_worker(false);
		return 0;
	}

	/* rescuers should never participate in concurrency management */
	WARN_ON_ONCE(!(rescuer->flags & WORKER_NOT_RUNNING));
	schedule();
	goto repeat;
}

/**
 * check_flush_dependency - check for flush dependency sanity
 * @target_wq: workqueue being flushed
 * @target_work: work item being flushed (NULL for workqueue flushes)
 *
 * %current is trying to flush the whole @target_wq or @target_work on it.
 * If @target_wq doesn't have %WQ_MEM_RECLAIM, verify that %current is not
 * reclaiming memory or running on a workqueue which doesn't have
 * %WQ_MEM_RECLAIM as that can break forward-progress guarantee leading to
 * a deadlock.
 */
/**
 * check_flush_dependency - 检查flush依赖是否会导致死锁
 *
 * 若@target_wq未设WQ_MEM_RECLAIM，而调用者处于内存回收路径
 * （PF_MEMALLOC）或正在执行WQ_MEM_RECLAIM队列的work，则警告：
 * 这种flush可能破坏前向进度保证，导致死锁。
 */
static void check_flush_dependency(struct workqueue_struct *target_wq,
				   struct work_struct *target_work)
{
	work_func_t target_func = target_work ? target_work->func : NULL;
	struct worker *worker;

	if (target_wq->flags & WQ_MEM_RECLAIM)
		return;

	worker = current_wq_worker();

	WARN_ONCE(current->flags & PF_MEMALLOC,
		  "workqueue: PF_MEMALLOC task %d(%s) is flushing !WQ_MEM_RECLAIM %s:%ps",
		  current->pid, current->comm, target_wq->name, target_func);
	WARN_ONCE(worker && ((worker->current_pwq->wq->flags &
			      (WQ_MEM_RECLAIM | __WQ_LEGACY)) == WQ_MEM_RECLAIM),
		  "workqueue: WQ_MEM_RECLAIM %s:%ps is flushing !WQ_MEM_RECLAIM %s:%ps",
		  worker->current_pwq->wq->name, worker->current_func,
		  target_wq->name, target_func);
}

struct wq_barrier {
	struct work_struct	work;
	struct completion	done;
	struct task_struct	*task;	/* purely informational */
};

/**
 * wq_barrier_func - flush屏障work的执行函数
 *
 * 作为barrier work的回调，执行时仅调用complete(&barr->done)，
 * 通知等待在wq_barrier上的flush调用者该屏障点已越过，所有
 * 在屏障之前提交的work均已执行完毕。
 */
static void wq_barrier_func(struct work_struct *work)
{
	struct wq_barrier *barr = container_of(work, struct wq_barrier, work);
	complete(&barr->done);
}

/**
 * insert_wq_barrier - insert a barrier work
 * @pwq: pwq to insert barrier into
 * @barr: wq_barrier to insert
 * @target: target work to attach @barr to
 * @worker: worker currently executing @target, NULL if @target is not executing
 *
 * @barr is linked to @target such that @barr is completed only after
 * @target finishes execution.  Please note that the ordering
 * guarantee is observed only with respect to @target and on the local
 * cpu.
 *
 * Currently, a queued barrier can't be canceled.  This is because
 * try_to_grab_pending() can't determine whether the work to be
 * grabbed is at the head of the queue and thus can't clear LINKED
 * flag of the previous work while there must be a valid next work
 * after a work with LINKED flag set.
 *
 * Note that when @worker is non-NULL, @target may be modified
 * underneath us, so we can't reliably determine pwq from @target.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/**
 * insert_wq_barrier - 在work执行路径上插入flush屏障
 *
 * 初始化@barr并将其作为一个特殊work插入@pwq。屏障保证@target
 * 执行完毕后@barr->done才会被完成，从而实现flush_work()的
 * 精确等待语义。屏障一旦入队不可取消。
 */
static void insert_wq_barrier(struct pool_workqueue *pwq,
			      struct wq_barrier *barr,
			      struct work_struct *target, struct worker *worker)
{
	struct list_head *head;
	unsigned int linked = 0;

	/*
	 * debugobject calls are safe here even with pool->lock locked
	 * as we know for sure that this will not trigger any of the
	 * checks and call back into the fixup functions where we
	 * might deadlock.
	 */
	INIT_WORK_ONSTACK(&barr->work, wq_barrier_func);
	__set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&barr->work));

	init_completion_map(&barr->done, &target->lockdep_map);

	barr->task = current;

	/*
	 * If @target is currently being executed, schedule the
	 * barrier to the worker; otherwise, put it after @target.
	 */
	if (worker)
		head = worker->scheduled.next;
	else {
		unsigned long *bits = work_data_bits(target);

		head = target->entry.next;
		/* there can already be other linked works, inherit and set */
		linked = *bits & WORK_STRUCT_LINKED;
		__set_bit(WORK_STRUCT_LINKED_BIT, bits);
	}

	debug_work_activate(&barr->work);
	insert_work(pwq, &barr->work, head,
		    work_color_to_flags(WORK_NO_COLOR) | linked);
}

/**
 * flush_workqueue_prep_pwqs - prepare pwqs for workqueue flushing
 * @wq: workqueue being flushed
 * @flush_color: new flush color, < 0 for no-op
 * @work_color: new work color, < 0 for no-op
 *
 * Prepare pwqs for workqueue flushing.
 *
 * If @flush_color is non-negative, flush_color on all pwqs should be
 * -1.  If no pwq has in-flight commands at the specified color, all
 * pwq->flush_color's stay at -1 and %false is returned.  If any pwq
 * has in flight commands, its pwq->flush_color is set to
 * @flush_color, @wq->nr_pwqs_to_flush is updated accordingly, pwq
 * wakeup logic is armed and %true is returned.
 *
 * The caller should have initialized @wq->first_flusher prior to
 * calling this function with non-negative @flush_color.  If
 * @flush_color is negative, no flush color update is done and %false
 * is returned.
 *
 * If @work_color is non-negative, all pwqs should have the same
 * work_color which is previous to @work_color and all will be
 * advanced to @work_color.
 *
 * CONTEXT:
 * mutex_lock(wq->mutex).
 *
 * Return:
 * %true if @flush_color >= 0 and there's something to flush.  %false
 * otherwise.
 */
/**
 * flush_workqueue_prep_pwqs - 为flush准备所有pwq（设置颜色和计数）
 *
 * 对@wq的所有pwq设置flush_color和work_color：flush_color用于标记
 * 当前需要等待完成的work批次，work_color用于标记新提交work的颜色。
 * 若有需要等待的pwq则返回true，调用者需等待nr_pwqs_to_flush降为0。
 */
static bool flush_workqueue_prep_pwqs(struct workqueue_struct *wq,
				      int flush_color, int work_color)
{
	bool wait = false;
	struct pool_workqueue *pwq;

	if (flush_color >= 0) {
		WARN_ON_ONCE(atomic_read(&wq->nr_pwqs_to_flush));
		atomic_set(&wq->nr_pwqs_to_flush, 1);
	}

	for_each_pwq(pwq, wq) {
		struct worker_pool *pool = pwq->pool;

		raw_spin_lock_irq(&pool->lock);

		if (flush_color >= 0) {
			WARN_ON_ONCE(pwq->flush_color != -1);

			if (pwq->nr_in_flight[flush_color]) {
				pwq->flush_color = flush_color;
				atomic_inc(&wq->nr_pwqs_to_flush);
				wait = true;
			}
		}

		if (work_color >= 0) {
			WARN_ON_ONCE(work_color != work_next_color(pwq->work_color));
			pwq->work_color = work_color;
		}

		raw_spin_unlock_irq(&pool->lock);
	}

	if (flush_color >= 0 && atomic_dec_and_test(&wq->nr_pwqs_to_flush))
		complete(&wq->first_flusher->done);

	return wait;
}

/**
 * flush_workqueue - ensure that any scheduled work has run to completion.
 * @wq: workqueue to flush
 *
 * This function sleeps until all work items which were queued on entry
 * have finished execution, but it is not livelocked by new incoming ones.
 */
/**
 * flush_workqueue - 等待工作队列中所有已提交的work执行完毕
 *
 * 实现颜色翻转（color flip）机制：分配一个新的flush_color，
 * 等待所有先前提交的work（旧颜色）全部完成后才返回。
 * 若有多个并发flush则形成flush链，按提交顺序依次完成。
 * 此函数可睡眠，不能在中断上下文调用。
 */
void flush_workqueue(struct workqueue_struct *wq)
{
	struct wq_flusher this_flusher = {
		.list = LIST_HEAD_INIT(this_flusher.list),
		.flush_color = -1,
		.done = COMPLETION_INITIALIZER_ONSTACK_MAP(this_flusher.done, wq->lockdep_map),
	};
	int next_color;

	if (WARN_ON(!wq_online))
		return;

	lock_map_acquire(&wq->lockdep_map);
	lock_map_release(&wq->lockdep_map);

	mutex_lock(&wq->mutex);

	/*
	 * Start-to-wait phase
	 */
	next_color = work_next_color(wq->work_color);

	if (next_color != wq->flush_color) {
		/*
		 * Color space is not full.  The current work_color
		 * becomes our flush_color and work_color is advanced
		 * by one.
		 */
		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow));
		this_flusher.flush_color = wq->work_color;
		wq->work_color = next_color;

		if (!wq->first_flusher) {
			/* no flush in progress, become the first flusher */
			WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

			wq->first_flusher = &this_flusher;

			if (!flush_workqueue_prep_pwqs(wq, wq->flush_color,
						       wq->work_color)) {
				/* nothing to flush, done */
				wq->flush_color = next_color;
				wq->first_flusher = NULL;
				goto out_unlock;
			}
		} else {
			/* wait in queue */
			WARN_ON_ONCE(wq->flush_color == this_flusher.flush_color);
			list_add_tail(&this_flusher.list, &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}
	} else {
		/*
		 * Oops, color space is full, wait on overflow queue.
		 * The next flush completion will assign us
		 * flush_color and transfer to flusher_queue.
		 */
		list_add_tail(&this_flusher.list, &wq->flusher_overflow);
	}

	check_flush_dependency(wq, NULL);

	mutex_unlock(&wq->mutex);

	wait_for_completion(&this_flusher.done);

	/*
	 * Wake-up-and-cascade phase
	 *
	 * First flushers are responsible for cascading flushes and
	 * handling overflow.  Non-first flushers can simply return.
	 */
	if (READ_ONCE(wq->first_flusher) != &this_flusher)
		return;

	mutex_lock(&wq->mutex);

	/* we might have raced, check again with mutex held */
	if (wq->first_flusher != &this_flusher)
		goto out_unlock;

	WRITE_ONCE(wq->first_flusher, NULL);

	WARN_ON_ONCE(!list_empty(&this_flusher.list));
	WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

	while (true) {
		struct wq_flusher *next, *tmp;

		/* complete all the flushers sharing the current flush color */
		list_for_each_entry_safe(next, tmp, &wq->flusher_queue, list) {
			if (next->flush_color != wq->flush_color)
				break;
			list_del_init(&next->list);
			complete(&next->done);
		}

		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow) &&
			     wq->flush_color != work_next_color(wq->work_color));

		/* this flush_color is finished, advance by one */
		wq->flush_color = work_next_color(wq->flush_color);

		/* one color has been freed, handle overflow queue */
		if (!list_empty(&wq->flusher_overflow)) {
			/*
			 * Assign the same color to all overflowed
			 * flushers, advance work_color and append to
			 * flusher_queue.  This is the start-to-wait
			 * phase for these overflowed flushers.
			 */
			list_for_each_entry(tmp, &wq->flusher_overflow, list)
				tmp->flush_color = wq->work_color;

			wq->work_color = work_next_color(wq->work_color);

			list_splice_tail_init(&wq->flusher_overflow,
					      &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}

		if (list_empty(&wq->flusher_queue)) {
			WARN_ON_ONCE(wq->flush_color != wq->work_color);
			break;
		}

		/*
		 * Need to flush more colors.  Make the next flusher
		 * the new first flusher and arm pwqs.
		 */
		WARN_ON_ONCE(wq->flush_color == wq->work_color);
		WARN_ON_ONCE(wq->flush_color != next->flush_color);

		list_del_init(&next->list);
		wq->first_flusher = next;

		if (flush_workqueue_prep_pwqs(wq, wq->flush_color, -1))
			break;

		/*
		 * Meh... this color is already done, clear first
		 * flusher and repeat cascading.
		 */
		wq->first_flusher = NULL;
	}

out_unlock:
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL(flush_workqueue);

/**
 * drain_workqueue - drain a workqueue
 * @wq: workqueue to drain
 *
 * Wait until the workqueue becomes empty.  While draining is in progress,
 * only chain queueing is allowed.  IOW, only currently pending or running
 * work items on @wq can queue further work items on it.  @wq is flushed
 * repeatedly until it becomes empty.  The number of flushing is determined
 * by the depth of chaining and should be relatively short.  Whine if it
 * takes too long.
 */
/**
 * drain_workqueue - 等待工作队列完全排空
 *
 * 反复调用flush_workqueue()直到队列为空。排空期间设置__WQ_DRAINING
 * 标志，只允许链式入队（当前正在执行的work再次入队）。若超过
 * 10次flush仍未清空则发出警告，通常意味着存在深度链式依赖。
 */
void drain_workqueue(struct workqueue_struct *wq)
{
	unsigned int flush_cnt = 0;
	struct pool_workqueue *pwq;

	/*
	 * __queue_work() needs to test whether there are drainers, is much
	 * hotter than drain_workqueue() and already looks at @wq->flags.
	 * Use __WQ_DRAINING so that queue doesn't have to check nr_drainers.
	 */
	mutex_lock(&wq->mutex);
	if (!wq->nr_drainers++)
		wq->flags |= __WQ_DRAINING;
	mutex_unlock(&wq->mutex);
reflush:
	flush_workqueue(wq);

	mutex_lock(&wq->mutex);

	for_each_pwq(pwq, wq) {
		bool drained;

		raw_spin_lock_irq(&pwq->pool->lock);
		drained = !pwq->nr_active && list_empty(&pwq->delayed_works);
		raw_spin_unlock_irq(&pwq->pool->lock);

		if (drained)
			continue;

		if (++flush_cnt == 10 ||
		    (flush_cnt % 100 == 0 && flush_cnt <= 1000))
			pr_warn("workqueue %s: drain_workqueue() isn't complete after %u tries\n",
				wq->name, flush_cnt);

		mutex_unlock(&wq->mutex);
		goto reflush;
	}

	if (!--wq->nr_drainers)
		wq->flags &= ~__WQ_DRAINING;
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(drain_workqueue);

/**
 * start_flush_work - 为单个work启动flush等待
 *
 * 若@work正在执行或在队列中，则向其所在pwq插入一个wq_barrier，
 * 调用者可wait_for_completion(&barr->done)等待该work完成。
 * 返回true表示已插入屏障，false表示work已完成无需等待。
 */
static bool start_flush_work(struct work_struct *work, struct wq_barrier *barr,
			     bool from_cancel)
{
	struct worker *worker = NULL;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	might_sleep();

	rcu_read_lock();
	pool = get_work_pool(work);
	if (!pool) {
		rcu_read_unlock();
		return false;
	}

	raw_spin_lock_irq(&pool->lock);
	/* see the comment in try_to_grab_pending() with the same code */
	pwq = get_work_pwq(work);
	if (pwq) {
		if (unlikely(pwq->pool != pool))
			goto already_gone;
	} else {
		worker = find_worker_executing_work(pool, work);
		if (!worker)
			goto already_gone;
		pwq = worker->current_pwq;
	}

	check_flush_dependency(pwq->wq, work);

	insert_wq_barrier(pwq, barr, work, worker);
	raw_spin_unlock_irq(&pool->lock);

	/*
	 * Force a lock recursion deadlock when using flush_work() inside a
	 * single-threaded or rescuer equipped workqueue.
	 *
	 * For single threaded workqueues the deadlock happens when the work
	 * is after the work issuing the flush_work(). For rescuer equipped
	 * workqueues the deadlock happens when the rescuer stalls, blocking
	 * forward progress.
	 */
	if (!from_cancel &&
	    (pwq->wq->saved_max_active == 1 || pwq->wq->rescuer)) {
		lock_map_acquire(&pwq->wq->lockdep_map);
		lock_map_release(&pwq->wq->lockdep_map);
	}
	rcu_read_unlock();
	return true;
already_gone:
	raw_spin_unlock_irq(&pool->lock);
	rcu_read_unlock();
	return false;
}

/**
 * __flush_work - 等待单个work执行完毕（内部实现）
 *
 * 调用start_flush_work()插入barrier并阻塞等待完成。
 * @from_cancel为true时跳过lockdep检查（来自cancel路径）。
 * 返回true表示成功等待，false表示work未在执行中。
 */
static bool __flush_work(struct work_struct *work, bool from_cancel)
{
	struct wq_barrier barr;

	if (WARN_ON(!wq_online))
		return false;

	if (WARN_ON(!work->func))
		return false;

	if (!from_cancel) {
		lock_map_acquire(&work->lockdep_map);
		lock_map_release(&work->lockdep_map);
	}

	if (start_flush_work(work, &barr, from_cancel)) {
		wait_for_completion(&barr.done);
		destroy_work_on_stack(&barr.work);
		return true;
	} else {
		return false;
	}
}

/**
 * flush_work - wait for a work to finish executing the last queueing instance
 * @work: the work to flush
 *
 * Wait until @work has finished execution.  @work is guaranteed to be idle
 * on return if it hasn't been requeued since flush started.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/**
 * flush_work - 等待work最近一次入队的执行完成
 *
 * 阻塞等待@work完成当前执行。若work在flush启动后未重新入队，
 * 则返回时保证work处于idle状态。通过insert_wq_barrier()实现
 * 精确同步。返回true表示等待了执行，false表示work已是idle。
 */
bool flush_work(struct work_struct *work)
{
	return __flush_work(work, false);
}
EXPORT_SYMBOL_GPL(flush_work);

struct cwt_wait {
	wait_queue_entry_t		wait;
	struct work_struct	*work;
};

/**
 * cwt_wakefn - cancel_work_timer的等待队列唤醒函数
 *
 * 自定义唤醒函数，仅在key（正在被取消的work指针）与等待项
 * 记录的work匹配时才唤醒。防止不相关的work完成错误唤醒
 * cancel_work_sync()的等待者。
 */
static int cwt_wakefn(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
	struct cwt_wait *cwait = container_of(wait, struct cwt_wait, wait);

	if (cwait->work != key)
		return 0;
	return autoremove_wake_function(wait, mode, sync, key);
}

static bool __cancel_work_timer(struct work_struct *work, bool is_dwork)
{
	static DECLARE_WAIT_QUEUE_HEAD(cancel_waitq);
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(work, is_dwork, &flags);
		/*
		 * If someone else is already canceling, wait for it to
		 * finish.  flush_work() doesn't work for PREEMPT_NONE
		 * because we may get scheduled between @work's completion
		 * and the other canceling task resuming and clearing
		 * CANCELING - flush_work() will return false immediately
		 * as @work is no longer busy, try_to_grab_pending() will
		 * return -ENOENT as @work is still being canceled and the
		 * other canceling task won't be able to clear CANCELING as
		 * we're hogging the CPU.
		 *
		 * Let's wait for completion using a waitqueue.  As this
		 * may lead to the thundering herd problem, use a custom
		 * wake function which matches @work along with exclusive
		 * wait and wakeup.
		 */
		if (unlikely(ret == -ENOENT)) {
			struct cwt_wait cwait;

			init_wait(&cwait.wait);
			cwait.wait.func = cwt_wakefn;
			cwait.work = work;

			prepare_to_wait_exclusive(&cancel_waitq, &cwait.wait,
						  TASK_UNINTERRUPTIBLE);
			if (work_is_canceling(work))
				schedule();
			finish_wait(&cancel_waitq, &cwait.wait);
		}
	} while (unlikely(ret < 0));

	/* tell other tasks trying to grab @work to back off */
	mark_work_canceling(work);
	local_irq_restore(flags);

	/*
	 * This allows canceling during early boot.  We know that @work
	 * isn't executing.
	 */
	if (wq_online)
		__flush_work(work, true);

	clear_work_data(work);

	/*
	 * Paired with prepare_to_wait() above so that either
	 * waitqueue_active() is visible here or !work_is_canceling() is
	 * visible there.
	 */
	smp_mb();
	if (waitqueue_active(&cancel_waitq))
		__wake_up(&cancel_waitq, TASK_NORMAL, 1, work);

	return ret;
}

/**
 * cancel_work_sync - cancel a work and wait for it to finish
 * @work: the work to cancel
 *
 * Cancel @work and wait for its execution to finish.  This function
 * can be used even if the work re-queues itself or migrates to
 * another workqueue.  On return from this function, @work is
 * guaranteed to be not pending or executing on any CPU.
 *
 * cancel_work_sync(&delayed_work->work) must not be used for
 * delayed_work's.  Use cancel_delayed_work_sync() instead.
 *
 * The caller must ensure that the workqueue on which @work was last
 * queued can't be destroyed before this function returns.
 *
 * Return:
 * %true if @work was pending, %false otherwise.
 */
/**
 * cancel_work_sync - 取消work并等待其执行完毕
 *
 * 取消@work并阻塞等待其结束（包括自我重新入队或迁移到其他
 * workqueue的情况）。返回时保证work既不在pending也不在执行中。
 * 不可用于delayed_work，应使用cancel_delayed_work_sync()。
 */
bool cancel_work_sync(struct work_struct *work)
{
	return __cancel_work_timer(work, false);
}
EXPORT_SYMBOL_GPL(cancel_work_sync);

/**
 * flush_delayed_work - wait for a dwork to finish executing the last queueing
 * @dwork: the delayed work to flush
 *
 * Delayed timer is cancelled and the pending work is queued for
 * immediate execution.  Like flush_work(), this function only
 * considers the last queueing instance of @dwork.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/**
 * flush_delayed_work - 立即执行并等待delayed_work完成
 *
 * 若@dwork定时器尚在计时，取消定时器并立即入队执行；然后
 * flush_work()等待执行完毕。只等待最后一次入队实例。
 * 返回true表示等待了执行，false表示work已是idle。
 */
bool flush_delayed_work(struct delayed_work *dwork)
{
	local_irq_disable();
	if (del_timer_sync(&dwork->timer))
		__queue_work(dwork->cpu, dwork->wq, &dwork->work);
	local_irq_enable();
	return flush_work(&dwork->work);
}
EXPORT_SYMBOL(flush_delayed_work);

/**
 * flush_rcu_work - wait for a rwork to finish executing the last queueing
 * @rwork: the rcu work to flush
 *
 * Return:
 * %true if flush_rcu_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/**
 * flush_rcu_work - 等待RCU work执行完成
 *
 * 若@rwork仍在pending（等待RCU宽限期），先调用rcu_barrier()
 * 等待所有RCU回调完成，再flush_work()等待work执行。
 * 返回true表示等待发生，false表示work已完成。
 */
bool flush_rcu_work(struct rcu_work *rwork)
{
	if (test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&rwork->work))) {
		rcu_barrier();
		flush_work(&rwork->work);
		return true;
	} else {
		return flush_work(&rwork->work);
	}
}
EXPORT_SYMBOL(flush_rcu_work);

/**
 * __cancel_work - 取消work（不等待执行完毕）
 *
 * 尝试抢占work的PENDING位并从worklist移除。对delayed_work
 * 还会先取消定时器。返回true表示work原本在pending状态。
 * 不保证work的回调已经执行完毕，需要sync版本才能等待。
 */
static bool __cancel_work(struct work_struct *work, bool is_dwork)
{
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(work, is_dwork, &flags);
	} while (unlikely(ret == -EAGAIN));

	if (unlikely(ret < 0))
		return false;

	set_work_pool_and_clear_pending(work, get_work_pool_id(work));
	local_irq_restore(flags);
	return ret;
}

/**
 * cancel_delayed_work - cancel a delayed work
 * @dwork: delayed_work to cancel
 *
 * Kill off a pending delayed_work.
 *
 * Return: %true if @dwork was pending and canceled; %false if it wasn't
 * pending.
 *
 * Note:
 * The work callback function may still be running on return, unless
 * it returns %true and the work doesn't re-arm itself.  Explicitly flush or
 * use cancel_delayed_work_sync() to wait on it.
 *
 * This function is safe to call from any context including IRQ handler.
 */
/**
 * cancel_delayed_work - 取消延迟work（不等待）
 *
 * 取消@dwork定时器并从worklist移除。回调可能仍在运行，
 * 不保证完全结束。可在任意上下文（包括IRQ）中安全调用。
 * 若需等待回调完成，使用cancel_delayed_work_sync()。
 */
bool cancel_delayed_work(struct delayed_work *dwork)
{
	return __cancel_work(&dwork->work, true);
}
EXPORT_SYMBOL(cancel_delayed_work);

/**
 * cancel_delayed_work_sync - cancel a delayed work and wait for it to finish
 * @dwork: the delayed work cancel
 *
 * This is cancel_work_sync() for delayed works.
 *
 * Return:
 * %true if @dwork was pending, %false otherwise.
 */
/**
 * cancel_delayed_work_sync - 取消延迟work并等待执行完毕
 *
 * cancel_work_sync()的delayed_work版本。取消@dwork并阻塞等待
 * 其回调执行结束。返回true表示work原本在pending状态。
 */
bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_timer(&dwork->work, true);
}
EXPORT_SYMBOL(cancel_delayed_work_sync);

/**
 * schedule_on_each_cpu - execute a function synchronously on each online CPU
 * @func: the function to call
 *
 * schedule_on_each_cpu() executes @func on each online CPU using the
 * system workqueue and blocks until all CPUs have completed.
 * schedule_on_each_cpu() is very slow.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/**
 * schedule_on_each_cpu - 在每个CPU上调度执行一个工作函数
 *
 * 为每个online CPU分配一个per-cpu work_struct，提交到system_wq
 * 并等待所有CPU执行完毕。用于需要在所有CPU上执行某操作的场景，
 * 如刷新per-cpu缓存。注意此函数非常慢，不应在热路径中使用。
 */
int schedule_on_each_cpu(work_func_t func)
{
	int cpu;
	struct work_struct __percpu *works;

	works = alloc_percpu(struct work_struct);
	if (!works)
		return -ENOMEM;

	get_online_cpus();

	for_each_online_cpu(cpu) {
		struct work_struct *work = per_cpu_ptr(works, cpu);

		INIT_WORK(work, func);
		schedule_work_on(cpu, work);
	}

	for_each_online_cpu(cpu)
		flush_work(per_cpu_ptr(works, cpu));

	put_online_cpus();
	free_percpu(works);
	return 0;
}

/**
 * execute_in_process_context - reliably execute the routine with user context
 * @fn:		the function to execute
 * @ew:		guaranteed storage for the execute work structure (must
 *		be available when the work executes)
 *
 * Executes the function immediately if process context is available,
 * otherwise schedules the function for delayed execution.
 *
 * Return:	0 - function was executed
 *		1 - function was scheduled for execution
 */
/**
 * execute_in_process_context - 在进程上下文中执行函数
 *
 * 若当前不在中断上下文，直接调用@fn；否则将其包装为work
 * 提交到system_wq异步执行。用于需要在进程上下文中运行但
 * 可能被中断调用的函数。返回0表示直接执行，1表示异步提交。
 */
int execute_in_process_context(work_func_t fn, struct execute_work *ew)
{
	if (!in_interrupt()) {
		fn(&ew->work);
		return 0;
	}

	INIT_WORK(&ew->work, fn);
	schedule_work(&ew->work);

	return 1;
}
EXPORT_SYMBOL_GPL(execute_in_process_context);

/**
 * free_workqueue_attrs - free a workqueue_attrs
 * @attrs: workqueue_attrs to free
 *
 * Undo alloc_workqueue_attrs().
 */
/**
 * free_workqueue_attrs - 释放workqueue_attrs结构
 *
 * 释放@attrs及其内嵌的cpumask，与alloc_workqueue_attrs()配对。
 * 安全处理NULL指针。
 */
void free_workqueue_attrs(struct workqueue_attrs *attrs)
{
	if (attrs) {
		free_cpumask_var(attrs->cpumask);
		kfree(attrs);
	}
}

/**
 * alloc_workqueue_attrs - allocate a workqueue_attrs
 *
 * Allocate a new workqueue_attrs, initialize with default settings and
 * return it.
 *
 * Return: The allocated new workqueue_attr on success. %NULL on failure.
 */
/**
 * alloc_workqueue_attrs - 分配并初始化workqueue_attrs
 *
 * 分配新的workqueue_attrs结构，初始化nice为0，cpumask为全集。
 * 用于创建unbound工作队列时指定属性（nice值、允许的CPU集合）。
 * 返回NULL表示内存分配失败。
 */
struct workqueue_attrs *alloc_workqueue_attrs(void)
{
	struct workqueue_attrs *attrs;

	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		goto fail;
	if (!alloc_cpumask_var(&attrs->cpumask, GFP_KERNEL))
		goto fail;

	cpumask_copy(attrs->cpumask, cpu_possible_mask);
	return attrs;
fail:
	free_workqueue_attrs(attrs);
	return NULL;
}

/**
 * copy_workqueue_attrs - 拷贝workqueue_attrs
 *
 * 将@from的属性（nice、cpumask）完整拷贝到@to，包括
 * no_numa标志。注意与哈希/相等性测试不同，此函数不忽略
 * no_numa字段，保留完整属性信息。
 */
static void copy_workqueue_attrs(struct workqueue_attrs *to,
				 const struct workqueue_attrs *from)
{
	to->nice = from->nice;
	cpumask_copy(to->cpumask, from->cpumask);
	/*
	 * Unlike hash and equality test, this function doesn't ignore
	 * ->no_numa as it is used for both pool and wq attrs.  Instead,
	 * get_unbound_pool() explicitly clears ->no_numa after copying.
	 */
	to->no_numa = from->no_numa;
}

/* hash value of the content of @attr */
/**
 * wqattrs_hash - 计算workqueue_attrs哈希值
 *
 * 对nice值和cpumask的位图进行哈希，用于在wq_unbound_pool_hash
 * 中快速查找匹配的unbound worker_pool，实现相同属性的pool复用。
 */
static u32 wqattrs_hash(const struct workqueue_attrs *attrs)
{
	u32 hash = 0;

	hash = jhash_1word(attrs->nice, hash);
	hash = jhash(cpumask_bits(attrs->cpumask),
		     BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
	return hash;
}

/**
 * wqattrs_equal - 比较两个workqueue_attrs是否等价
 *
 * 比较nice值和cpumask（忽略no_numa）。用于判断现有unbound
 * worker_pool是否可复用，避免为属性相同的工作队列创建重复pool。
 */
/* content equality test */
static bool wqattrs_equal(const struct workqueue_attrs *a,
			  const struct workqueue_attrs *b)
{
	if (a->nice != b->nice)
		return false;
	if (!cpumask_equal(a->cpumask, b->cpumask))
		return false;
	return true;
}

/**
 * init_worker_pool - initialize a newly zalloc'd worker_pool
 * @pool: worker_pool to initialize
 *
 * Initialize a newly zalloc'd @pool.  It also allocates @pool->attrs.
 *
 * Return: 0 on success, -errno on failure.  Even on failure, all fields
 * inside @pool proper are initialized and put_unbound_pool() can be called
 * on @pool safely to release it.
 */
/**
 * init_worker_pool - 初始化新分配的worker_pool结构
 *
 * 初始化@pool的自旋锁、链表、哈希表、定时器、NUMA节点、IDA等
 * 字段，并分配pool->attrs。即使初始化失败，pool内部字段已被
 * 正确初始化，可安全调用put_unbound_pool()释放。
 */
static int init_worker_pool(struct worker_pool *pool)
{
	raw_spin_lock_init(&pool->lock);
	pool->id = -1;
	pool->cpu = -1;
	pool->node = NUMA_NO_NODE;
	pool->flags |= POOL_DISASSOCIATED;
	pool->watchdog_ts = jiffies;
	INIT_LIST_HEAD(&pool->worklist);
	INIT_LIST_HEAD(&pool->idle_list);
	hash_init(pool->busy_hash);

	timer_setup(&pool->idle_timer, idle_worker_timeout, TIMER_DEFERRABLE);

	timer_setup(&pool->mayday_timer, pool_mayday_timeout, 0);

	INIT_LIST_HEAD(&pool->workers);

	ida_init(&pool->worker_ida);
	INIT_HLIST_NODE(&pool->hash_node);
	pool->refcnt = 1;

	/* shouldn't fail above this point */
	pool->attrs = alloc_workqueue_attrs();
	if (!pool->attrs)
		return -ENOMEM;
	return 0;
}

/**
 * wq_init_lockdep - 初始化工作队列的lockdep映射
 * @wq: 目标工作队列
 *
 * 为工作队列注册独立的lockdep key，并以"(wq_completion)wq_name"格式
 * 命名lockdep map，用于检测flush_workqueue()的死锁依赖。
 */
#ifdef CONFIG_LOCKDEP
static void wq_init_lockdep(struct workqueue_struct *wq)
{
	char *lock_name;

	lockdep_register_key(&wq->key);
	lock_name = kasprintf(GFP_KERNEL, "%s%s", "(wq_completion)", wq->name);
	if (!lock_name)
		lock_name = wq->name;

	wq->lock_name = lock_name;
	lockdep_init_map(&wq->lockdep_map, lock_name, &wq->key, 0);
}

/**
 * wq_unregister_lockdep - 注销工作队列的lockdep key
 * @wq: 目标工作队列
 *
 * 在destroy_workqueue()时调用，释放wq的专属lockdep key。
 */
static void wq_unregister_lockdep(struct workqueue_struct *wq)
{
	lockdep_unregister_key(&wq->key);
}

/**
 * wq_free_lockdep - 释放lockdep使用的lock_name字符串
 * @wq: 目标工作队列
 *
 * 若lock_name是单独分配的（非直接指向wq->name），则kfree释放。
 */
static void wq_free_lockdep(struct workqueue_struct *wq)
{
	if (wq->lock_name != wq->name)
		kfree(wq->lock_name);
}
#else
static void wq_init_lockdep(struct workqueue_struct *wq)
{
}

static void wq_unregister_lockdep(struct workqueue_struct *wq)
{
}

static void wq_free_lockdep(struct workqueue_struct *wq)
{
}
#endif

/**
 * rcu_free_wq - RCU延迟释放workqueue_struct
 *
 * destroy_workqueue()通过call_rcu()注册的回调，在RCU宽限期后
 * 执行：释放lockdep map，释放per-cpu pwqs或unbound_attrs，
 * 最后kfree整个wq结构。
 */
static void rcu_free_wq(struct rcu_head *rcu)
{
	struct workqueue_struct *wq =
		container_of(rcu, struct workqueue_struct, rcu);

	wq_free_lockdep(wq);

	if (!(wq->flags & WQ_UNBOUND))
		free_percpu(wq->cpu_pwqs);
	else
		free_workqueue_attrs(wq->unbound_attrs);

	kfree(wq);
}

/**
 * rcu_free_pool - RCU延迟释放worker_pool
 *
 * RCU回调，在所有RCU读者退出后，销毁pool->worker_ida，
 * 释放pool->attrs，最后kfree整个pool结构。
 */
static void rcu_free_pool(struct rcu_head *rcu)
{
	struct worker_pool *pool = container_of(rcu, struct worker_pool, rcu);

	ida_destroy(&pool->worker_ida);
	free_workqueue_attrs(pool->attrs);
	kfree(pool);
}

/**
 * wq_manager_inactive - 等待pool manager退出并持有lock
 *
 * 若pool当前没有活跃manager（POOL_MANAGER_ACTIVE未设），
 * 获取pool->lock并返回true；否则释放lock并返回false。
 * 用于put_unbound_pool()在销毁pool前确保无manager在运行。
 */
/* This returns with the lock held on success (pool manager is inactive). */
static bool wq_manager_inactive(struct worker_pool *pool)
{
	raw_spin_lock_irq(&pool->lock);

	if (pool->flags & POOL_MANAGER_ACTIVE) {
		raw_spin_unlock_irq(&pool->lock);
		return false;
	}
	return true;
}

/**
 * put_unbound_pool - put a worker_pool
 * @pool: worker_pool to put
 *
 * Put @pool.  If its refcnt reaches zero, it gets destroyed in RCU
 * safe manner.  get_unbound_pool() calls this function on its failure path
 * and this function should be able to release pools which went through,
 * successfully or not, init_worker_pool().
 *
 * Should be called with wq_pool_mutex held.
 */
/**
 * put_unbound_pool - 释放unbound worker_pool引用
 *
 * 对@pool引用计数减一，若降至0则销毁：先驱逐所有idle worker，
 * 等待detach完成，从哈希表移除，最后通过call_rcu()延迟释放
 * pool结构。需持有wq_pool_mutex。
 */
static void put_unbound_pool(struct worker_pool *pool)
{
	DECLARE_COMPLETION_ONSTACK(detach_completion);
	struct worker *worker;

	lockdep_assert_held(&wq_pool_mutex);

	if (--pool->refcnt)
		return;

	/* sanity checks */
	if (WARN_ON(!(pool->cpu < 0)) ||
	    WARN_ON(!list_empty(&pool->worklist)))
		return;

	/* release id and unhash */
	if (pool->id >= 0)
		idr_remove(&worker_pool_idr, pool->id);
	hash_del(&pool->hash_node);

	/*
	 * Become the manager and destroy all workers.  This prevents
	 * @pool's workers from blocking on attach_mutex.  We're the last
	 * manager and @pool gets freed with the flag set.
	 * Because of how wq_manager_inactive() works, we will hold the
	 * spinlock after a successful wait.
	 */
	rcuwait_wait_event(&manager_wait, wq_manager_inactive(pool),
			   TASK_UNINTERRUPTIBLE);
	pool->flags |= POOL_MANAGER_ACTIVE;

	while ((worker = first_idle_worker(pool)))
		destroy_worker(worker);
	WARN_ON(pool->nr_workers || pool->nr_idle);
	raw_spin_unlock_irq(&pool->lock);

	mutex_lock(&wq_pool_attach_mutex);
	if (!list_empty(&pool->workers))
		pool->detach_completion = &detach_completion;
	mutex_unlock(&wq_pool_attach_mutex);

	if (pool->detach_completion)
		wait_for_completion(pool->detach_completion);

	/* shut down the timers */
	del_timer_sync(&pool->idle_timer);
	del_timer_sync(&pool->mayday_timer);

	/* RCU protected to allow dereferences from get_work_pool() */
	call_rcu(&pool->rcu, rcu_free_pool);
}

/**
 * get_unbound_pool - get a worker_pool with the specified attributes
 * @attrs: the attributes of the worker_pool to get
 *
 * Obtain a worker_pool which has the same attributes as @attrs, bump the
 * reference count and return it.  If there already is a matching
 * worker_pool, it will be used; otherwise, this function attempts to
 * create a new one.
 *
 * Should be called with wq_pool_mutex held.
 *
 * Return: On success, a worker_pool with the same attributes as @attrs.
 * On failure, %NULL.
 */
/**
 * get_unbound_pool - 获取或创建匹配属性的unbound worker_pool
 *
 * 在wq_unbound_pool_hash中按@attrs哈希查找已有pool，找到则
 * 递增引用计数返回；否则创建新pool，绑定对应NUMA节点，插入
 * 哈希表并启动pool管理。需持有wq_pool_mutex。
 */
static struct worker_pool *get_unbound_pool(const struct workqueue_attrs *attrs)
{
	u32 hash = wqattrs_hash(attrs);
	struct worker_pool *pool;
	int node;
	int target_node = NUMA_NO_NODE;

	lockdep_assert_held(&wq_pool_mutex);

	/* do we already have a matching pool? */
	hash_for_each_possible(unbound_pool_hash, pool, hash_node, hash) {
		if (wqattrs_equal(pool->attrs, attrs)) {
			pool->refcnt++;
			return pool;
		}
	}

	/* if cpumask is contained inside a NUMA node, we belong to that node */
	if (wq_numa_enabled) {
		for_each_node(node) {
			if (cpumask_subset(attrs->cpumask,
					   wq_numa_possible_cpumask[node])) {
				target_node = node;
				break;
			}
		}
	}

	/* nope, create a new one */
	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, target_node);
	if (!pool || init_worker_pool(pool) < 0)
		goto fail;

	lockdep_set_subclass(&pool->lock, 1);	/* see put_pwq() */
	copy_workqueue_attrs(pool->attrs, attrs);
	pool->node = target_node;

	/*
	 * no_numa isn't a worker_pool attribute, always clear it.  See
	 * 'struct workqueue_attrs' comments for detail.
	 */
	pool->attrs->no_numa = false;

	if (worker_pool_assign_id(pool) < 0)
		goto fail;

	/* create and start the initial worker */
	if (wq_online && !create_worker(pool))
		goto fail;

	/* install */
	hash_add(unbound_pool_hash, &pool->hash_node, hash);

	return pool;
fail:
	if (pool)
		put_unbound_pool(pool);
	return NULL;
}

/**
 * rcu_free_pwq - RCU延迟释放pool_workqueue结构
 *
 * pwq_unbound_release_workfn()通过call_rcu()注册的回调，
 * 在所有RCU读者退出后直接kfree @pwq结构。
 */
static void rcu_free_pwq(struct rcu_head *rcu)
{
	kmem_cache_free(pwq_cache,
			container_of(rcu, struct pool_workqueue, rcu));
}

/*
 * Scheduled on system_wq by put_pwq() when an unbound pwq hits zero refcnt
 * and needs to be destroyed.
 */
/**
 * pwq_unbound_release_workfn - 异步释放unbound pwq的工作函数
 *
 * put_pwq()在unbound pwq引用计数降为0时调度此函数到system_wq。
 * 从wq和pool解绑pwq，调用put_unbound_pool()，然后通过
 * call_rcu()延迟释放pwq结构。若这是最后一个pwq则同时释放wq。
 */
static void pwq_unbound_release_workfn(struct work_struct *work)
{
	struct pool_workqueue *pwq = container_of(work, struct pool_workqueue,
						  unbound_release_work);
	struct workqueue_struct *wq = pwq->wq;
	struct worker_pool *pool = pwq->pool;
	bool is_last;

	if (WARN_ON_ONCE(!(wq->flags & WQ_UNBOUND)))
		return;

	mutex_lock(&wq->mutex);
	list_del_rcu(&pwq->pwqs_node);
	is_last = list_empty(&wq->pwqs);
	mutex_unlock(&wq->mutex);

	mutex_lock(&wq_pool_mutex);
	put_unbound_pool(pool);
	mutex_unlock(&wq_pool_mutex);

	call_rcu(&pwq->rcu, rcu_free_pwq);

	/*
	 * If we're the last pwq going away, @wq is already dead and no one
	 * is gonna access it anymore.  Schedule RCU free.
	 */
	if (is_last) {
		wq_unregister_lockdep(wq);
		call_rcu(&wq->rcu, rcu_free_wq);
	}
}

/**
 * pwq_adjust_max_active - update a pwq's max_active to the current setting
 * @pwq: target pool_workqueue
 *
 * If @pwq isn't freezing, set @pwq->max_active to the associated
 * workqueue's saved_max_active and activate delayed work items
 * accordingly.  If @pwq is freezing, clear @pwq->max_active to zero.
 */
/**
 * pwq_adjust_max_active - 更新pwq的max_active为当前设置值
 *
 * 若队列正在冻结（WQ_FREEZABLE且系统frozen），将max_active
 * 置0阻止新work执行；解冻后恢复saved_max_active并激活
 * 所有积压的delayed_works，恢复正常并发处理能力。
 */
static void pwq_adjust_max_active(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;
	bool freezable = wq->flags & WQ_FREEZABLE;
	unsigned long flags;

	/* for @wq->saved_max_active */
	lockdep_assert_held(&wq->mutex);

	/* fast exit for non-freezable wqs */
	if (!freezable && pwq->max_active == wq->saved_max_active)
		return;

	/* this function can be called during early boot w/ irq disabled */
	raw_spin_lock_irqsave(&pwq->pool->lock, flags);

	/*
	 * During [un]freezing, the caller is responsible for ensuring that
	 * this function is called at least once after @workqueue_freezing
	 * is updated and visible.
	 */
	if (!freezable || !workqueue_freezing) {
		pwq->max_active = wq->saved_max_active;

		while (!list_empty(&pwq->delayed_works) &&
		       pwq->nr_active < pwq->max_active)
			pwq_activate_first_delayed(pwq);

		/*
		 * Need to kick a worker after thawed or an unbound wq's
		 * max_active is bumped.  It's a slow path.  Do it always.
		 */
		wake_up_worker(pwq->pool);
	} else {
		pwq->max_active = 0;
	}

	raw_spin_unlock_irqrestore(&pwq->pool->lock, flags);
}

/**
 * init_pwq - 初始化新分配的pool_workqueue结构
 *
 * 将@pwq与@wq和@pool关联，初始化nr_in_flight数组、nr_active、
 * max_active（来自wq->saved_max_active）、delayed_works链表
 * 和unbound_release_work。是创建pwq的第一步。
 */
/* initialize newly alloced @pwq which is associated with @wq and @pool */
static void init_pwq(struct pool_workqueue *pwq, struct workqueue_struct *wq,
		     struct worker_pool *pool)
{
	BUG_ON((unsigned long)pwq & WORK_STRUCT_FLAG_MASK);

	memset(pwq, 0, sizeof(*pwq));

	pwq->pool = pool;
	pwq->wq = wq;
	pwq->flush_color = -1;
	pwq->refcnt = 1;
	INIT_LIST_HEAD(&pwq->delayed_works);
	INIT_LIST_HEAD(&pwq->pwqs_node);
	INIT_LIST_HEAD(&pwq->mayday_node);
	INIT_WORK(&pwq->unbound_release_work, pwq_unbound_release_workfn);
}

/**
 * link_pwq - 将pwq同步到wq当前状态并链入wq数据结构
 *
 * 在持有wq->mutex时调用：将@pwq加入wq->pwqs链表，调用
 * pwq_adjust_max_active()同步max_active。可被多次调用，
 * 已链接时直接返回。
 */
/* sync @pwq with the current state of its associated wq and link it */
static void link_pwq(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq->mutex);

	/* may be called multiple times, ignore if already linked */
	if (!list_empty(&pwq->pwqs_node))
		return;

	/* set the matching work_color */
	pwq->work_color = wq->work_color;

	/* sync max_active to the current setting */
	pwq_adjust_max_active(pwq);

	/* link in @pwq */
	list_add_rcu(&pwq->pwqs_node, &wq->pwqs);
}

/**
 * alloc_unbound_pwq - 获取匹配pool并创建新的unbound pwq
 *
 * 调用get_unbound_pool()获取与@attrs匹配的worker_pool，
 * 分配pwq并调用init_pwq()初始化。需持有wq_pool_mutex。
 * 失败时put_unbound_pool()并返回NULL。
 */
/* obtain a pool matching @attr and create a pwq associating the pool and @wq */
static struct pool_workqueue *alloc_unbound_pwq(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	lockdep_assert_held(&wq_pool_mutex);

	pool = get_unbound_pool(attrs);
	if (!pool)
		return NULL;

	pwq = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL, pool->node);
	if (!pwq) {
		put_unbound_pool(pool);
		return NULL;
	}

	init_pwq(pwq, wq, pool);
	return pwq;
}

/**
 * wq_calc_node_cpumask - calculate a wq_attrs' cpumask for the specified node
 * @attrs: the wq_attrs of the default pwq of the target workqueue
 * @node: the target NUMA node
 * @cpu_going_down: if >= 0, the CPU to consider as offline
 * @cpumask: outarg, the resulting cpumask
 *
 * Calculate the cpumask a workqueue with @attrs should use on @node.  If
 * @cpu_going_down is >= 0, that cpu is considered offline during
 * calculation.  The result is stored in @cpumask.
 *
 * If NUMA affinity is not enabled, @attrs->cpumask is always used.  If
 * enabled and @node has online CPUs requested by @attrs, the returned
 * cpumask is the intersection of the possible CPUs of @node and
 * @attrs->cpumask.
 *
 * The caller is responsible for ensuring that the cpumask of @node stays
 * stable.
 *
 * Return: %true if the resulting @cpumask is different from @attrs->cpumask,
 * %false if equal.
 */
/**
 * wq_calc_node_cpumask - 计算特定NUMA节点的有效cpumask
 *
 * 当wq_numa_enabled且@node有在线CPU时，将@cpumask设置为
 * @attrs->cpumask与@node可用CPU的交集；否则直接用@attrs->cpumask。
 * 返回true表示结果与@attrs->cpumask不同（需要独立pool），false表示相同。
 */
static bool wq_calc_node_cpumask(const struct workqueue_attrs *attrs, int node,
				 int cpu_going_down, cpumask_t *cpumask)
{
	if (!wq_numa_enabled || attrs->no_numa)
		goto use_dfl;

	/* does @node have any online CPUs @attrs wants? */
	cpumask_and(cpumask, cpumask_of_node(node), attrs->cpumask);
	if (cpu_going_down >= 0)
		cpumask_clear_cpu(cpu_going_down, cpumask);

	if (cpumask_empty(cpumask))
		goto use_dfl;

	/* yeap, return possible CPUs in @node that @attrs wants */
	cpumask_and(cpumask, attrs->cpumask, wq_numa_possible_cpumask[node]);

	if (cpumask_empty(cpumask)) {
		pr_warn_once("WARNING: workqueue cpumask: online intersect > "
				"possible intersect\n");
		return false;
	}

	return !cpumask_equal(cpumask, attrs->cpumask);

use_dfl:
	cpumask_copy(cpumask, attrs->cpumask);
	return false;
}

/**
 * numa_pwq_tbl_install - 安装新pwq到wq的NUMA节点pwq表
 *
 * 以RCU方式将@pwq安装到@wq->numa_pwq_tbl[@node]，返回旧的pwq。
 * 调用者需持有wq->mutex，确保安装的原子性，旧pwq由调用者负责
 * put_pwq_unlocked()释放。
 */
/* install @pwq into @wq's numa_pwq_tbl[] for @node and return the old pwq */
static struct pool_workqueue *numa_pwq_tbl_install(struct workqueue_struct *wq,
						   int node,
						   struct pool_workqueue *pwq)
{
	struct pool_workqueue *old_pwq;

	lockdep_assert_held(&wq_pool_mutex);
	lockdep_assert_held(&wq->mutex);

	/* link_pwq() can handle duplicate calls */
	link_pwq(pwq);

	old_pwq = rcu_access_pointer(wq->numa_pwq_tbl[node]);
	rcu_assign_pointer(wq->numa_pwq_tbl[node], pwq);
	return old_pwq;
}

/* context to store the prepared attrs & pwqs before applying */
struct apply_wqattrs_ctx {
	struct workqueue_struct	*wq;		/* target workqueue */
	struct workqueue_attrs	*attrs;		/* attrs to apply */
	struct list_head	list;		/* queued for batching commit */
	struct pool_workqueue	*dfl_pwq;
	struct pool_workqueue	*pwq_tbl[];
};

/**
 * apply_wqattrs_cleanup - 释放apply_wqattrs_ctx及其pwq资源
 *
 * 无论apply成功还是中止，都调用此函数清理上下文：逐NUMA节点
 * put_pwq_unlocked()旧pwq，释放dfl_pwq，最后kfree ctx本身。
 */
/* free the resources after success or abort */
static void apply_wqattrs_cleanup(struct apply_wqattrs_ctx *ctx)
{
	if (ctx) {
		int node;

		for_each_node(node)
			put_pwq_unlocked(ctx->pwq_tbl[node]);
		put_pwq_unlocked(ctx->dfl_pwq);

		free_workqueue_attrs(ctx->attrs);

		kfree(ctx);
	}
}

/* allocate the attrs and pwqs for later installation */
static struct apply_wqattrs_ctx *
apply_wqattrs_prepare(struct workqueue_struct *wq,
		      const struct workqueue_attrs *attrs)
{
	struct apply_wqattrs_ctx *ctx;
	struct workqueue_attrs *new_attrs, *tmp_attrs;
	int node;

	lockdep_assert_held(&wq_pool_mutex);

	ctx = kzalloc(struct_size(ctx, pwq_tbl, nr_node_ids), GFP_KERNEL);

	new_attrs = alloc_workqueue_attrs();
	tmp_attrs = alloc_workqueue_attrs();
	if (!ctx || !new_attrs || !tmp_attrs)
		goto out_free;

	/*
	 * Calculate the attrs of the default pwq.
	 * If the user configured cpumask doesn't overlap with the
	 * wq_unbound_cpumask, we fallback to the wq_unbound_cpumask.
	 */
	copy_workqueue_attrs(new_attrs, attrs);
	cpumask_and(new_attrs->cpumask, new_attrs->cpumask, wq_unbound_cpumask);
	if (unlikely(cpumask_empty(new_attrs->cpumask)))
		cpumask_copy(new_attrs->cpumask, wq_unbound_cpumask);

	/*
	 * We may create multiple pwqs with differing cpumasks.  Make a
	 * copy of @new_attrs which will be modified and used to obtain
	 * pools.
	 */
	copy_workqueue_attrs(tmp_attrs, new_attrs);

	/*
	 * If something goes wrong during CPU up/down, we'll fall back to
	 * the default pwq covering whole @attrs->cpumask.  Always create
	 * it even if we don't use it immediately.
	 */
	ctx->dfl_pwq = alloc_unbound_pwq(wq, new_attrs);
	if (!ctx->dfl_pwq)
		goto out_free;

	for_each_node(node) {
		if (wq_calc_node_cpumask(new_attrs, node, -1, tmp_attrs->cpumask)) {
			ctx->pwq_tbl[node] = alloc_unbound_pwq(wq, tmp_attrs);
			if (!ctx->pwq_tbl[node])
				goto out_free;
		} else {
			ctx->dfl_pwq->refcnt++;
			ctx->pwq_tbl[node] = ctx->dfl_pwq;
		}
	}

	/* save the user configured attrs and sanitize it. */
	copy_workqueue_attrs(new_attrs, attrs);
	cpumask_and(new_attrs->cpumask, new_attrs->cpumask, cpu_possible_mask);
	ctx->attrs = new_attrs;

	ctx->wq = wq;
	free_workqueue_attrs(tmp_attrs);
	return ctx;

out_free:
	free_workqueue_attrs(tmp_attrs);
	free_workqueue_attrs(new_attrs);
	apply_wqattrs_cleanup(ctx);
	return NULL;
}

/**
 * apply_wqattrs_commit - 提交新pwq配置，原子替换wq中的旧pwq
 *
 * 持有wq->mutex后，将ctx中准备好的新pwq逐NUMA节点安装到wq，
 * 更新dfl_pwq和unbound_attrs，并link所有新pwq。ctx返回时
 * 携带旧pwq，由调用者通过apply_wqattrs_cleanup()释放。
 */
/* set attrs and install prepared pwqs, @ctx points to old pwqs on return */
static void apply_wqattrs_commit(struct apply_wqattrs_ctx *ctx)
{
	int node;

	/* all pwqs have been created successfully, let's install'em */
	mutex_lock(&ctx->wq->mutex);

	copy_workqueue_attrs(ctx->wq->unbound_attrs, ctx->attrs);

	/* save the previous pwq and install the new one */
	for_each_node(node)
		ctx->pwq_tbl[node] = numa_pwq_tbl_install(ctx->wq, node,
							  ctx->pwq_tbl[node]);

	/* @dfl_pwq might not have been used, ensure it's linked */
	link_pwq(ctx->dfl_pwq);
	swap(ctx->wq->dfl_pwq, ctx->dfl_pwq);

	mutex_unlock(&ctx->wq->mutex);
}

/**
 * apply_wqattrs_lock - 获取apply_wqattrs所需的全局锁
 *
 * 在创建和安装新pwq前调用：先get_online_cpus()防止CPU热插拔，
 * 再锁wq_pool_mutex保护pool哈希表，确保pwq创建期间CPU拓扑稳定。
 */
static void apply_wqattrs_lock(void)
{
	/* CPUs should stay stable across pwq creations and installations */
	get_online_cpus();
	mutex_lock(&wq_pool_mutex);
}

/**
 * apply_wqattrs_unlock - 释放apply_wqattrs所需的全局锁
 *
 * 与apply_wqattrs_lock()配对：解锁wq_pool_mutex，然后
 * put_online_cpus()允许CPU热插拔继续。
 */
static void apply_wqattrs_unlock(void)
{
	mutex_unlock(&wq_pool_mutex);
	put_online_cpus();
}

/**
 * apply_workqueue_attrs_locked - 已持锁版本的wq属性应用
 *
 * 在已持有apply_wqattrs_lock()的情况下，为unbound工作队列
 * 创建新的per-NUMA pwq集合并原子替换旧配置。仅允许unbound
 * 工作队列修改属性，bound工作队列返回-EINVAL。
 */
static int apply_workqueue_attrs_locked(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct apply_wqattrs_ctx *ctx;

	/* only unbound workqueues can change attributes */
	if (WARN_ON(!(wq->flags & WQ_UNBOUND)))
		return -EINVAL;

	/* creating multiple pwqs breaks ordering guarantee */
	if (!list_empty(&wq->pwqs)) {
		if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
			return -EINVAL;

		wq->flags &= ~__WQ_ORDERED;
	}

	ctx = apply_wqattrs_prepare(wq, attrs);
	if (!ctx)
		return -ENOMEM;

	/* the ctx has been prepared successfully, let's commit it */
	apply_wqattrs_commit(ctx);
	apply_wqattrs_cleanup(ctx);

	return 0;
}

/**
 * apply_workqueue_attrs - apply new workqueue_attrs to an unbound workqueue
 * @wq: the target workqueue
 * @attrs: the workqueue_attrs to apply, allocated with alloc_workqueue_attrs()
 *
 * Apply @attrs to an unbound workqueue @wq.  Unless disabled, on NUMA
 * machines, this function maps a separate pwq to each NUMA node with
 * possibles CPUs in @attrs->cpumask so that work items are affine to the
 * NUMA node it was issued on.  Older pwqs are released as in-flight work
 * items finish.  Note that a work item which repeatedly requeues itself
 * back-to-back will stay on its current pwq.
 *
 * Performs GFP_KERNEL allocations.
 *
 * Assumes caller has CPU hotplug read exclusion, i.e. get_online_cpus().
 *
 * Return: 0 on success and -errno on failure.
 */
/**
 * apply_workqueue_attrs - 为unbound工作队列应用新属性
 *
 * 对@wq应用@attrs：在NUMA机器上为每个cpumask覆盖的NUMA节点
 * 创建独立pwq，实现work的NUMA亲和性。旧pwq在in-flight work
 * 完成后异步释放。仅适用于unbound工作队列。
 */
int apply_workqueue_attrs(struct workqueue_struct *wq,
			  const struct workqueue_attrs *attrs)
{
	int ret;

	lockdep_assert_cpus_held();

	mutex_lock(&wq_pool_mutex);
	ret = apply_workqueue_attrs_locked(wq, attrs);
	mutex_unlock(&wq_pool_mutex);

	return ret;
}

/**
 * wq_update_unbound_numa - update NUMA affinity of a wq for CPU hot[un]plug
 * @wq: the target workqueue
 * @cpu: the CPU coming up or going down
 * @online: whether @cpu is coming up or going down
 *
 * This function is to be called from %CPU_DOWN_PREPARE, %CPU_ONLINE and
 * %CPU_DOWN_FAILED.  @cpu is being hot[un]plugged, update NUMA affinity of
 * @wq accordingly.
 *
 * If NUMA affinity can't be adjusted due to memory allocation failure, it
 * falls back to @wq->dfl_pwq which may not be optimal but is always
 * correct.
 *
 * Note that when the last allowed CPU of a NUMA node goes offline for a
 * workqueue with a cpumask spanning multiple nodes, the workers which were
 * already executing the work items for the workqueue will lose their CPU
 * affinity and may execute on any CPU.  This is similar to how per-cpu
 * workqueues behave on CPU_DOWN.  If a workqueue user wants strict
 * affinity, it's the user's responsibility to flush the work item from
 * CPU_DOWN_PREPARE.
 */
/**
 * wq_update_unbound_numa - CPU上下线时更新unbound工作队列NUMA映射
 *
 * 当CPU online/offline时调用，为该CPU所属NUMA节点重新计算
 * cpumask并更新对应pwq。确保工作队列的NUMA亲和性在CPU拓扑
 * 变化时保持正确。需持有wq_pool_mutex。
 */
static void wq_update_unbound_numa(struct workqueue_struct *wq, int cpu,
				   bool online)
{
	int node = cpu_to_node(cpu);
	int cpu_off = online ? -1 : cpu;
	struct pool_workqueue *old_pwq = NULL, *pwq;
	struct workqueue_attrs *target_attrs;
	cpumask_t *cpumask;

	lockdep_assert_held(&wq_pool_mutex);

	if (!wq_numa_enabled || !(wq->flags & WQ_UNBOUND) ||
	    wq->unbound_attrs->no_numa)
		return;

	/*
	 * We don't wanna alloc/free wq_attrs for each wq for each CPU.
	 * Let's use a preallocated one.  The following buf is protected by
	 * CPU hotplug exclusion.
	 */
	target_attrs = wq_update_unbound_numa_attrs_buf;
	cpumask = target_attrs->cpumask;

	copy_workqueue_attrs(target_attrs, wq->unbound_attrs);
	pwq = unbound_pwq_by_node(wq, node);

	/*
	 * Let's determine what needs to be done.  If the target cpumask is
	 * different from the default pwq's, we need to compare it to @pwq's
	 * and create a new one if they don't match.  If the target cpumask
	 * equals the default pwq's, the default pwq should be used.
	 */
	if (wq_calc_node_cpumask(wq->dfl_pwq->pool->attrs, node, cpu_off, cpumask)) {
		if (cpumask_equal(cpumask, pwq->pool->attrs->cpumask))
			return;
	} else {
		goto use_dfl_pwq;
	}

	/* create a new pwq */
	pwq = alloc_unbound_pwq(wq, target_attrs);
	if (!pwq) {
		pr_warn("workqueue: allocation failed while updating NUMA affinity of \"%s\"\n",
			wq->name);
		goto use_dfl_pwq;
	}

	/* Install the new pwq. */
	mutex_lock(&wq->mutex);
	old_pwq = numa_pwq_tbl_install(wq, node, pwq);
	goto out_unlock;

use_dfl_pwq:
	mutex_lock(&wq->mutex);
	raw_spin_lock_irq(&wq->dfl_pwq->pool->lock);
	get_pwq(wq->dfl_pwq);
	raw_spin_unlock_irq(&wq->dfl_pwq->pool->lock);
	old_pwq = numa_pwq_tbl_install(wq, node, wq->dfl_pwq);
out_unlock:
	mutex_unlock(&wq->mutex);
	put_pwq_unlocked(old_pwq);
}

/**
 * alloc_and_link_pwqs - 为新工作队列分配并链接所有pwq
 *
 * 对于bound工作队列：为每个CPU分配per-cpu pwq并与对应的per-cpu
 * worker_pool关联；对于unbound工作队列：调用apply_workqueue_attrs()
 * 按属性创建NUMA感知的pwq集合。
 */
static int alloc_and_link_pwqs(struct workqueue_struct *wq)
{
	bool highpri = wq->flags & WQ_HIGHPRI;
	int cpu, ret;

	if (!(wq->flags & WQ_UNBOUND)) {
		wq->cpu_pwqs = alloc_percpu(struct pool_workqueue);
		if (!wq->cpu_pwqs)
			return -ENOMEM;

		for_each_possible_cpu(cpu) {
			struct pool_workqueue *pwq =
				per_cpu_ptr(wq->cpu_pwqs, cpu);
			struct worker_pool *cpu_pools =
				per_cpu(cpu_worker_pools, cpu);

			init_pwq(pwq, wq, &cpu_pools[highpri]);

			mutex_lock(&wq->mutex);
			link_pwq(pwq);
			mutex_unlock(&wq->mutex);
		}
		return 0;
	}

	get_online_cpus();
	if (wq->flags & __WQ_ORDERED) {
		ret = apply_workqueue_attrs(wq, ordered_wq_attrs[highpri]);
		/* there should only be single pwq for ordering guarantee */
		WARN(!ret && (wq->pwqs.next != &wq->dfl_pwq->pwqs_node ||
			      wq->pwqs.prev != &wq->dfl_pwq->pwqs_node),
		     "ordering guarantee broken for workqueue %s\n", wq->name);
	} else {
		ret = apply_workqueue_attrs(wq, unbound_std_wq_attrs[highpri]);
	}
	put_online_cpus();

	return ret;
}

/**
 * wq_clamp_max_active - 将max_active限制在合法范围内
 *
 * 根据队列类型（UNBOUND限制更宽松）计算上限，将@max_active
 * 夹在[1, lim]区间并发出越界警告。用于alloc_workqueue()和
 * workqueue_set_max_active()时规范化用户输入。
 */
static int wq_clamp_max_active(int max_active, unsigned int flags,
			       const char *name)
{
	int lim = flags & WQ_UNBOUND ? WQ_UNBOUND_MAX_ACTIVE : WQ_MAX_ACTIVE;

	if (max_active < 1 || max_active > lim)
		pr_warn("workqueue: max_active %d requested for %s is out of range, clamping between %d and %d\n",
			max_active, name, 1, lim);

	return clamp_val(max_active, 1, lim);
}

/*
 * Workqueues which may be used during memory reclaim should have a rescuer
 * to guarantee forward progress.
 */
/**
 * init_rescuer - 为WQ_MEM_RECLAIM工作队列初始化rescuer线程
 *
 * 仅WQ_MEM_RECLAIM队列需要rescuer。分配worker结构，创建
 * rescuer_thread内核线程并设置PF_MEMALLOC，确保在内存紧张
 * 时能为该队列的work提供前向进度保证。
 */
static int init_rescuer(struct workqueue_struct *wq)
{
	struct worker *rescuer;
	int ret;

	if (!(wq->flags & WQ_MEM_RECLAIM))
		return 0;

	rescuer = alloc_worker(NUMA_NO_NODE);
	if (!rescuer)
		return -ENOMEM;

	rescuer->rescue_wq = wq;
	rescuer->task = kthread_create(rescuer_thread, rescuer, "%s", wq->name);
	if (IS_ERR(rescuer->task)) {
		ret = PTR_ERR(rescuer->task);
		kfree(rescuer);
		return ret;
	}

	wq->rescuer = rescuer;
	kthread_bind_mask(rescuer->task, cpu_possible_mask);
	wake_up_process(rescuer->task);

	return 0;
}

__printf(1, 4)
/**
 * alloc_workqueue - 创建工作队列（通用接口）
 *
 * 分配workqueue_struct和per-CPU的pool_workqueue，初始化各项参数：
 * - WQ_UNBOUND：创建unbound worker_pool，不绑定特定CPU
 * - WQ_MEM_RECLAIM：创建救援线程（rescuer_thread）防止内存回收死锁
 * - max_active：限制同时执行的work数量（0=无限制）
 * 返回workqueue指针，失败返回NULL。通常通过alloc_ordered_workqueue()
 * 或系统预定义队列（system_wq等）使用而非直接调用。
 */
struct workqueue_struct *alloc_workqueue(const char *fmt,
					 unsigned int flags,
					 int max_active, ...)
{
	size_t tbl_size = 0;
	va_list args;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	/*
	 * Unbound && max_active == 1 used to imply ordered, which is no
	 * longer the case on NUMA machines due to per-node pools.  While
	 * alloc_ordered_workqueue() is the right way to create an ordered
	 * workqueue, keep the previous behavior to avoid subtle breakages
	 * on NUMA.
	 */
	if ((flags & WQ_UNBOUND) && max_active == 1)
		flags |= __WQ_ORDERED;

	/* see the comment above the definition of WQ_POWER_EFFICIENT */
	if ((flags & WQ_POWER_EFFICIENT) && wq_power_efficient)
		flags |= WQ_UNBOUND;

	/* allocate wq and format name */
	if (flags & WQ_UNBOUND)
		tbl_size = nr_node_ids * sizeof(wq->numa_pwq_tbl[0]);

	wq = kzalloc(sizeof(*wq) + tbl_size, GFP_KERNEL);
	if (!wq)
		return NULL;

	if (flags & WQ_UNBOUND) {
		wq->unbound_attrs = alloc_workqueue_attrs();
		if (!wq->unbound_attrs)
			goto err_free_wq;
	}

	va_start(args, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	va_end(args);

	max_active = max_active ?: WQ_DFL_ACTIVE;
	max_active = wq_clamp_max_active(max_active, flags, wq->name);

	/* init wq */
	wq->flags = flags;
	wq->saved_max_active = max_active;
	mutex_init(&wq->mutex);
	atomic_set(&wq->nr_pwqs_to_flush, 0);
	INIT_LIST_HEAD(&wq->pwqs);
	INIT_LIST_HEAD(&wq->flusher_queue);
	INIT_LIST_HEAD(&wq->flusher_overflow);
	INIT_LIST_HEAD(&wq->maydays);

	wq_init_lockdep(wq);
	INIT_LIST_HEAD(&wq->list);

	if (alloc_and_link_pwqs(wq) < 0)
		goto err_unreg_lockdep;

	if (wq_online && init_rescuer(wq) < 0)
		goto err_destroy;

	if ((wq->flags & WQ_SYSFS) && workqueue_sysfs_register(wq))
		goto err_destroy;

	/*
	 * wq_pool_mutex protects global freeze state and workqueues list.
	 * Grab it, adjust max_active and add the new @wq to workqueues
	 * list.
	 */
	mutex_lock(&wq_pool_mutex);

	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);
	mutex_unlock(&wq->mutex);

	list_add_tail_rcu(&wq->list, &workqueues);

	mutex_unlock(&wq_pool_mutex);

	return wq;

err_unreg_lockdep:
	wq_unregister_lockdep(wq);
	wq_free_lockdep(wq);
err_free_wq:
	free_workqueue_attrs(wq->unbound_attrs);
	kfree(wq);
	return NULL;
err_destroy:
	destroy_workqueue(wq);
	return NULL;
}
EXPORT_SYMBOL_GPL(alloc_workqueue);

/**
 * pwq_busy - 检查pwq是否仍有未完成的work活动
 * @pwq: 目标pool_workqueue
 *
 * 检查pwq是否可安全销毁：若任一颜色的nr_in_flight非零、
 * 非默认pwq且引用计数>1、或有active/delayed work则返回true。
 * 在destroy_workqueue()中用于判断是否可以安全释放pwq。
 */
static bool pwq_busy(struct pool_workqueue *pwq)
{
	int i;

	for (i = 0; i < WORK_NR_COLORS; i++)
		if (pwq->nr_in_flight[i])
			return true;

	if ((pwq != pwq->wq->dfl_pwq) && (pwq->refcnt > 1))
		return true;
	if (pwq->nr_active || !list_empty(&pwq->delayed_works))
		return true;

	return false;
}

/**
 * destroy_workqueue - safely terminate a workqueue
 * @wq: target workqueue
 *
 * Safely destroy a workqueue. All work currently pending will be done first.
 */
/**
 * destroy_workqueue - 安全销毁工作队列
 *
 * 销毁流程：先注销sysfs条目，再drain_workqueue()等待所有work完成，
 * 然后停止rescuer线程，最后释放per-cpu或NUMA pwq引用。对于bound队列
 * 通过call_rcu()延迟释放；对于unbound队列逐NUMA节点drop pwq引用，
 * 最后一个pwq释放时自动free整个wq结构。
 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	int node;

	/*
	 * Remove it from sysfs first so that sanity check failure doesn't
	 * lead to sysfs name conflicts.
	 */
	workqueue_sysfs_unregister(wq);

	/* drain it before proceeding with destruction */
	drain_workqueue(wq);

	/* kill rescuer, if sanity checks fail, leave it w/o rescuer */
	if (wq->rescuer) {
		struct worker *rescuer = wq->rescuer;

		/* this prevents new queueing */
		raw_spin_lock_irq(&wq_mayday_lock);
		wq->rescuer = NULL;
		raw_spin_unlock_irq(&wq_mayday_lock);

		/* rescuer will empty maydays list before exiting */
		kthread_stop(rescuer->task);
		kfree(rescuer);
	}

	/*
	 * Sanity checks - grab all the locks so that we wait for all
	 * in-flight operations which may do put_pwq().
	 */
	mutex_lock(&wq_pool_mutex);
	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		raw_spin_lock_irq(&pwq->pool->lock);
		if (WARN_ON(pwq_busy(pwq))) {
			pr_warn("%s: %s has the following busy pwq\n",
				__func__, wq->name);
			show_pwq(pwq);
			raw_spin_unlock_irq(&pwq->pool->lock);
			mutex_unlock(&wq->mutex);
			mutex_unlock(&wq_pool_mutex);
			show_workqueue_state();
			return;
		}
		raw_spin_unlock_irq(&pwq->pool->lock);
	}
	mutex_unlock(&wq->mutex);

	/*
	 * wq list is used to freeze wq, remove from list after
	 * flushing is complete in case freeze races us.
	 */
	list_del_rcu(&wq->list);
	mutex_unlock(&wq_pool_mutex);

	if (!(wq->flags & WQ_UNBOUND)) {
		wq_unregister_lockdep(wq);
		/*
		 * The base ref is never dropped on per-cpu pwqs.  Directly
		 * schedule RCU free.
		 */
		call_rcu(&wq->rcu, rcu_free_wq);
	} else {
		/*
		 * We're the sole accessor of @wq at this point.  Directly
		 * access numa_pwq_tbl[] and dfl_pwq to put the base refs.
		 * @wq will be freed when the last pwq is released.
		 */
		for_each_node(node) {
			pwq = rcu_access_pointer(wq->numa_pwq_tbl[node]);
			RCU_INIT_POINTER(wq->numa_pwq_tbl[node], NULL);
			put_pwq_unlocked(pwq);
		}

		/*
		 * Put dfl_pwq.  @wq may be freed any time after dfl_pwq is
		 * put.  Don't access it afterwards.
		 */
		pwq = wq->dfl_pwq;
		wq->dfl_pwq = NULL;
		put_pwq_unlocked(pwq);
	}
}
EXPORT_SYMBOL_GPL(destroy_workqueue);

/**
 * workqueue_set_max_active - adjust max_active of a workqueue
 * @wq: target workqueue
 * @max_active: new max_active value.
 *
 * Set max_active of @wq to @max_active.
 *
 * CONTEXT:
 * Don't call from IRQ context.
 */
/**
 * workqueue_set_max_active - 动态修改工作队列的max_active
 *
 * 修改@wq的max_active并立即更新所有关联pwq的max_active值，
 * 若有delayed_works等待，适时激活。有序队列（__WQ_ORDERED_EXPLICIT）
 * 禁止修改。不可从IRQ上下文调用。
 */
void workqueue_set_max_active(struct workqueue_struct *wq, int max_active)
{
	struct pool_workqueue *pwq;

	/* disallow meddling with max_active for ordered workqueues */
	if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
		return;

	max_active = wq_clamp_max_active(max_active, wq->flags, wq->name);

	mutex_lock(&wq->mutex);

	wq->flags &= ~__WQ_ORDERED;
	wq->saved_max_active = max_active;

	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);

	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(workqueue_set_max_active);

/**
 * current_work - retrieve %current task's work struct
 *
 * Determine if %current task is a workqueue worker and what it's working on.
 * Useful to find out the context that the %current task is running in.
 *
 * Return: work struct if %current task is a workqueue worker, %NULL otherwise.
 */
/**
 * current_work - 获取当前worker正在执行的work
 *
 * 若当前任务是工作线程，返回其正在执行的work_struct指针；
 * 否则返回NULL。可在work回调中调用以获取自身work对象。
 */
struct work_struct *current_work(void)
{
	struct worker *worker = current_wq_worker();

	return worker ? worker->current_work : NULL;
}
EXPORT_SYMBOL(current_work);

/**
 * current_is_workqueue_rescuer - is %current workqueue rescuer?
 *
 * Determine whether %current is a workqueue rescuer.  Can be used from
 * work functions to determine whether it's being run off the rescuer task.
 *
 * Return: %true if %current is a workqueue rescuer. %false otherwise.
 */
/**
 * current_is_workqueue_rescuer - 检查当前任务是否为workqueue rescuer
 *
 * 返回true表示当前任务是WQ_MEM_RECLAIM队列的rescuer线程
 * （worker->rescue_wq非NULL）。可用于判断当前是否在内存
 * 回收安全路径中执行。
 */
bool current_is_workqueue_rescuer(void)
{
	struct worker *worker = current_wq_worker();

	return worker && worker->rescue_wq;
}

/**
 * workqueue_congested - test whether a workqueue is congested
 * @cpu: CPU in question
 * @wq: target workqueue
 *
 * Test whether @wq's cpu workqueue for @cpu is congested.  There is
 * no synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * If @cpu is WORK_CPU_UNBOUND, the test is performed on the local CPU.
 * Note that both per-cpu and unbound workqueues may be associated with
 * multiple pool_workqueues which have separate congested states.  A
 * workqueue being congested on one CPU doesn't mean the workqueue is also
 * contested on other CPUs / NUMA nodes.
 *
 * Return:
 * %true if congested, %false otherwise.
 */
/**
 * workqueue_congested - 检查工作队列是否拥塞
 *
 * 检查@cpu（或当前CPU）对应pwq的delayed_works链表是否非空，
 * 非空表示有work因达到max_active限制而被延迟，队列已拥塞。
 * 可用于背压控制，决定是否继续入队或等待。
 */
bool workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	bool ret;

	rcu_read_lock();
	preempt_disable();

	if (cpu == WORK_CPU_UNBOUND)
		cpu = smp_processor_id();

	if (!(wq->flags & WQ_UNBOUND))
		pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	else
		pwq = unbound_pwq_by_node(wq, cpu_to_node(cpu));

	ret = !list_empty(&pwq->delayed_works);
	preempt_enable();
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(workqueue_congested);

/**
 * work_busy - 查询work当前是否处于pending或running状态
 * @work: 要检测的work_struct
 *
 * 无同步保证，结果仅供参考（调试/建议用）。检查PENDING位判断是否待执行，
 * 并在关联pool中查找是否有worker正在执行该work。
 *
 * Return: WORK_BUSY_PENDING和/或WORK_BUSY_RUNNING的OR组合。
 */
unsigned int work_busy(struct work_struct *work)
{
	struct worker_pool *pool;
	unsigned long flags;
	unsigned int ret = 0;

	if (work_pending(work))
		ret |= WORK_BUSY_PENDING;

	rcu_read_lock();
	pool = get_work_pool(work);
	if (pool) {
		raw_spin_lock_irqsave(&pool->lock, flags);
		if (find_worker_executing_work(pool, work))
			ret |= WORK_BUSY_RUNNING;
		raw_spin_unlock_irqrestore(&pool->lock, flags);
	}
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(work_busy);

/**
 * set_worker_desc - set description for the current work item
 * @fmt: printf-style format string
 * @...: arguments for the format string
 *
 * This function can be called by a running work function to describe what
 * the work item is about.  If the worker task gets dumped, this
 * information will be printed out together to help debugging.  The
 * description can be at most WORKER_DESC_LEN including the trailing '\0'.
 */
/**
 * set_worker_desc - 设置当前worker的描述字符串
 *
 * 在work回调中调用，为当前worker设置描述信息（最多
 * WORKER_DESC_LEN字节）。信息在任务崩溃时随task dump
 * 一起输出，方便调试定位具体是哪个work导致问题。
 */
void set_worker_desc(const char *fmt, ...)
{
	struct worker *worker = current_wq_worker();
	va_list args;

	if (worker) {
		va_start(args, fmt);
		vsnprintf(worker->desc, sizeof(worker->desc), fmt, args);
		va_end(args);
	}
}
EXPORT_SYMBOL_GPL(set_worker_desc);

/**
 * print_worker_info - print out worker information and description
 * @log_lvl: the log level to use when printing
 * @task: target task
 *
 * If @task is a worker and currently executing a work item, print out the
 * name of the workqueue being serviced and worker description set with
 * set_worker_desc() by the currently executing work item.
 *
 * This function can be safely called on any task as long as the
 * task_struct itself is accessible.  While safe, this function isn't
 * synchronized and may print out mixups or garbages of limited length.
 */
/**
 * print_worker_info - 打印worker任务信息（用于任务转储）
 * @log_lvl: 内核日志级别字符串（如KERN_INFO）
 * @task: 目标任务结构体
 *
 * 若@task是一个worker且正在执行work，则打印当前work函数名、
 * 所属工作队列名以及通过set_worker_desc()设置的描述字符串。
 * 使用copy_from_kernel_nofault()安全读取可能未同步的worker状态，
 * 避免因并发修改导致崩溃，适合在任务转储（如sysrq-t）中调用。
 */
void print_worker_info(const char *log_lvl, struct task_struct *task)
{
	work_func_t *fn = NULL;
	char name[WQ_NAME_LEN] = { };
	char desc[WORKER_DESC_LEN] = { };
	struct pool_workqueue *pwq = NULL;
	struct workqueue_struct *wq = NULL;
	struct worker *worker;

	if (!(task->flags & PF_WQ_WORKER))
		return;

	/*
	 * This function is called without any synchronization and @task
	 * could be in any state.  Be careful with dereferences.
	 */
	worker = kthread_probe_data(task);

	/*
	 * Carefully copy the associated workqueue's workfn, name and desc.
	 * Keep the original last '\0' in case the original is garbage.
	 */
	copy_from_kernel_nofault(&fn, &worker->current_func, sizeof(fn));
	copy_from_kernel_nofault(&pwq, &worker->current_pwq, sizeof(pwq));
	copy_from_kernel_nofault(&wq, &pwq->wq, sizeof(wq));
	copy_from_kernel_nofault(name, wq->name, sizeof(name) - 1);
	copy_from_kernel_nofault(desc, worker->desc, sizeof(desc) - 1);

	if (fn || name[0] || desc[0]) {
		printk("%sWorkqueue: %s %ps", log_lvl, name, fn);
		if (strcmp(name, desc))
			pr_cont(" (%s)", desc);
		pr_cont("\n");
	}
}

/**
 * pr_cont_pool_info - 打印worker_pool基本属性信息
 * @pool: 目标worker_pool
 *
 * 以pr_cont方式（续接上一行）输出pool的cpumask、NUMA节点号、
 * flags和nice值，供show_pwq()和show_workqueue_state()调用。
 */
static void pr_cont_pool_info(struct worker_pool *pool)
{
	pr_cont(" cpus=%*pbl", nr_cpumask_bits, pool->attrs->cpumask);
	if (pool->node != NUMA_NO_NODE)
		pr_cont(" node=%d", pool->node);
	pr_cont(" flags=0x%x nice=%d", pool->flags, pool->attrs->nice);
}

/**
 * pr_cont_work - 打印单个work项信息（续接当前行）
 * @comma: 是否在输出前加逗号分隔符
 * @work: 目标work_struct
 *
 * 若work是flush屏障（wq_barrier_func），则输出"BAR(pid)"格式；
 * 否则输出work->func的函数名符号。用于show_pwq()的in-flight/pending列表展示。
 */
static void pr_cont_work(bool comma, struct work_struct *work)
{
	if (work->func == wq_barrier_func) {
		struct wq_barrier *barr;

		barr = container_of(work, struct wq_barrier, work);

		pr_cont("%s BAR(%d)", comma ? "," : "",
			task_pid_nr(barr->task));
	} else {
		pr_cont("%s %ps", comma ? "," : "", work->func);
	}
}

/**
 * show_pwq - 显示pool_workqueue的详细运行状态
 * @pwq: 目标pool_workqueue
 *
 * 依次打印pwq的active计数、max_active、refcnt、是否处于MAYDAY状态，
 * 以及in-flight（正在执行）、pending（等待执行）和delayed（延迟等待激活）
 * 三类work列表。由show_workqueue_state()在持pool->lock时调用。
 */
static void show_pwq(struct pool_workqueue *pwq)
{
	struct worker_pool *pool = pwq->pool;
	struct work_struct *work;
	struct worker *worker;
	bool has_in_flight = false, has_pending = false;
	int bkt;

	pr_info("  pwq %d:", pool->id);
	pr_cont_pool_info(pool);

	pr_cont(" active=%d/%d refcnt=%d%s\n",
		pwq->nr_active, pwq->max_active, pwq->refcnt,
		!list_empty(&pwq->mayday_node) ? " MAYDAY" : "");

	hash_for_each(pool->busy_hash, bkt, worker, hentry) {
		if (worker->current_pwq == pwq) {
			has_in_flight = true;
			break;
		}
	}
	if (has_in_flight) {
		bool comma = false;

		pr_info("    in-flight:");
		hash_for_each(pool->busy_hash, bkt, worker, hentry) {
			if (worker->current_pwq != pwq)
				continue;

			pr_cont("%s %d%s:%ps", comma ? "," : "",
				task_pid_nr(worker->task),
				worker->rescue_wq ? "(RESCUER)" : "",
				worker->current_func);
			list_for_each_entry(work, &worker->scheduled, entry)
				pr_cont_work(false, work);
			comma = true;
		}
		pr_cont("\n");
	}

	list_for_each_entry(work, &pool->worklist, entry) {
		if (get_work_pwq(work) == pwq) {
			has_pending = true;
			break;
		}
	}
	if (has_pending) {
		bool comma = false;

		pr_info("    pending:");
		list_for_each_entry(work, &pool->worklist, entry) {
			if (get_work_pwq(work) != pwq)
				continue;

			pr_cont_work(comma, work);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont("\n");
	}

	if (!list_empty(&pwq->delayed_works)) {
		bool comma = false;

		pr_info("    delayed:");
		list_for_each_entry(work, &pwq->delayed_works, entry) {
			pr_cont_work(comma, work);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont("\n");
	}
}

/**
 * show_workqueue_state - 转储所有工作队列和worker pool的当前状态
 *
 * 由sysrq处理程序或try_to_freeze_tasks()调用，遍历所有工作队列和worker pool，
 * 打印所有处于忙碌状态（有active或delayed work）的pwq详情，
 * 以及所有非全空闲的pool的线程信息。每处理一个条目后调用
 * touch_nmi_watchdog()以防止在原子上下文中长时间打印触发硬锁死检测。
 */
void show_workqueue_state(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	unsigned long flags;
	int pi;

	rcu_read_lock();

	pr_info("Showing busy workqueues and worker pools:\n");

	list_for_each_entry_rcu(wq, &workqueues, list) {
		struct pool_workqueue *pwq;
		bool idle = true;

		for_each_pwq(pwq, wq) {
			if (pwq->nr_active || !list_empty(&pwq->delayed_works)) {
				idle = false;
				break;
			}
		}
		if (idle)
			continue;

		pr_info("workqueue %s: flags=0x%x\n", wq->name, wq->flags);

		for_each_pwq(pwq, wq) {
			raw_spin_lock_irqsave(&pwq->pool->lock, flags);
			if (pwq->nr_active || !list_empty(&pwq->delayed_works))
				show_pwq(pwq);
			raw_spin_unlock_irqrestore(&pwq->pool->lock, flags);
			/*
			 * We could be printing a lot from atomic context, e.g.
			 * sysrq-t -> show_workqueue_state(). Avoid triggering
			 * hard lockup.
			 */
			touch_nmi_watchdog();
		}
	}

	for_each_pool(pool, pi) {
		struct worker *worker;
		bool first = true;

		raw_spin_lock_irqsave(&pool->lock, flags);
		if (pool->nr_workers == pool->nr_idle)
			goto next_pool;

		pr_info("pool %d:", pool->id);
		pr_cont_pool_info(pool);
		pr_cont(" hung=%us workers=%d",
			jiffies_to_msecs(jiffies - pool->watchdog_ts) / 1000,
			pool->nr_workers);
		if (pool->manager)
			pr_cont(" manager: %d",
				task_pid_nr(pool->manager->task));
		list_for_each_entry(worker, &pool->idle_list, entry) {
			pr_cont(" %s%d", first ? "idle: " : "",
				task_pid_nr(worker->task));
			first = false;
		}
		pr_cont("\n");
	next_pool:
		raw_spin_unlock_irqrestore(&pool->lock, flags);
		/*
		 * We could be printing a lot from atomic context, e.g.
		 * sysrq-t -> show_workqueue_state(). Avoid triggering
		 * hard lockup.
		 */
		touch_nmi_watchdog();
	}

	rcu_read_unlock();
}

/**
 * wq_worker_comm - 生成worker任务的进程名字符串
 * @buf: 输出缓冲区
 * @size: 缓冲区大小
 * @task: 目标任务结构体
 *
 * 用于/proc/PID/{comm,stat,status}接口，先写入实际comm名，
 * 若任务是worker且有desc，则追加'+'（正在执行）或'-'（上次执行）
 * 前缀的描述字符串，便于通过ps等工具查看worker当前处理的工作内容。
 */
/* used to show worker information through /proc/PID/{comm,stat,status} */
void wq_worker_comm(char *buf, size_t size, struct task_struct *task)
{
	int off;

	/* always show the actual comm */
	off = strscpy(buf, task->comm, size);
	if (off < 0)
		return;

	/* stabilize PF_WQ_WORKER and worker pool association */
	mutex_lock(&wq_pool_attach_mutex);

	if (task->flags & PF_WQ_WORKER) {
		struct worker *worker = kthread_data(task);
		struct worker_pool *pool = worker->pool;

		if (pool) {
			raw_spin_lock_irq(&pool->lock);
			/*
			 * ->desc tracks information (wq name or
			 * set_worker_desc()) for the latest execution.  If
			 * current, prepend '+', otherwise '-'.
			 */
			if (worker->desc[0] != '\0') {
				if (worker->current_work)
					scnprintf(buf + off, size - off, "+%s",
						  worker->desc);
				else
					scnprintf(buf + off, size - off, "-%s",
						  worker->desc);
			}
			raw_spin_unlock_irq(&pool->lock);
		}
	}

	mutex_unlock(&wq_pool_attach_mutex);
}

#ifdef CONFIG_SMP

/*
 * CPU hotplug.
 *
 * There are two challenges in supporting CPU hotplug.  Firstly, there
 * are a lot of assumptions on strong associations among work, pwq and
 * pool which make migrating pending and scheduled works very
 * difficult to implement without impacting hot paths.  Secondly,
 * worker pools serve mix of short, long and very long running works making
 * blocked draining impractical.
 *
 * This is solved by allowing the pools to be disassociated from the CPU
 * running as an unbound one and allowing it to be reattached later if the
 * cpu comes back online.
 */

/**
 * unbind_workers - CPU下线时解除该CPU所有worker的绑定
 * @cpu: 下线的CPU编号
 *
 * 遍历该CPU的所有worker_pool，将所有worker打上WORKER_UNBOUND标志，
 * 并置POOL_DISASSOCIATED，随后清零nr_running关闭并发管理，
 * 使pool以unbound方式继续处理work。调用schedule()穿越rq->lock，
 * 确保调度回调能看到WORKER_UNBOUND标志。最后唤醒worker以避免stall。
 */
static void unbind_workers(int cpu)
{
	struct worker_pool *pool;
	struct worker *worker;

	for_each_cpu_worker_pool(pool, cpu) {
		mutex_lock(&wq_pool_attach_mutex);
		raw_spin_lock_irq(&pool->lock);

		/*
		 * We've blocked all attach/detach operations. Make all workers
		 * unbound and set DISASSOCIATED.  Before this, all workers
		 * except for the ones which are still executing works from
		 * before the last CPU down must be on the cpu.  After
		 * this, they may become diasporas.
		 */
		for_each_pool_worker(worker, pool)
			worker->flags |= WORKER_UNBOUND;

		pool->flags |= POOL_DISASSOCIATED;

		raw_spin_unlock_irq(&pool->lock);
		mutex_unlock(&wq_pool_attach_mutex);

		/*
		 * Call schedule() so that we cross rq->lock and thus can
		 * guarantee sched callbacks see the %WORKER_UNBOUND flag.
		 * This is necessary as scheduler callbacks may be invoked
		 * from other cpus.
		 */
		schedule();

		/*
		 * Sched callbacks are disabled now.  Zap nr_running.
		 * After this, nr_running stays zero and need_more_worker()
		 * and keep_working() are always true as long as the
		 * worklist is not empty.  This pool now behaves as an
		 * unbound (in terms of concurrency management) pool which
		 * are served by workers tied to the pool.
		 */
		atomic_set(&pool->nr_running, 0);

		/*
		 * With concurrency management just turned off, a busy
		 * worker blocking could lead to lengthy stalls.  Kick off
		 * unbound chain execution of currently pending work items.
		 */
		raw_spin_lock_irq(&pool->lock);
		wake_up_worker(pool);
		raw_spin_unlock_irq(&pool->lock);
	}
}

/**
 * rebind_workers - CPU上线时将pool中所有worker重新绑定到该CPU
 * @pool: 目标worker_pool（其关联CPU正在上线）
 *
 * 先恢复所有worker的CPU亲和性，再清除POOL_DISASSOCIATED标志，
 * 然后将WORKER_UNBOUND替换为WORKER_REBOUND，唤醒所有空闲worker
 * 让其迁移到上线CPU。worker在下次执行周期开始时通过worker_clr_flags()
 * 清除REBOUND并恢复并发管理。需持wq_pool_attach_mutex调用。
 */
static void rebind_workers(struct worker_pool *pool)
{
	struct worker *worker;

	lockdep_assert_held(&wq_pool_attach_mutex);

	/*
	 * Restore CPU affinity of all workers.  As all idle workers should
	 * be on the run-queue of the associated CPU before any local
	 * wake-ups for concurrency management happen, restore CPU affinity
	 * of all workers first and then clear UNBOUND.  As we're called
	 * from CPU_ONLINE, the following shouldn't fail.
	 */
	for_each_pool_worker(worker, pool)
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task,
						  pool->attrs->cpumask) < 0);

	raw_spin_lock_irq(&pool->lock);

	pool->flags &= ~POOL_DISASSOCIATED;

	for_each_pool_worker(worker, pool) {
		unsigned int worker_flags = worker->flags;

		/*
		 * A bound idle worker should actually be on the runqueue
		 * of the associated CPU for local wake-ups targeting it to
		 * work.  Kick all idle workers so that they migrate to the
		 * associated CPU.  Doing this in the same loop as
		 * replacing UNBOUND with REBOUND is safe as no worker will
		 * be bound before @pool->lock is released.
		 */
		if (worker_flags & WORKER_IDLE)
			wake_up_process(worker->task);

		/*
		 * We want to clear UNBOUND but can't directly call
		 * worker_clr_flags() or adjust nr_running.  Atomically
		 * replace UNBOUND with another NOT_RUNNING flag REBOUND.
		 * @worker will clear REBOUND using worker_clr_flags() when
		 * it initiates the next execution cycle thus restoring
		 * concurrency management.  Note that when or whether
		 * @worker clears REBOUND doesn't affect correctness.
		 *
		 * WRITE_ONCE() is necessary because @worker->flags may be
		 * tested without holding any lock in
		 * wq_worker_running().  Without it, NOT_RUNNING test may
		 * fail incorrectly leading to premature concurrency
		 * management operations.
		 */
		WARN_ON_ONCE(!(worker_flags & WORKER_UNBOUND));
		worker_flags |= WORKER_REBOUND;
		worker_flags &= ~WORKER_UNBOUND;
		WRITE_ONCE(worker->flags, worker_flags);
	}

	raw_spin_unlock_irq(&pool->lock);
}

/**
 * restore_unbound_workers_cpumask - restore cpumask of unbound workers
 * @pool: unbound pool of interest
 * @cpu: the CPU which is coming up
 *
 * An unbound pool may end up with a cpumask which doesn't have any online
 * CPUs.  When a worker of such pool get scheduled, the scheduler resets
 * its cpus_allowed.  If @cpu is in @pool's cpumask which didn't have any
 * online CPU before, cpus_allowed of all its workers should be restored.
 */
/**
 * restore_unbound_workers_cpumask - 恢复unbound worker的cpumask绑定
 * @pool: 目标unbound worker_pool
 * @cpu: 正在上线的CPU
 *
 * unbound pool的cpumask可能不包含任何在线CPU，此时调度器会重置
 * worker的cpus_allowed。当@cpu上线且属于@pool的cpumask范围时，
 * 将pool中所有worker的cpus_allowed恢复为pool->attrs->cpumask与
 * cpu_online_mask的交集，需持wq_pool_attach_mutex调用。
 */
static void restore_unbound_workers_cpumask(struct worker_pool *pool, int cpu)
{
	static cpumask_t cpumask;
	struct worker *worker;

	lockdep_assert_held(&wq_pool_attach_mutex);

	/* is @cpu allowed for @pool? */
	if (!cpumask_test_cpu(cpu, pool->attrs->cpumask))
		return;

	cpumask_and(&cpumask, pool->attrs->cpumask, cpu_online_mask);

	/* as we're called from CPU_ONLINE, the following shouldn't fail */
	for_each_pool_worker(worker, pool)
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, &cpumask) < 0);
}

/**
 * workqueue_prepare_cpu - CPU上线前的工作队列准备（热插拔回调）
 * @cpu: 即将上线的CPU编号
 *
 * 在CPU进入online状态之前调用，确保该CPU的每个worker_pool至少有
 * 一个worker线程，若pool为空则创建初始worker，失败返回-ENOMEM。
 */
int workqueue_prepare_cpu(unsigned int cpu)
{
	struct worker_pool *pool;

	for_each_cpu_worker_pool(pool, cpu) {
		if (pool->nr_workers)
			continue;
		if (!create_worker(pool))
			return -ENOMEM;
	}
	return 0;
}

/**
 * workqueue_online_cpu - CPU上线时的工作队列处理（热插拔回调）
 * @cpu: 上线的CPU编号
 *
 * 遍历所有worker_pool：对绑定到该CPU的pool调用rebind_workers()重新绑定；
 * 对unbound pool调用restore_unbound_workers_cpumask()恢复cpumask；
 * 然后更新所有unbound工作队列的NUMA亲和性映射（wq_update_unbound_numa）。
 */
int workqueue_online_cpu(unsigned int cpu)
{
	struct worker_pool *pool;
	struct workqueue_struct *wq;
	int pi;

	mutex_lock(&wq_pool_mutex);

	for_each_pool(pool, pi) {
		mutex_lock(&wq_pool_attach_mutex);

		if (pool->cpu == cpu)
			rebind_workers(pool);
		else if (pool->cpu < 0)
			restore_unbound_workers_cpumask(pool, cpu);

		mutex_unlock(&wq_pool_attach_mutex);
	}

	/* update NUMA affinity of unbound workqueues */
	list_for_each_entry(wq, &workqueues, list)
		wq_update_unbound_numa(wq, cpu, true);

	mutex_unlock(&wq_pool_mutex);
	return 0;
}

/**
 * workqueue_offline_cpu - CPU下线时的工作队列处理（热插拔回调）
 * @cpu: 下线的CPU编号（必须是当前CPU）
 *
 * 必须在目标CPU本地执行（unbind_workers要求），解绑该CPU所有worker，
 * 然后更新所有unbound工作队列的NUMA亲和性映射，移除对下线CPU的依赖。
 */
int workqueue_offline_cpu(unsigned int cpu)
{
	struct workqueue_struct *wq;

	/* unbinding per-cpu workers should happen on the local CPU */
	if (WARN_ON(cpu != smp_processor_id()))
		return -1;

	unbind_workers(cpu);

	/* update NUMA affinity of unbound workqueues */
	mutex_lock(&wq_pool_mutex);
	list_for_each_entry(wq, &workqueues, list)
		wq_update_unbound_numa(wq, cpu, false);
	mutex_unlock(&wq_pool_mutex);

	return 0;
}

struct work_for_cpu {
	struct work_struct work;
	long (*fn)(void *);
	void *arg;
	long ret;
};

/**
 * work_for_cpu_fn - 在目标CPU上执行指定函数的work回调
 * @work: 嵌入在work_for_cpu结构中的work_struct
 *
 * 从work_for_cpu容器中取出fn和arg，执行并将返回值存入ret，
 * 供work_on_cpu()和work_on_cpu_safe()通过flush_work()同步获取结果。
 */
static void work_for_cpu_fn(struct work_struct *work)
{
	struct work_for_cpu *wfc = container_of(work, struct work_for_cpu, work);

	wfc->ret = wfc->fn(wfc->arg);
}

/**
 * work_on_cpu - run a function in thread context on a particular cpu
 * @cpu: the cpu to run on
 * @fn: the function to run
 * @arg: the function arg
 *
 * It is up to the caller to ensure that the cpu doesn't go offline.
 * The caller must not hold any locks which would prevent @fn from completing.
 *
 * Return: The value @fn returns.
 */
/**
 * work_on_cpu - 在指定CPU的线程上下文中运行函数
 * @cpu: 目标CPU编号
 * @fn: 要执行的函数指针
 * @arg: 传给@fn的参数
 *
 * 将@fn封装成栈上work，调度到@cpu上执行，同步等待完成后返回@fn的返回值。
 * 调用者需确保@cpu不会下线，且不能持有会阻止@fn完成的锁。
 *
 * Return: @fn的返回值。
 */
long work_on_cpu(int cpu, long (*fn)(void *), void *arg)
{
	struct work_for_cpu wfc = { .fn = fn, .arg = arg };

	INIT_WORK_ONSTACK(&wfc.work, work_for_cpu_fn);
	schedule_work_on(cpu, &wfc.work);
	flush_work(&wfc.work);
	destroy_work_on_stack(&wfc.work);
	return wfc.ret;
}
EXPORT_SYMBOL_GPL(work_on_cpu);

/**
 * work_on_cpu_safe - run a function in thread context on a particular cpu
 * @cpu: the cpu to run on
 * @fn:  the function to run
 * @arg: the function argument
 *
 * Disables CPU hotplug and calls work_on_cpu(). The caller must not hold
 * any locks which would prevent @fn from completing.
 *
 * Return: The value @fn returns.
 */
/**
 * work_on_cpu_safe - 安全地在指定CPU的线程上下文中运行函数
 * @cpu: 目标CPU编号
 * @fn: 要执行的函数指针
 * @arg: 传给@fn的参数
 *
 * 在持有cpu_hotplug锁（get_online_cpus）的情况下调用work_on_cpu()，
 * 防止执行期间CPU下线。若@cpu已离线则返回-ENODEV。
 * 调用者不能持有会阻止@fn完成的锁。
 *
 * Return: @fn的返回值，或-ENODEV（CPU已离线）。
 */
long work_on_cpu_safe(int cpu, long (*fn)(void *), void *arg)
{
	long ret = -ENODEV;

	get_online_cpus();
	if (cpu_online(cpu))
		ret = work_on_cpu(cpu, fn, arg);
	put_online_cpus();
	return ret;
}
EXPORT_SYMBOL_GPL(work_on_cpu_safe);
#endif /* CONFIG_SMP */

#ifdef CONFIG_FREEZER

/**
 * freeze_workqueues_begin - 开始冻结所有可冻结工作队列
 *
 * 设置workqueue_freezing标志，然后对所有工作队列调用pwq_adjust_max_active()，
 * 使WQ_FREEZABLE队列将新work放入delayed_works而不是立即执行，
 * 为系统挂起/休眠做准备。需持wq_pool_mutex、wq->mutex和pool->lock。
 */
void freeze_workqueues_begin(void)
{
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(workqueue_freezing);
	workqueue_freezing = true;

	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		for_each_pwq(pwq, wq)
			pwq_adjust_max_active(pwq);
		mutex_unlock(&wq->mutex);
	}

	mutex_unlock(&wq_pool_mutex);
}

/**
 * freeze_workqueues_busy - 检查可冻结工作队列是否仍有work在执行
 *
 * 必须在freeze_workqueues_begin()之后、thaw_workqueues()之前调用。
 * 遍历所有WQ_FREEZABLE工作队列，若任一pwq的nr_active不为零则返回true，
 * 表示冻结尚未完成。nr_active单调递减，可在RCU读锁下无锁检查。
 *
 * Return: %true表示仍有忙碌的可冻结工作队列，%false表示冻结完成。
 */
bool freeze_workqueues_busy(void)
{
	bool busy = false;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(!workqueue_freezing);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_FREEZABLE))
			continue;
		/*
		 * nr_active is monotonically decreasing.  It's safe
		 * to peek without lock.
		 */
		rcu_read_lock();
		for_each_pwq(pwq, wq) {
			WARN_ON_ONCE(pwq->nr_active < 0);
			if (pwq->nr_active) {
				busy = true;
				rcu_read_unlock();
				goto out_unlock;
			}
		}
		rcu_read_unlock();
	}
out_unlock:
	mutex_unlock(&wq_pool_mutex);
	return busy;
}

/**
 * thaw_workqueues - thaw workqueues
 *
 * Thaw workqueues.  Normal queueing is restored and all collected
 * frozen works are transferred to their respective pool worklists.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
/**
 * thaw_workqueues - 解冻所有可冻结工作队列
 *
 * 清除workqueue_freezing标志，然后对所有工作队列调用pwq_adjust_max_active()，
 * 恢复max_active并将delayed_works中积压的work重新推入worklist执行，
 * 在系统从挂起/休眠恢复时调用。
 */
void thaw_workqueues(void)
{
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	if (!workqueue_freezing)
		goto out_unlock;

	workqueue_freezing = false;

	/* restore max_active and repopulate worklist */
	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		for_each_pwq(pwq, wq)
			pwq_adjust_max_active(pwq);
		mutex_unlock(&wq->mutex);
	}

out_unlock:
	mutex_unlock(&wq_pool_mutex);
}
#endif /* CONFIG_FREEZER */

/**
 * workqueue_apply_unbound_cpumask - 将wq_unbound_cpumask应用到所有unbound工作队列
 *
 * 遍历所有WQ_UNBOUND且非__WQ_ORDERED的工作队列，以当前wq_unbound_cpumask
 * 为基础通过apply_wqattrs_prepare()准备新pwq配置，全部准备好后批量提交。
 * 若任一prepare失败则放弃所有提交，返回错误码。需持wq_pool_mutex调用。
 */
static int workqueue_apply_unbound_cpumask(void)
{
	LIST_HEAD(ctxs);
	int ret = 0;
	struct workqueue_struct *wq;
	struct apply_wqattrs_ctx *ctx, *n;

	lockdep_assert_held(&wq_pool_mutex);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_UNBOUND))
			continue;
		/* creating multiple pwqs breaks ordering guarantee */
		if (wq->flags & __WQ_ORDERED)
			continue;

		ctx = apply_wqattrs_prepare(wq, wq->unbound_attrs);
		if (!ctx) {
			ret = -ENOMEM;
			break;
		}

		list_add_tail(&ctx->list, &ctxs);
	}

	list_for_each_entry_safe(ctx, n, &ctxs, list) {
		if (!ret)
			apply_wqattrs_commit(ctx);
		apply_wqattrs_cleanup(ctx);
	}

	return ret;
}

/**
 * workqueue_set_unbound_cpumask - 设置全局unbound工作队列的cpumask
 * @cpumask: 新的cpumask（与cpu_possible_mask取交集后生效）
 *
 * wq_unbound_cpumask是限制所有unbound工作队列CPU亲和性的全局掩码。
 * 先保存旧值，更新后调用workqueue_apply_unbound_cpumask()应用到全部unbound wq；
 * 若应用失败则回滚到旧值。允许包含isolated CPU。
 *
 * Return: 0成功，-EINVAL cpumask为空，-ENOMEM内存不足。
 */
int workqueue_set_unbound_cpumask(cpumask_var_t cpumask)
{
	int ret = -EINVAL;
	cpumask_var_t saved_cpumask;

	if (!zalloc_cpumask_var(&saved_cpumask, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * Not excluding isolated cpus on purpose.
	 * If the user wishes to include them, we allow that.
	 */
	cpumask_and(cpumask, cpumask, cpu_possible_mask);
	if (!cpumask_empty(cpumask)) {
		apply_wqattrs_lock();

		/* save the old wq_unbound_cpumask. */
		cpumask_copy(saved_cpumask, wq_unbound_cpumask);

		/* update wq_unbound_cpumask at first and apply it to wqs. */
		cpumask_copy(wq_unbound_cpumask, cpumask);
		ret = workqueue_apply_unbound_cpumask();

		/* restore the wq_unbound_cpumask when failed. */
		if (ret < 0)
			cpumask_copy(wq_unbound_cpumask, saved_cpumask);

		apply_wqattrs_unlock();
	}

	free_cpumask_var(saved_cpumask);
	return ret;
}

#ifdef CONFIG_SYSFS
/*
 * Workqueues with WQ_SYSFS flag set is visible to userland via
 * /sys/bus/workqueue/devices/WQ_NAME.  All visible workqueues have the
 * following attributes.
 *
 *  per_cpu	RO bool	: whether the workqueue is per-cpu or unbound
 *  max_active	RW int	: maximum number of in-flight work items
 *
 * Unbound workqueues have the following extra attributes.
 *
 *  pool_ids	RO int	: the associated pool IDs for each node
 *  nice	RW int	: nice value of the workers
 *  cpumask	RW mask	: bitmask of allowed CPUs for the workers
 *  numa	RW bool	: whether enable NUMA affinity
 */
struct wq_device {
	struct workqueue_struct		*wq;
	struct device			dev;
};

/**
 * dev_to_wq - 从sysfs device获取关联的workqueue_struct
 * @dev: sysfs设备对象
 *
 * 通过container_of从wq_device结构取出对应的workqueue_struct指针，
 * 供各sysfs属性show/store回调使用。
 */
static struct workqueue_struct *dev_to_wq(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	return wq_dev->wq;
}

static ssize_t per_cpu_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", (bool)!(wq->flags & WQ_UNBOUND));
}
static DEVICE_ATTR_RO(per_cpu);

/**
 * max_active_show - 读取工作队列max_active属性
 * @dev: sysfs device对象
 * @attr: 设备属性描述符
 * @buf: 输出缓冲区
 *
 * 返回wq->saved_max_active当前值（字符串形式），对应
 * /sys/bus/workqueue/devices/WQ_NAME/max_active。
 */
static ssize_t max_active_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", wq->saved_max_active);
}

/**
 * max_active_store - 写入工作队列max_active属性
 * @dev: sysfs device对象
 * @attr: 设备属性描述符
 * @buf: 用户写入的字符串
 * @count: 字符串长度
 *
 * 解析正整数并调用workqueue_set_max_active()动态修改wq的并发度上限。
 */
static ssize_t max_active_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int val;

	if (sscanf(buf, "%d", &val) != 1 || val <= 0)
		return -EINVAL;

	workqueue_set_max_active(wq, val);
	return count;
}
static DEVICE_ATTR_RW(max_active);

static struct attribute *wq_sysfs_attrs[] = {
	&dev_attr_per_cpu.attr,
	&dev_attr_max_active.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wq_sysfs);

static ssize_t wq_pool_ids_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	const char *delim = "";
	int node, written = 0;

	get_online_cpus();
	rcu_read_lock();
	for_each_node(node) {
		written += scnprintf(buf + written, PAGE_SIZE - written,
				     "%s%d:%d", delim, node,
				     unbound_pwq_by_node(wq, node)->pool->id);
		delim = " ";
	}
	written += scnprintf(buf + written, PAGE_SIZE - written, "\n");
	rcu_read_unlock();
	put_online_cpus();

	return written;
}

static ssize_t wq_nice_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%d\n", wq->unbound_attrs->nice);
	mutex_unlock(&wq->mutex);

	return written;
}

/**
 * wq_sysfs_prep_attrs - 为sysfs store操作准备workqueue_attrs副本
 * @wq: 目标工作队列
 *
 * 分配新attrs并拷贝wq->unbound_attrs当前值，供wq_nice_store/
 * wq_cpumask_store等sysfs写回调修改后通过apply_workqueue_attrs()应用。
 * 需持wq_pool_mutex调用。返回NULL表示内存分配失败。
 */
/* prepare workqueue_attrs for sysfs store operations */
static struct workqueue_attrs *wq_sysfs_prep_attrs(struct workqueue_struct *wq)
{
	struct workqueue_attrs *attrs;

	lockdep_assert_held(&wq_pool_mutex);

	attrs = alloc_workqueue_attrs();
	if (!attrs)
		return NULL;

	copy_workqueue_attrs(attrs, wq->unbound_attrs);
	return attrs;
}

static ssize_t wq_nice_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	if (sscanf(buf, "%d", &attrs->nice) == 1 &&
	    attrs->nice >= MIN_NICE && attrs->nice <= MAX_NICE)
		ret = apply_workqueue_attrs_locked(wq, attrs);
	else
		ret = -EINVAL;

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_cpumask_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq->unbound_attrs->cpumask));
	mutex_unlock(&wq->mutex);
	return written;
}

static ssize_t wq_cpumask_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	ret = cpumask_parse(buf, attrs->cpumask);
	if (!ret)
		ret = apply_workqueue_attrs_locked(wq, attrs);

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_numa_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%d\n",
			    !wq->unbound_attrs->no_numa);
	mutex_unlock(&wq->mutex);

	return written;
}

static ssize_t wq_numa_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int v, ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	ret = -EINVAL;
	if (sscanf(buf, "%d", &v) == 1) {
		attrs->no_numa = !v;
		ret = apply_workqueue_attrs_locked(wq, attrs);
	}

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static struct device_attribute wq_sysfs_unbound_attrs[] = {
	__ATTR(pool_ids, 0444, wq_pool_ids_show, NULL),
	__ATTR(nice, 0644, wq_nice_show, wq_nice_store),
	__ATTR(cpumask, 0644, wq_cpumask_show, wq_cpumask_store),
	__ATTR(numa, 0644, wq_numa_show, wq_numa_store),
	__ATTR_NULL,
};

static struct bus_type wq_subsys = {
	.name				= "workqueue",
	.dev_groups			= wq_sysfs_groups,
};

static ssize_t wq_unbound_cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int written;

	mutex_lock(&wq_pool_mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq_unbound_cpumask));
	mutex_unlock(&wq_pool_mutex);

	return written;
}

static ssize_t wq_unbound_cpumask_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	cpumask_var_t cpumask;
	int ret;

	if (!zalloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpumask_parse(buf, cpumask);
	if (!ret)
		ret = workqueue_set_unbound_cpumask(cpumask);

	free_cpumask_var(cpumask);
	return ret ? ret : count;
}

static struct device_attribute wq_sysfs_cpumask_attr =
	__ATTR(cpumask, 0644, wq_unbound_cpumask_show,
	       wq_unbound_cpumask_store);

/**
 * wq_sysfs_init - 初始化workqueue sysfs子系统
 *
 * 作为core_initcall注册workqueue虚拟总线（/sys/bus/workqueue），
 * 并在总线根设备上创建全局unbound cpumask属性文件，
 * 供用户空间通过sysfs查看和修改全局unbound工作队列CPU亲和性。
 */
static int __init wq_sysfs_init(void)
{
	int err;

	err = subsys_virtual_register(&wq_subsys, NULL);
	if (err)
		return err;

	return device_create_file(wq_subsys.dev_root, &wq_sysfs_cpumask_attr);
}
core_initcall(wq_sysfs_init);

/**
 * wq_device_release - 释放wq_device结构体
 * @dev: 对应的sysfs device对象
 *
 * 当wq_device的引用计数归零时由设备框架调用，释放包含dev的wq_device内存。
 */
static void wq_device_release(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	kfree(wq_dev);
}

/**
 * workqueue_sysfs_register - 将工作队列注册到sysfs
 * @wq: 要注册的工作队列
 *
 * 在/sys/bus/workqueue/devices/下暴露@wq，供用户空间读写属性。
 * alloc_workqueue*()在设置WQ_SYSFS时自动调用；若需在注册前先应用
 * workqueue_attrs，则应直接调用本函数，避免用户空间与内核属性应用竞争。
 * __WQ_ORDERED_EXPLICIT队列禁止注册（防止max_active被修改破坏顺序保证）。
 *
 * Return: 0成功，-errno失败。
 */
int workqueue_sysfs_register(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev;
	int ret;

	/*
	 * Adjusting max_active or creating new pwqs by applying
	 * attributes breaks ordering guarantee.  Disallow exposing ordered
	 * workqueues.
	 */
	if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
		return -EINVAL;

	wq->wq_dev = wq_dev = kzalloc(sizeof(*wq_dev), GFP_KERNEL);
	if (!wq_dev)
		return -ENOMEM;

	wq_dev->wq = wq;
	wq_dev->dev.bus = &wq_subsys;
	wq_dev->dev.release = wq_device_release;
	dev_set_name(&wq_dev->dev, "%s", wq->name);

	/*
	 * unbound_attrs are created separately.  Suppress uevent until
	 * everything is ready.
	 */
	dev_set_uevent_suppress(&wq_dev->dev, true);

	ret = device_register(&wq_dev->dev);
	if (ret) {
		put_device(&wq_dev->dev);
		wq->wq_dev = NULL;
		return ret;
	}

	if (wq->flags & WQ_UNBOUND) {
		struct device_attribute *attr;

		for (attr = wq_sysfs_unbound_attrs; attr->attr.name; attr++) {
			ret = device_create_file(&wq_dev->dev, attr);
			if (ret) {
				device_unregister(&wq_dev->dev);
				wq->wq_dev = NULL;
				return ret;
			}
		}
	}

	dev_set_uevent_suppress(&wq_dev->dev, false);
	kobject_uevent(&wq_dev->dev.kobj, KOBJ_ADD);
	return 0;
}

/**
 * workqueue_sysfs_unregister - 从sysfs注销工作队列
 * @wq: 要注销的工作队列
 *
 * 若@wq已通过workqueue_sysfs_register()注册，则将其从sysfs移除。
 * 在destroy_workqueue()中调用，清理wq_dev设备对象。
 */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev = wq->wq_dev;

	if (!wq->wq_dev)
		return;

	wq->wq_dev = NULL;
	device_unregister(&wq_dev->dev);
}
#else	/* CONFIG_SYSFS */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)	{ }
#endif	/* CONFIG_SYSFS */

/*
 * Workqueue watchdog.
 *
 * Stall may be caused by various bugs - missing WQ_MEM_RECLAIM, illegal
 * flush dependency, a concurrency managed work item which stays RUNNING
 * indefinitely.  Workqueue stalls can be very difficult to debug as the
 * usual warning mechanisms don't trigger and internal workqueue state is
 * largely opaque.
 *
 * Workqueue watchdog monitors all worker pools periodically and dumps
 * state if some pools failed to make forward progress for a while where
 * forward progress is defined as the first item on ->worklist changing.
 *
 * This mechanism is controlled through the kernel parameter
 * "workqueue.watchdog_thresh" which can be updated at runtime through the
 * corresponding sysfs parameter file.
 */
#ifdef CONFIG_WQ_WATCHDOG

static unsigned long wq_watchdog_thresh = 30;
static struct timer_list wq_watchdog_timer;

static unsigned long wq_watchdog_touched = INITIAL_JIFFIES;
static DEFINE_PER_CPU(unsigned long, wq_watchdog_touched_cpu) = INITIAL_JIFFIES;

/**
 * wq_watchdog_reset_touched - 重置watchdog的所有touched时间戳
 *
 * 将全局wq_watchdog_touched和每个CPU的wq_watchdog_touched_cpu
 * 都更新为当前jiffies，防止watchdog误报stall。
 */
static void wq_watchdog_reset_touched(void)
{
	int cpu;

	wq_watchdog_touched = jiffies;
	for_each_possible_cpu(cpu)
		per_cpu(wq_watchdog_touched_cpu, cpu) = jiffies;
}

/**
 * wq_watchdog_timer_fn - workqueue watchdog定时器回调
 * @unused: 未使用的timer_list指针
 *
 * 周期性检查所有非空的worker_pool是否在阈值时间内有进展
 * （worklist首项发生变化）。综合pool->watchdog_ts、全局touched和
 * per-CPU touched三个时间戳取最新值，若超过阈值则打印"BUG: workqueue lockup"
 * 并调用show_workqueue_state()转储状态，随后重置时间戳并重新arm定时器。
 */
static void wq_watchdog_timer_fn(struct timer_list *unused)
{
	unsigned long thresh = READ_ONCE(wq_watchdog_thresh) * HZ;
	bool lockup_detected = false;
	struct worker_pool *pool;
	int pi;

	if (!thresh)
		return;

	rcu_read_lock();

	for_each_pool(pool, pi) {
		unsigned long pool_ts, touched, ts;

		if (list_empty(&pool->worklist))
			continue;

		/* get the latest of pool and touched timestamps */
		pool_ts = READ_ONCE(pool->watchdog_ts);
		touched = READ_ONCE(wq_watchdog_touched);

		if (time_after(pool_ts, touched))
			ts = pool_ts;
		else
			ts = touched;

		if (pool->cpu >= 0) {
			unsigned long cpu_touched =
				READ_ONCE(per_cpu(wq_watchdog_touched_cpu,
						  pool->cpu));
			if (time_after(cpu_touched, ts))
				ts = cpu_touched;
		}

		/* did we stall? */
		if (time_after(jiffies, ts + thresh)) {
			lockup_detected = true;
			pr_emerg("BUG: workqueue lockup - pool");
			pr_cont_pool_info(pool);
			pr_cont(" stuck for %us!\n",
				jiffies_to_msecs(jiffies - pool_ts) / 1000);
		}
	}

	rcu_read_unlock();

	if (lockup_detected)
		show_workqueue_state();

	wq_watchdog_reset_touched();
	mod_timer(&wq_watchdog_timer, jiffies + thresh);
}

/**
 * wq_watchdog_touch - 更新watchdog的活跃时间戳（防误报）
 * @cpu: 触摸的CPU编号，负数表示更新全局时间戳
 *
 * 在worker有实际进展时调用（由wq_worker_running等路径触发），
 * 更新对应CPU或全局的touched时间戳，告知watchdog系统仍在正常运行。
 */
notrace void wq_watchdog_touch(int cpu)
{
	if (cpu >= 0)
		per_cpu(wq_watchdog_touched_cpu, cpu) = jiffies;
	else
		wq_watchdog_touched = jiffies;
}

/**
 * wq_watchdog_set_thresh - 设置watchdog超时阈值并重置定时器
 * @thresh: 新阈值（秒），0表示禁用watchdog
 *
 * 先将wq_watchdog_thresh置0并删除定时器，若新阈值非零则重新设置阈值、
 * 重置所有touched时间戳并重新arm定时器，使新阈值立即生效。
 */
static void wq_watchdog_set_thresh(unsigned long thresh)
{
	wq_watchdog_thresh = 0;
	del_timer_sync(&wq_watchdog_timer);

	if (thresh) {
		wq_watchdog_thresh = thresh;
		wq_watchdog_reset_touched();
		mod_timer(&wq_watchdog_timer, jiffies + thresh * HZ);
	}
}

/**
 * wq_watchdog_param_set_thresh - module_param写回调：设置watchdog阈值
 * @val: 用户写入的字符串值
 * @kp: kernel_param描述符
 *
 * 将字符串解析为无符号长整数，若workqueue子系统已初始化则调用
 * wq_watchdog_set_thresh()使其立即生效，否则仅更新全局变量供启动时使用。
 */
static int wq_watchdog_param_set_thresh(const char *val,
					const struct kernel_param *kp)
{
	unsigned long thresh;
	int ret;

	ret = kstrtoul(val, 0, &thresh);
	if (ret)
		return ret;

	if (system_wq)
		wq_watchdog_set_thresh(thresh);
	else
		wq_watchdog_thresh = thresh;

	return 0;
}

static const struct kernel_param_ops wq_watchdog_thresh_ops = {
	.set	= wq_watchdog_param_set_thresh,
	.get	= param_get_ulong,
};

module_param_cb(watchdog_thresh, &wq_watchdog_thresh_ops, &wq_watchdog_thresh,
		0644);

/**
 * wq_watchdog_init - 初始化workqueue watchdog定时器
 *
 * 设置TIMER_DEFERRABLE定时器并以默认阈值（wq_watchdog_thresh秒）
 * 启动周期性检测，在workqueue子系统初始化时调用。
 */
static void wq_watchdog_init(void)
{
	timer_setup(&wq_watchdog_timer, wq_watchdog_timer_fn, TIMER_DEFERRABLE);
	wq_watchdog_set_thresh(wq_watchdog_thresh);
}

#else	/* CONFIG_WQ_WATCHDOG */

static inline void wq_watchdog_init(void) { }

#endif	/* CONFIG_WQ_WATCHDOG */

/**
 * wq_numa_init - 初始化workqueue的NUMA亲和性支持
 *
 * 构建wq_numa_possible_cpumask表：为每个NUMA节点计算其可能的CPU掩码，
 * 基于cpu_to_node()映射关系填充。若系统只有一个NUMA节点或
 * wq_disable_numa为真则直接返回。失败或CPU-节点映射异常时禁用NUMA支持。
 * 在workqueue_init_early()之后、SMP初始化完成后调用。
 */
static void __init wq_numa_init(void)
{
	cpumask_var_t *tbl;
	int node, cpu;

	if (num_possible_nodes() <= 1)
		return;

	if (wq_disable_numa) {
		pr_info("workqueue: NUMA affinity support disabled\n");
		return;
	}

	wq_update_unbound_numa_attrs_buf = alloc_workqueue_attrs();
	BUG_ON(!wq_update_unbound_numa_attrs_buf);

	/*
	 * We want masks of possible CPUs of each node which isn't readily
	 * available.  Build one from cpu_to_node() which should have been
	 * fully initialized by now.
	 */
	tbl = kcalloc(nr_node_ids, sizeof(tbl[0]), GFP_KERNEL);
	BUG_ON(!tbl);

	for_each_node(node)
		BUG_ON(!zalloc_cpumask_var_node(&tbl[node], GFP_KERNEL,
				node_online(node) ? node : NUMA_NO_NODE));

	for_each_possible_cpu(cpu) {
		node = cpu_to_node(cpu);
		if (WARN_ON(node == NUMA_NO_NODE)) {
			pr_warn("workqueue: NUMA node mapping not available for cpu%d, disabling NUMA support\n", cpu);
			/* happens iff arch is bonkers, let's just proceed */
			return;
		}
		cpumask_set_cpu(cpu, tbl[node]);
	}

	wq_numa_possible_cpumask = tbl;
	wq_numa_enabled = true;
}

/**
 * workqueue_init_early - workqueue子系统早期初始化（第一阶段）
 *
 * 在内存分配、cpumask和IDR就绪后尽早调用。完成：
 * 1) 初始化所有per-CPU worker_pool并分配ID；
 * 2) 创建标准unbound和ordered wq的attrs模板；
 * 3) 创建系统内置工作队列（system_wq、system_highpri_wq等）。
 * 此后可创建工作队列和入队/取消work，但实际执行需等到kthread可创建时。
 */
void __init workqueue_init_early(void)
{
	int std_nice[NR_STD_WORKER_POOLS] = { 0, HIGHPRI_NICE_LEVEL };
	int hk_flags = HK_FLAG_DOMAIN | HK_FLAG_WQ;
	int i, cpu;

	BUILD_BUG_ON(__alignof__(struct pool_workqueue) < __alignof__(long long));

	BUG_ON(!alloc_cpumask_var(&wq_unbound_cpumask, GFP_KERNEL));
	cpumask_copy(wq_unbound_cpumask, housekeeping_cpumask(hk_flags));

	pwq_cache = KMEM_CACHE(pool_workqueue, SLAB_PANIC);

	/* initialize CPU pools */
	for_each_possible_cpu(cpu) {
		struct worker_pool *pool;

		i = 0;
		for_each_cpu_worker_pool(pool, cpu) {
			BUG_ON(init_worker_pool(pool));
			pool->cpu = cpu;
			cpumask_copy(pool->attrs->cpumask, cpumask_of(cpu));
			pool->attrs->nice = std_nice[i++];
			pool->node = cpu_to_node(cpu);

			/* alloc pool ID */
			mutex_lock(&wq_pool_mutex);
			BUG_ON(worker_pool_assign_id(pool));
			mutex_unlock(&wq_pool_mutex);
		}
	}

	/* create default unbound and ordered wq attrs */
	for (i = 0; i < NR_STD_WORKER_POOLS; i++) {
		struct workqueue_attrs *attrs;

		BUG_ON(!(attrs = alloc_workqueue_attrs()));
		attrs->nice = std_nice[i];
		unbound_std_wq_attrs[i] = attrs;

		/*
		 * An ordered wq should have only one pwq as ordering is
		 * guaranteed by max_active which is enforced by pwqs.
		 * Turn off NUMA so that dfl_pwq is used for all nodes.
		 */
		BUG_ON(!(attrs = alloc_workqueue_attrs()));
		attrs->nice = std_nice[i];
		attrs->no_numa = true;
		ordered_wq_attrs[i] = attrs;
	}

	system_wq = alloc_workqueue("events", 0, 0);
	system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	system_long_wq = alloc_workqueue("events_long", 0, 0);
	system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND,
					    WQ_UNBOUND_MAX_ACTIVE);
	system_freezable_wq = alloc_workqueue("events_freezable",
					      WQ_FREEZABLE, 0);
	system_power_efficient_wq = alloc_workqueue("events_power_efficient",
					      WQ_POWER_EFFICIENT, 0);
	system_freezable_power_efficient_wq = alloc_workqueue("events_freezable_power_efficient",
					      WQ_FREEZABLE | WQ_POWER_EFFICIENT,
					      0);
	BUG_ON(!system_wq || !system_highpri_wq || !system_long_wq ||
	       !system_unbound_wq || !system_freezable_wq ||
	       !system_power_efficient_wq ||
	       !system_freezable_power_efficient_wq);
}

/**
 * workqueue_init - workqueue子系统完全上线（第二阶段初始化）
 *
 * 在kthread可以被创建和调度后调用。此时工作队列已存在、work已入队，
 * 但尚无worker在执行。本函数完成：
 * 1) 调用wq_numa_init()初始化NUMA映射并修正per-CPU pool的节点提示；
 * 2) 为已有工作队列更新NUMA亲和性并创建rescuer线程（WQ_MEM_RECLAIM）；
 * 3) 为所有在线CPU的worker_pool和unbound pool创建初始worker；
 * 4) 置wq_online=true并启动watchdog定时器。
 */
void __init workqueue_init(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	int cpu, bkt;

	/*
	 * It'd be simpler to initialize NUMA in workqueue_init_early() but
	 * CPU to node mapping may not be available that early on some
	 * archs such as power and arm64.  As per-cpu pools created
	 * previously could be missing node hint and unbound pools NUMA
	 * affinity, fix them up.
	 *
	 * Also, while iterating workqueues, create rescuers if requested.
	 */
	wq_numa_init();

	mutex_lock(&wq_pool_mutex);

	for_each_possible_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			pool->node = cpu_to_node(cpu);
		}
	}

	list_for_each_entry(wq, &workqueues, list) {
		wq_update_unbound_numa(wq, smp_processor_id(), true);
		WARN(init_rescuer(wq),
		     "workqueue: failed to create early rescuer for %s",
		     wq->name);
	}

	mutex_unlock(&wq_pool_mutex);

	/* create the initial workers */
	for_each_online_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			pool->flags &= ~POOL_DISASSOCIATED;
			BUG_ON(!create_worker(pool));
		}
	}

	hash_for_each(unbound_pool_hash, bkt, pool, hash_node)
		BUG_ON(!create_worker(pool));

	wq_online = true;
	wq_watchdog_init();
}
