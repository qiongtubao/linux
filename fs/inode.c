// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux VFS inode管理
 *
 * 本文件实现了VFS层的inode（索引节点）管理：
 * - inode的分配、初始化和释放
 * - inode缓存（icache）管理
 * - inode的查找和哈希
 * - 脏inode的跟踪和同步
 * - inode的引用计数管理
 *
 * (C) 1997 Linus Torvalds
 * (C) 1999 Andrea Arcangeli <andrea@suse.de> (dynamic inode allocation)
 */
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/backing-dev.h>
#include <linux/hash.h>
#include <linux/swap.h>
#include <linux/security.h>
#include <linux/cdev.h>
#include <linux/memblock.h>
#include <linux/fscrypt.h>
#include <linux/fsnotify.h>
#include <linux/mount.h>
#include <linux/posix_acl.h>
#include <linux/prefetch.h>
#include <linux/buffer_head.h> /* for inode_has_buffers */
#include <linux/ratelimit.h>
#include <linux/list_lru.h>
#include <linux/iversion.h>
#include <trace/events/writeback.h>
#include "internal.h"

/*
 * Inode锁定规则:
 *
 * inode->i_lock 保护:
 *   inode->i_state, inode->i_hash, __iget()
 * Inode LRU列表锁保护:
 *   inode->i_sb->s_inode_lru, inode->i_lru
 * inode->i_sb->s_inode_list_lock 保护:
 *   inode->i_sb->s_inodes, inode->i_sb_list
 * bdi->wb.list_lock 保护:
 *   bdi->wb.b_{dirty,io,more_io,dirty_time}, inode->i_io_list
 * inode_hash_lock 保护:
 *   inode_hashtable, inode->i_hash
 *
 * 锁定顺序:
 *
 * inode->i_sb->s_inode_list_lock
 *   inode->i_lock
 *     Inode LRU list locks
 *
 * bdi->wb.list_lock
 *   inode->i_lock
 *
 * inode_hash_lock
 *   inode->i_sb->s_inode_list_lock
 *   inode->i_lock
 *
 * iunique_lock
 *   inode_hash_lock
 */

/* inode哈希表相关全局变量 */
static unsigned int i_hash_mask __read_mostly;       /* 哈希掩码，用于计算哈希值 */
static unsigned int i_hash_shift __read_mostly;      /* 哈希移位值 */
static struct hlist_head *inode_hashtable __read_mostly; /* inode哈希表 */
static __cacheline_aligned_in_smp DEFINE_SPINLOCK(inode_hash_lock);  /* 哈希表锁 */

/*
 * 空的地址空间操作集合
 * 当用户没有定义任何地址空间操作时可以使用这个
 */
const struct address_space_operations empty_aops = {
};
EXPORT_SYMBOL(empty_aops);

/*
 * inode统计信息收集
 */
struct inodes_stat_t inodes_stat;

/* 每CPU变量：记录inode总数和未使用数 */
static DEFINE_PER_CPU(unsigned long, nr_inodes);    /* 总inode数量 */
static DEFINE_PER_CPU(unsigned long, nr_unused);    /* 未使用inode数量 */

static struct kmem_cache *inode_cachep __read_mostly;  /* inode缓存池 */

/**
 * get_nr_inodes - 获取系统中inode总数
 *
 * 遍历所有CPU统计每CPU的inode数量，返回总和
 * 返回值: inode总数
 */
static long get_nr_inodes(void)
{
	int i;
	long sum = 0;
	for_each_possible_cpu(i)
		sum += per_cpu(nr_inodes, i);
	return sum < 0 ? 0 : sum;
}

/**
 * get_nr_inodes_unused - 获取未使用的inode数量
 *
 * 遍历所有CPU统计未使用的inode数量，返回总和
 * 返回值: 未使用的inode总数
 */
static inline long get_nr_inodes_unused(void)
{
	int i;
	long sum = 0;
	for_each_possible_cpu(i)
		sum += per_cpu(nr_unused, i);
	return sum < 0 ? 0 : sum;
}

/**
 * get_nr_dirty_inodes - 获取脏inode的数量
 *
 * 这个函数计算脏inode的近似数量，实际上不是真正的脏inode数，
 * 而是一个粗略的近似值：总inode数 - 未使用的inode数
 *
 * 返回值: 脏inode的近似数量
 */
long get_nr_dirty_inodes(void)
{
	/* not actually dirty inodes, but a wild approximation */
	long nr_dirty = get_nr_inodes() - get_nr_inodes_unused();
	return nr_dirty > 0 ? nr_dirty : 0;
}

/*
 * 处理nr_inode系统控制
 */
#ifdef CONFIG_SYSCTL
/**
 * proc_nr_inodes - 处理/proc/sys/fs/inode-nr的读写
 * @table: sysctl表项
 * @write: 是否为写操作
 * @buffer: 用户数据缓冲区
 * @lenp: 数据长度
 * @ppos: 文件位置
 *
 * 返回值: 成功返回0，失败返回错误码
 */
int proc_nr_inodes(struct ctl_table *table, int write,
		   void *buffer, size_t *lenp, loff_t *ppos)
{
	inodes_stat.nr_inodes = get_nr_inodes();
	inodes_stat.nr_unused = get_nr_inodes_unused();
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}
#endif

/**
 * no_open - 默认的打开操作，总是返回错误
 * @inode: inode节点
 * @file: 文件结构
 *
 * 这是一个默认的文件打开操作，用于没有定义打开操作的inode
 * 返回值: 总是返回-ENXIO (设备不存在)
 */
static int no_open(struct inode *inode, struct file *file)
{
	return -ENXIO;
}

/**
 * inode_init_always - 执行inode结构的初始化
 * @sb: inode所属的超级块
 * @inode: 要初始化的inode
 *
 * 这些是每次分配inode时都需要进行的初始化，因为slab分配器
 * 不会初始化这些字段。初始化inode的各种字段、锁、地址空间等。
 *
 * 返回值: 成功返回0，失败返回-ENOMEM
 */
int inode_init_always(struct super_block *sb, struct inode *inode)
{
	static const struct inode_operations empty_iops;
	static const struct file_operations no_open_fops = {.open = no_open};
	struct address_space *const mapping = &inode->i_data;

	inode->i_sb = sb;
	inode->i_blkbits = sb->s_blocksize_bits;
	inode->i_flags = 0;
	atomic64_set(&inode->i_sequence, 0);
	atomic_set(&inode->i_count, 1);
	inode->i_op = &empty_iops;
	inode->i_fop = &no_open_fops;
	inode->__i_nlink = 1;
	inode->i_opflags = 0;
	if (sb->s_xattr)
		inode->i_opflags |= IOP_XATTR;
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	atomic_set(&inode->i_writecount, 0);
	inode->i_size = 0;
	inode->i_write_hint = WRITE_LIFE_NOT_SET;
	inode->i_blocks = 0;
	inode->i_bytes = 0;
	inode->i_generation = 0;
	inode->i_pipe = NULL;
	inode->i_bdev = NULL;
	inode->i_cdev = NULL;
	inode->i_link = NULL;
	inode->i_dir_seq = 0;
	inode->i_rdev = 0;
	inode->dirtied_when = 0;

#ifdef CONFIG_CGROUP_WRITEBACK
	inode->i_wb_frn_winner = 0;
	inode->i_wb_frn_avg_time = 0;
	inode->i_wb_frn_history = 0;
#endif

	if (security_inode_alloc(inode))
		goto out;
	spin_lock_init(&inode->i_lock);
	lockdep_set_class(&inode->i_lock, &sb->s_type->i_lock_key);

	init_rwsem(&inode->i_rwsem);
	lockdep_set_class(&inode->i_rwsem, &sb->s_type->i_mutex_key);

	atomic_set(&inode->i_dio_count, 0);

	mapping->a_ops = &empty_aops;
	mapping->host = inode;
	mapping->flags = 0;
	if (sb->s_type->fs_flags & FS_THP_SUPPORT)
		__set_bit(AS_THP_SUPPORT, &mapping->flags);
	mapping->wb_err = 0;
	atomic_set(&mapping->i_mmap_writable, 0);
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	atomic_set(&mapping->nr_thps, 0);
#endif
	mapping_set_gfp_mask(mapping, GFP_HIGHUSER_MOVABLE);
	mapping->private_data = NULL;
	mapping->writeback_index = 0;
	inode->i_private = NULL;
	inode->i_mapping = mapping;
	INIT_HLIST_HEAD(&inode->i_dentry);	/* buggered by rcu freeing */
#ifdef CONFIG_FS_POSIX_ACL
	inode->i_acl = inode->i_default_acl = ACL_NOT_CACHED;
#endif

#ifdef CONFIG_FSNOTIFY
	inode->i_fsnotify_mask = 0;
#endif
	inode->i_flctx = NULL;
	this_cpu_inc(nr_inodes);

	return 0;
out:
	return -ENOMEM;
}
EXPORT_SYMBOL(inode_init_always);

/**
 * free_inode_nonrcu - 非RCU方式释放inode内存
 * @inode: 要释放的inode
 *
 * 直接将inode内存释放回slab缓存，不使用RCU延迟释放
 */
void free_inode_nonrcu(struct inode *inode)
{
	kmem_cache_free(inode_cachep, inode);
}
EXPORT_SYMBOL(free_inode_nonrcu);

/**
 * i_callback - RCU回调函数，用于释放inode
 * @head: RCU头部
 *
 * RCU宽限期结束后调用此函数来实际释放inode内存。
 * 如果文件系统定义了free_inode操作则使用它，否则使用默认释放函数。
 */
static void i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	if (inode->free_inode)
		inode->free_inode(inode);
	else
		free_inode_nonrcu(inode);
}

/**
 * alloc_inode - 分配一个新的inode
 * @sb: 超级块指针
 *
 * 为指定的超级块分配一个新的inode。如果文件系统定义了自己的
 * alloc_inode操作，则使用它；否则从inode缓存中分配。
 *
 * 返回值: 成功返回inode指针，失败返回NULL
 */
static struct inode *alloc_inode(struct super_block *sb)
{
	const struct super_operations *ops = sb->s_op;
	struct inode *inode;

	if (ops->alloc_inode)
		inode = ops->alloc_inode(sb);
	else
		inode = kmem_cache_alloc(inode_cachep, GFP_KERNEL);

	if (!inode)
		return NULL;

	if (unlikely(inode_init_always(sb, inode))) {
		if (ops->destroy_inode) {
			ops->destroy_inode(inode);
			if (!ops->free_inode)
				return NULL;
		}
		inode->free_inode = ops->free_inode;
		i_callback(&inode->i_rcu);
		return NULL;
	}

	return inode;
}

/**
 * __destroy_inode - 销毁inode的内部处理
 * @inode: 要销毁的inode
 *
 * 执行inode销毁的核心工作：
 * - 检查并清理缓冲区
 * - 分离writeback相关资源
 * - 释放安全和通知相关资源
 * - 释放文件锁上下文
 * - 清理POSIX ACL
 * - 更新统计计数
 */
