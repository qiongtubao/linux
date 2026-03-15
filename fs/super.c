// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/super.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  super.c contains code to handle: - mount structures
 *                                   - super-block tables
 *                                   - filesystem drivers list
 *                                   - mount system call
 *                                   - umount system call
 *                                   - ustat system call
 *
 * GK 2/5/95  -  Changed to support mounting the root fs via NFS
 *
 *  Added kerneld support: Jacques Gelinas and Bjorn Ekwall
 *  Added change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Added options to /proc/mounts:
 *    Torbjörn Lindh (torbjorn.lindh@gopta.se), April 14, 1996.
 *  Added devfs support: Richard Gooch <rgooch@atnf.csiro.au>, 13-JAN-1998
 *  Heavily rewritten for 'one fs - one tree' dcache architecture. AV, Mar 2000
 */

#include <linux/export.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/writeback.h>		/* for the emergency remount stuff */
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/backing-dev.h>
#include <linux/rculist_bl.h>
#include <linux/cleancache.h>
#include <linux/fscrypt.h>
#include <linux/fsnotify.h>
#include <linux/lockdep.h>
#include <linux/user_namespace.h>
#include <linux/fs_context.h>
#include <uapi/linux/mount.h>
#include "internal.h"

static int thaw_super_locked(struct super_block *sb);

static LIST_HEAD(super_blocks);
static DEFINE_SPINLOCK(sb_lock);

static char *sb_writers_name[SB_FREEZE_LEVELS] = {
	"sb_writers",
	"sb_pagefaults",
	"sb_internal",
};

/**
 * super_cache_scan - 超级块缓存扫描函数
 * @shrink: 收缩器结构
 * @sc: 收缩控制参数
 *
 * 对于每个超级块的收缩器，我们必须注意不要在收缩器内部释放
 * 超级块的最后一个活跃引用。如果发生这种情况，我们可能会在
 * 收缩器路径内触发注销收缩器，这会导致shrinker_rwsem死锁。
 * 因此我们获取超级块的被动引用以避免这种情况发生。
 *
 * 返回值: 释放的对象数量
 */
static unsigned long super_cache_scan(struct shrinker *shrink,
				      struct shrink_control *sc)
{
	struct super_block *sb;
	long	fs_objects = 0;
	long	total_objects;
	long	freed = 0;
	long	dentries;
	long	inodes;

	sb = container_of(shrink, struct super_block, s_shrink);

	/*
	 * Deadlock avoidance.  We may hold various FS locks, and we don't want
	 * to recurse into the FS that called us in clear_inode() and friends..
	 */
	if (!(sc->gfp_mask & __GFP_FS))
		return SHRINK_STOP;

	if (!trylock_super(sb))
		return SHRINK_STOP;

	if (sb->s_op->nr_cached_objects)
		fs_objects = sb->s_op->nr_cached_objects(sb, sc);

	inodes = list_lru_shrink_count(&sb->s_inode_lru, sc);
	dentries = list_lru_shrink_count(&sb->s_dentry_lru, sc);
	total_objects = dentries + inodes + fs_objects + 1;
	if (!total_objects)
		total_objects = 1;

	/* proportion the scan between the caches */
	dentries = mult_frac(sc->nr_to_scan, dentries, total_objects);
	inodes = mult_frac(sc->nr_to_scan, inodes, total_objects);
	fs_objects = mult_frac(sc->nr_to_scan, fs_objects, total_objects);

	/*
	 * prune the dcache first as the icache is pinned by it, then
	 * prune the icache, followed by the filesystem specific caches
	 *
	 * Ensure that we always scan at least one object - memcg kmem
	 * accounting uses this to fully empty the caches.
	 */
	sc->nr_to_scan = dentries + 1;
	freed = prune_dcache_sb(sb, sc);
	sc->nr_to_scan = inodes + 1;
	freed += prune_icache_sb(sb, sc);

	if (fs_objects) {
		sc->nr_to_scan = fs_objects + 1;
		freed += sb->s_op->free_cached_objects(sb, sc);
	}

	up_read(&sb->s_umount);
	return freed;
}

/**
 * super_cache_count - 统计超级块缓存对象数量
 * @shrink: 收缩器结构
 * @sc: 收缩控制参数
 *
 * 统计超级块中可收缩的缓存对象总数，包括dentry缓存、inode缓存
 * 和文件系统特定的缓存对象。我们不使用trylock_super()因为它
 * 是性能瓶颈，所以我们会暴露在部分设置状态下。
 *
 * 返回值: 可收缩的对象总数
 */
static unsigned long super_cache_count(struct shrinker *shrink,
				       struct shrink_control *sc)
{
	struct super_block *sb;
	long	total_objects = 0;

	sb = container_of(shrink, struct super_block, s_shrink);

	/*
	 * We don't call trylock_super() here as it is a scalability bottleneck,
	 * so we're exposed to partial setup state. The shrinker rwsem does not
	 * protect filesystem operations backing list_lru_shrink_count() or
	 * s_op->nr_cached_objects(). Counts can change between
	 * super_cache_count and super_cache_scan, so we really don't need locks
	 * here.
	 *
	 * However, if we are currently mounting the superblock, the underlying
	 * filesystem might be in a state of partial construction and hence it
	 * is dangerous to access it.  trylock_super() uses a SB_BORN check to
	 * avoid this situation, so do the same here. The memory barrier is
	 * matched with the one in mount_fs() as we don't hold locks here.
	 */
	if (!(sb->s_flags & SB_BORN))
		return 0;
	smp_rmb();

	if (sb->s_op && sb->s_op->nr_cached_objects)
		total_objects = sb->s_op->nr_cached_objects(sb, sc);

	total_objects += list_lru_shrink_count(&sb->s_dentry_lru, sc);
	total_objects += list_lru_shrink_count(&sb->s_inode_lru, sc);

	if (!total_objects)
		return SHRINK_EMPTY;

	total_objects = vfs_pressure_ratio(total_objects);
	return total_objects;
}

/**
 * destroy_super_work - 超级块销毁工作函数
 * @work: 工作队列项
 *
 * 在工作队列上下文中执行超级块的最终销毁工作，包括释放
 * 每CPU读写信号量和超级块结构本身。
 */
static void destroy_super_work(struct work_struct *work)
{
	struct super_block *s = container_of(work, struct super_block,
							destroy_work);
	int i;

	for (i = 0; i < SB_FREEZE_LEVELS; i++)
		percpu_free_rwsem(&s->s_writers.rw_sem[i]);
	kfree(s);
}

/**
 * destroy_super_rcu - RCU回调中的超级块销毁
 * @head: RCU回调头
 *
 * 在RCU宽限期后通过工作队列调度超级块的最终销毁。
 * 这确保了所有可能持有对超级块引用的RCU读端临界区都已完成。
 */
static void destroy_super_rcu(struct rcu_head *head)
{
	struct super_block *s = container_of(head, struct super_block, rcu);
	INIT_WORK(&s->destroy_work, destroy_super_work);
	schedule_work(&s->destroy_work);
}

/**
 * destroy_unused_super - 销毁未使用的超级块
 * @s: 要销毁的超级块
 *
 * 释放一个从未被任何人看到的超级块。这用于在分配后但在
 * 实际使用前发生错误时清理超级块。直接调用destroy_super_work
 * 因为不需要延迟处理。
 */
/* Free a superblock that has never been seen by anyone */
static void destroy_unused_super(struct super_block *s)
{
	if (!s)
		return;
	up_write(&s->s_umount);
	list_lru_destroy(&s->s_dentry_lru);
	list_lru_destroy(&s->s_inode_lru);
	security_sb_free(s);
	put_user_ns(s->s_user_ns);
	kfree(s->s_subtype);
	free_prealloced_shrinker(&s->s_shrink);
	/* no delays needed */
	destroy_super_work(&s->destroy_work);
}

/**
 * alloc_super - 创建新的超级块
 * @type: 超级块所属的文件系统类型
 * @flags: 挂载标志
 * @user_ns: 超级块的用户命名空间
 *
 * 分配并初始化一个新的 &struct super_block。alloc_super()
 * 返回一个指向新超级块的指针，如果分配失败则返回 %NULL。
 *
 * 返回值: 成功返回超级块指针，失败返回NULL
 */
static struct super_block *alloc_super(struct file_system_type *type, int flags,
				       struct user_namespace *user_ns)
{
	struct super_block *s = kzalloc(sizeof(struct super_block),  GFP_USER);
	static const struct super_operations default_op;
	int i;

	if (!s)
		return NULL;

	INIT_LIST_HEAD(&s->s_mounts);
	s->s_user_ns = get_user_ns(user_ns);
	init_rwsem(&s->s_umount);
	lockdep_set_class(&s->s_umount, &type->s_umount_key);
	/*
	 * sget() can have s_umount recursion.
	 *
	 * When it cannot find a suitable sb, it allocates a new
	 * one (this one), and tries again to find a suitable old
	 * one.
	 *
	 * In case that succeeds, it will acquire the s_umount
	 * lock of the old one. Since these are clearly distrinct
	 * locks, and this object isn't exposed yet, there's no
	 * risk of deadlocks.
	 *
	 * Annotate this by putting this lock in a different
	 * subclass.
	 */
	down_write_nested(&s->s_umount, SINGLE_DEPTH_NESTING);

	if (security_sb_alloc(s))
		goto fail;

	for (i = 0; i < SB_FREEZE_LEVELS; i++) {
		if (__percpu_init_rwsem(&s->s_writers.rw_sem[i],
					sb_writers_name[i],
					&type->s_writers_key[i]))
			goto fail;
	}
	init_waitqueue_head(&s->s_writers.wait_unfrozen);
	s->s_bdi = &noop_backing_dev_info;
	s->s_flags = flags;
	if (s->s_user_ns != &init_user_ns)
		s->s_iflags |= SB_I_NODEV;
	INIT_HLIST_NODE(&s->s_instances);
	INIT_HLIST_BL_HEAD(&s->s_roots);
	mutex_init(&s->s_sync_lock);
	INIT_LIST_HEAD(&s->s_inodes);
	spin_lock_init(&s->s_inode_list_lock);
	INIT_LIST_HEAD(&s->s_inodes_wb);
	spin_lock_init(&s->s_inode_wblist_lock);

	s->s_count = 1;
	atomic_set(&s->s_active, 1);
	mutex_init(&s->s_vfs_rename_mutex);
	lockdep_set_class(&s->s_vfs_rename_mutex, &type->s_vfs_rename_key);
	init_rwsem(&s->s_dquot.dqio_sem);
	s->s_maxbytes = MAX_NON_LFS;
	s->s_op = &default_op;
	s->s_time_gran = 1000000000;
	s->s_time_min = TIME64_MIN;
	s->s_time_max = TIME64_MAX;
	s->cleancache_poolid = CLEANCACHE_NO_POOL;

	s->s_shrink.seeks = DEFAULT_SEEKS;
	s->s_shrink.scan_objects = super_cache_scan;
	s->s_shrink.count_objects = super_cache_count;
	s->s_shrink.batch = 1024;
	s->s_shrink.flags = SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE;
	if (prealloc_shrinker(&s->s_shrink))
		goto fail;
	if (list_lru_init_memcg(&s->s_dentry_lru, &s->s_shrink))
		goto fail;
	if (list_lru_init_memcg(&s->s_inode_lru, &s->s_shrink))
		goto fail;
	return s;

fail:
	destroy_unused_super(s);
	return NULL;
}

/* Superblock refcounting  */

/**
 * __put_super - 减少超级块引用计数
 * @s: 超级块
 *
 * 减少超级块的引用计数。调用者必须持有sb_lock。
 * 当引用计数归零时，执行清理并通过RCU延迟销毁超级块。
 */
/*
 * Drop a superblock's refcount.  The caller must hold sb_lock.
 */
static void __put_super(struct super_block *s)
{
	if (!--s->s_count) {
		list_del_init(&s->s_list);
		WARN_ON(s->s_dentry_lru.node);
		WARN_ON(s->s_inode_lru.node);
		WARN_ON(!list_empty(&s->s_mounts));
		security_sb_free(s);
		fscrypt_sb_free(s);
		put_user_ns(s->s_user_ns);
		kfree(s->s_subtype);
		call_rcu(&s->rcu, destroy_super_rcu);
	}
}

/**
 * put_super - 释放超级块的临时引用
 * @sb: 要释放的超级块
 *
 * 释放一个临时引用，如果没有剩余引用则释放超级块。
 * 这是__put_super的线程安全版本，会获取必要的锁。
 */
/**
 *	put_super	-	drop a temporary reference to superblock
 *	@sb: superblock in question
 *
 *	Drops a temporary reference, frees superblock if there's no
 *	references left.
 */
static void put_super(struct super_block *sb)
{
	spin_lock(&sb_lock);
	__put_super(sb);
	spin_unlock(&sb_lock);
}


/**
 * deactivate_locked_super - 停用锁定的超级块
 * @s: 要停用的超级块
 *
 * 释放超级块的一个活跃引用，如果没有其他活跃引用则将其转换为
 * 临时引用。在这种情况下，我们告诉文件系统驱动程序关闭它并
 * 释放我们刚刚获得的临时引用。调用者持有超级块的独占锁，
 * 该锁在函数返回时被释放。
 */
/**
 *	deactivate_locked_super	-	drop an active reference to superblock
 *	@s: superblock to deactivate
 *
 *	Drops an active reference to superblock, converting it into a temporary
 *	one if there is no other active references left.  In that case we
 *	tell fs driver to shut it down and drop the temporary reference we
 *	had just acquired.
 *
 *	Caller holds exclusive lock on superblock; that lock is released.
 */
void deactivate_locked_super(struct super_block *s)
{
	struct file_system_type *fs = s->s_type;
	if (atomic_dec_and_test(&s->s_active)) {
		cleancache_invalidate_fs(s);
		unregister_shrinker(&s->s_shrink);
		fs->kill_sb(s);

		/*
		 * Since list_lru_destroy() may sleep, we cannot call it from
		 * put_super(), where we hold the sb_lock. Therefore we destroy
		 * the lru lists right now.
		 */
		list_lru_destroy(&s->s_dentry_lru);
		list_lru_destroy(&s->s_inode_lru);

		put_filesystem(fs);
		put_super(s);
	} else {
		up_write(&s->s_umount);
	}
}

EXPORT_SYMBOL(deactivate_locked_super);

/**
 * deactivate_super - 释放超级块的活跃引用
 * @s: 要停用的超级块
 *
 * deactivate_locked_super()的变体，区别是超级块没有被调用者锁定。
 * 如果我们要释放最后一个活跃引用，会在此之前获取锁。
 *
 * 返回值: 无
 */
void deactivate_super(struct super_block *s)
{
	if (!atomic_add_unless(&s->s_active, -1, 1)) {
		down_write(&s->s_umount);
		deactivate_locked_super(s);
	}
}

EXPORT_SYMBOL(deactivate_super);

/**
 * grab_super - 获取活跃引用
 * @s: 尝试激活的引用
 *
 * 尝试获取一个活跃引用。grab_super()用于当我们刚刚在super_blocks
 * 或fs_type->fs_supers中找到一个超级块并希望将其转换为完整的活跃引用时。
 * grab_super()在持有sb_lock的情况下调用并释放它。成功时返回1，
 * 失败时返回0（当grab_super()被调用时超级块内容已经死亡或正在死亡）。
 * 注意这仅对不在关闭模式下的超级块调用（== 仍在其类型的->fs_supers上的超级块），
 * 因此在这里增加->s_count是可以的。
 *
 * 返回值: 成功返回1，失败返回0
 */
static int grab_super(struct super_block *s) __releases(sb_lock)
{
	s->s_count++;
	spin_unlock(&sb_lock);
	down_write(&s->s_umount);
	if ((s->s_flags & SB_BORN) && atomic_inc_not_zero(&s->s_active)) {
		put_super(s);
		return 1;
	}
	up_write(&s->s_umount);
	put_super(s);
	return 0;
}

/**
 * trylock_super - 尝试获取超级块共享锁
 * @sb: 尝试锁定的超级块
 *
 * 尝试防止文件系统关闭。这在我们无法获取活跃引用但需要确保
 * 在我们工作时文件系统不会关闭的地方使用。如果无法获取s_umount
 * 或者我们失去了竞争且文件系统已进入关闭状态，则返回false。
 * 成功时返回true并以读模式持有s_umount锁。成功返回时，调用者
 * 必须在完成后释放s_umount锁。
 *
 * 注意：与get_super()等不同，这个函数不会增加->s_count。
 * 这样做是安全的，因为我们可以使用trylock而不是down_read()。
 *
 * 返回值: 成功返回true，失败返回false
 */
/*
 *	trylock_super - try to grab ->s_umount shared
 *	@sb: reference we are trying to grab
 *
 *	Try to prevent fs shutdown.  This is used in places where we
 *	cannot take an active reference but we need to ensure that the
 *	filesystem is not shut down while we are working on it. It returns
 *	false if we cannot acquire s_umount or if we lose the race and
 *	filesystem already got into shutdown, and returns true with the s_umount
 *	lock held in read mode in case of success. On successful return,
 *	the caller must drop the s_umount lock when done.
 *
 *	Note that unlike get_super() et.al. this one does *not* bump ->s_count.
 *	The reason why it's safe is that we are OK with doing trylock instead
 *	of down_read().  There's a couple of places that are OK with that, but
 *	it's very much not a general-purpose interface.
 */
bool trylock_super(struct super_block *sb)
{
	if (down_read_trylock(&sb->s_umount)) {
		if (!hlist_unhashed(&sb->s_instances) &&
		    sb->s_root && (sb->s_flags & SB_BORN))
			return true;
		up_read(&sb->s_umount);
	}

	return false;
}

/**
 * generic_shutdown_super - ->kill_sb()的通用帮助函数
 * @sb: 要关闭的超级块
 *
 * generic_shutdown_super()在超级块关闭时执行所有与文件系统无关的工作。
 * 典型的->kill_sb()应该从超级块中挑选出所有需要销毁的文件系统特定对象，
 * 调用generic_shutdown_super()并释放上述对象。注意：dentries和inodes
 * 已经被处理，不需要特定的处理。
 *
 * 调用此函数后，文件系统不能再更改或重新排列属于此super_block的dentries
 * 集合，也不能更改dentries到inodes的附件。
 */
/**
 *	generic_shutdown_super	-	common helper for ->kill_sb()
 *	@sb: superblock to kill
 *
 *	generic_shutdown_super() does all fs-independent work on superblock
 *	shutdown.  Typical ->kill_sb() should pick all fs-specific objects
 *	that need destruction out of superblock, call generic_shutdown_super()
 *	and release aforementioned objects.  Note: dentries and inodes _are_
 *	taken care of and do not need specific handling.
 *
 *	Upon calling this function, the filesystem may no longer alter or
 *	rearrange the set of dentries belonging to this super_block, nor may it
 *	change the attachments of dentries to inodes.
 */
void generic_shutdown_super(struct super_block *sb)
{
	const struct super_operations *sop = sb->s_op;

	if (sb->s_root) {
		shrink_dcache_for_umount(sb);
		sync_filesystem(sb);
		sb->s_flags &= ~SB_ACTIVE;

		cgroup_writeback_umount();

		/* evict all inodes with zero refcount */
		evict_inodes(sb);
		/* only nonzero refcount inodes can have marks */
		fsnotify_sb_delete(sb);

		if (sb->s_dio_done_wq) {
			destroy_workqueue(sb->s_dio_done_wq);
			sb->s_dio_done_wq = NULL;
		}

		if (sop->put_super)
			sop->put_super(sb);

		if (!list_empty(&sb->s_inodes)) {
			printk("VFS: Busy inodes after unmount of %s. "
			   "Self-destruct in 5 seconds.  Have a nice day...\n",
			   sb->s_id);
		}
	}
	spin_lock(&sb_lock);
	/* should be initialized for __put_super_and_need_restart() */
	hlist_del_init(&sb->s_instances);
	spin_unlock(&sb_lock);
	up_write(&sb->s_umount);
	if (sb->s_bdi != &noop_backing_dev_info) {
		bdi_put(sb->s_bdi);
		sb->s_bdi = &noop_backing_dev_info;
	}
}