void __destroy_inode(struct inode *inode)
{
	BUG_ON(inode_has_buffers(inode));
	inode_detach_wb(inode);
	security_inode_free(inode);
	fsnotify_inode_delete(inode);
	locks_free_lock_context(inode);
	if (!inode->i_nlink) {
		WARN_ON(atomic_long_read(&inode->i_sb->s_remove_count) == 0);
		atomic_long_dec(&inode->i_sb->s_remove_count);
	}

#ifdef CONFIG_FS_POSIX_ACL
	if (inode->i_acl && !is_uncached_acl(inode->i_acl))
		posix_acl_release(inode->i_acl);
	if (inode->i_default_acl && !is_uncached_acl(inode->i_default_acl))
		posix_acl_release(inode->i_default_acl);
#endif
	this_cpu_dec(nr_inodes);
}
EXPORT_SYMBOL(__destroy_inode);

/**
 * destroy_inode - 销毁inode
 * @inode: 要销毁的inode
 *
 * 完整销毁一个inode：
 * 1. 确保inode不在LRU列表中
 * 2. 调用__destroy_inode执行内部清理
 * 3. 如果文件系统定义了destroy_inode操作则调用它
 * 4. 使用RCU延迟释放内存
 */
static void destroy_inode(struct inode *inode)
{
	const struct super_operations *ops = inode->i_sb->s_op;

	BUG_ON(!list_empty(&inode->i_lru));
	__destroy_inode(inode);
	if (ops->destroy_inode) {
		ops->destroy_inode(inode);
		if (!ops->free_inode)
			return;
	}
	inode->free_inode = ops->free_inode;
	call_rcu(&inode->i_rcu, i_callback);
}

/**
 * drop_nlink - 直接减少inode的链接计数
 * @inode: 目标inode
 *
 * 这是一个低级文件系统辅助函数，用于替代直接操作i_nlink。
 * 在我们试图跟踪写入到文件系统时，减少到零意味着当文件被
 * 截断并在文件系统上真正取消链接时即将发生写入。
 */
void drop_nlink(struct inode *inode)
{
	WARN_ON(inode->i_nlink == 0);
	inode->__i_nlink--;
	if (!inode->i_nlink)
		atomic_long_inc(&inode->i_sb->s_remove_count);
}
EXPORT_SYMBOL(drop_nlink);

/**
 * clear_nlink - 直接将inode的链接计数清零
 * @inode: 目标inode
 *
 * 这是一个低级文件系统辅助函数，用于替代直接操作i_nlink。
 * 请参见drop_nlink()了解为什么我们关心i_nlink变为零。
 */
void clear_nlink(struct inode *inode)
{
	if (inode->i_nlink) {
		inode->__i_nlink = 0;
		atomic_long_inc(&inode->i_sb->s_remove_count);
	}
}
EXPORT_SYMBOL(clear_nlink);

/**
 * set_nlink - 直接设置inode的链接计数
 * @inode: 目标inode
 * @nlink: 新的链接计数（应该非零）
 *
 * 这是一个低级文件系统辅助函数，用于替代直接操作i_nlink。
 */
void set_nlink(struct inode *inode, unsigned int nlink)
{
	if (!nlink) {
		clear_nlink(inode);
	} else {
		/* Yes, some filesystems do change nlink from zero to one */
		if (inode->i_nlink == 0)
			atomic_long_dec(&inode->i_sb->s_remove_count);

		inode->__i_nlink = nlink;
	}
}
EXPORT_SYMBOL(set_nlink);

/**
 * inc_nlink - 直接增加inode的链接计数
 * @inode: 目标inode
 *
 * 这是一个低级文件系统辅助函数，用于替代直接操作i_nlink。
 * 目前它在这里只是为了与dec_nlink()保持对称性。
 */
void inc_nlink(struct inode *inode)
{
	if (unlikely(inode->i_nlink == 0)) {
		WARN_ON(!(inode->i_state & I_LINKABLE));
		atomic_long_dec(&inode->i_sb->s_remove_count);
	}

	inode->__i_nlink++;
}
EXPORT_SYMBOL(inc_nlink);

/**
 * __address_space_init_once - 地址空间结构的内部初始化
 * @mapping: 要初始化的地址空间
 *
 * 初始化地址空间的核心数据结构：
 * - 页面缓存(i_pages)
 * - 内存映射读写信号量
 * - 私有数据列表和锁
 * - VMA红黑树
 */
static void __address_space_init_once(struct address_space *mapping)
{
	xa_init_flags(&mapping->i_pages, XA_FLAGS_LOCK_IRQ | XA_FLAGS_ACCOUNT);
	init_rwsem(&mapping->i_mmap_rwsem);
	INIT_LIST_HEAD(&mapping->private_list);
	spin_lock_init(&mapping->private_lock);
	mapping->i_mmap = RB_ROOT_CACHED;
}

/**
 * address_space_init_once - 完整初始化地址空间结构
 * @mapping: 要初始化的地址空间
 *
 * 清零整个结构并调用内部初始化函数。这个函数通常在
 * slab构造函数中使用。
 */
void address_space_init_once(struct address_space *mapping)
{
	memset(mapping, 0, sizeof(*mapping));
	__address_space_init_once(mapping);
}
EXPORT_SYMBOL(address_space_init_once);

/*
 * 这些初始化只需要做一次，因为这些字段在inode的
 * 整个使用过程中是幂等的，所以让slab知道这一点。
 */
/**
 * inode_init_once - inode结构的一次性初始化
 * @inode: 要初始化的inode
 *
 * 初始化inode中只需要设置一次的字段，如各种列表头、
 * 哈希节点、地址空间等。这些字段在inode的整个生命
 * 周期中保持不变。
 */
void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_devices);
	INIT_LIST_HEAD(&inode->i_io_list);
	INIT_LIST_HEAD(&inode->i_wb_list);
	INIT_LIST_HEAD(&inode->i_lru);
	__address_space_init_once(&inode->i_data);
	i_size_ordered_init(inode);
}
EXPORT_SYMBOL(inode_init_once);

/**
 * init_once - slab缓存构造函数
 * @foo: 要初始化的对象（实际上是inode指针）
 *
 * 这是inode_cachep的slab构造函数，在对象第一次分配时调用
 */
static void init_once(void *foo)
{
	struct inode *inode = (struct inode *) foo;

	inode_init_once(inode);
}

/*
 * 必须持有inode->i_lock
 */
/**
 * __iget - 增加inode引用计数（需要持锁）
 * @inode: 要增加引用的inode
 *
 * 原子性地增加inode的引用计数。调用前必须持有inode->i_lock。
 * 这是内部函数，外部调用应使用ihold()或igrab()。
 */
void __iget(struct inode *inode)
{
	atomic_inc(&inode->i_count);
}

/**
 * ihold - 获取inode的额外引用
 * @inode: 要增加引用的inode
 *
 * 获取inode的额外引用；调用者必须已经持有一个引用。
 * 用于在已有引用的基础上增加引用计数。
 *
 * 返回值: 无
 */
/*
 * get additional reference to inode; caller must already hold one.
 */
void ihold(struct inode *inode)
{
	WARN_ON(atomic_inc_return(&inode->i_count) < 2);
}
EXPORT_SYMBOL(ihold);

/**
 * inode_lru_list_add - 将inode添加到LRU列表
 * @inode: 要添加的inode
 *
 * 尝试将inode添加到其超级块的LRU列表中。如果添加成功，
 * 增加未使用inode计数；如果失败（已在列表中），则标记
 * inode为已引用状态。
 */
static void inode_lru_list_add(struct inode *inode)
{
	if (list_lru_add(&inode->i_sb->s_inode_lru, &inode->i_lru))
		this_cpu_inc(nr_unused);
	else
		inode->i_state |= I_REFERENCED;
}

/*
 * 如果需要，将inode添加到LRU（inode未使用且干净）
 *
 * 需要持有inode->i_lock
 */
/**
 * inode_add_lru - 条件性地将inode添加到LRU列表
 * @inode: 要检查和添加的inode
 *
 * 如果inode满足以下条件则将其添加到LRU列表：
 * - 不脏（没有I_DIRTY_ALL | I_SYNC | I_FREEING | I_WILL_FREE标志）
 * - 引用计数为0
 * - 超级块处于活动状态
 * 调用前必须持有inode->i_lock
 */
void inode_add_lru(struct inode *inode)
{
	if (!(inode->i_state & (I_DIRTY_ALL | I_SYNC |
				I_FREEING | I_WILL_FREE)) &&
	    !atomic_read(&inode->i_count) && inode->i_sb->s_flags & SB_ACTIVE)
		inode_lru_list_add(inode);
}


/**
 * inode_lru_list_del - 从LRU列表删除inode
 * @inode: 要删除的inode
 *
 * 从LRU列表中删除inode，如果删除成功则减少未使用inode计数
 */
static void inode_lru_list_del(struct inode *inode)
{

	if (list_lru_del(&inode->i_sb->s_inode_lru, &inode->i_lru))
		this_cpu_dec(nr_unused);
}

/**
 * inode_sb_list_add - 将inode添加到超级块的inode列表
 * @inode: 要添加的inode
 *
 * 将inode添加到其超级块的s_inodes列表中，这个列表包含
 * 超级块上的所有活动inode
 */
void inode_sb_list_add(struct inode *inode)
{
	spin_lock(&inode->i_sb->s_inode_list_lock);
	list_add(&inode->i_sb_list, &inode->i_sb->s_inodes);
	spin_unlock(&inode->i_sb->s_inode_list_lock);
}
EXPORT_SYMBOL_GPL(inode_sb_list_add);

/**
 * inode_sb_list_del - 从超级块列表删除inode
 * @inode: 要删除的inode
 *
 * 从超级块的s_inodes列表中删除inode，如果inode确实在列表中的话
 */
static inline void inode_sb_list_del(struct inode *inode)
{
	if (!list_empty(&inode->i_sb_list)) {
		spin_lock(&inode->i_sb->s_inode_list_lock);
		list_del_init(&inode->i_sb_list);
		spin_unlock(&inode->i_sb->s_inode_list_lock);
	}
}

/**
 * hash - 计算inode哈希值
 * @sb: 超级块指针
 * @hashval: 用于哈希的值（通常是inode号）
 *
 * 基于超级块指针和给定值计算哈希表索引。使用黄金比例
 * 和移位操作来产生良好的哈希分布。
 *
 * 返回值: 哈希表索引
 */
static unsigned long hash(struct super_block *sb, unsigned long hashval)
{
	unsigned long tmp;

	tmp = (hashval * (unsigned long)sb) ^ (GOLDEN_RATIO_PRIME + hashval) /
			L1_CACHE_BYTES;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> i_hash_shift);
	return tmp & i_hash_mask;
}

/**
 *	__insert_inode_hash - 将inode加入哈希表
 *	@inode: 未哈希的inode
 *	@hashval: 用于在inode_hashtable中定位此对象的无符号长整型值
 *
 *	将inode添加到此超级块的inode哈希表中
 */
void __insert_inode_hash(struct inode *inode, unsigned long hashval)
{
	struct hlist_head *b = inode_hashtable + hash(inode->i_sb, hashval);

	spin_lock(&inode_hash_lock);
	spin_lock(&inode->i_lock);
	hlist_add_head_rcu(&inode->i_hash, b);
	spin_unlock(&inode->i_lock);
	spin_unlock(&inode_hash_lock);
}
EXPORT_SYMBOL(__insert_inode_hash);

/**
 *	__remove_inode_hash - 从哈希表移除inode
 *	@inode: 要取消哈希的inode
 *
 *	从超级块中移除inode
 */