EXPORT_SYMBOL(generic_shutdown_super);

/**
 * mount_capable - 检查是否有挂载权限
 * @fc: 文件系统上下文
 *
 * 检查当前进程是否有权限挂载指定的文件系统。如果文件系统类型
 * 不支持用户命名空间挂载，则需要CAP_SYS_ADMIN权限。否则，
 * 在相应的用户命名空间中需要CAP_SYS_ADMIN权限。
 *
 * 返回值: 有权限返回true，无权限返回false
 */
bool mount_capable(struct fs_context *fc)
{
	if (!(fc->fs_type->fs_flags & FS_USERNS_MOUNT))
		return capable(CAP_SYS_ADMIN);
	else
		return ns_capable(fc->user_ns, CAP_SYS_ADMIN);
}

/**
 * sget_fc - 查找或创建超级块
 * @fc: 文件系统上下文
 * @test: 比较回调函数
 * @set: 设置回调函数
 *
 * 使用存储在文件系统上下文中的参数和两个回调函数查找或创建超级块。
 *
 * 如果匹配到现有的超级块，则返回该超级块，其引用计数已增加，
 * 调用者必须转移或丢弃该引用。
 *
 * 如果没有匹配，将分配一个新的超级块并执行基本初始化
 * (设置s_type、s_fs_info和s_id，并调用set()回调)，
 * 超级块将被发布并以部分构造状态返回，SB_BORN和SB_ACTIVE尚未设置。
 *
 * 返回值: 成功返回超级块指针，失败返回错误指针
 */
/**
 * sget_fc - Find or create a superblock
 * @fc:	Filesystem context.
 * @test: Comparison callback
 * @set: Setup callback
 *
 * Find or create a superblock using the parameters stored in the filesystem
 * context and the two callback functions.
 *
 * If an extant superblock is matched, then that will be returned with an
 * elevated reference count that the caller must transfer or discard.
 *
 * If no match is made, a new superblock will be allocated and basic
 * initialisation will be performed (s_type, s_fs_info and s_id will be set and
 * the set() callback will be invoked), the superblock will be published and it
 * will be returned in a partially constructed state with SB_BORN and SB_ACTIVE
 * as yet unset.
 */
struct super_block *sget_fc(struct fs_context *fc,
			    int (*test)(struct super_block *, struct fs_context *),
			    int (*set)(struct super_block *, struct fs_context *))
{
	struct super_block *s = NULL;
	struct super_block *old;
	struct user_namespace *user_ns = fc->global ? &init_user_ns : fc->user_ns;
	int err;

retry:
	spin_lock(&sb_lock);
	if (test) {
		hlist_for_each_entry(old, &fc->fs_type->fs_supers, s_instances) {
			if (test(old, fc))
				goto share_extant_sb;
		}
	}
	if (!s) {
		spin_unlock(&sb_lock);
		s = alloc_super(fc->fs_type, fc->sb_flags, user_ns);
		if (!s)
			return ERR_PTR(-ENOMEM);
		goto retry;
	}

	s->s_fs_info = fc->s_fs_info;
	err = set(s, fc);
	if (err) {
		s->s_fs_info = NULL;
		spin_unlock(&sb_lock);
		destroy_unused_super(s);
		return ERR_PTR(err);
	}
	fc->s_fs_info = NULL;
	s->s_type = fc->fs_type;
	s->s_iflags |= fc->s_iflags;
	strlcpy(s->s_id, s->s_type->name, sizeof(s->s_id));
	list_add_tail(&s->s_list, &super_blocks);
	hlist_add_head(&s->s_instances, &s->s_type->fs_supers);
	spin_unlock(&sb_lock);
	get_filesystem(s->s_type);
	register_shrinker_prepared(&s->s_shrink);
	return s;

share_extant_sb:
	if (user_ns != old->s_user_ns) {
		spin_unlock(&sb_lock);
		destroy_unused_super(s);
		return ERR_PTR(-EBUSY);
	}
	if (!grab_super(old))
		goto retry;
	destroy_unused_super(s);
	return old;
}
EXPORT_SYMBOL(sget_fc);

/**
 * sget - 查找或创建超级块
 * @type: 超级块所属的文件系统类型
 * @test: 比较回调函数
 * @set: 设置回调函数
 * @flags: 挂载标志
 * @data: 传递给回调函数的参数
 *
 * 在现有的超级块列表中查找匹配的超级块，如果没有找到则创建一个新的。
 * 返回值: 成功返回超级块指针，失败返回错误指针
 */
struct super_block *sget(struct file_system_type *type,
			int (*test)(struct super_block *,void *),
			int (*set)(struct super_block *,void *),
			int flags,
			void *data)
{
	struct user_namespace *user_ns = current_user_ns();
	struct super_block *s = NULL;
	struct super_block *old;
	int err;

	/* We don't yet pass the user namespace of the parent
	 * mount through to here so always use &init_user_ns
	 * until that changes.
	 */
	if (flags & SB_SUBMOUNT)
		user_ns = &init_user_ns;

retry:
	spin_lock(&sb_lock);
	if (test) {
		hlist_for_each_entry(old, &type->fs_supers, s_instances) {
			if (!test(old, data))
				continue;
			if (user_ns != old->s_user_ns) {
				spin_unlock(&sb_lock);
				destroy_unused_super(s);
				return ERR_PTR(-EBUSY);
			}
			if (!grab_super(old))
				goto retry;
			destroy_unused_super(s);
			return old;
		}
	}
	if (!s) {
		spin_unlock(&sb_lock);
		s = alloc_super(type, (flags & ~SB_SUBMOUNT), user_ns);
		if (!s)
			return ERR_PTR(-ENOMEM);
		goto retry;
	}

	err = set(s, data);
	if (err) {
		spin_unlock(&sb_lock);
		destroy_unused_super(s);
		return ERR_PTR(err);
	}
	s->s_type = type;
	strlcpy(s->s_id, type->name, sizeof(s->s_id));
	list_add_tail(&s->s_list, &super_blocks);
	hlist_add_head(&s->s_instances, &type->fs_supers);
	spin_unlock(&sb_lock);
	get_filesystem(type);
	register_shrinker_prepared(&s->s_shrink);
	return s;
}
EXPORT_SYMBOL(sget);

/**
 * drop_super - 释放超级块的共享锁和引用
 * @sb: 要释放的超级块
 *
 * 释放超级块的共享s_umount锁并减少其引用计数。
 * 通常与get_super()或类似函数配对使用。
 */
void drop_super(struct super_block *sb)
{
	up_read(&sb->s_umount);
	put_super(sb);
}

EXPORT_SYMBOL(drop_super);

/**
 * drop_super_exclusive - 释放超级块的独占锁和引用
 * @sb: 要释放的超级块
 *
 * 释放超级块的独占s_umount锁并减少其引用计数。
 * 通常与需要独占访问的操作配对使用。
 */
void drop_super_exclusive(struct super_block *sb)
{
	up_write(&sb->s_umount);
	put_super(sb);
}
EXPORT_SYMBOL(drop_super_exclusive);

/**
 * __iterate_supers - 遍历所有超级块并调用函数
 * @f: 要对每个超级块调用的函数
 *
 * 扫描超级块列表并对每个活跃的超级块调用给定函数。
 * 这是一个内部函数，不提供锁定保护给回调函数。
 */
static void __iterate_supers(void (*f)(struct super_block *))
{
	struct super_block *sb, *p = NULL;

	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (hlist_unhashed(&sb->s_instances))
			continue;
		sb->s_count++;
		spin_unlock(&sb_lock);

		f(sb);

		spin_lock(&sb_lock);
		if (p)
			__put_super(p);
		p = sb;
	}
	if (p)
		__put_super(p);
	spin_unlock(&sb_lock);
}
/**
 * iterate_supers - 对所有活跃超级块调用函数
 * @f: 要调用的函数
 * @arg: 传递给函数的参数
 *
 * 扫描超级块列表并对每个活跃的超级块调用给定函数，
 * 传递锁定的超级块和给定的参数。确保超级块在回调期间保持锁定状态。
 */
/**
 *	iterate_supers - call function for all active superblocks
 *	@f: function to call
 *	@arg: argument to pass to it
 *
 *	Scans the superblock list and calls given function, passing it
 *	locked superblock and given argument.
 */
void iterate_supers(void (*f)(struct super_block *, void *), void *arg)
{
	struct super_block *sb, *p = NULL;

	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (hlist_unhashed(&sb->s_instances))
			continue;
		sb->s_count++;
		spin_unlock(&sb_lock);

		down_read(&sb->s_umount);
		if (sb->s_root && (sb->s_flags & SB_BORN))
			f(sb, arg);
		up_read(&sb->s_umount);

		spin_lock(&sb_lock);
		if (p)
			__put_super(p);
		p = sb;
	}
	if (p)
		__put_super(p);
	spin_unlock(&sb_lock);
}

/**
 * iterate_supers_type - 对指定类型的超级块调用函数
 * @type: 文件系统类型
 * @f: 要调用的函数
 * @arg: 传递给函数的参数
 *
 * 扫描超级块列表并对给定类型的每个活跃超级块调用给定函数，
 * 传递锁定的超级块和给定的参数。
 */
/**
 *	iterate_supers_type - call function for superblocks of given type
 *	@type: fs type
 *	@f: function to call
 *	@arg: argument to pass to it
 *
 *	Scans the superblock list and calls given function, passing it
 *	locked superblock and given argument.
 */
void iterate_supers_type(struct file_system_type *type,
	void (*f)(struct super_block *, void *), void *arg)
{
	struct super_block *sb, *p = NULL;

	spin_lock(&sb_lock);
	hlist_for_each_entry(sb, &type->fs_supers, s_instances) {
		sb->s_count++;
		spin_unlock(&sb_lock);

		down_read(&sb->s_umount);
		if (sb->s_root && (sb->s_flags & SB_BORN))
			f(sb, arg);
		up_read(&sb->s_umount);

		spin_lock(&sb_lock);
		if (p)
			__put_super(p);
		p = sb;
	}
	if (p)
		__put_super(p);
	spin_unlock(&sb_lock);
}

EXPORT_SYMBOL(iterate_supers_type);

/**
 * __get_super - 获取块设备的超级块(内部函数)
 * @bdev: 块设备
 * @excl: 是否需要独占锁
 *
 * 扫描超级块列表，查找挂载在指定块设备上的文件系统的超级块。
 * 根据excl参数决定获取共享锁还是独占锁。
 *
 * 返回值: 成功返回超级块指针，未找到返回NULL
 */
static struct super_block *__get_super(struct block_device *bdev, bool excl)
{
	struct super_block *sb;

	if (!bdev)
		return NULL;

	spin_lock(&sb_lock);
rescan:
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (hlist_unhashed(&sb->s_instances))
			continue;
		if (sb->s_bdev == bdev) {
			sb->s_count++;
			spin_unlock(&sb_lock);
			if (!excl)
				down_read(&sb->s_umount);
			else
				down_write(&sb->s_umount);
			/* still alive? */
			if (sb->s_root && (sb->s_flags & SB_BORN))
				return sb;
			if (!excl)
				up_read(&sb->s_umount);
			else
				up_write(&sb->s_umount);
			/* nope, got unmounted */
			spin_lock(&sb_lock);
			__put_super(sb);
			goto rescan;
		}
	}
	spin_unlock(&sb_lock);
	return NULL;
}

/**
 * get_super - 获取设备的超级块
 * @bdev: 要获取超级块的设备
 *
 * 扫描超级块列表，查找挂载在指定设备上的文件系统的超级块。
 * 如果没有找到匹配项则返回 %NULL。
 *
 * 返回值: 成功返回超级块指针，未找到返回NULL
 */
struct super_block *get_super(struct block_device *bdev)
{
	return __get_super(bdev, false);
}
EXPORT_SYMBOL(get_super);

/**
 * __get_super_thawed - 获取已解冻的超级块(内部函数)
 * @bdev: 块设备
 * @excl: 是否需要独占锁
 *
 * 循环获取超级块直到它处于解冻状态。如果超级块被冻结，
 * 会等待直到它解冻后再返回。
 *
 * 返回值: 成功返回解冻的超级块指针，未找到返回NULL
 */
static struct super_block *__get_super_thawed(struct block_device *bdev,
					      bool excl)
{
	while (1) {
		struct super_block *s = __get_super(bdev, excl);
		if (!s || s->s_writers.frozen == SB_UNFROZEN)
			return s;
		if (!excl)
			up_read(&s->s_umount);
		else
			up_write(&s->s_umount);
		wait_event(s->s_writers.wait_unfrozen,
			   s->s_writers.frozen == SB_UNFROZEN);
		put_super(s);
	}
}

/**
 * get_super_thawed - 获取设备的已解冻超级块
 * @bdev: 要获取超级块的设备
 *
 * 扫描超级块列表，查找挂载在指定设备上的文件系统的超级块。
 * 超级块在解冻后返回(如果没有被冻结则立即返回)。
 * 如果没有找到匹配项则返回%NULL。
 *
 * 返回值: 成功返回解冻的超级块指针，未找到返回NULL
 */
/**
 *	get_super_thawed - get thawed superblock of a device
 *	@bdev: device to get the superblock for
 *
 *	Scans the superblock list and finds the superblock of the file system
 *	mounted on the device. The superblock is returned once it is thawed
 *	(or immediately if it was not frozen). %NULL is returned if no match
 *	is found.
 */
struct super_block *get_super_thawed(struct block_device *bdev)
{
	return __get_super_thawed(bdev, false);
}
EXPORT_SYMBOL(get_super_thawed);

/**
 * get_super_exclusive_thawed - 以独占模式获取设备的已解冻超级块
 * @bdev: 要获取超级块的设备
 *
 * 扫描超级块列表，查找挂载在指定设备上的文件系统的超级块。
 * 超级块在解冻后返回(如果没有被冻结则立即返回)，并且以独占模式
 * 持有s_umount信号量。如果没有找到匹配项则返回%NULL。
 *
 * 返回值: 成功返回解冻的超级块指针(独占锁定)，未找到返回NULL
 */
/**
 *	get_super_exclusive_thawed - get thawed superblock of a device
 *	@bdev: device to get the superblock for
 *
 *	Scans the superblock list and finds the superblock of the file system
 *	mounted on the device. The superblock is returned once it is thawed
 *	(or immediately if it was not frozen) and s_umount semaphore is held
 *	in exclusive mode. %NULL is returned if no match is found.
 */
struct super_block *get_super_exclusive_thawed(struct block_device *bdev)
{
	return __get_super_thawed(bdev, true);
}
EXPORT_SYMBOL(get_super_exclusive_thawed);

/**
 * get_active_super - 获取设备超级块的活跃引用
 * @bdev: 要获取超级块的设备
 *
 * 扫描超级块列表，查找挂载在指定设备上的文件系统的超级块。
 * 返回带有活跃引用的超级块，如果没有找到则返回%NULL。
 * 与get_super()不同，这会获取活跃引用而不是临时引用。
 *
 * 返回值: 成功返回带活跃引用的超级块指针，未找到返回NULL
 */
/**
 * get_active_super - get an active reference to the superblock of a device
 * @bdev: device to get the superblock for
 *
 * Scans the superblock list and finds the superblock of the file system
 * mounted on the device given.  Returns the superblock with an active
 * reference or %NULL if none was found.
 */
struct super_block *get_active_super(struct block_device *bdev)
{
	struct super_block *sb;

	if (!bdev)
		return NULL;

restart:
	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (hlist_unhashed(&sb->s_instances))
			continue;
		if (sb->s_bdev == bdev) {
			if (!grab_super(sb))
				goto restart;
			up_write(&sb->s_umount);
			return sb;
		}
	}
	spin_unlock(&sb_lock);
	return NULL;
}

/**
 * user_get_super - 根据设备号获取超级块
 * @dev: 设备号
 *
 * 扫描超级块列表，查找挂载在指定设备号上的文件系统的超级块。
 * 这是用户空间接口使用的函数，通过设备号而不是block_device结构查找。
 *
 * 返回值: 成功返回超级块指针，未找到返回NULL
 */
struct super_block *user_get_super(dev_t dev)
{
	struct super_block *sb;

	spin_lock(&sb_lock);
rescan:
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (hlist_unhashed(&sb->s_instances))
			continue;
		if (sb->s_dev ==  dev) {
			sb->s_count++;
			spin_unlock(&sb_lock);
			down_read(&sb->s_umount);
			/* still alive? */
			if (sb->s_root && (sb->s_flags & SB_BORN))
				return sb;
			up_read(&sb->s_umount);
			/* nope, got unmounted */
			spin_lock(&sb_lock);
			__put_super(sb);
			goto rescan;
		}
	}
	spin_unlock(&sb_lock);
	return NULL;
}

/**
 * reconfigure_super - 请求文件系统更改超级块参数
 * @fc: 超级块和配置信息
 *
 * 更改活跃超级块的配置参数。这包括处理只读/读写状态的改变、
 * 安全选项的更新等。函数会进行各种检查以确保重新配置的安全性。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/**
 * reconfigure_super - asks filesystem to change superblock parameters
 * @fc: The superblock and configuration
 *
 * Alters the configuration parameters of a live superblock.
 */
int reconfigure_super(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	int retval;
	bool remount_ro = false;
	bool force = fc->sb_flags & SB_FORCE;

	if (fc->sb_flags_mask & ~MS_RMT_MASK)
		return -EINVAL;
	if (sb->s_writers.frozen != SB_UNFROZEN)
		return -EBUSY;

	retval = security_sb_remount(sb, fc->security);
	if (retval)
		return retval;

	if (fc->sb_flags_mask & SB_RDONLY) {
#ifdef CONFIG_BLOCK
		if (!(fc->sb_flags & SB_RDONLY) && bdev_read_only(sb->s_bdev))
			return -EACCES;
#endif

		remount_ro = (fc->sb_flags & SB_RDONLY) && !sb_rdonly(sb);
	}

	if (remount_ro) {
		if (!hlist_empty(&sb->s_pins)) {
			up_write(&sb->s_umount);
			group_pin_kill(&sb->s_pins);
			down_write(&sb->s_umount);
			if (!sb->s_root)
				return 0;
			if (sb->s_writers.frozen != SB_UNFROZEN)
				return -EBUSY;
			remount_ro = !sb_rdonly(sb);
		}
	}
	shrink_dcache_sb(sb);

	/* If we are reconfiguring to RDONLY and current sb is read/write,
	 * make sure there are no files open for writing.
	 */
	if (remount_ro) {
		if (force) {
			sb->s_readonly_remount = 1;
			smp_wmb();
		} else {
			retval = sb_prepare_remount_readonly(sb);
			if (retval)
				return retval;
		}
	}

	if (fc->ops->reconfigure) {
		retval = fc->ops->reconfigure(fc);
		if (retval) {
			if (!force)
				goto cancel_readonly;
			/* If forced remount, go ahead despite any errors */
			WARN(1, "forced remount of a %s fs returned %i\n",
			     sb->s_type->name, retval);
		}
	}

	WRITE_ONCE(sb->s_flags, ((sb->s_flags & ~fc->sb_flags_mask) |
				 (fc->sb_flags & fc->sb_flags_mask)));
	/* Needs to be ordered wrt mnt_is_readonly() */
	smp_wmb();
	sb->s_readonly_remount = 0;

	/*
	 * Some filesystems modify their metadata via some other path than the
	 * bdev buffer cache (eg. use a private mapping, or directories in
	 * pagecache, etc). Also file data modifications go via their own
	 * mappings. So If we try to mount readonly then copy the filesystem
	 * from bdev, we could get stale data, so invalidate it to give a best
	 * effort at coherency.
	 */
	if (remount_ro && sb->s_bdev)
		invalidate_bdev(sb->s_bdev);
	return 0;

cancel_readonly:
	sb->s_readonly_remount = 0;
	return retval;
}