void __remove_inode_hash(struct inode *inode)
{
	spin_lock(&inode_hash_lock);
	spin_lock(&inode->i_lock);
	hlist_del_init_rcu(&inode->i_hash);
	spin_unlock(&inode->i_lock);
	spin_unlock(&inode_hash_lock);
}
EXPORT_SYMBOL(__remove_inode_hash);

/**
 * clear_inode - 清理inode使其可以被释放
 * @inode: 要清理的inode
 *
 * 清理inode的各种状态，确保所有页面都已清空，没有私有数据，
 * 然后将状态设置为I_FREEING | I_CLEAR。这个函数在evict_inode
 * 或文件系统的自定义清理函数中调用。
 */
void clear_inode(struct inode *inode)
{
	/*
	 * We have to cycle the i_pages lock here because reclaim can be in the
	 * process of removing the last page (in __delete_from_page_cache())
	 * and we must not free the mapping under it.
	 */
	xa_lock_irq(&inode->i_data.i_pages);
	BUG_ON(inode->i_data.nrpages);
	BUG_ON(inode->i_data.nrexceptional);
	xa_unlock_irq(&inode->i_data.i_pages);
	BUG_ON(!list_empty(&inode->i_data.private_list));
	BUG_ON(!(inode->i_state & I_FREEING));
	BUG_ON(inode->i_state & I_CLEAR);
	BUG_ON(!list_empty(&inode->i_wb_list));
	/* don't need i_lock here, no concurrent mods to i_state */
	inode->i_state = I_FREEING | I_CLEAR;
}
EXPORT_SYMBOL(clear_inode);

/*
 * 释放传入的inode，将其从仍连接的列表中移除。我们移除仍附加到inode的
 * 任何页面，并等待任何仍在进行的IO完成，然后最终销毁inode。
 *
 * inode必须已经标记为I_FREEING，这样我们可以避免在与操作列表的其他代码
 * （如writeback_single_inode）竞争时inode被移回列表。调用者负责设置此标志。
 *
 * 在从缓存中驱逐之前，inode必须已经从LRU列表中移除。这应该与设置I_FREEING
 * 状态标志原子性地发生，因此这里被驱逐的inode都不应该在LRU上。
 */
/**
 * evict - 驱逐并销毁inode
 * @inode: 要驱逐的inode（必须已标记I_FREEING）
 *
 * 完整地驱逐一个inode的步骤：
 * 1. 从IO列表和超级块列表移除
 * 2. 等待writeback完成
 * 3. 调用文件系统的evict_inode或执行默认清理
 * 4. 处理块/字符设备特殊情况
 * 5. 从哈希表移除
 * 6. 唤醒等待者并销毁inode
 */
static void evict(struct inode *inode)
{
	const struct super_operations *op = inode->i_sb->s_op;

	BUG_ON(!(inode->i_state & I_FREEING));
	BUG_ON(!list_empty(&inode->i_lru));

	if (!list_empty(&inode->i_io_list))
		inode_io_list_del(inode);

	inode_sb_list_del(inode);

	/*
	 * Wait for flusher thread to be done with the inode so that filesystem
	 * does not start destroying it while writeback is still running. Since
	 * the inode has I_FREEING set, flusher thread won't start new work on
	 * the inode.  We just have to wait for running writeback to finish.
	 */
	inode_wait_for_writeback(inode);

	if (op->evict_inode) {
		op->evict_inode(inode);
	} else {
		truncate_inode_pages_final(&inode->i_data);
		clear_inode(inode);
	}
	if (S_ISBLK(inode->i_mode) && inode->i_bdev)
		bd_forget(inode);
	if (S_ISCHR(inode->i_mode) && inode->i_cdev)
		cd_forget(inode);

	remove_inode_hash(inode);

	spin_lock(&inode->i_lock);
	wake_up_bit(&inode->i_state, __I_NEW);
	BUG_ON(inode->i_state != (I_FREEING | I_CLEAR));
	spin_unlock(&inode->i_lock);

	destroy_inode(inode);
}

/*
 * dispose_list - 处理本地列表的内容
 * @head: 要释放的列表头
 *
 * Dispose-list获得一个包含本地inode的本地列表，因此它不需要
 * 担心列表破坏和SMP锁。
 */
/**
 * dispose_list - 批量处理待释放的inode列表
 * @head: inode列表的头部
 *
 * 遍历列表中的所有inode并逐个驱逐它们。这是一个内部函数，
 * 处理已经从各种列表中分离出来的inode。在每次驱逐后调用
 * cond_resched()以避免长时间占用CPU。
 */
static void dispose_list(struct list_head *head)
{
	while (!list_empty(head)) {
		struct inode *inode;

		inode = list_first_entry(head, struct inode, i_lru);
		list_del_init(&inode->i_lru);

		evict(inode);
		cond_resched();
	}
}

/**
 * evict_inodes - 驱逐超级块的所有可驱逐inode
 * @sb: 要操作的超级块
 *
 * 确保没有引用计数为零的inode被保留。这在超级块关闭时调用，
 * 在移除SB_ACTIVE标志之后，因此在该调用期间或之后达到零引用
 * 计数的任何inode都将被立即驱逐。
 */
void evict_inodes(struct super_block *sb)
{
	struct inode *inode, *next;
	LIST_HEAD(dispose);

again:
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry_safe(inode, next, &sb->s_inodes, i_sb_list) {
		if (atomic_read(&inode->i_count))
			continue;

		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_NEW | I_FREEING | I_WILL_FREE)) {
			spin_unlock(&inode->i_lock);
			continue;
		}

		inode->i_state |= I_FREEING;
		inode_lru_list_del(inode);
		spin_unlock(&inode->i_lock);
		list_add(&inode->i_lru, &dispose);

		/*
		 * We can have a ton of inodes to evict at unmount time given
		 * enough memory, check to see if we need to go to sleep for a
		 * bit so we don't livelock.
		 */
		if (need_resched()) {
			spin_unlock(&sb->s_inode_list_lock);
			cond_resched();
			dispose_list(&dispose);
			goto again;
		}
	}
	spin_unlock(&sb->s_inode_list_lock);

	dispose_list(&dispose);
}
EXPORT_SYMBOL_GPL(evict_inodes);

/**
 * invalidate_inodes - 尝试释放超级块上的所有inode
 * @sb: 要操作的超级块
 * @kill_dirty: 指导如何处理脏inode的标志
 *
 * 尝试释放给定超级块的所有inode。如果有繁忙的inode则返回
 * 非零值，否则返回零。
 * 如果@kill_dirty被设置，则丢弃脏inode，否则将它们视为繁忙。
 */
int invalidate_inodes(struct super_block *sb, bool kill_dirty)
{
	int busy = 0;
	struct inode *inode, *next;
	LIST_HEAD(dispose);

again:
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry_safe(inode, next, &sb->s_inodes, i_sb_list) {
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_NEW | I_FREEING | I_WILL_FREE)) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		if (inode->i_state & I_DIRTY_ALL && !kill_dirty) {
			spin_unlock(&inode->i_lock);
			busy = 1;
			continue;
		}
		if (atomic_read(&inode->i_count)) {
			spin_unlock(&inode->i_lock);
			busy = 1;
			continue;
		}

		inode->i_state |= I_FREEING;
		inode_lru_list_del(inode);
		spin_unlock(&inode->i_lock);
		list_add(&inode->i_lru, &dispose);
		if (need_resched()) {
			spin_unlock(&sb->s_inode_list_lock);
			cond_resched();
			dispose_list(&dispose);
			goto again;
		}
	}
	spin_unlock(&sb->s_inode_list_lock);

	dispose_list(&dispose);

	return busy;
}

/*
 * 为释放做准备，将inode从LRU隔离出来。
 *
 * 任何纯粹因为附加页缓存而被固定的inode会删除其页缓存。
 * 如果inode有元数据缓冲区附加到mapping->private_list，
 * 则尝试移除它们。
 *
 * 如果inode设置了I_REFERENCED标志，则意味着它最近被使用过 -
 * 该标志在iput_final()中设置。当我们遇到这样的inode时，
 * 清除标志并将其移到LRU的后面，这样它在被回收之前会在LRU中
 * 再获得一次机会。这是必要的，因为我们正在进行延迟LRU更新
 * 以最小化锁争用，所以LRU没有严格的顺序。因此我们不想回收
 * 设置了此标志的inode，因为它们是无序的inode。
 */
/**
 * inode_lru_isolate - LRU隔离回调函数，用于inode回收
 * @item: LRU列表项（实际上是inode的i_lru）
 * @lru: LRU控制结构
 * @lru_lock: LRU锁
 * @arg: 传递给函数的参数（可释放列表）
 *
 * 这是内存回收子系统的回调函数，用于决定是否可以回收一个inode。
 * 返回值: LRU_SKIP（跳过）、LRU_REMOVED（已移除）、LRU_ROTATE（旋转到后面）、LRU_RETRY（重试）
 */
static enum lru_status inode_lru_isolate(struct list_head *item,
		struct list_lru_one *lru, spinlock_t *lru_lock, void *arg)
{
	struct list_head *freeable = arg;
	struct inode	*inode = container_of(item, struct inode, i_lru);

	/*
	 * we are inverting the lru lock/inode->i_lock here, so use a trylock.
	 * If we fail to get the lock, just skip it.
	 */
	if (!spin_trylock(&inode->i_lock))
		return LRU_SKIP;

	/*
	 * Referenced or dirty inodes are still in use. Give them another pass
	 * through the LRU as we canot reclaim them now.
	 */
	if (atomic_read(&inode->i_count) ||
	    (inode->i_state & ~I_REFERENCED)) {
		list_lru_isolate(lru, &inode->i_lru);
		spin_unlock(&inode->i_lock);
		this_cpu_dec(nr_unused);
		return LRU_REMOVED;
	}

	/* recently referenced inodes get one more pass */
	if (inode->i_state & I_REFERENCED) {
		inode->i_state &= ~I_REFERENCED;
		spin_unlock(&inode->i_lock);
		return LRU_ROTATE;
	}

	if (inode_has_buffers(inode) || inode->i_data.nrpages) {
		__iget(inode);
		spin_unlock(&inode->i_lock);
		spin_unlock(lru_lock);
		if (remove_inode_buffers(inode)) {
			unsigned long reap;
			reap = invalidate_mapping_pages(&inode->i_data, 0, -1);
			if (current_is_kswapd())
				__count_vm_events(KSWAPD_INODESTEAL, reap);
			else
				__count_vm_events(PGINODESTEAL, reap);
			if (current->reclaim_state)
				current->reclaim_state->reclaimed_slab += reap;
		}
		iput(inode);
		spin_lock(lru_lock);
		return LRU_RETRY;
	}

	WARN_ON(inode->i_state & I_NEW);
	inode->i_state |= I_FREEING;
	list_lru_isolate_move(lru, &inode->i_lru, freeable);
	spin_unlock(&inode->i_lock);

	this_cpu_dec(nr_unused);
	return LRU_REMOVED;
}

/*
 * 遍历超级块inode LRU寻找可释放的inode并尝试释放它们。
 * 这从超级块收缩器函数调用，带有要从LRU修剪的inode数量。
 * 要释放的inode移到临时列表，然后在inode_lock外通过dispose_list()释放。
 */
/**
 * prune_icache_sb - 修剪超级块的inode缓存
 * @sb: 目标超级块
 * @sc: 收缩控制结构
 *
 * 从指定超级块的inode LRU列表中回收inode。这是内存管理
 * 子系统在内存压力下调用的函数。
 *
 * 返回值: 实际释放的inode数量
 */
long prune_icache_sb(struct super_block *sb, struct shrink_control *sc)
{
	LIST_HEAD(freeable);
	long freed;

	freed = list_lru_shrink_walk(&sb->s_inode_lru, sc,
				     inode_lru_isolate, &freeable);
	dispose_list(&freeable);
	return freed;
}

/**
 * __wait_on_freeing_inode - 等待正在释放的inode完成释放
 * @inode: 正在释放的inode
 */
static void __wait_on_freeing_inode(struct inode *inode);
/*
 * 在持有inode锁的情况下调用
 */
/**
 * find_inode - 在哈希表中查找匹配的inode
 * @sb: 超级块
 * @head: 哈希表头部
 * @test: 用于比较inode的回调函数
 * @data: 传递给test函数的不透明数据
 *
 * 在指定哈希链中查找匹配的inode。如果找到匹配项且inode
 * 没有正在被释放，则增加引用计数并返回。
 *
 * 返回值: 匹配的inode（增加了引用计数）或NULL
 */
static struct inode *find_inode(struct super_block *sb,
				struct hlist_head *head,
				int (*test)(struct inode *, void *),
				void *data)
{
	struct inode *inode = NULL;

repeat:
	hlist_for_each_entry(inode, head, i_hash) {
		if (inode->i_sb != sb)
			continue;
		if (!test(inode, data))
			continue;
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_FREEING|I_WILL_FREE)) {
			__wait_on_freeing_inode(inode);
			goto repeat;
		}
		if (unlikely(inode->i_state & I_CREATING)) {
			spin_unlock(&inode->i_lock);
			return ERR_PTR(-ESTALE);
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		return inode;
	}
	return NULL;
}

/*
 * find_inode_fast是find_inode的快速路径版本，详情参见iget_locked的注释
 */
/**
 * find_inode_fast - 通过inode号快速查找inode
 * @sb: 超级块
 * @head: 哈希表头部
 * @ino: 要查找的inode号
 *
 * 这是find_inode的优化版本，用于只需要通过inode号查找的情况。
 * 比通用的find_inode函数更快，因为不需要调用测试函数。
 *
 * 返回值: 匹配的inode（增加了引用计数）或NULL
 */
static struct inode *find_inode_fast(struct super_block *sb,
				struct hlist_head *head, unsigned long ino)
{
	struct inode *inode = NULL;

repeat:
	hlist_for_each_entry(inode, head, i_hash) {
		if (inode->i_ino != ino)
			continue;
		if (inode->i_sb != sb)
			continue;
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_FREEING|I_WILL_FREE)) {
			__wait_on_freeing_inode(inode);
			goto repeat;
		}
		if (unlikely(inode->i_state & I_CREATING)) {
			spin_unlock(&inode->i_lock);
			return ERR_PTR(-ESTALE);
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		return inode;
	}
	return NULL;
}

/*
 * 每个CPU拥有一个LAST_INO_BATCH数字范围。
 * 'shared_last_ino'只在LAST_INO_BATCH分配中被弄脏一次，
 * 以更新耗尽的范围。
 *
 * 这不会显著增加溢出率，因为每个CPU最多可以消耗
 * LAST_INO_BATCH-1个未使用的inode号。所以有
 * NR_CPUS*(LAST_INO_BATCH-1)的浪费。在4096和1024时，
 * 这大约是2^32范围的0.1%，这是最坏情况。即使50%的
 * 浪费也只会将溢出率增加2倍，这似乎并不太重要。
 *
 * 在32位，非LFS stat()调用上，如果st_ino不适合目标
 * 结构字段，glibc会生成EOVERFLOW错误。这里使用32位
 * 计数器来尝试避免这种情况。
 */
#define LAST_INO_BATCH 1024
static DEFINE_PER_CPU(unsigned int, last_ino);

/**
 * get_next_ino - 获取下一个inode号
 *
 * 获取一个唯一的inode号，每个CPU维护自己的计数器以减少争用。
 * 当CPU本地计数器用完时，从全局共享计数器获取新的批次。
 *
 * 返回值: 唯一的inode号（保证非零）
 */
unsigned int get_next_ino(void)
{
	unsigned int *p = &get_cpu_var(last_ino);
	unsigned int res = *p;

#ifdef CONFIG_SMP
	if (unlikely((res & (LAST_INO_BATCH-1)) == 0)) {
		static atomic_t shared_last_ino;
		int next = atomic_add_return(LAST_INO_BATCH, &shared_last_ino);

		res = next - LAST_INO_BATCH;
	}
#endif

	res++;
	/* get_next_ino should not provide a 0 inode number */
	if (unlikely(!res))
		res++;
	*p = res;
	put_cpu_var(last_ino);
	return res;
}
EXPORT_SYMBOL(get_next_ino);

/**
 *	new_inode_pseudo - 获取一个伪inode
 *	@sb: 超级块
 *
 *	为给定的超级块分配一个新的inode。
 *	inode不会链接到超级块的s_inodes列表中
 *	这意味着：
 *	- 文件系统无法卸载
 *	- 配额、fsnotify、writeback无法工作
 */
struct inode *new_inode_pseudo(struct super_block *sb)
{
	struct inode *inode = alloc_inode(sb);

	if (inode) {
		spin_lock(&inode->i_lock);
		inode->i_state = 0;
		spin_unlock(&inode->i_lock);
		INIT_LIST_HEAD(&inode->i_sb_list);
	}
	return inode;
}

/**
 *	new_inode - 获取一个inode
 *	@sb: 超级块
 *
 *	为给定超级块分配一个新的inode。与inode->i_mapping相关的
 *	分配的默认gfp_mask是GFP_HIGHUSER_MOVABLE。
 *	如果HIGHMEM页面不合适或已知为页面缓存分配的页面不可回收
 *	或不可迁移，必须在新创建的inode映射上使用合适的标志调用
 *	mapping_set_gfp_mask()
 *
 */
struct inode *new_inode(struct super_block *sb)
{
	struct inode *inode;

	spin_lock_prefetch(&sb->s_inode_list_lock);

	inode = new_inode_pseudo(sb);
	if (inode)
		inode_sb_list_add(inode);
	return inode;
}
EXPORT_SYMBOL(new_inode);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void lockdep_annotate_inode_mutex_key(struct inode *inode)
{
	if (S_ISDIR(inode->i_mode)) {
		struct file_system_type *type = inode->i_sb->s_type;

		/* Set new key only if filesystem hasn't already changed it */
		if (lockdep_match_class(&inode->i_rwsem, &type->i_mutex_key)) {
			/*
			 * ensure nobody is actually holding i_mutex
			 */
			// mutex_destroy(&inode->i_mutex);
			init_rwsem(&inode->i_rwsem);
			lockdep_set_class(&inode->i_rwsem,
					  &type->i_mutex_dir_key);
		}
	}
}
EXPORT_SYMBOL(lockdep_annotate_inode_mutex_key);
#endif

/**
 * unlock_new_inode - 清除I_NEW状态并唤醒等待者
 * @inode: 要解锁的新inode
 *
 * 当inode完全初始化后调用，清除inode的新状态并唤醒
 * 任何等待inode完成初始化的进程。
 */
void unlock_new_inode(struct inode *inode)
{
	lockdep_annotate_inode_mutex_key(inode);
	spin_lock(&inode->i_lock);
	WARN_ON(!(inode->i_state & I_NEW));
	inode->i_state &= ~I_NEW & ~I_CREATING;
	smp_mb();
	wake_up_bit(&inode->i_state, __I_NEW);
	spin_unlock(&inode->i_lock);
}
EXPORT_SYMBOL(unlock_new_inode);

/**
 * discard_new_inode - 丢弃新创建的inode
 * @inode: 要丢弃的新inode
 *
 * 当新inode初始化失败时调用，清除I_NEW状态，唤醒等待者，
 * 并释放inode。这用于处理inode创建过程中的错误情况。
 */
void discard_new_inode(struct inode *inode)
{
	lockdep_annotate_inode_mutex_key(inode);
	spin_lock(&inode->i_lock);
	WARN_ON(!(inode->i_state & I_NEW));
	inode->i_state &= ~I_NEW;
	smp_mb();
	wake_up_bit(&inode->i_state, __I_NEW);
	spin_unlock(&inode->i_lock);
	iput(inode);
}
EXPORT_SYMBOL(discard_new_inode);

/**
 * lock_two_nondirectories - 对两个非目录对象获取i_mutex锁
 *
 * 锁定任何非NULL且不是目录的参数。
 * 此函数可能锁定零个、一个或两个对象。
 *
 * @inode1: 第一个要锁定的inode
 * @inode2: 第二个要锁定的inode
 */
void lock_two_nondirectories(struct inode *inode1, struct inode *inode2)
{
	if (inode1 > inode2)
		swap(inode1, inode2);

	if (inode1 && !S_ISDIR(inode1->i_mode))
		inode_lock(inode1);
	if (inode2 && !S_ISDIR(inode2->i_mode) && inode2 != inode1)
		inode_lock_nested(inode2, I_MUTEX_NONDIR2);
}
EXPORT_SYMBOL(lock_two_nondirectories);

/**
 * unlock_two_nondirectories - 释放从lock_two_nondirectories()获得的锁
 * @inode1: 第一个要解锁的inode
 * @inode2: 第二个要解锁的inode
 */
void unlock_two_nondirectories(struct inode *inode1, struct inode *inode2)
{
	if (inode1 && !S_ISDIR(inode1->i_mode))
		inode_unlock(inode1);
	if (inode2 && !S_ISDIR(inode2->i_mode) && inode2 != inode1)
		inode_unlock(inode2);
}
EXPORT_SYMBOL(unlock_two_nondirectories);

/**
 * inode_insert5 - 从挂载的文件系统获取inode
 * @inode: 用于插入缓存的预分配inode
 * @hashval: 哈希值（通常是inode号）
 * @test: 用于inode间比较的回调函数
 * @set: 用于初始化新inode结构的回调函数
 * @data: 传递给@test和@set的不透明数据指针
 *
 * 在inode缓存中搜索由@hashval和@data指定的inode，
 * 如果存在则返回它并增加引用计数。这是iget5_locked()
 * 的变体，用于不希望在inode内存分配失败时失败的调用者。
 *
 * 如果inode不在缓存中，将预分配的inode插入缓存并返回它
 * （已锁定、已哈希，并设置I_NEW标志）。文件系统在通过
 * unlock_new_inode()解锁之前需要填充它。
 *
 * 注意@test和@set都在持有inode_hash_lock的情况下调用，所以不能睡眠。
 */
struct inode *inode_insert5(struct inode *inode, unsigned long hashval,
			    int (*test)(struct inode *, void *),
			    int (*set)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(inode->i_sb, hashval);
	struct inode *old;
	bool creating = inode->i_state & I_CREATING;

again:
	spin_lock(&inode_hash_lock);
	old = find_inode(inode->i_sb, head, test, data);
	if (unlikely(old)) {
		/*
		 * Uhhuh, somebody else created the same inode under us.
		 * Use the old inode instead of the preallocated one.
		 */
		spin_unlock(&inode_hash_lock);
		if (IS_ERR(old))
			return NULL;
		wait_on_inode(old);
		if (unlikely(inode_unhashed(old))) {
			iput(old);
			goto again;
		}
		return old;
	}

	if (set && unlikely(set(inode, data))) {
		inode = NULL;
		goto unlock;
	}

	/*
	 * Return the locked inode with I_NEW set, the
	 * caller is responsible for filling in the contents
	 */
	spin_lock(&inode->i_lock);
	inode->i_state |= I_NEW;
	hlist_add_head_rcu(&inode->i_hash, head);
	spin_unlock(&inode->i_lock);
	if (!creating)
		inode_sb_list_add(inode);
unlock:
	spin_unlock(&inode_hash_lock);

	return inode;
}
EXPORT_SYMBOL(inode_insert5);