/**
 * do_emergency_remount_callback - 紧急重新挂载回调函数
 * @sb: 要重新挂载的超级块
 *
 * 对单个超级块执行紧急重新挂载为只读模式。这通常在系统
 * 紧急情况下调用，以防止数据损坏。
 */
static void do_emergency_remount_callback(struct super_block *sb)
{
	down_write(&sb->s_umount);
	if (sb->s_root && sb->s_bdev && (sb->s_flags & SB_BORN) &&
	    !sb_rdonly(sb)) {
		struct fs_context *fc;

		fc = fs_context_for_reconfigure(sb->s_root,
					SB_RDONLY | SB_FORCE, SB_RDONLY);
		if (!IS_ERR(fc)) {
			if (parse_monolithic_mount_data(fc, NULL) == 0)
				(void)reconfigure_super(fc);
			put_fs_context(fc);
		}
	}
	up_write(&sb->s_umount);
}

/**
 * do_emergency_remount - 执行紧急重新挂载工作
 * @work: 工作队列项
 *
 * 工作队列函数，遍历所有超级块并将它们重新挂载为只读模式。
 * 这是紧急情况下保护数据的最后手段。
 */
static void do_emergency_remount(struct work_struct *work)
{
	__iterate_supers(do_emergency_remount_callback);
	kfree(work);
	printk("Emergency Remount complete\n");
}

/**
 * emergency_remount - 启动紧急重新挂载
 *
 * 在系统紧急情况下调用，异步地将所有文件系统重新挂载为只读模式。
 * 这有助于防止在系统即将崩溃或出现严重问题时的数据损坏。
 * 使用工作队列确保操作不会阻塞调用者。
 */
void emergency_remount(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_emergency_remount);
		schedule_work(work);
	}
}

/**
 * do_thaw_all_callback - 解冻单个超级块的回调函数
 * @sb: 要解冻的超级块
 *
 * 对单个超级块执行紧急解冻操作。这包括解冻底层块设备
 * 和超级块本身的冻结状态。
 */
static void do_thaw_all_callback(struct super_block *sb)
{
	down_write(&sb->s_umount);
	if (sb->s_root && sb->s_flags & SB_BORN) {
		emergency_thaw_bdev(sb);
		thaw_super_locked(sb);
	} else {
		up_write(&sb->s_umount);
	}
}

/**
 * do_thaw_all - 执行解冻所有文件系统的工作
 * @work: 工作队列项
 *
 * 工作队列函数，遍历所有超级块并强制解冻它们。
 * 这通常通过SysRq触发，用于紧急解冻所有冻结的文件系统。
 */
static void do_thaw_all(struct work_struct *work)
{
	__iterate_supers(do_thaw_all_callback);
	kfree(work);
	printk(KERN_WARNING "Emergency Thaw complete\n");
}

/**
 * emergency_thaw_all - 强制解冻每个冻结的文件系统
 *
 * 通过SysRq用于紧急解冻所有文件系统。在系统管理员需要
 * 快速解除所有文件系统冻结状态的紧急情况下使用。
 */
/**
 * emergency_thaw_all -- forcibly thaw every frozen filesystem
 *
 * Used for emergency unfreeze of all filesystems via SysRq
 */
void emergency_thaw_all(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_thaw_all);
		schedule_work(work);
	}
}

static DEFINE_IDA(unnamed_dev_ida);

/**
 * get_anon_bdev - 为没有块设备的文件系统分配一个虚拟块设备
 * @p: 指向dev_t的指针
 *
 * 不使用真实块设备的文件系统可以调用此函数来分配一个虚拟块设备。
 * 许多用户空间实用程序认为FSID为0是无效的，所以总是返回至少为1的值。
 *
 * 上下文: 任何上下文。经常在持有sb_lock时调用。
 * 返回值: 成功返回0，没有可用的匿名bdev返回-EMFILE，内存分配失败返回-ENOMEM
 */
/**
 * get_anon_bdev - Allocate a block device for filesystems which don't have one.
 * @p: Pointer to a dev_t.
 *
 * Filesystems which don't use real block devices can call this function
 * to allocate a virtual block device.
 *
 * Context: Any context.  Frequently called while holding sb_lock.
 * Return: 0 on success, -EMFILE if there are no anonymous bdevs left
 * or -ENOMEM if memory allocation failed.
 */
int get_anon_bdev(dev_t *p)
{
	int dev;

	/*
	 * Many userspace utilities consider an FSID of 0 invalid.
	 * Always return at least 1 from get_anon_bdev.
	 */
	dev = ida_alloc_range(&unnamed_dev_ida, 1, (1 << MINORBITS) - 1,
			GFP_ATOMIC);
	if (dev == -ENOSPC)
		dev = -EMFILE;
	if (dev < 0)
		return dev;

	*p = MKDEV(0, dev);
	return 0;
}
EXPORT_SYMBOL(get_anon_bdev);

/**
 * free_anon_bdev - 释放匿名块设备号
 * @dev: 要释放的设备号
 *
 * 释放之前通过get_anon_bdev()分配的虚拟块设备号，
 * 使其可以被其他文件系统重新使用。
 */
void free_anon_bdev(dev_t dev)
{
	ida_free(&unnamed_dev_ida, MINOR(dev));
}
EXPORT_SYMBOL(free_anon_bdev);

/**
 * set_anon_super - 为超级块设置匿名设备号
 * @s: 超级块
 * @data: 未使用的数据参数
 *
 * 为不使用真实块设备的文件系统设置匿名设备号。
 * 这是sget()函数使用的标准set回调函数。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int set_anon_super(struct super_block *s, void *data)
{
	return get_anon_bdev(&s->s_dev);
}
EXPORT_SYMBOL(set_anon_super);

/**
 * kill_anon_super - 销毁匿名超级块
 * @sb: 要销毁的超级块
 *
 * 销毁一个使用匿名设备号的超级块。执行通用关闭操作后
 * 释放分配的匿名设备号。这是不使用真实块设备的文件系统
 * 的标准kill_sb回调函数。
 */
void kill_anon_super(struct super_block *sb)
{
	dev_t dev = sb->s_dev;
	generic_shutdown_super(sb);
	free_anon_bdev(dev);
}
EXPORT_SYMBOL(kill_anon_super);

/**
 * kill_litter_super - 销毁临时文件系统超级块
 * @sb: 要销毁的超级块
 *
 * 销毁一个临时文件系统的超级块，这种文件系统通常用于测试
 * 或临时用途。首先清理所有dentry，然后调用kill_anon_super
 * 完成销毁过程。d_genocide确保所有目录项都被移除。
 */
void kill_litter_super(struct super_block *sb)
{
	if (sb->s_root)
		d_genocide(sb->s_root);
	kill_anon_super(sb);
}
EXPORT_SYMBOL(kill_litter_super);

/**
 * set_anon_super_fc - 为文件系统上下文设置匿名超级块
 * @sb: 超级块
 * @fc: 文件系统上下文
 *
 * set_anon_super的文件系统上下文版本，用于新的挂载API。
 * 为超级块分配匿名设备号。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int set_anon_super_fc(struct super_block *sb, struct fs_context *fc)
{
	return set_anon_super(sb, NULL);
}
EXPORT_SYMBOL(set_anon_super_fc);

/**
 * test_keyed_super - 基于键值测试超级块
 * @sb: 要测试的超级块
 * @fc: 文件系统上下文
 *
 * 比较超级块的s_fs_info与文件系统上下文中的s_fs_info，
 * 用于识别具有特定键的超级块。
 *
 * 返回值: 匹配返回非零值，不匹配返回0
 */
static int test_keyed_super(struct super_block *sb, struct fs_context *fc)
{
	return sb->s_fs_info == fc->s_fs_info;
}

/**
 * test_single_super - 单一超级块测试函数
 * @s: 超级块
 * @fc: 文件系统上下文
 *
 * 总是返回1，用于只允许存在单个实例的文件系统类型。
 * 这确保了只有一个该类型的超级块存在于系统中。
 *
 * 返回值: 总是返回1
 */
static int test_single_super(struct super_block *s, struct fs_context *fc)
{
	return 1;
}

/**
 * vfs_get_super - 获取带搜索键的超级块
 * @fc: 持有参数的文件系统上下文
 * @keying: 如何区分超级块
 * @fill_super: 初始化新超级块的帮助函数
 *
 * 搜索超级块，如果未找到则创建一个新的。搜索条件由@keying控制。
 * 如果搜索失败，创建新超级块并调用@fill_super()初始化它。
 *
 * @keying可以取以下值之一：
 *
 * (1) vfs_get_single_super - 系统中只能存在一个此类型的超级块。
 *     通常用于特殊系统文件系统。
 *
 * (2) vfs_get_keyed_super - 可以存在多个超级块，但它们必须有
 *     不同的键(键在s_fs_info中)。搜索相同键会找到该键的超级块。
 *
 * (3) vfs_get_independent_super - 可以存在多个超级块且无键。
 *     每次调用都会得到一个新超级块。
 *
 * 除非我们获取的是内核内部挂载或子挂载的超级块，否则sget_fc()会进行权限检查。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/**
 * vfs_get_super - Get a superblock with a search key set in s_fs_info.
 * @fc: The filesystem context holding the parameters
 * @keying: How to distinguish superblocks
 * @fill_super: Helper to initialise a new superblock
 *
 * Search for a superblock and create a new one if not found.  The search
 * criterion is controlled by @keying.  If the search fails, a new superblock
 * is created and @fill_super() is called to initialise it.
 *
 * @keying can take one of a number of values:
 *
 * (1) vfs_get_single_super - Only one superblock of this type may exist on the
 *     system.  This is typically used for special system filesystems.
 *
 * (2) vfs_get_keyed_super - Multiple superblocks may exist, but they must have
 *     distinct keys (where the key is in s_fs_info).  Searching for the same
 *     key again will turn up the superblock for that key.
 *
 * (3) vfs_get_independent_super - Multiple superblocks may exist and are
 *     unkeyed.  Each call will get a new superblock.
 *
 * A permissions check is made by sget_fc() unless we're getting a superblock
 * for a kernel-internal mount or a submount.
 */