/**
 * iget5_locked - 从挂载的文件系统获取inode
 * @sb: 文件系统的超级块
 * @hashval: 哈希值（通常是inode号）
 * @test: 用于inode间比较的回调函数
 * @set: 用于初始化新inode结构的回调函数
 * @data: 传递给@test和@set的不透明数据指针
 *
 * 在inode缓存中搜索由@hashval和@data指定的inode，
 * 如果inode在缓存中，则返回它并增加引用计数。这是
 * iget_locked()的通用版本，用于inode号不足以唯一
 * 标识inode的文件系统。
 *
 * 如果inode不在缓存中，分配新inode并返回它（已锁定、
 * 已哈希，并设置I_NEW标志）。文件系统在通过
 * unlock_new_inode()解锁之前需要填充它。
 *
 * 注意@test和@set都在持有inode_hash_lock的情况下调用，所以不能睡眠。
 */
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *),
		int (*set)(struct inode *, void *), void *data)
{
	struct inode *inode = ilookup5(sb, hashval, test, data);

	if (!inode) {
		struct inode *new = alloc_inode(sb);

		if (new) {
			new->i_state = 0;
			inode = inode_insert5(new, hashval, test, set, data);
			if (unlikely(inode != new))
				destroy_inode(new);
		}
	}
	return inode;
}
EXPORT_SYMBOL(iget5_locked);

/**
 * iget_locked - 从挂载的文件系统获取inode
 * @sb: 文件系统的超级块
 * @ino: 要获取的inode号
 *
 * 在inode缓存中搜索由@ino指定的inode，如果存在则返回它
 * 并增加引用计数。这用于inode号足以唯一标识inode的文件系统。
 *
 * 如果inode不在缓存中，分配新inode并返回它（已锁定、已哈希，
 * 并设置I_NEW标志）。文件系统在通过unlock_new_inode()解锁
 * 之前需要填充它。
 */
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);
	struct inode *inode;
again:
	spin_lock(&inode_hash_lock);
	inode = find_inode_fast(sb, head, ino);
	spin_unlock(&inode_hash_lock);
	if (inode) {
		if (IS_ERR(inode))
			return NULL;
		wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
		return inode;
	}

	inode = alloc_inode(sb);
	if (inode) {
		struct inode *old;

		spin_lock(&inode_hash_lock);
		/* We released the lock, so.. */
		old = find_inode_fast(sb, head, ino);
		if (!old) {
			inode->i_ino = ino;
			spin_lock(&inode->i_lock);
			inode->i_state = I_NEW;
			hlist_add_head_rcu(&inode->i_hash, head);
			spin_unlock(&inode->i_lock);
			inode_sb_list_add(inode);
			spin_unlock(&inode_hash_lock);

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		spin_unlock(&inode_hash_lock);
		destroy_inode(inode);
		if (IS_ERR(old))
			return NULL;
		inode = old;
		wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
	}
	return inode;
}
EXPORT_SYMBOL(iget_locked);

/*
 * 在inode缓存中搜索匹配的inode号。
 * 如果我们找到一个，那么我们试图分配的inode号不是唯一的，
 * 所以我们不应该使用它。
 *
 * 如果inode号是唯一的则返回1，如果不是则返回0。
 */
/**
 * test_inode_iunique - 测试inode号是否唯一
 * @sb: 超级块
 * @ino: 要测试的inode号
 *
 * 检查给定的inode号在指定超级块中是否已经被使用。
 *
 * 返回值: 如果唯一返回1，如果已使用返回0
 */
static int test_inode_iunique(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *b = inode_hashtable + hash(sb, ino);
	struct inode *inode;

	hlist_for_each_entry_rcu(inode, b, i_hash) {
		if (inode->i_ino == ino && inode->i_sb == sb)
			return 0;
	}
	return 1;
}

/**
 *	iunique - 获取唯一的inode号
 *	@sb: 超级块
 *	@max_reserved: 最高保留的inode号
 *
 *	为给定超级块获取在系统上唯一的inode号。这由没有
 *	自然永久inode编号系统的文件系统使用。返回的inode号
 *	高于保留限制但是唯一的。
 *
 *	缺陷:
 *	当文件系统上有大量活动inode时，此函数目前变得相当慢。
 */
ino_t iunique(struct super_block *sb, ino_t max_reserved)
{
	/*
	 * On a 32bit, non LFS stat() call, glibc will generate an EOVERFLOW
	 * error if st_ino won't fit in target struct field. Use 32bit counter
	 * here to attempt to avoid that.
	 */
	static DEFINE_SPINLOCK(iunique_lock);
	static unsigned int counter;
	ino_t res;

	rcu_read_lock();
	spin_lock(&iunique_lock);
	do {
		if (counter <= max_reserved)
			counter = max_reserved + 1;
		res = counter++;
	} while (!test_inode_iunique(sb, res));
	spin_unlock(&iunique_lock);
	rcu_read_unlock();

	return res;
}
EXPORT_SYMBOL(iunique);

/**
 * igrab - 安全地获取inode引用
 * @inode: 要获取引用的inode
 *
 * 如果inode没有正在被释放，则增加其引用计数。这是一个
 * "安全"的引用获取函数，它检查inode状态以避免在inode
 * 正在被销毁时获取引用。
 *
 * 返回值: 成功返回inode指针，如果inode正在被释放则返回NULL
 */
struct inode *igrab(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	if (!(inode->i_state & (I_FREEING|I_WILL_FREE))) {
		__iget(inode);
		spin_unlock(&inode->i_lock);
	} else {
		spin_unlock(&inode->i_lock);
		/*
		 * Handle the case where s_op->clear_inode is not been
		 * called yet, and somebody is calling igrab
		 * while the inode is getting freed.
		 */
		inode = NULL;
	}
	return inode;
}
EXPORT_SYMBOL(igrab);

/**
 * ilookup5_nowait - 在inode缓存中搜索inode
 * @sb: 要搜索的文件系统超级块
 * @hashval: 要搜索的哈希值（通常是inode号）
 * @test: 用于inode间比较的回调函数
 * @data: 传递给@test的不透明数据指针
 *
 * 在inode缓存中搜索由@hashval和@data指定的inode。
 * 如果inode在缓存中，返回inode并增加引用计数。
 *
 * 注意: 不等待I_NEW，所以你必须非常小心处理返回的inode。
 * 你可能应该使用ilookup5()替代。
 *
 * 注意2: @test在持有inode_hash_lock的情况下调用，所以不能睡眠。
 */
struct inode *ilookup5_nowait(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);
	struct inode *inode;

	spin_lock(&inode_hash_lock);
	inode = find_inode(sb, head, test, data);
	spin_unlock(&inode_hash_lock);

	return IS_ERR(inode) ? NULL : inode;
}
EXPORT_SYMBOL(ilookup5_nowait);

/**
 * ilookup5 - 在inode缓存中搜索inode
 * @sb: 要搜索的文件系统超级块
 * @hashval: 要搜索的哈希值（通常是inode号）
 * @test: 用于inode间比较的回调函数
 * @data: 传递给@test的不透明数据指针
 *
 * 在inode缓存中搜索由@hashval和@data指定的inode，
 * 如果inode在缓存中，返回inode并增加引用计数。
 * 在返回inode前等待I_NEW完成。
 *
 * 这是ilookup()的通用版本，用于inode号不足以唯一标识
 * inode的文件系统。
 *
 * 注意: @test在持有inode_hash_lock的情况下调用，所以不能睡眠。
 */
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct inode *inode;
again:
	inode = ilookup5_nowait(sb, hashval, test, data);
	if (inode) {
		wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
	}
	return inode;
}
EXPORT_SYMBOL(ilookup5);

/**
 * ilookup - 在inode缓存中搜索inode
 * @sb: 要搜索的文件系统超级块
 * @ino: 要搜索的inode号
 *
 * 在inode缓存中搜索@ino，如果inode在缓存中，
 * 返回inode并增加引用计数。
 */
struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);
	struct inode *inode;
again:
	spin_lock(&inode_hash_lock);
	inode = find_inode_fast(sb, head, ino);
	spin_unlock(&inode_hash_lock);

	if (inode) {
		if (IS_ERR(inode))
			return NULL;
		wait_on_inode(inode);
		if (unlikely(inode_unhashed(inode))) {
			iput(inode);
			goto again;
		}
	}
	return inode;
}
EXPORT_SYMBOL(ilookup);

/**
 * find_inode_nowait - 在inode缓存中查找inode
 * @sb: 要搜索的文件系统超级块
 * @hashval: 要搜索的哈希值（通常是inode号）
 * @match: 用于inode间比较的回调函数
 * @data: 传递给@match的不透明数据指针
 *
 * 在inode缓存中搜索由@hashval和@data指定的inode，
 * 其中辅助函数@match在inode不匹配时返回0，匹配时返回1，
 * 应该停止搜索时返回-1。@match函数必须负责获取i_lock
 * 自旋锁并检查正在释放或正在初始化的inode的i_state，
 * 并在返回1之前增加引用计数。它也不能睡眠，因为它是在
 * 持有inode_hash_lock自旋锁的情况下调用的。
 *
 * 这是ilookup5()的更通用版本，当函数绝不能阻塞时使用---
 * find_inode()可能在__wait_on_freeing_inode()中阻塞---
 * 或当调用者不能增加引用计数时，因为resulting iput()可能
 * 导致inode驱逐。权衡是@match函数必须非常小心地实现。
 */
struct inode *find_inode_nowait(struct super_block *sb,
				unsigned long hashval,
				int (*match)(struct inode *, unsigned long,
					     void *),
				void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);
	struct inode *inode, *ret_inode = NULL;
	int mval;

	spin_lock(&inode_hash_lock);
	hlist_for_each_entry(inode, head, i_hash) {
		if (inode->i_sb != sb)
			continue;
		mval = match(inode, hashval, data);
		if (mval == 0)
			continue;
		if (mval == 1)
			ret_inode = inode;
		goto out;
	}
out:
	spin_unlock(&inode_hash_lock);
	return ret_inode;
}
EXPORT_SYMBOL(find_inode_nowait);

/**
 * find_inode_rcu - 在inode缓存中查找inode
 * @sb: 要搜索的文件系统超级块
 * @hashval: 哈希键
 * @test: 在inode上测试匹配的函数
 * @data: 测试函数的数据
 *
 * 在inode缓存中搜索由@hashval和@data指定的inode，
 * 其中辅助函数@test在inode不匹配时返回0，匹配时返回1。
 * @test函数必须负责获取i_lock自旋锁并检查正在释放或
 * 正在初始化的inode的i_state。
 *
 * 如果成功，将返回@test函数返回1的inode，否则返回NULL。
 *
 * @test函数不允许对任何呈现的inode获取引用。
 * 它也不允许睡眠。
 *
 * 调用者必须持有RCU读锁。
 */
struct inode *find_inode_rcu(struct super_block *sb, unsigned long hashval,
			     int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);
	struct inode *inode;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "suspicious find_inode_rcu() usage");

	hlist_for_each_entry_rcu(inode, head, i_hash) {
		if (inode->i_sb == sb &&
		    !(READ_ONCE(inode->i_state) & (I_FREEING | I_WILL_FREE)) &&
		    test(inode, data))
			return inode;
	}
	return NULL;
}
EXPORT_SYMBOL(find_inode_rcu);

/**
 * find_inode_by_ino_rcu - 在inode缓存中查找inode
 * @sb: 要搜索的文件系统超级块
 * @ino: 要匹配的inode号
 *
 * 在inode缓存中搜索由@ino指定的inode。这是find_inode_rcu
 * 的简化版本，专门用于通过inode号查找。
 *
 * 如果成功，返回匹配的inode，否则返回NULL。
 *
 * 此函数不允许对任何呈现的inode获取引用。
 * 它也不允许睡眠。
 *
 * 调用者必须持有RCU读锁。
 */
struct inode *find_inode_by_ino_rcu(struct super_block *sb,
				    unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);
	struct inode *inode;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "suspicious find_inode_by_ino_rcu() usage");

	hlist_for_each_entry_rcu(inode, head, i_hash) {
		if (inode->i_ino == ino &&
		    inode->i_sb == sb &&
		    !(READ_ONCE(inode->i_state) & (I_FREEING | I_WILL_FREE)))
		    return inode;
	}
	return NULL;
}
EXPORT_SYMBOL(find_inode_by_ino_rcu);

/**
 * insert_inode_locked - 插入inode到哈希表并锁定
 * @inode: 要插入的inode
 *
 * 将inode插入到哈希表中，如果成功则设置I_NEW和I_CREATING标志。
 * 如果已经存在相同inode号的inode，则等待其完成并返回-EBUSY。
 *
 * 返回值: 成功返回0，如果存在冲突则返回-EBUSY
 */
int insert_inode_locked(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	ino_t ino = inode->i_ino;
	struct hlist_head *head = inode_hashtable + hash(sb, ino);

	while (1) {
		struct inode *old = NULL;
		spin_lock(&inode_hash_lock);
		hlist_for_each_entry(old, head, i_hash) {
			if (old->i_ino != ino)
				continue;
			if (old->i_sb != sb)
				continue;
			spin_lock(&old->i_lock);
			if (old->i_state & (I_FREEING|I_WILL_FREE)) {
				spin_unlock(&old->i_lock);
				continue;
			}
			break;
		}
		if (likely(!old)) {
			spin_lock(&inode->i_lock);
			inode->i_state |= I_NEW | I_CREATING;
			hlist_add_head_rcu(&inode->i_hash, head);
			spin_unlock(&inode->i_lock);
			spin_unlock(&inode_hash_lock);
			return 0;
		}
		if (unlikely(old->i_state & I_CREATING)) {
			spin_unlock(&old->i_lock);
			spin_unlock(&inode_hash_lock);
			return -EBUSY;
		}
		__iget(old);
		spin_unlock(&old->i_lock);
		spin_unlock(&inode_hash_lock);
		wait_on_inode(old);
		if (unlikely(!inode_unhashed(old))) {
			iput(old);
			return -EBUSY;
		}
		iput(old);
	}
}
EXPORT_SYMBOL(insert_inode_locked);

/**
 * insert_inode_locked4 - 使用自定义测试函数插入并锁定inode
 * @inode: 要插入的inode
 * @hashval: 哈希值
 * @test: 用于比较的测试函数
 * @data: 传递给测试函数的数据
 *
 * 类似于insert_inode_locked，但使用自定义的测试函数来
 * 确定是否存在冲突的inode。设置I_CREATING标志并使用
 * inode_insert5进行实际的插入工作。
 *
 * 返回值: 成功返回0，如果存在冲突则返回-EBUSY
 */
int insert_inode_locked4(struct inode *inode, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct inode *old;

	inode->i_state |= I_CREATING;
	old = inode_insert5(inode, hashval, test, NULL, data);

	if (old != inode) {
		iput(old);
		return -EBUSY;
	}
	return 0;
}
EXPORT_SYMBOL(insert_inode_locked4);


/**
 * generic_delete_inode - 通用的删除inode函数
 * @inode: 要删除的inode
 *
 * 这是一个简单的删除inode函数，总是返回1，表示应该删除inode。
 * 文件系统可以将此函数用作其drop_inode操作的默认实现。
 *
 * 返回值: 总是返回1（表示删除inode）
 */
int generic_delete_inode(struct inode *inode)
{
	return 1;
}
EXPORT_SYMBOL(generic_delete_inode);

/*
 * 当我们丢弃对inode的最后一个引用时调用。
 *
 * 调用FS的"drop_inode()"函数，默认为传统的UNIX文件系统行为。
 * 如果它告诉我们驱逐inode，就这样做。否则，如果fs是活动的，
 * 则在缓存中保留inode，如果fs正在关闭，则同步并驱逐。
 */
/**
 * iput_final - inode引用计数降为0时的最终处理
 * @inode: 引用计数为0的inode
 *
 * 当inode的最后一个引用被释放时调用此函数。决定是将inode
 * 放入LRU列表（如果文件系统活动且不需要立即删除），还是
 * 立即驱逐inode（如果文件系统要求删除或正在关闭）。
 */
static void iput_final(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	const struct super_operations *op = inode->i_sb->s_op;
	unsigned long state;
	int drop;

	WARN_ON(inode->i_state & I_NEW);

	if (op->drop_inode)
		drop = op->drop_inode(inode);
	else
		drop = generic_drop_inode(inode);

	if (!drop && (sb->s_flags & SB_ACTIVE)) {
		inode_add_lru(inode);
		spin_unlock(&inode->i_lock);
		return;
	}

	state = inode->i_state;
	if (!drop) {
		WRITE_ONCE(inode->i_state, state | I_WILL_FREE);
		spin_unlock(&inode->i_lock);

		write_inode_now(inode, 1);

		spin_lock(&inode->i_lock);
		state = inode->i_state;
		WARN_ON(state & I_NEW);
		state &= ~I_WILL_FREE;
	}

	WRITE_ONCE(inode->i_state, state | I_FREEING);
	if (!list_empty(&inode->i_lru))
		inode_lru_list_del(inode);
	spin_unlock(&inode->i_lock);

	evict(inode);
}

/**
 * iput - 释放inode引用
 * @inode: 要释放的inode
 *
 * 释放一个inode，减少其使用计数。如果inode使用计数降为零，
 * inode将被释放并可能被销毁。
 *
 * 因此，iput()可能会睡眠。
 *
 * 返回值: 无
 */
void iput(struct inode *inode)
{
	if (!inode)
		return;
	BUG_ON(inode->i_state & I_CLEAR);
retry:
	if (atomic_dec_and_lock(&inode->i_count, &inode->i_lock)) {
		if (inode->i_nlink && (inode->i_state & I_DIRTY_TIME)) {
			atomic_inc(&inode->i_count);
			spin_unlock(&inode->i_lock);
			trace_writeback_lazytime_iput(inode);
			mark_inode_dirty_sync(inode);
			goto retry;
		}
		iput_final(inode);
	}
}
EXPORT_SYMBOL(iput);

#ifdef CONFIG_BLOCK
/**
 *	bmap - 在文件中查找块号
 *	@inode: 拥有请求块号的inode
 *	@block: 指向要查找的块的指针
 *
 *	将``*block``中的值替换为持有文件中请求块号对应块的设备上的块号。
 *	也就是说，请求inode 1的第4块，函数将用保存该文件块的
 *	相对于磁盘开始的磁盘块替换``*block``中的4。
 *
 *	错误时返回-EINVAL，否则返回0。如果映射落入洞中，
 *	返回0且``*block``也设置为0。
 */
int bmap(struct inode *inode, sector_t *block)
{
	if (!inode->i_mapping->a_ops->bmap)
		return -EINVAL;

	*block = inode->i_mapping->a_ops->bmap(inode->i_mapping, *block);
	return 0;
}
EXPORT_SYMBOL(bmap);
#endif

/*
 * 使用相对atime时，只有在以前的atime早于ctime或mtime，
 * 或者距离上次atime更新至少过了一天时，才更新atime。
 */
/**
 * relatime_need_update - 检查是否需要更新atime
 * @mnt: 挂载点
 * @inode: 要检查的inode
 * @now: 当前时间
 *
 * 在relatime模式下决定是否需要更新访问时间。只有在以下情况下
 * 才更新atime：
 * 1. mtime比atime新
 * 2. ctime比atime新
 * 3. 距离上次atime更新超过24小时
 *
 * 返回值: 需要更新返回1，否则返回0
 */
static int relatime_need_update(struct vfsmount *mnt, struct inode *inode,
			     struct timespec64 now)
{

	if (!(mnt->mnt_flags & MNT_RELATIME))
		return 1;
	/*
	 * Is mtime younger than atime? If yes, update atime:
	 */
	if (timespec64_compare(&inode->i_mtime, &inode->i_atime) >= 0)
		return 1;
	/*
	 * Is ctime younger than atime? If yes, update atime:
	 */
	if (timespec64_compare(&inode->i_ctime, &inode->i_atime) >= 0)
		return 1;

	/*
	 * Is the previous atime value older than a day? If yes,
	 * update atime:
	 */
	if ((long)(now.tv_sec - inode->i_atime.tv_sec) >= 24*60*60)
		return 1;
	/*
	 * Good, we can skip the atime update:
	 */
	return 0;
}

/**
 * generic_update_time - 通用的时间更新函数
 * @inode: 要更新的inode
 * @time: 新的时间值
 * @flags: 指定要更新哪些时间字段的标志
 *
 * 根据flags参数更新inode的时间字段（atime、ctime、mtime）
 * 和版本号。如果文件系统不支持lazy time，则立即标记为脏。
 *
 * 返回值: 总是返回0
 */
int generic_update_time(struct inode *inode, struct timespec64 *time, int flags)
{
	int iflags = I_DIRTY_TIME;
	bool dirty = false;

	if (flags & S_ATIME)
		inode->i_atime = *time;
	if (flags & S_VERSION)
		dirty = inode_maybe_inc_iversion(inode, false);
	if (flags & S_CTIME)
		inode->i_ctime = *time;
	if (flags & S_MTIME)
		inode->i_mtime = *time;
	if ((flags & (S_ATIME | S_CTIME | S_MTIME)) &&
	    !(inode->i_sb->s_flags & SB_LAZYTIME))
		dirty = true;

	if (dirty)
		iflags |= I_DIRTY_SYNC;
	__mark_inode_dirty(inode, iflags);
	return 0;
}
EXPORT_SYMBOL(generic_update_time);

/*
 * 这执行更新inode时间或版本的实际工作。在调用此函数之前
 * 必须已经调用了mnt_want_write()。
 */
/**
 * update_time - 更新inode时间字段
 * @inode: 要更新的inode
 * @time: 新的时间值
 * @flags: 指定要更新哪些时间字段的标志
 *
 * 如果inode操作定义了update_time函数则调用它，否则使用
 * 通用的generic_update_time函数。
 *
 * 返回值: 成功返回0，失败返回错误码
 */