int vfs_get_super(struct fs_context *fc,
		  enum vfs_get_super_keying keying,
		  int (*fill_super)(struct super_block *sb,
				    struct fs_context *fc))
{
	int (*test)(struct super_block *, struct fs_context *);
	struct super_block *sb;
	int err;

	switch (keying) {
	case vfs_get_single_super:
	case vfs_get_single_reconf_super:
		test = test_single_super;
		break;
	case vfs_get_keyed_super:
		test = test_keyed_super;
		break;
	case vfs_get_independent_super:
		test = NULL;
		break;
	default:
		BUG();
	}

	sb = sget_fc(fc, test, set_anon_super_fc);
	if (IS_ERR(sb))
		return PTR_ERR(sb);

	if (!sb->s_root) {
		err = fill_super(sb, fc);
		if (err)
			goto error;

		sb->s_flags |= SB_ACTIVE;
		fc->root = dget(sb->s_root);
	} else {
		fc->root = dget(sb->s_root);
		if (keying == vfs_get_single_reconf_super) {
			err = reconfigure_super(fc);
			if (err < 0) {
				dput(fc->root);
				fc->root = NULL;
				goto error;
			}
		}
	}

	return 0;

error:
	deactivate_locked_super(sb);
	return err;
}
EXPORT_SYMBOL(vfs_get_super);

/**
 * get_tree_nodev - 获取无设备文件系统树
 * @fc: 文件系统上下文
 * @fill_super: 超级块初始化函数
 *
 * 为不需要块设备的文件系统获取超级块树。每次调用都创建
 * 一个独立的超级块实例。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int get_tree_nodev(struct fs_context *fc,
		  int (*fill_super)(struct super_block *sb,
				    struct fs_context *fc))
{
	return vfs_get_super(fc, vfs_get_independent_super, fill_super);
}
EXPORT_SYMBOL(get_tree_nodev);

/**
 * get_tree_single - 获取单例文件系统树
 * @fc: 文件系统上下文
 * @fill_super: 超级块初始化函数
 *
 * 为只允许单个实例存在的文件系统获取超级块树。
 * 系统中只能有一个该类型的超级块。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int get_tree_single(struct fs_context *fc,
		  int (*fill_super)(struct super_block *sb,
				    struct fs_context *fc))
{
	return vfs_get_super(fc, vfs_get_single_super, fill_super);
}
EXPORT_SYMBOL(get_tree_single);

/**
 * get_tree_single_reconf - 获取可重新配置的单例文件系统树
 * @fc: 文件系统上下文
 * @fill_super: 超级块初始化函数
 *
 * 类似get_tree_single，但支持重新配置现有的单例超级块。
 * 如果超级块已存在，会尝试重新配置它而不是失败。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int get_tree_single_reconf(struct fs_context *fc,
		  int (*fill_super)(struct super_block *sb,
				    struct fs_context *fc))
{
	return vfs_get_super(fc, vfs_get_single_reconf_super, fill_super);
}
EXPORT_SYMBOL(get_tree_single_reconf);

/**
 * get_tree_keyed - 获取基于键的文件系统树
 * @fc: 文件系统上下文
 * @fill_super: 超级块初始化函数
 * @key: 用于区分超级块的键值
 *
 * 为使用键值区分不同实例的文件系统获取超级块树。
 * 具有相同键的挂载会共享同一个超级块。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int get_tree_keyed(struct fs_context *fc,
		  int (*fill_super)(struct super_block *sb,
				    struct fs_context *fc),
		void *key)
{
	fc->s_fs_info = key;
	return vfs_get_super(fc, vfs_get_keyed_super, fill_super);
}
EXPORT_SYMBOL(get_tree_keyed);

#ifdef CONFIG_BLOCK

/**
 * set_bdev_super - 为超级块设置块设备
 * @s: 超级块
 * @data: 块设备指针
 *
 * 将块设备与超级块关联，设置相应的设备号和后备设备信息。
 * 如果块设备队列支持稳定写入，则设置相应标志。
 *
 * 返回值: 总是返回0
 */
static int set_bdev_super(struct super_block *s, void *data)
{
	s->s_bdev = data;
	s->s_dev = s->s_bdev->bd_dev;
	s->s_bdi = bdi_get(s->s_bdev->bd_bdi);

	if (blk_queue_stable_writes(s->s_bdev->bd_disk->queue))
		s->s_iflags |= SB_I_STABLE_WRITES;
	return 0;
}

/**
 * set_bdev_super_fc - 文件系统上下文版本的set_bdev_super
 * @s: 超级块
 * @fc: 文件系统上下文
 *
 * set_bdev_super的文件系统上下文版本，用于新挂载API。
 *
 * 返回值: set_bdev_super的返回值
 */
static int set_bdev_super_fc(struct super_block *s, struct fs_context *fc)
{
	return set_bdev_super(s, fc->sget_key);
}

/**
 * test_bdev_super_fc - 文件系统上下文版本的块设备超级块测试
 * @s: 要测试的超级块
 * @fc: 文件系统上下文
 *
 * 比较超级块的块设备与文件系统上下文中的sget_key，
 * 用于识别使用特定块设备的超级块。
 *
 * 返回值: 匹配返回非零值，不匹配返回0
 */
static int test_bdev_super_fc(struct super_block *s, struct fs_context *fc)
{
	return s->s_bdev == fc->sget_key;
}

/**
 * get_tree_bdev - 基于单个块设备获取超级块
 * @fc: 持有参数的文件系统上下文
 * @fill_super: 初始化新超级块的帮助函数
 *
 * 为基于单个块设备的文件系统获取超级块。这是大多数传统
 * 文件系统使用的方法。函数处理设备打开、冻结检查、
 * 超级块创建或重用等复杂逻辑。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/**
 * get_tree_bdev - Get a superblock based on a single block device
 * @fc: The filesystem context holding the parameters
 * @fill_super: Helper to initialise a new superblock
 */
int get_tree_bdev(struct fs_context *fc,
		int (*fill_super)(struct super_block *,
				  struct fs_context *))
{
	struct block_device *bdev;
	struct super_block *s;
	fmode_t mode = FMODE_READ | FMODE_EXCL;
	int error = 0;

	if (!(fc->sb_flags & SB_RDONLY))
		mode |= FMODE_WRITE;

	if (!fc->source)
		return invalf(fc, "No source specified");

	bdev = blkdev_get_by_path(fc->source, mode, fc->fs_type);
	if (IS_ERR(bdev)) {
		errorf(fc, "%s: Can't open blockdev", fc->source);
		return PTR_ERR(bdev);
	}

	/* Once the superblock is inserted into the list by sget_fc(), s_umount
	 * will protect the lockfs code from trying to start a snapshot while
	 * we are mounting
	 */
	mutex_lock(&bdev->bd_fsfreeze_mutex);
	if (bdev->bd_fsfreeze_count > 0) {
		mutex_unlock(&bdev->bd_fsfreeze_mutex);
		warnf(fc, "%pg: Can't mount, blockdev is frozen", bdev);
		blkdev_put(bdev, mode);
		return -EBUSY;
	}

	fc->sb_flags |= SB_NOSEC;
	fc->sget_key = bdev;
	s = sget_fc(fc, test_bdev_super_fc, set_bdev_super_fc);
	mutex_unlock(&bdev->bd_fsfreeze_mutex);
	if (IS_ERR(s)) {
		blkdev_put(bdev, mode);
		return PTR_ERR(s);
	}

	if (s->s_root) {
		/* Don't summarily change the RO/RW state. */
		if ((fc->sb_flags ^ s->s_flags) & SB_RDONLY) {
			warnf(fc, "%pg: Can't mount, would change RO state", bdev);
			deactivate_locked_super(s);
			blkdev_put(bdev, mode);
			return -EBUSY;
		}

		/*
		 * s_umount nests inside bd_mutex during
		 * __invalidate_device().  blkdev_put() acquires
		 * bd_mutex and can't be called under s_umount.  Drop
		 * s_umount temporarily.  This is safe as we're
		 * holding an active reference.
		 */
		up_write(&s->s_umount);
		blkdev_put(bdev, mode);
		down_write(&s->s_umount);
	} else {
		s->s_mode = mode;
		snprintf(s->s_id, sizeof(s->s_id), "%pg", bdev);
		sb_set_blocksize(s, block_size(bdev));
		error = fill_super(s, fc);
		if (error) {
			deactivate_locked_super(s);
			return error;
		}

		s->s_flags |= SB_ACTIVE;
		bdev->bd_super = s;
	}

	BUG_ON(fc->root);
	fc->root = dget(s->s_root);
	return 0;
}
EXPORT_SYMBOL(get_tree_bdev);

/**
 * test_bdev_super - 测试块设备超级块
 * @s: 要测试的超级块
 * @data: 块设备指针
 *
 * 比较超级块的块设备与给定的块设备指针，
 * 用于旧挂载API中识别特定块设备的超级块。
 *
 * 返回值: 匹配返回非零值，不匹配返回0
 */
static int test_bdev_super(struct super_block *s, void *data)
{
	return (void *)s->s_bdev == data;
}

/**
 * mount_bdev - 挂载块设备上的文件系统
 * @fs_type: 文件系统类型
 * @flags: 挂载标志
 * @dev_name: 设备名称
 * @data: 文件系统特定数据
 * @fill_super: 初始化超级块的回调函数
 *
 * 在指定的块设备上挂载文件系统。这是大多数传统文件系统使用的挂载方法。
 *
 * 返回值: 成功返回根目录dentry，失败返回错误指针
 */
struct dentry *mount_bdev(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	struct block_device *bdev;
	struct super_block *s;
	fmode_t mode = FMODE_READ | FMODE_EXCL;
	int error = 0;

	if (!(flags & SB_RDONLY))
		mode |= FMODE_WRITE;

	bdev = blkdev_get_by_path(dev_name, mode, fs_type);
	if (IS_ERR(bdev))
		return ERR_CAST(bdev);

	/*
	 * once the super is inserted into the list by sget, s_umount
	 * will protect the lockfs code from trying to start a snapshot
	 * while we are mounting
	 */
	mutex_lock(&bdev->bd_fsfreeze_mutex);
	if (bdev->bd_fsfreeze_count > 0) {
		mutex_unlock(&bdev->bd_fsfreeze_mutex);
		error = -EBUSY;
		goto error_bdev;
	}
	s = sget(fs_type, test_bdev_super, set_bdev_super, flags | SB_NOSEC,
		 bdev);
	mutex_unlock(&bdev->bd_fsfreeze_mutex);
	if (IS_ERR(s))
		goto error_s;

	if (s->s_root) {
		if ((flags ^ s->s_flags) & SB_RDONLY) {
			deactivate_locked_super(s);
			error = -EBUSY;
			goto error_bdev;
		}

		/*
		 * s_umount nests inside bd_mutex during
		 * __invalidate_device().  blkdev_put() acquires
		 * bd_mutex and can't be called under s_umount.  Drop
		 * s_umount temporarily.  This is safe as we're
		 * holding an active reference.
		 */
		up_write(&s->s_umount);
		blkdev_put(bdev, mode);
		down_write(&s->s_umount);
	} else {
		s->s_mode = mode;
		snprintf(s->s_id, sizeof(s->s_id), "%pg", bdev);
		sb_set_blocksize(s, block_size(bdev));
		error = fill_super(s, data, flags & SB_SILENT ? 1 : 0);
		if (error) {
			deactivate_locked_super(s);
			goto error;
		}

		s->s_flags |= SB_ACTIVE;
		bdev->bd_super = s;
	}

	return dget(s->s_root);

error_s:
	error = PTR_ERR(s);
error_bdev:
	blkdev_put(bdev, mode);
error:
	return ERR_PTR(error);
}
EXPORT_SYMBOL(mount_bdev);

/**
 * kill_block_super - 销毁块设备文件系统超级块
 * @sb: 要销毁的超级块
 *
 * 销毁一个基于块设备的文件系统超级块。执行通用关闭操作，
 * 同步块设备，然后释放块设备。确保设备以独占模式释放。
 */
void kill_block_super(struct super_block *sb)
{
	struct block_device *bdev = sb->s_bdev;
	fmode_t mode = sb->s_mode;

	bdev->bd_super = NULL;
	generic_shutdown_super(sb);
	sync_blockdev(bdev);
	WARN_ON_ONCE(!(mode & FMODE_EXCL));
	blkdev_put(bdev, mode | FMODE_EXCL);
}

EXPORT_SYMBOL(kill_block_super);
#endif

/**
 * mount_nodev - 挂载不需要块设备的文件系统
 * @fs_type: 文件系统类型
 * @flags: 挂载标志
 * @data: 文件系统特定数据
 * @fill_super: 初始化超级块的回调函数
 *
 * 挂载不需要底层块设备的文件系统，如proc、sysfs等虚拟文件系统。
 *
 * 返回值: 成功返回根目录dentry，失败返回错误指针
 */
struct dentry *mount_nodev(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	int error;
	struct super_block *s = sget(fs_type, NULL, set_anon_super, flags, NULL);

	if (IS_ERR(s))
		return ERR_CAST(s);

	error = fill_super(s, data, flags & SB_SILENT ? 1 : 0);
	if (error) {
		deactivate_locked_super(s);
		return ERR_PTR(error);
	}
	s->s_flags |= SB_ACTIVE;
	return dget(s->s_root);
}
EXPORT_SYMBOL(mount_nodev);

/**
 * reconfigure_single - 重新配置单例超级块
 * @s: 超级块
 * @flags: 新的挂载标志
 * @data: 挂载数据
 *
 * 重新配置一个单例文件系统的超级块。创建文件系统上下文
 * 并解析挂载数据，然后调用reconfigure_super执行实际的重新配置。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
static int reconfigure_single(struct super_block *s,
			      int flags, void *data)
{
	struct fs_context *fc;
	int ret;

	/* The caller really need to be passing fc down into mount_single(),
	 * then a chunk of this can be removed.  [Bollocks -- AV]
	 * Better yet, reconfiguration shouldn't happen, but rather the second
	 * mount should be rejected if the parameters are not compatible.
	 */
	fc = fs_context_for_reconfigure(s->s_root, flags, MS_RMT_MASK);
	if (IS_ERR(fc))
		return PTR_ERR(fc);

	ret = parse_monolithic_mount_data(fc, data);
	if (ret < 0)
		goto out;

	ret = reconfigure_super(fc);
out:
	put_fs_context(fc);
	return ret;
}

/**
 * compare_single - 单例超级块比较函数
 * @s: 超级块
 * @p: 比较参数(未使用)
 *
 * 用于单例文件系统的比较函数，总是返回1表示匹配。
 * 这确保了单例文件系统总是重用现有的超级块。
 *
 * 返回值: 总是返回1
 */
static int compare_single(struct super_block *s, void *p)
{
	return 1;
}

/**
 * mount_single - 挂载单例文件系统
 * @fs_type: 文件系统类型
 * @flags: 挂载标志
 * @data: 文件系统特定数据
 * @fill_super: 初始化超级块的回调函数
 *
 * 挂载一个单例文件系统，系统中只能存在一个该类型的实例。
 * 如果超级块已存在，则尝试重新配置它；否则创建新的超级块。
 *
 * 返回值: 成功返回根目录dentry，失败返回错误指针
 */
struct dentry *mount_single(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	struct super_block *s;
	int error;

	s = sget(fs_type, compare_single, set_anon_super, flags, NULL);
	if (IS_ERR(s))
		return ERR_CAST(s);
	if (!s->s_root) {
		error = fill_super(s, data, flags & SB_SILENT ? 1 : 0);
		if (!error)
			s->s_flags |= SB_ACTIVE;
	} else {
		error = reconfigure_single(s, flags, data);
	}
	if (unlikely(error)) {
		deactivate_locked_super(s);
		return ERR_PTR(error);
	}
	return dget(s->s_root);
}
EXPORT_SYMBOL(mount_single);

/**
 * vfs_get_tree - 获取可挂载的根目录
 * @fc: 超级块配置上下文
 *
 * 调用文件系统来获取或创建一个超级块，该超级块稍后可以用于挂载。
 * 文件系统将用于挂载的根目录指针放在@fc->root中。执行必要的
 * 安全检查和验证，确保超级块正确配置。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/**
 * vfs_get_tree - Get the mountable root
 * @fc: The superblock configuration context.
 *
 * The filesystem is invoked to get or create a superblock which can then later
 * be used for mounting.  The filesystem places a pointer to the root to be
 * used for mounting in @fc->root.
 */
int vfs_get_tree(struct fs_context *fc)
{
	struct super_block *sb;
	int error;

	if (fc->root)
		return -EBUSY;

	/* Get the mountable root in fc->root, with a ref on the root and a ref
	 * on the superblock.
	 */
	error = fc->ops->get_tree(fc);
	if (error < 0)
		return error;

	if (!fc->root) {
		pr_err("Filesystem %s get_tree() didn't set fc->root\n",
		       fc->fs_type->name);
		/* We don't know what the locking state of the superblock is -
		 * if there is a superblock.
		 */
		BUG();
	}

	sb = fc->root->d_sb;
	WARN_ON(!sb->s_bdi);

	/*
	 * Write barrier is for super_cache_count(). We place it before setting
	 * SB_BORN as the data dependency between the two functions is the
	 * superblock structure contents that we just set up, not the SB_BORN
	 * flag.
	 */
	smp_wmb();
	sb->s_flags |= SB_BORN;

	error = security_sb_set_mnt_opts(sb, fc->security, 0, NULL);
	if (unlikely(error)) {
		fc_drop_locked(fc);
		return error;
	}

	/*
	 * filesystems should never set s_maxbytes larger than MAX_LFS_FILESIZE
	 * but s_maxbytes was an unsigned long long for many releases. Throw
	 * this warning for a little while to try and catch filesystems that
	 * violate this rule.
	 */
	WARN((sb->s_maxbytes < 0), "%s set sb->s_maxbytes to "
		"negative value (%lld)\n", fc->fs_type->name, sb->s_maxbytes);

	return 0;
}
EXPORT_SYMBOL(vfs_get_tree);

/**
 * super_setup_bdi_name - 为超级块设置私有BDI(带名称)
 * @sb: 超级块
 * @fmt: 名称格式字符串
 * @...: 格式参数
 *
 * 为给定的超级块设置私有的后备设备信息(BDI)。它会在
 * generic_shutdown_super()中自动清理。使用指定的格式
 * 字符串创建BDI名称。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/*
 * Setup private BDI for given superblock. It gets automatically cleaned up
 * in generic_shutdown_super().
 */
int super_setup_bdi_name(struct super_block *sb, char *fmt, ...)
{
	struct backing_dev_info *bdi;
	int err;
	va_list args;

	bdi = bdi_alloc(NUMA_NO_NODE);
	if (!bdi)
		return -ENOMEM;

	va_start(args, fmt);
	err = bdi_register_va(bdi, fmt, args);
	va_end(args);
	if (err) {
		bdi_put(bdi);
		return err;
	}
	WARN_ON(sb->s_bdi != &noop_backing_dev_info);
	sb->s_bdi = bdi;

	return 0;
}
EXPORT_SYMBOL(super_setup_bdi_name);

/**
 * super_setup_bdi - 为超级块设置私有BDI
 * @sb: 超级块
 *
 * 为给定的超级块设置私有的后备设备信息(BDI)。它会在
 * generic_shutdown_super()中自动清理。使用文件系统类型名称
 * 和序列号生成BDI名称。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/*
 * Setup private BDI for given superblock. I gets automatically cleaned up
 * in generic_shutdown_super().
 */