static int update_time(struct inode *inode, struct timespec64 *time, int flags)
{
	if (inode->i_op->update_time)
		return inode->i_op->update_time(inode, time, flags);
	return generic_update_time(inode, time, flags);
}

/**
 *	atime_needs_update - 检查是否需要更新访问时间
 *	@path: 要更新的&struct path
 *	@inode: 要检查的inode
 *
 *	检查inode的访问时间是否需要更新。此函数自动处理只读文件系统
 *	和媒体，以及"noatime"标志和inode特定的"noatime"标记。
 *
 *	返回值: 需要更新返回true，否则返回false
 */
bool atime_needs_update(const struct path *path, struct inode *inode)
{
	struct vfsmount *mnt = path->mnt;
	struct timespec64 now;

	if (inode->i_flags & S_NOATIME)
		return false;

	/* Atime updates will likely cause i_uid and i_gid to be written
	 * back improprely if their true value is unknown to the vfs.
	 */
	if (HAS_UNMAPPED_ID(inode))
		return false;

	if (IS_NOATIME(inode))
		return false;
	if ((inode->i_sb->s_flags & SB_NODIRATIME) && S_ISDIR(inode->i_mode))
		return false;

	if (mnt->mnt_flags & MNT_NOATIME)
		return false;
	if ((mnt->mnt_flags & MNT_NODIRATIME) && S_ISDIR(inode->i_mode))
		return false;

	now = current_time(inode);

	if (!relatime_need_update(mnt, inode, now))
		return false;

	if (timespec64_equal(&inode->i_atime, &now))
		return false;

	return true;
}

/**
 * touch_atime - 更新访问时间
 * @path: 要更新的路径
 *
 * 更新inode上的访问时间并标记为需要回写。此函数自动处理
 * 只读文件系统和媒体，以及"noatime"标志和inode特定的
 * "noatime"标记。首先检查是否需要更新，然后获取写权限
 * 并调用update_time。
 */
void touch_atime(const struct path *path)
{
	struct vfsmount *mnt = path->mnt;
	struct inode *inode = d_inode(path->dentry);
	struct timespec64 now;

	if (!atime_needs_update(path, inode))
		return;

	if (!sb_start_write_trylock(inode->i_sb))
		return;

	if (__mnt_want_write(mnt) != 0)
		goto skip_update;
	/*
	 * File systems can error out when updating inodes if they need to
	 * allocate new space to modify an inode (such is the case for
	 * Btrfs), but since we touch atime while walking down the path we
	 * really don't care if we failed to update the atime of the file,
	 * so just ignore the return value.
	 * We may also fail on filesystems that have the ability to make parts
	 * of the fs read only, e.g. subvolumes in Btrfs.
	 */
	now = current_time(inode);
	update_time(inode, &now, S_ATIME);
	__mnt_drop_write(mnt);
skip_update:
	sb_end_write(inode->i_sb);
}
EXPORT_SYMBOL(touch_atime);

/*
 * 我们想要的逻辑是
 *
 *	if suid or (sgid and xgrp)
 *		remove privs
 */
/**
 * should_remove_suid - 检查是否应该移除suid/sgid位
 * @dentry: 要检查的dentry
 *
 * 检查文件的权限位，确定是否需要移除setuid或setgid位。
 * 规则：
 * - suid位总是必须被清除
 * - sgid位只有在设置了执行权限时才需要清除
 *
 * 返回值: 需要清除的权限位掩码，如果不需要清除则返回0
 */
int should_remove_suid(struct dentry *dentry)
{
	umode_t mode = d_inode(dentry)->i_mode;
	int kill = 0;

	/* suid always must be killed */
	if (unlikely(mode & S_ISUID))
		kill = ATTR_KILL_SUID;

	/*
	 * sgid without any exec bits is just a mandatory locking mark; leave
	 * it alone.  If some exec bits are set, it's a real sgid; kill it.
	 */
	if (unlikely((mode & S_ISGID) && (mode & S_IXGRP)))
		kill |= ATTR_KILL_SGID;

	if (unlikely(kill && !capable(CAP_FSETID) && S_ISREG(mode)))
		return kill;

	return 0;
}
EXPORT_SYMBOL(should_remove_suid);

/*
 * 返回notify_change()需要的更改掩码，作为对写入或截断的响应。
 * 如果没有需要更改的内容则返回0。错误时返回负值（应拒绝更改）。
 */
/**
 * dentry_needs_remove_privs - 检查dentry是否需要移除特权
 * @dentry: 要检查的dentry
 *
 * 检查在写入或截断操作后是否需要移除文件的特权位。
 * 结合suid/sgid检查和安全模块的检查。
 *
 * 返回值: 需要清除的属性掩码，0表示不需要，负值表示错误
 */
int dentry_needs_remove_privs(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int mask = 0;
	int ret;

	if (IS_NOSEC(inode))
		return 0;

	mask = should_remove_suid(dentry);
	ret = security_inode_need_killpriv(dentry);
	if (ret < 0)
		return ret;
	if (ret)
		mask |= ATTR_KILL_PRIV;
	return mask;
}

/**
 * __remove_privs - 实际移除文件特权位
 * @dentry: 要处理的dentry
 * @kill: 要移除的特权位掩码
 *
 * 执行实际的特权位移除操作。设置适当的iattr结构并调用
 * notify_change来执行更改。
 *
 * 返回值: 成功返回0，失败返回错误码
 */
static int __remove_privs(struct dentry *dentry, int kill)
{
	struct iattr newattrs;

	newattrs.ia_valid = ATTR_FORCE | kill;
	/*
	 * Note we call this on write, so notify_change will not
	 * encounter any conflicting delegations:
	 */
	return notify_change(dentry, &newattrs, NULL);
}

/*
 * 当文件被写入或截断时移除特殊文件特权（suid、capabilities）。
 */
/**
 * file_remove_privs - 移除文件的特权位
 * @file: 要处理的文件
 *
 * 在文件写入或截断时移除特殊文件特权（suid、sgid、capabilities）。
 * 这是一个安全措施，防止通过修改设置了特权位的可执行文件来
 * 进行权限提升攻击。
 *
 * 返回值: 成功返回0，失败返回错误码
 */
int file_remove_privs(struct file *file)
{
	struct dentry *dentry = file_dentry(file);
	struct inode *inode = file_inode(file);
	int kill;
	int error = 0;

	/*
	 * Fast path for nothing security related.
	 * As well for non-regular files, e.g. blkdev inodes.
	 * For example, blkdev_write_iter() might get here
	 * trying to remove privs which it is not allowed to.
	 */
	if (IS_NOSEC(inode) || !S_ISREG(inode->i_mode))
		return 0;

	kill = dentry_needs_remove_privs(dentry);
	if (kill < 0)
		return kill;
	if (kill)
		error = __remove_privs(dentry, kill);
	if (!error)
		inode_has_no_xattr(inode);

	return error;
}
EXPORT_SYMBOL(file_remove_privs);

/**
 *	file_update_time - 更新mtime和ctime时间
 *	@file: 被访问的文件
 *
 *	更新inode的mtime和ctime成员并标记inode为需要写回。
 *	注意此函数专门用于文件系统的文件写入路径，文件系统可以
 *	选择通过S_NOCMTIME inode标志显式忽略通过此函数进行的
 *	更新，例如对于网络文件系统，这些时间戳由服务器处理。
 *	对于需要分配空间以更新inode的文件系统，这可能返回错误。
 */

int file_update_time(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct timespec64 now;
	int sync_it = 0;
	int ret;

	/* First try to exhaust all avenues to not sync */
	if (IS_NOCMTIME(inode))
		return 0;

	now = current_time(inode);
	if (!timespec64_equal(&inode->i_mtime, &now))
		sync_it = S_MTIME;

	if (!timespec64_equal(&inode->i_ctime, &now))
		sync_it |= S_CTIME;

	if (IS_I_VERSION(inode) && inode_iversion_need_inc(inode))
		sync_it |= S_VERSION;

	if (!sync_it)
		return 0;

	/* Finally allowed to write? Takes lock. */
	if (__mnt_want_write_file(file))
		return 0;

	ret = update_time(inode, &now, sync_it);
	__mnt_drop_write_file(file);

	return ret;
}
EXPORT_SYMBOL(file_update_time);

/* 调用者必须持有文件的inode锁 */
/**
 * file_modified - 处理文件修改时的权限和时间更新
 * @file: 被修改的文件
 *
 * 当文件被修改时调用，执行两个主要操作：
 * 1. 清除安全位（如果进程不是以root运行），防止修改setuid和setgid二进制文件
 * 2. 更新文件的时间戳
 *
 * 调用者必须持有文件的inode锁。
 *
 * 返回值: 成功返回0，失败返回错误码
 */
int file_modified(struct file *file)
{
	int err;

	/*
	 * Clear the security bits if the process is not being run by root.
	 * This keeps people from modifying setuid and setgid binaries.
	 */
	err = file_remove_privs(file);
	if (err)
		return err;

	if (unlikely(file->f_mode & FMODE_NOCMTIME))
		return 0;

	return file_update_time(file);
}
EXPORT_SYMBOL(file_modified);

/**
 * inode_needs_sync - 检查inode是否需要同步
 * @inode: 要检查的inode
 *
 * 检查inode是否需要同步写入。对于设置了IS_SYNC标志的inode
 * 或者是目录且设置了IS_DIRSYNC标志的inode返回真。
 *
 * 返回值: 需要同步返回1，否则返回0
 */
int inode_needs_sync(struct inode *inode)
{
	if (IS_SYNC(inode))
		return 1;
	if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
		return 1;
	return 0;
}
EXPORT_SYMBOL(inode_needs_sync);

/*
 * 如果我们尝试在inode哈希中查找一个正在被删除的inode，
 * 我们必须等到文件系统完成删除后才能报告找不到它。
 * 此函数等待删除_可能_已完成。调用者负责重新检查inode状态。
 *
 * 最初是否设置I_NEW并不重要，在从哈希列表中移除后调用
 * wake_up_bit(&inode->i_state, __I_NEW)将正确处理。
 */
/**
 * __wait_on_freeing_inode - 等待正在释放的inode完成释放
 * @inode: 正在释放的inode
 *
 * 当发现inode正在被释放时，等待释放过程完成。这避免了
 * 在inode释放过程中返回错误的查找结果。使用等待队列机制
 * 等待I_NEW位被清除，这发生在inode释放完成时。
 */
static void __wait_on_freeing_inode(struct inode *inode)
{
	wait_queue_head_t *wq;
	DEFINE_WAIT_BIT(wait, &inode->i_state, __I_NEW);
	wq = bit_waitqueue(&inode->i_state, __I_NEW);
	prepare_to_wait(wq, &wait.wq_entry, TASK_UNINTERRUPTIBLE);
	spin_unlock(&inode->i_lock);
	spin_unlock(&inode_hash_lock);
	schedule();
	finish_wait(wq, &wait.wq_entry);
	spin_lock(&inode_hash_lock);
}

static __initdata unsigned long ihash_entries;  /* inode哈希表条目数 */
/**
 * set_ihash_entries - 设置inode哈希表条目数的启动参数处理函数
 * @str: 参数字符串
 *
 * 处理启动参数"ihash_entries="，用于设置inode哈希表的大小。
 *
 * 返回值: 成功返回1，失败返回0
 */
static int __init set_ihash_entries(char *str)
{
	if (!str)
		return 0;
	ihash_entries = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("ihash_entries=", set_ihash_entries);

/*
 * 初始化等待队列和inode哈希表。
 */
/**
 * inode_init_early - inode子系统的早期初始化
 *
 * 在系统启动的早期阶段初始化inode哈希表。如果哈希分布在
 * NUMA节点上，则推迟哈希分配直到vmalloc空间可用。
 * 这是在内存管理完全初始化之前调用的。
 */
void __init inode_init_early(void)
{
	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	if (hashdist)
		return;

	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					HASH_EARLY | HASH_ZERO,
					&i_hash_shift,
					&i_hash_mask,
					0,
					0);
}

/**
 * inode_init - inode子系统的完整初始化
 *
 * 完成inode子系统的初始化：
 * 1. 创建inode slab缓存
 * 2. 如果之前没有在inode_init_early中创建，则创建inode哈希表
 *
 * 这在内存管理系统完全初始化后调用。
 */
void __init inode_init(void)
{
	/* inode slab cache */
	inode_cachep = kmem_cache_create("inode_cache",
					 sizeof(struct inode),
					 0,
					 (SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|
					 SLAB_MEM_SPREAD|SLAB_ACCOUNT),
					 init_once);

	/* Hash may have been set up in inode_init_early */
	if (!hashdist)
		return;

	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					HASH_ZERO,
					&i_hash_shift,
					&i_hash_mask,
					0,
					0);
}

/**
 * init_special_inode - 初始化特殊类型的inode
 * @inode: 要初始化的inode
 * @mode: inode的模式（类型和权限）
 * @rdev: 设备号（对设备文件有效）
 *
 * 根据inode类型初始化特殊inode的文件操作：
 * - 字符设备: 设置字符设备文件操作
 * - 块设备: 设置块设备文件操作
 * - FIFO: 设置管道文件操作
 * - 套接字: 保持默认（no_open_fops）
 *
 * 对于未知类型会打印调试信息。
 */
void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
	inode->i_mode = mode;
	if (S_ISCHR(mode)) {
		inode->i_fop = &def_chr_fops;
		inode->i_rdev = rdev;
	} else if (S_ISBLK(mode)) {
		inode->i_fop = &def_blk_fops;
		inode->i_rdev = rdev;
	} else if (S_ISFIFO(mode))
		inode->i_fop = &pipefifo_fops;
	else if (S_ISSOCK(mode))
		;	/* leave it no_open_fops */
	else
		printk(KERN_DEBUG "init_special_inode: bogus i_mode (%o) for"
				  " inode %s:%lu\n", mode, inode->i_sb->s_id,
				  inode->i_ino);
}
EXPORT_SYMBOL(init_special_inode);

/**
 * inode_init_owner - Init uid,gid,mode for new inode according to posix standards
 * @inode: New inode
 * @dir: Directory inode
 * @mode: mode of the new inode
 */
void inode_init_owner(struct inode *inode, const struct inode *dir,
			umode_t mode)
{
	inode->i_uid = current_fsuid();
	if (dir && dir->i_mode & S_ISGID) {
		inode->i_gid = dir->i_gid;

		/* Directories are special, and always inherit S_ISGID */
		if (S_ISDIR(mode))
			mode |= S_ISGID;
		else if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP) &&
			 !in_group_p(inode->i_gid) &&
			 !capable_wrt_inode_uidgid(dir, CAP_FSETID))
			mode &= ~S_ISGID;
	} else
		inode->i_gid = current_fsgid();
	inode->i_mode = mode;
}
EXPORT_SYMBOL(inode_init_owner);

/**
 * inode_owner_or_capable - check current task permissions to inode
 * @inode: inode being checked
 *
 * Return true if current either has CAP_FOWNER in a namespace with the
 * inode owner uid mapped, or owns the file.
 */
bool inode_owner_or_capable(const struct inode *inode)
{
	struct user_namespace *ns;

	if (uid_eq(current_fsuid(), inode->i_uid))
		return true;

	ns = current_user_ns();
	if (kuid_has_mapping(ns, inode->i_uid) && ns_capable(ns, CAP_FOWNER))
		return true;
	return false;
}
EXPORT_SYMBOL(inode_owner_or_capable);

/*
 * Direct i/o helper functions
 */
static void __inode_dio_wait(struct inode *inode)
{
	wait_queue_head_t *wq = bit_waitqueue(&inode->i_state, __I_DIO_WAKEUP);
	DEFINE_WAIT_BIT(q, &inode->i_state, __I_DIO_WAKEUP);

	do {
		prepare_to_wait(wq, &q.wq_entry, TASK_UNINTERRUPTIBLE);
		if (atomic_read(&inode->i_dio_count))
			schedule();
	} while (atomic_read(&inode->i_dio_count));
	finish_wait(wq, &q.wq_entry);
}

/**
 * inode_dio_wait - wait for outstanding DIO requests to finish
 * @inode: inode to wait for
 *
 * Waits for all pending direct I/O requests to finish so that we can
 * proceed with a truncate or equivalent operation.
 *
 * Must be called under a lock that serializes taking new references
 * to i_dio_count, usually by inode->i_mutex.
 */
void inode_dio_wait(struct inode *inode)
{
	if (atomic_read(&inode->i_dio_count))
		__inode_dio_wait(inode);
}
EXPORT_SYMBOL(inode_dio_wait);

/*
 * inode_set_flags - atomically set some inode flags
 *
 * Note: the caller should be holding i_mutex, or else be sure that
 * they have exclusive access to the inode structure (i.e., while the
 * inode is being instantiated).  The reason for the cmpxchg() loop
 * --- which wouldn't be necessary if all code paths which modify
 * i_flags actually followed this rule, is that there is at least one
 * code path which doesn't today so we use cmpxchg() out of an abundance
 * of caution.
 *
 * In the long run, i_mutex is overkill, and we should probably look
 * at using the i_lock spinlock to protect i_flags, and then make sure
 * it is so documented in include/linux/fs.h and that all code follows
 * the locking convention!!
 */
void inode_set_flags(struct inode *inode, unsigned int flags,
		     unsigned int mask)
{
	WARN_ON_ONCE(flags & ~mask);
	set_mask_bits(&inode->i_flags, mask, flags);
}
EXPORT_SYMBOL(inode_set_flags);

void inode_nohighmem(struct inode *inode)
{
	mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
}
EXPORT_SYMBOL(inode_nohighmem);

/**
 * timestamp_truncate - Truncate timespec to a granularity
 * @t: Timespec
 * @inode: inode being updated
 *
 * Truncate a timespec to the granularity supported by the fs
 * containing the inode. Always rounds down. gran must
 * not be 0 nor greater than a second (NSEC_PER_SEC, or 10^9 ns).
 */
struct timespec64 timestamp_truncate(struct timespec64 t, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	unsigned int gran = sb->s_time_gran;

	t.tv_sec = clamp(t.tv_sec, sb->s_time_min, sb->s_time_max);
	if (unlikely(t.tv_sec == sb->s_time_max || t.tv_sec == sb->s_time_min))
		t.tv_nsec = 0;

	/* Avoid division in the common cases 1 ns and 1 s. */
	if (gran == 1)
		; /* nothing */
	else if (gran == NSEC_PER_SEC)
		t.tv_nsec = 0;
	else if (gran > 1 && gran < NSEC_PER_SEC)
		t.tv_nsec -= t.tv_nsec % gran;
	else
		WARN(1, "invalid file time granularity: %u", gran);
	return t;
}
EXPORT_SYMBOL(timestamp_truncate);

/**
 * current_time - Return FS time
 * @inode: inode.
 *
 * Return the current time truncated to the time granularity supported by
 * the fs.
 *
 * Note that inode and inode->sb cannot be NULL.
 * Otherwise, the function warns and returns time without truncation.
 */
struct timespec64 current_time(struct inode *inode)
{
	struct timespec64 now;

	ktime_get_coarse_real_ts64(&now);

	if (unlikely(!inode->i_sb)) {
		WARN(1, "current_time() called with uninitialized super_block in the inode");
		return now;
	}

	return timestamp_truncate(now, inode);
}
EXPORT_SYMBOL(current_time);

/*
 * Generic function to check FS_IOC_SETFLAGS values and reject any invalid
 * configurations.
 *
 * Note: the caller should be holding i_mutex, or else be sure that they have
 * exclusive access to the inode structure.
 */
int vfs_ioc_setflags_prepare(struct inode *inode, unsigned int oldflags,
			     unsigned int flags)
{
	/*
	 * The IMMUTABLE and APPEND_ONLY flags can only be changed by
	 * the relevant capability.
	 *
	 * This test looks nicer. Thanks to Pauline Middelink
	 */
	if ((flags ^ oldflags) & (FS_APPEND_FL | FS_IMMUTABLE_FL) &&
	    !capable(CAP_LINUX_IMMUTABLE))
		return -EPERM;

	return fscrypt_prepare_setflags(inode, oldflags, flags);
}
EXPORT_SYMBOL(vfs_ioc_setflags_prepare);

/*
 * Generic function to check FS_IOC_FSSETXATTR values and reject any invalid
 * configurations.
 *
 * Note: the caller should be holding i_mutex, or else be sure that they have
 * exclusive access to the inode structure.
 */
int vfs_ioc_fssetxattr_check(struct inode *inode, const struct fsxattr *old_fa,
			     struct fsxattr *fa)
{
	/*
	 * Can't modify an immutable/append-only file unless we have
	 * appropriate permission.
	 */
	if ((old_fa->fsx_xflags ^ fa->fsx_xflags) &
			(FS_XFLAG_IMMUTABLE | FS_XFLAG_APPEND) &&
	    !capable(CAP_LINUX_IMMUTABLE))
		return -EPERM;

	/*
	 * Project Quota ID state is only allowed to change from within the init
	 * namespace. Enforce that restriction only if we are trying to change
	 * the quota ID state. Everything else is allowed in user namespaces.
	 */
	if (current_user_ns() != &init_user_ns) {
		if (old_fa->fsx_projid != fa->fsx_projid)
			return -EINVAL;
		if ((old_fa->fsx_xflags ^ fa->fsx_xflags) &
				FS_XFLAG_PROJINHERIT)
			return -EINVAL;
	}

	/* Check extent size hints. */
	if ((fa->fsx_xflags & FS_XFLAG_EXTSIZE) && !S_ISREG(inode->i_mode))
		return -EINVAL;

	if ((fa->fsx_xflags & FS_XFLAG_EXTSZINHERIT) &&
			!S_ISDIR(inode->i_mode))
		return -EINVAL;

	if ((fa->fsx_xflags & FS_XFLAG_COWEXTSIZE) &&
	    !S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode))
		return -EINVAL;

	/*
	 * It is only valid to set the DAX flag on regular files and
	 * directories on filesystems.
	 */
	if ((fa->fsx_xflags & FS_XFLAG_DAX) &&
	    !(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))
		return -EINVAL;

	/* Extent size hints of zero turn off the flags. */
	if (fa->fsx_extsize == 0)
		fa->fsx_xflags &= ~(FS_XFLAG_EXTSIZE | FS_XFLAG_EXTSZINHERIT);
	if (fa->fsx_cowextsize == 0)
		fa->fsx_xflags &= ~FS_XFLAG_COWEXTSIZE;

	return 0;
}
EXPORT_SYMBOL(vfs_ioc_fssetxattr_check);