int super_setup_bdi(struct super_block *sb)
{
	static atomic_long_t bdi_seq = ATOMIC_LONG_INIT(0);

	return super_setup_bdi_name(sb, "%.28s-%ld", sb->s_type->name,
				    atomic_long_inc_return(&bdi_seq));
}
EXPORT_SYMBOL(super_setup_bdi);

/**
 * sb_wait_write - 等待文件系统的所有写入者完成
 * @sb: 等待的超级块
 * @level: 等待的写入者类型(普通写入者 vs 页面错误)
 *
 * 此函数等待直到给定文件系统没有指定类型的写入者。
 * 用于文件系统冻结过程中确保所有写入操作完成。
 */
/**
 * sb_wait_write - wait until all writers to given file system finish
 * @sb: the super for which we wait
 * @level: type of writers we wait for (normal vs page fault)
 *
 * This function waits until there are no writers of given type to given file
 * system.
 */
static void sb_wait_write(struct super_block *sb, int level)
{
	percpu_down_write(sb->s_writers.rw_sem + level-1);
}

/**
 * lockdep_sb_freeze_release - 释放超级块冻结的lockdep锁
 * @sb: 超级块
 *
 * 我们即将返回用户空间并忘记这些锁，锁的所有权转移给
 * thaw_super()的调用者来执行unlock()。释放所有冻结级别的
 * percpu读写信号量的lockdep信息。
 */
/*
 * We are going to return to userspace and forget about these locks, the
 * ownership goes to the caller of thaw_super() which does unlock().
 */
static void lockdep_sb_freeze_release(struct super_block *sb)
{
	int level;

	for (level = SB_FREEZE_LEVELS - 1; level >= 0; level--)
		percpu_rwsem_release(sb->s_writers.rw_sem + level, 0, _THIS_IP_);
}

/**
 * lockdep_sb_freeze_acquire - 获取超级块冻结的lockdep锁
 * @sb: 超级块
 *
 * 告诉lockdep我们在调用->unfreeze_fs(sb)之前持有这些锁。
 * 获取所有冻结级别的percpu读写信号量的lockdep信息。
 */
/*
 * Tell lockdep we are holding these locks before we call ->unfreeze_fs(sb).
 */
static void lockdep_sb_freeze_acquire(struct super_block *sb)
{
	int level;

	for (level = 0; level < SB_FREEZE_LEVELS; ++level)
		percpu_rwsem_acquire(sb->s_writers.rw_sem + level, 0, _THIS_IP_);
}

/**
 * sb_freeze_unlock - 解锁超级块的所有冻结级别
 * @sb: 超级块
 *
 * 从最高冻结级别到最低级别依次释放所有写入者信号量，
 * 完成超级块的解冻过程。
 */
static void sb_freeze_unlock(struct super_block *sb)
{
	int level;

	for (level = SB_FREEZE_LEVELS - 1; level >= 0; level--)
		percpu_up_write(sb->s_writers.rw_sem + level);
}

/**
 * freeze_super - 锁定文件系统并强制其进入一致状态
 * @sb: 要锁定的超级块
 *
 * 同步超级块以确保文件系统一致，并调用文件系统的freeze_fs。
 * 在没有首先解冻文件系统的情况下对此函数的后续调用将返回-EBUSY。
 *
 * 在此函数期间，sb->s_writers.frozen经历以下值：
 *
 * SB_UNFROZEN: 文件系统正常，所有写入正常进行。
 *
 * SB_FREEZE_WRITE: 文件系统正在冻结过程中。新的写入应该被阻塞，
 * 但页面错误仍然被允许。我们等待所有写入完成然后进入下一阶段。
 *
 * SB_FREEZE_PAGEFAULT: 冻结继续。现在页面错误也被阻塞，但内部
 * 文件系统线程仍可修改文件系统(虽然不应该弄脏新页面或inode)，
 * 回写可以运行等。等待所有运行的页面错误后我们同步文件系统，
 * 这将清理所有脏页面和inode。
 *
 * SB_FREEZE_FS: 文件系统被冻结。现在所有内部文件系统修改源都被
 * 阻塞。这通常通过阻塞新事务来实现。所有内部写入者完成后我们
 * 调用->freeze_fs()完成文件系统冻结。然后转换到SB_FREEZE_COMPLETE状态。
 *
 * sb->s_writers.frozen由sb->s_umount保护。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/**
 * freeze_super - lock the filesystem and force it into a consistent state
 * @sb: the super to lock
 *
 * Syncs the super to make sure the filesystem is consistent and calls the fs's
 * freeze_fs.  Subsequent calls to this without first thawing the fs will return
 * -EBUSY.
 *
 * During this function, sb->s_writers.frozen goes through these values:
 *
 * SB_UNFROZEN: File system is normal, all writes progress as usual.
 *
 * SB_FREEZE_WRITE: The file system is in the process of being frozen.  New
 * writes should be blocked, though page faults are still allowed. We wait for
 * all writes to complete and then proceed to the next stage.
 *
 * SB_FREEZE_PAGEFAULT: Freezing continues. Now also page faults are blocked
 * but internal fs threads can still modify the filesystem (although they
 * should not dirty new pages or inodes), writeback can run etc. After waiting
 * for all running page faults we sync the filesystem which will clean all
 * dirty pages and inodes (no new dirty pages or inodes can be created when
 * sync is running).
 *
 * SB_FREEZE_FS: The file system is frozen. Now all internal sources of fs
 * modification are blocked (e.g. XFS preallocation truncation on inode
 * reclaim). This is usually implemented by blocking new transactions for
 * filesystems that have them and need this additional guard. After all
 * internal writers are finished we call ->freeze_fs() to finish filesystem
 * freezing. Then we transition to SB_FREEZE_COMPLETE state. This state is
 * mostly auxiliary for filesystems to verify they do not modify frozen fs.
 *
 * sb->s_writers.frozen is protected by sb->s_umount.
 */
int freeze_super(struct super_block *sb)
{
	int ret;

	atomic_inc(&sb->s_active);
	down_write(&sb->s_umount);
	if (sb->s_writers.frozen != SB_UNFROZEN) {
		deactivate_locked_super(sb);
		return -EBUSY;
	}

	if (!(sb->s_flags & SB_BORN)) {
		up_write(&sb->s_umount);
		return 0;	/* sic - it's "nothing to do" */
	}

	if (sb_rdonly(sb)) {
		/* Nothing to do really... */
		sb->s_writers.frozen = SB_FREEZE_COMPLETE;
		up_write(&sb->s_umount);
		return 0;
	}

	sb->s_writers.frozen = SB_FREEZE_WRITE;
	/* Release s_umount to preserve sb_start_write -> s_umount ordering */
	up_write(&sb->s_umount);
	sb_wait_write(sb, SB_FREEZE_WRITE);
	down_write(&sb->s_umount);

	/* Now we go and block page faults... */
	sb->s_writers.frozen = SB_FREEZE_PAGEFAULT;
	sb_wait_write(sb, SB_FREEZE_PAGEFAULT);

	/* All writers are done so after syncing there won't be dirty data */
	sync_filesystem(sb);

	/* Now wait for internal filesystem counter */
	sb->s_writers.frozen = SB_FREEZE_FS;
	sb_wait_write(sb, SB_FREEZE_FS);

	if (sb->s_op->freeze_fs) {
		ret = sb->s_op->freeze_fs(sb);
		if (ret) {
			printk(KERN_ERR
				"VFS:Filesystem freeze failed\n");
			sb->s_writers.frozen = SB_UNFROZEN;
			sb_freeze_unlock(sb);
			wake_up(&sb->s_writers.wait_unfrozen);
			deactivate_locked_super(sb);
			return ret;
		}
	}
	/*
	 * For debugging purposes so that fs can warn if it sees write activity
	 * when frozen is set to SB_FREEZE_COMPLETE, and for thaw_super().
	 */
	sb->s_writers.frozen = SB_FREEZE_COMPLETE;
	lockdep_sb_freeze_release(sb);
	up_write(&sb->s_umount);
	return 0;
}
EXPORT_SYMBOL(freeze_super);

/**
 * thaw_super_locked - 解锁文件系统(内部函数)
 * @sb: 要解冻的超级块
 *
 * 在freeze_super()之后解锁文件系统并将其标记为可写。
 * 这是thaw_super的内部实现，假设调用者已经持有s_umount锁。
 * 逐步撤销冻结过程中设置的各种限制。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/**
 * thaw_super -- unlock filesystem
 * @sb: the super to thaw
 *
 * Unlocks the filesystem and marks it writeable again after freeze_super().
 */
static int thaw_super_locked(struct super_block *sb)
{
	int error;

	if (sb->s_writers.frozen != SB_FREEZE_COMPLETE) {
		up_write(&sb->s_umount);
		return -EINVAL;
	}

	if (sb_rdonly(sb)) {
		sb->s_writers.frozen = SB_UNFROZEN;
		goto out;
	}

	lockdep_sb_freeze_acquire(sb);

	if (sb->s_op->unfreeze_fs) {
		error = sb->s_op->unfreeze_fs(sb);
		if (error) {
			printk(KERN_ERR
				"VFS:Filesystem thaw failed\n");
			lockdep_sb_freeze_release(sb);
			up_write(&sb->s_umount);
			return error;
		}
	}

	sb->s_writers.frozen = SB_UNFROZEN;
	sb_freeze_unlock(sb);
out:
	wake_up(&sb->s_writers.wait_unfrozen);
	deactivate_locked_super(sb);
	return 0;
}

/**
 * thaw_super - 解冻文件系统
 * @sb: 要解冻的超级块
 *
 * 在freeze_super()之后解锁文件系统并将其标记为可写。
 * 获取s_umount锁后调用thaw_super_locked执行实际的解冻操作。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int thaw_super(struct super_block *sb)
{
	down_write(&sb->s_umount);
	return thaw_super_locked(sb);
}
EXPORT_SYMBOL(thaw_super);
