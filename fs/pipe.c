// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/pipe.c
 *  Linux 管道文件系统实现
 *
 *  实现了UNIX管道机制，包括匿名管道和命名管道(FIFO)
 *  提供进程间通信的基础设施
 *
 *  Copyright (C) 1991, 1992, 1999  Linus Torvalds
 */

#include <linux/mm.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/magic.h>
#include <linux/pipe_fs_i.h>
#include <linux/uio.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/audit.h>
#include <linux/syscalls.h>
#include <linux/fcntl.h>
#include <linux/memcontrol.h>
#include <linux/watch_queue.h>

#include <linux/uaccess.h>
#include <asm/ioctls.h>

#include "internal.h"

/*
 * 非root用户允许扩展管道的最大大小
 * 可以由root通过 /proc/sys/fs/pipe-max-size 设置
 */
unsigned int pipe_max_size = 1048576;

/* 每个用户可分配的最大页面数量
 * 硬限制默认未设置，软限制匹配默认值
 */
unsigned long pipe_user_pages_hard;
unsigned long pipe_user_pages_soft = PIPE_DEF_BUFFERS * INR_OPEN_CUR;

/*
 * We use head and tail indices that aren't masked off, except at the point of
 * dereference, but rather they're allowed to wrap naturally.  This means there
 * isn't a dead spot in the buffer, but the ring has to be a power of two and
 * <= 2^31.
 * -- David Howells 2019-09-23.
 *
 * Reads with count = 0 should always return 0.
 * -- Julian Bradfield 1999-06-07.
 *
 * FIFOs and Pipes now generate SIGIO for both readers and writers.
 * -- Jeremy Elson <jelson@circlemud.org> 2001-08-16
 *
 * pipe_read & write cleanup
 * -- Manfred Spraul <manfred@colorfullife.com> 2002-05-09
 */

/**
 * pipe_lock_nested - 嵌套地锁定管道
 * @pipe: 要锁定的管道信息结构
 * @subclass: 锁的子类别，用于避免死锁检测器误报
 *
 * 在嵌套场景中获取管道的互斥锁
 */
static void pipe_lock_nested(struct pipe_inode_info *pipe, int subclass)
{
	if (pipe->files)  /* 检查管道是否还有打开的文件引用 */
		mutex_lock_nested(&pipe->mutex, subclass);
}

/**
 * pipe_lock - 锁定管道
 * @pipe: 要锁定的管道信息结构
 *
 * pipe_lock() 嵌套非管道inode锁(用于写入文件)
 */
void pipe_lock(struct pipe_inode_info *pipe)
{
	/*
	 * pipe_lock() 嵌套非管道inode锁(用于写入文件)
	 */
	pipe_lock_nested(pipe, I_MUTEX_PARENT);
}
EXPORT_SYMBOL(pipe_lock);

/**
 * pipe_unlock - 解锁管道
 * @pipe: 要解锁的管道信息结构
 *
 * 释放管道的互斥锁
 */
void pipe_unlock(struct pipe_inode_info *pipe)
{
	if (pipe->files)  /* 检查管道是否还有打开的文件引用 */
		mutex_unlock(&pipe->mutex);
}
EXPORT_SYMBOL(pipe_unlock);

/**
 * __pipe_lock - 内部管道锁定函数
 * @pipe: 要锁定的管道信息结构
 *
 * 获取管道的互斥锁，用于内部调用
 */
static inline void __pipe_lock(struct pipe_inode_info *pipe)
{
	mutex_lock_nested(&pipe->mutex, I_MUTEX_PARENT);
}

/**
 * __pipe_unlock - 内部管道解锁函数
 * @pipe: 要解锁的管道信息结构
 *
 * 释放管道的互斥锁，用于内部调用
 */
static inline void __pipe_unlock(struct pipe_inode_info *pipe)
{
	mutex_unlock(&pipe->mutex);
}

/**
 * pipe_double_lock - 同时锁定两个管道
 * @pipe1: 第一个管道
 * @pipe2: 第二个管道
 *
 * 按地址顺序锁定两个管道，避免死锁
 * 用于需要操作两个管道的场景(如splice操作)
 */
void pipe_double_lock(struct pipe_inode_info *pipe1,
		      struct pipe_inode_info *pipe2)
{
	BUG_ON(pipe1 == pipe2);  /* 两个管道不能相同 */

	/* 按地址顺序锁定，确保一致的锁定顺序以避免死锁 */
	if (pipe1 < pipe2) {
		pipe_lock_nested(pipe1, I_MUTEX_PARENT);
		pipe_lock_nested(pipe2, I_MUTEX_CHILD);
	} else {
		pipe_lock_nested(pipe2, I_MUTEX_PARENT);
		pipe_lock_nested(pipe1, I_MUTEX_CHILD);
	}
}

/**
 * anon_pipe_buf_release - 释放匿名管道缓冲区
 * @pipe: 缓冲区所属的管道
 * @buf: 要释放的缓冲区
 *
 * 释放匿名管道缓冲区的页面。如果没有其他人使用该页面，
 * 且管道还没有临时页面，则将其保存为一级分配缓存。
 * 否则只是释放我们对它的引用。
 */
static void anon_pipe_buf_release(struct pipe_inode_info *pipe,
				  struct pipe_buffer *buf)
{
	struct page *page = buf->page;

	/*
	 * 如果没有其他人使用该页面，且我们还没有
	 * 临时页面，则将其保存为一级分配缓存。
	 * (否则只是释放我们对它的引用)
	 */
	if (page_count(page) == 1 && !pipe->tmp_page)  /* 页面引用计数为1且无临时页面 */
		pipe->tmp_page = page;  /* 保存为临时页面供后续使用 */
	else
		put_page(page);  /* 释放页面引用 */
}

/**
 * anon_pipe_buf_try_steal - 尝试窃取匿名管道缓冲区页面
 * @pipe: 缓冲区所属的管道
 * @buf: 要窃取的缓冲区
 *
 * 尝试获取页面的所有权。成功返回true。
 *
 * 返回值: 成功窃取返回true，否则返回false
 */
static bool anon_pipe_buf_try_steal(struct pipe_inode_info *pipe,
		struct pipe_buffer *buf)
{
	struct page *page = buf->page;

	if (page_count(page) != 1)  /* 页面引用计数不为1，无法窃取 */
		return false;
	memcg_kmem_uncharge_page(page, 0);  /* 取消内存控制组计费 */
	__SetPageLocked(page);  /* 设置页面锁定状态 */
	return true;
}

/**
 * generic_pipe_buf_try_steal - 通用管道缓冲区窃取尝试
 * @pipe: 缓冲区所属的管道
 * @buf: 要窃取的缓冲区
 *
 * 描述:
 *	此函数尝试窃取附加到@buf的&struct page。如果成功，
 *	此函数返回0并返回时页面已锁定。调用者可以随后重用
 *	该页面用于任何目的；典型用途是插入到不同的文件页面缓存中。
 */
bool generic_pipe_buf_try_steal(struct pipe_inode_info *pipe,
		struct pipe_buffer *buf)
{
	struct page *page = buf->page;

	/*
	 * 引用计数为1是黄金标准，这意味着该页面的所有者是
	 * 唯一持有对它的引用。锁定页面并返回OK。
	 */
	if (page_count(page) == 1) {
		lock_page(page);  /* 锁定页面 */
		return true;
	}
	return false;
}
EXPORT_SYMBOL(generic_pipe_buf_try_steal);

/**
 * generic_pipe_buf_get - 获取对&struct pipe_buffer的引用
 * @pipe: 缓冲区所属的管道
 * @buf: 要获取引用的缓冲区
 *
 * 描述:
 *	此函数获取对@buf的额外引用。它在tee()系统调用中使用，
 *	当我们将一个管道中的缓冲区复制到另一个管道时。
 */
bool generic_pipe_buf_get(struct pipe_inode_info *pipe, struct pipe_buffer *buf)
{
	return try_get_page(buf->page);  /* 尝试获取页面引用 */
}
EXPORT_SYMBOL(generic_pipe_buf_get);

/**
 * generic_pipe_buf_release - 释放对&struct pipe_buffer的引用
 * @pipe: 缓冲区所属的管道
 * @buf: 要释放引用的缓冲区
 *
 * 描述:
 *	此函数释放对@buf的引用。
 */
void generic_pipe_buf_release(struct pipe_inode_info *pipe,
			      struct pipe_buffer *buf)
{
	put_page(buf->page);  /* 释放页面引用 */
}
EXPORT_SYMBOL(generic_pipe_buf_release);

static const struct pipe_buf_operations anon_pipe_buf_ops = {
	.release	= anon_pipe_buf_release,
	.try_steal	= anon_pipe_buf_try_steal,
	.get		= generic_pipe_buf_get,
};

/* 在等待时无需持有管道锁的情况下完成 - 因此使用READ_ONCE() */
static inline bool pipe_readable(const struct pipe_inode_info *pipe)
{
	unsigned int head = READ_ONCE(pipe->head);  /* 原子读取头指针 */
	unsigned int tail = READ_ONCE(pipe->tail);  /* 原子读取尾指针 */
	unsigned int writers = READ_ONCE(pipe->writers);  /* 原子读取写者数量 */

	return !pipe_empty(head, tail) || !writers;  /* 管道非空或无写者时可读 */
}

/**
 * pipe_read - 从管道读取数据
 * @iocb: I/O控制块
 * @to: 目标缓冲区迭代器
 *
 * 从管道中读取数据到用户空间缓冲区
 *
 * 返回值: 成功读取的字节数，或负数错误码
 */
static ssize_t
pipe_read(struct kiocb *iocb, struct iov_iter *to)
{
	size_t total_len = iov_iter_count(to);  /* 请求读取的总长度 */
	struct file *filp = iocb->ki_filp;  /* 文件指针 */
	struct pipe_inode_info *pipe = filp->private_data;  /* 管道信息 */
	bool was_full, wake_next_reader = false;
	ssize_t ret;

	/* 空读取成功返回 */
	if (unlikely(total_len == 0))
		return 0;

	ret = 0;
	__pipe_lock(pipe);  /* 锁定管道 */

	/*
	 * 我们只在管道在开始读取时已满的情况下唤醒写者，
	 * 以避免不必要的唤醒。
	 *
	 * 但当我们确实唤醒写者时，我们使用同步唤醒(WF_SYNC)，
	 * 因为我们希望它们立即开始并为我们生成更多数据。
	 */
	was_full = pipe_full(pipe->head, pipe->tail, pipe->max_usage);
	for (;;) {
		unsigned int head = pipe->head;  /* 当前头位置 */
		unsigned int tail = pipe->tail;  /* 当前尾位置 */
		unsigned int mask = pipe->ring_size - 1;  /* 环形缓冲区掩码 */

#ifdef CONFIG_WATCH_QUEUE
		if (pipe->note_loss) {
			struct watch_notification n;

			if (total_len < 8) {
				if (ret == 0)
					ret = -ENOBUFS;
				break;
			}

			n.type = WATCH_TYPE_META;
			n.subtype = WATCH_META_LOSS_NOTIFICATION;
			n.info = watch_sizeof(n);
			if (copy_to_iter(&n, sizeof(n), to) != sizeof(n)) {
				if (ret == 0)
					ret = -EFAULT;
				break;
			}
			ret += sizeof(n);
			total_len -= sizeof(n);
			pipe->note_loss = false;
		}
#endif

		if (!pipe_empty(head, tail)) {  /* 管道非空，有数据可读 */
			struct pipe_buffer *buf = &pipe->bufs[tail & mask];  /* 获取尾部缓冲区 */
			size_t chars = buf->len;  /* 缓冲区中的字节数 */
			size_t written;
			int error;

			if (chars > total_len) {  /* 缓冲区数据超过请求长度 */
				if (buf->flags & PIPE_BUF_FLAG_WHOLE) {  /* 需要完整读取的缓冲区 */
					if (ret == 0)
						ret = -ENOBUFS;  /* 缓冲区空间不足 */
					break;
				}
				chars = total_len;  /* 只读取请求的长度 */
			}

			error = pipe_buf_confirm(pipe, buf);  /* 确认缓冲区状态 */
			if (error) {
				if (!ret)
					ret = error;
				break;
			}

			/* 将页面数据拷贝到用户空间 */
			written = copy_page_to_iter(buf->page, buf->offset, chars, to);
			if (unlikely(written < chars)) {  /* 拷贝失败 */
				if (!ret)
					ret = -EFAULT;  /* 页面错误 */
				break;
			}
			ret += chars;  /* 累计读取的字节数 */
			buf->offset += chars;  /* 更新缓冲区偏移 */
			buf->len -= chars;  /* 减少缓冲区剩余长度 */

			/* 是否为数据包缓冲区？清理并退出 */
			if (buf->flags & PIPE_BUF_FLAG_PACKET) {
				total_len = chars;
				buf->len = 0;
			}

			if (!buf->len) {  /* 缓冲区已读完 */
				pipe_buf_release(pipe, buf);  /* 释放缓冲区 */
				spin_lock_irq(&pipe->rd_wait.lock);  /* 锁定读等待队列 */
#ifdef CONFIG_WATCH_QUEUE
				if (buf->flags & PIPE_BUF_FLAG_LOSS)  /* 标记数据丢失 */
					pipe->note_loss = true;
#endif
				tail++;  /* 移动尾指针 */
				pipe->tail = tail;
				spin_unlock_irq(&pipe->rd_wait.lock);  /* 解锁读等待队列 */
			}
			total_len -= chars;  /* 减少剩余需读取长度 */
			if (!total_len)
				break;	/* 通用路径：读取成功完成 */
			if (!pipe_empty(head, tail))	/* 还有更多数据？ */
				continue;
		}

		if (!pipe->writers)  /* 没有写者 */
			break;
		if (ret)  /* 已有数据读取 */
			break;
		if (filp->f_flags & O_NONBLOCK) {  /* 非阻塞模式 */
			ret = -EAGAIN;
			break;
		}
		__pipe_unlock(pipe);  /* 解锁管道 */

		/*
		 * 我们只有在实际上没有读取任何东西时才到这里。
		 *
		 * 然而，我们可能已经看到(并移除)了一个零大小的
		 * 管道缓冲区，并可能以这种方式在缓冲区中腾出了空间。
		 *
		 * 你不能通过空写(即使在数据包模式下)来制造零大小
		 * 的管道缓冲区，但如果写者在尝试填充已经分配并
		 * 插入到缓冲区数组中的缓冲区时得到EFAULT，就会发生这种情况。
		 *
		 * 所以在管道已满但我们没有得到数据的极不可能情况下，
		 * 我们仍然需要唤醒任何待处理的写者。
		 */
		if (unlikely(was_full)) {
			wake_up_interruptible_sync_poll(&pipe->wr_wait, EPOLLOUT | EPOLLWRNORM);
			kill_fasync(&pipe->fasync_writers, SIGIO, POLL_OUT);
		}

		/*
		 * 但因为我们没有读取任何东西，此时我们可以
		 * 在被中断时直接返回-ERESTARTSYS，因为我们已经
		 * 完成了任何必需的唤醒，不需要标记任何访问。
		 * 而且我们已经放弃了锁。
		 */
		if (wait_event_interruptible_exclusive(pipe->rd_wait, pipe_readable(pipe)) < 0)
			return -ERESTARTSYS;

		__pipe_lock(pipe);  /* 重新锁定管道 */
		was_full = pipe_full(pipe->head, pipe->tail, pipe->max_usage);
		wake_next_reader = true;
	}
	if (pipe_empty(pipe->head, pipe->tail))  /* 管道为空 */
		wake_next_reader = false;
	__pipe_unlock(pipe);  /* 解锁管道 */

	if (was_full) {  /* 如果管道曾经满了，唤醒写者 */
		wake_up_interruptible_sync_poll(&pipe->wr_wait, EPOLLOUT | EPOLLWRNORM);
		kill_fasync(&pipe->fasync_writers, SIGIO, POLL_OUT);
	}
	if (wake_next_reader)  /* 唤醒下一个读者 */
		wake_up_interruptible_sync_poll(&pipe->rd_wait, EPOLLIN | EPOLLRDNORM);
	if (ret > 0)  /* 如果成功读取了数据，更新访问时间 */
		file_accessed(filp);
	return ret;
}

/**
 * is_packetized - 检查文件是否为数据包模式
 * @file: 要检查的文件
 *
 * 返回值: 如果文件设置了O_DIRECT标志则返回非零值
 */
static inline int is_packetized(struct file *file)
{
	return (file->f_flags & O_DIRECT) != 0;
}

/* 在等待时无需持有管道锁的情况下完成 - 因此使用READ_ONCE() */
static inline bool pipe_writable(const struct pipe_inode_info *pipe)
{
	unsigned int head = READ_ONCE(pipe->head);  /* 原子读取头指针 */
	unsigned int tail = READ_ONCE(pipe->tail);  /* 原子读取尾指针 */
	unsigned int max_usage = READ_ONCE(pipe->max_usage);  /* 原子读取最大使用量 */

	return !pipe_full(head, tail, max_usage) ||  /* 管道未满或无读者时可写 */
		!READ_ONCE(pipe->readers);
}

/**
 * pipe_write - 向管道写入数据
 * @iocb: I/O控制块
 * @from: 源缓冲区迭代器
 *
 * 将数据从用户空间缓冲区写入管道
 *
 * 返回值: 成功写入的字节数，或负数错误码
 */
static ssize_t
pipe_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *filp = iocb->ki_filp;  /* 文件指针 */
	struct pipe_inode_info *pipe = filp->private_data;  /* 管道信息 */
	unsigned int head;
	ssize_t ret = 0;
	size_t total_len = iov_iter_count(from);  /* 请求写入的总长度 */
	ssize_t chars;
	bool was_empty = false;
	bool wake_next_writer = false;

	/* 空写入成功返回 */
	if (unlikely(total_len == 0))
		return 0;

	__pipe_lock(pipe);  /* 锁定管道 */

	if (!pipe->readers) {  /* 没有读者 */
		send_sig(SIGPIPE, current, 0);  /* 发送SIGPIPE信号 */
		ret = -EPIPE;
		goto out;
	}

#ifdef CONFIG_WATCH_QUEUE
	if (pipe->watch_queue) {
		ret = -EXDEV;
		goto out;
	}
#endif

	/*
	 * Only wake up if the pipe started out empty, since
	 * otherwise there should be no readers waiting.
	 *
	 * If it wasn't empty we try to merge new data into
	 * the last buffer.
	 *
	 * That naturally merges small writes, but it also
	 * page-aligs the rest of the writes for large writes
	 * spanning multiple pages.
	 */
	head = pipe->head;
	was_empty = pipe_empty(head, pipe->tail);
	chars = total_len & (PAGE_SIZE-1);
	if (chars && !was_empty) {
		unsigned int mask = pipe->ring_size - 1;
		struct pipe_buffer *buf = &pipe->bufs[(head - 1) & mask];
		int offset = buf->offset + buf->len;

		if ((buf->flags & PIPE_BUF_FLAG_CAN_MERGE) &&
		    offset + chars <= PAGE_SIZE) {
			ret = pipe_buf_confirm(pipe, buf);
			if (ret)
				goto out;

			ret = copy_page_from_iter(buf->page, offset, chars, from);
			if (unlikely(ret < chars)) {
				ret = -EFAULT;
				goto out;
			}

			buf->len += ret;
			if (!iov_iter_count(from))
				goto out;
		}
	}

	for (;;) {
		if (!pipe->readers) {
			send_sig(SIGPIPE, current, 0);
			if (!ret)
				ret = -EPIPE;
			break;
		}

		head = pipe->head;
		if (!pipe_full(head, pipe->tail, pipe->max_usage)) {
			unsigned int mask = pipe->ring_size - 1;
			struct pipe_buffer *buf = &pipe->bufs[head & mask];
			struct page *page = pipe->tmp_page;
			int copied;

			if (!page) {
				page = alloc_page(GFP_HIGHUSER | __GFP_ACCOUNT);
				if (unlikely(!page)) {
					ret = ret ? : -ENOMEM;
					break;
				}
				pipe->tmp_page = page;
			}

			/* Allocate a slot in the ring in advance and attach an
			 * empty buffer.  If we fault or otherwise fail to use
			 * it, either the reader will consume it or it'll still
			 * be there for the next write.
			 */
			spin_lock_irq(&pipe->rd_wait.lock);

			head = pipe->head;
			if (pipe_full(head, pipe->tail, pipe->max_usage)) {
				spin_unlock_irq(&pipe->rd_wait.lock);
				continue;
			}

			pipe->head = head + 1;
			spin_unlock_irq(&pipe->rd_wait.lock);

			/* Insert it into the buffer array */
			buf = &pipe->bufs[head & mask];
			buf->page = page;
			buf->ops = &anon_pipe_buf_ops;
			buf->offset = 0;
			buf->len = 0;
			if (is_packetized(filp))
				buf->flags = PIPE_BUF_FLAG_PACKET;
			else
				buf->flags = PIPE_BUF_FLAG_CAN_MERGE;
			pipe->tmp_page = NULL;

			copied = copy_page_from_iter(page, 0, PAGE_SIZE, from);
			if (unlikely(copied < PAGE_SIZE && iov_iter_count(from))) {
				if (!ret)
					ret = -EFAULT;
				break;
			}
			ret += copied;
			buf->offset = 0;
			buf->len = copied;

			if (!iov_iter_count(from))
				break;
		}

		if (!pipe_full(head, pipe->tail, pipe->max_usage))
			continue;

		/* Wait for buffer space to become available. */
		if (filp->f_flags & O_NONBLOCK) {
			if (!ret)
				ret = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			if (!ret)
				ret = -ERESTARTSYS;
			break;
		}

		/*
		 * We're going to release the pipe lock and wait for more
		 * space. We wake up any readers if necessary, and then
		 * after waiting we need to re-check whether the pipe
		 * become empty while we dropped the lock.
		 */
		__pipe_unlock(pipe);
		if (was_empty) {
			wake_up_interruptible_sync_poll(&pipe->rd_wait, EPOLLIN | EPOLLRDNORM);
			kill_fasync(&pipe->fasync_readers, SIGIO, POLL_IN);
		}
		wait_event_interruptible_exclusive(pipe->wr_wait, pipe_writable(pipe));
		__pipe_lock(pipe);
		was_empty = pipe_empty(pipe->head, pipe->tail);
		wake_next_writer = true;
	}
out:
	if (pipe_full(pipe->head, pipe->tail, pipe->max_usage))
		wake_next_writer = false;
	__pipe_unlock(pipe);

	/*
	 * If we do do a wakeup event, we do a 'sync' wakeup, because we
	 * want the reader to start processing things asap, rather than
	 * leave the data pending.
	 *
	 * This is particularly important for small writes, because of
	 * how (for example) the GNU make jobserver uses small writes to
	 * wake up pending jobs
	 */
	if (was_empty) {
		wake_up_interruptible_sync_poll(&pipe->rd_wait, EPOLLIN | EPOLLRDNORM);
		kill_fasync(&pipe->fasync_readers, SIGIO, POLL_IN);
	}
	if (wake_next_writer)
		wake_up_interruptible_sync_poll(&pipe->wr_wait, EPOLLOUT | EPOLLWRNORM);
	if (ret > 0 && sb_start_write_trylock(file_inode(filp)->i_sb)) {
		int err = file_update_time(filp);
		if (err)
			ret = err;
		sb_end_write(file_inode(filp)->i_sb);
	}
	return ret;
}

static long pipe_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct pipe_inode_info *pipe = filp->private_data;
	int count, head, tail, mask;

	switch (cmd) {
	case FIONREAD:
		__pipe_lock(pipe);
		count = 0;
		head = pipe->head;
		tail = pipe->tail;
		mask = pipe->ring_size - 1;

		while (tail != head) {
			count += pipe->bufs[tail & mask].len;
			tail++;
		}
		__pipe_unlock(pipe);

		return put_user(count, (int __user *)arg);

#ifdef CONFIG_WATCH_QUEUE
	case IOC_WATCH_QUEUE_SET_SIZE: {
		int ret;
		__pipe_lock(pipe);
		ret = watch_queue_set_size(pipe, arg);
		__pipe_unlock(pipe);
		return ret;
	}

	case IOC_WATCH_QUEUE_SET_FILTER:
		return watch_queue_set_filter(
			pipe, (struct watch_notification_filter __user *)arg);
#endif

	default:
		return -ENOIOCTLCMD;
	}
}

/* No kernel lock held - fine */
static __poll_t
pipe_poll(struct file *filp, poll_table *wait)
{
	__poll_t mask;
	struct pipe_inode_info *pipe = filp->private_data;
	unsigned int head, tail;

	/*
	 * Reading pipe state only -- no need for acquiring the semaphore.
	 *
	 * But because this is racy, the code has to add the
	 * entry to the poll table _first_ ..
	 */
	if (filp->f_mode & FMODE_READ)
		poll_wait(filp, &pipe->rd_wait, wait);
	if (filp->f_mode & FMODE_WRITE)
		poll_wait(filp, &pipe->wr_wait, wait);

	/*
	 * .. and only then can you do the racy tests. That way,
	 * if something changes and you got it wrong, the poll
	 * table entry will wake you up and fix it.
	 */
	head = READ_ONCE(pipe->head);
	tail = READ_ONCE(pipe->tail);

	mask = 0;
	if (filp->f_mode & FMODE_READ) {
		if (!pipe_empty(head, tail))
			mask |= EPOLLIN | EPOLLRDNORM;
		if (!pipe->writers && filp->f_version != pipe->w_counter)
			mask |= EPOLLHUP;
	}

	if (filp->f_mode & FMODE_WRITE) {
		if (!pipe_full(head, tail, pipe->max_usage))
			mask |= EPOLLOUT | EPOLLWRNORM;
		/*
		 * Most Unices do not set EPOLLERR for FIFOs but on Linux they
		 * behave exactly like pipes for poll().
		 */
		if (!pipe->readers)
			mask |= EPOLLERR;
	}

	return mask;
}

static void put_pipe_info(struct inode *inode, struct pipe_inode_info *pipe)
{
	int kill = 0;

	spin_lock(&inode->i_lock);
	if (!--pipe->files) {
		inode->i_pipe = NULL;
		kill = 1;
	}
	spin_unlock(&inode->i_lock);

	if (kill)
		free_pipe_info(pipe);
}

/**
 * pipe_release - 管道文件释放函数
 * @inode: 管道的inode结构
 * @file: 要释放的文件结构
 *
 * 当管道的读端或写端被关闭时调用此函数。
 * 更新读者/写者计数，如果是最后一个读者或写者，
 * 则唤醒等待的进程。
 *
 * 返回值: 总是返回0
 */
static int
pipe_release(struct inode *inode, struct file *file)
{
	struct pipe_inode_info *pipe = file->private_data;

	__pipe_lock(pipe);
	if (file->f_mode & FMODE_READ)
		pipe->readers--;
	if (file->f_mode & FMODE_WRITE)
		pipe->writers--;

	/* Was that the last reader or writer, but not the other side? */
	if (!pipe->readers != !pipe->writers) {
		wake_up_interruptible_all(&pipe->rd_wait);
		wake_up_interruptible_all(&pipe->wr_wait);
		kill_fasync(&pipe->fasync_readers, SIGIO, POLL_IN);
		kill_fasync(&pipe->fasync_writers, SIGIO, POLL_OUT);
	}
	__pipe_unlock(pipe);

	put_pipe_info(inode, pipe);
	return 0;
}

static int
pipe_fasync(int fd, struct file *filp, int on)
{
	struct pipe_inode_info *pipe = filp->private_data;
	int retval = 0;

	__pipe_lock(pipe);
	if (filp->f_mode & FMODE_READ)
		retval = fasync_helper(fd, filp, on, &pipe->fasync_readers);
	if ((filp->f_mode & FMODE_WRITE) && retval >= 0) {
		retval = fasync_helper(fd, filp, on, &pipe->fasync_writers);
		if (retval < 0 && (filp->f_mode & FMODE_READ))
			/* this can happen only if on == T */
			fasync_helper(-1, filp, 0, &pipe->fasync_readers);
	}
	__pipe_unlock(pipe);
	return retval;
}

unsigned long account_pipe_buffers(struct user_struct *user,
				   unsigned long old, unsigned long new)
{
	return atomic_long_add_return(new - old, &user->pipe_bufs);
}

bool too_many_pipe_buffers_soft(unsigned long user_bufs)
{
	unsigned long soft_limit = READ_ONCE(pipe_user_pages_soft);

	return soft_limit && user_bufs > soft_limit;
}

bool too_many_pipe_buffers_hard(unsigned long user_bufs)
{
	unsigned long hard_limit = READ_ONCE(pipe_user_pages_hard);

	return hard_limit && user_bufs > hard_limit;
}

bool pipe_is_unprivileged_user(void)
{
	return !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN);
}

/**
 * alloc_pipe_info - 分配管道信息结构
 *
 * 为新的管道分配并初始化pipe_inode_info结构
 * 包括缓冲区分配、用户配额检查等
 *
 * 返回值: 成功返回分配的pipe_inode_info指针，失败返回NULL
 */
struct pipe_inode_info *alloc_pipe_info(void)
{
	struct pipe_inode_info *pipe;
	unsigned long pipe_bufs = PIPE_DEF_BUFFERS;  /* 默认缓冲区数量 */
	struct user_struct *user = get_current_user();  /* 获取当前用户结构 */
	unsigned long user_bufs;
	unsigned int max_size = READ_ONCE(pipe_max_size);  /* 读取最大管道大小 */

	pipe = kzalloc(sizeof(struct pipe_inode_info), GFP_KERNEL_ACCOUNT);
	if (pipe == NULL)
		goto out_free_uid;

	/* 如果缓冲区大小超过最大值且用户没有特权，则限制缓冲区数量 */
	if (pipe_bufs * PAGE_SIZE > max_size && !capable(CAP_SYS_RESOURCE))
		pipe_bufs = max_size >> PAGE_SHIFT;

	/* 计算用户已使用的管道缓冲区页面数 */
	user_bufs = account_pipe_buffers(user, 0, pipe_bufs);

	/* 检查软限制：非特权用户是否超过软限制 */
	if (too_many_pipe_buffers_soft(user_bufs) && pipe_is_unprivileged_user()) {
		user_bufs = account_pipe_buffers(user, pipe_bufs, 1);
		pipe_bufs = 1;  /* 将缓冲区数量降到最小 */
	}

	/* 检查硬限制：非特权用户是否超过硬限制 */
	if (too_many_pipe_buffers_hard(user_bufs) && pipe_is_unprivileged_user())
		goto out_revert_acct;

	/* 分配管道缓冲区数组 */
	pipe->bufs = kcalloc(pipe_bufs, sizeof(struct pipe_buffer),
			     GFP_KERNEL_ACCOUNT);

	if (pipe->bufs) {
		/* 初始化等待队列和管道参数 */
		init_waitqueue_head(&pipe->rd_wait);  /* 读等待队列 */
		init_waitqueue_head(&pipe->wr_wait);  /* 写等待队列 */
		pipe->r_counter = pipe->w_counter = 1;  /* 读写计数器 */
		pipe->max_usage = pipe_bufs;  /* 最大使用量 */
		pipe->ring_size = pipe_bufs;  /* 环形缓冲区大小 */
		pipe->nr_accounted = pipe_bufs;  /* 已计费的缓冲区数 */
		pipe->user = user;  /* 关联用户 */
		mutex_init(&pipe->mutex);  /* 初始化互斥锁 */
		return pipe;
	}

out_revert_acct:
	(void) account_pipe_buffers(user, pipe_bufs, 0);  /* 回滚账户统计 */
	kfree(pipe);
out_free_uid:
	free_uid(user);  /* 释放用户引用 */
	return NULL;
}

void free_pipe_info(struct pipe_inode_info *pipe)
{
	int i;

#ifdef CONFIG_WATCH_QUEUE
	if (pipe->watch_queue) {
		watch_queue_clear(pipe->watch_queue);
		put_watch_queue(pipe->watch_queue);
	}
#endif

	(void) account_pipe_buffers(pipe->user, pipe->nr_accounted, 0);
	free_uid(pipe->user);
	for (i = 0; i < pipe->ring_size; i++) {
		struct pipe_buffer *buf = pipe->bufs + i;
		if (buf->ops)
			pipe_buf_release(pipe, buf);
	}
	if (pipe->tmp_page)
		__free_page(pipe->tmp_page);
	kfree(pipe->bufs);
	kfree(pipe);
}

static struct vfsmount *pipe_mnt __read_mostly;

/*
 * pipefs_dname() is called from d_path().
 */
static char *pipefs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	return dynamic_dname(dentry, buffer, buflen, "pipe:[%lu]",
				d_inode(dentry)->i_ino);
}

static const struct dentry_operations pipefs_dentry_operations = {
	.d_dname	= pipefs_dname,
};

static struct inode * get_pipe_inode(void)
{
	struct inode *inode = new_inode_pseudo(pipe_mnt->mnt_sb);
	struct pipe_inode_info *pipe;

	if (!inode)
		goto fail_inode;

	inode->i_ino = get_next_ino();

	pipe = alloc_pipe_info();
	if (!pipe)
		goto fail_iput;

	inode->i_pipe = pipe;
	pipe->files = 2;
	pipe->readers = pipe->writers = 1;
	inode->i_fop = &pipefifo_fops;

	/*
	 * Mark the inode dirty from the very beginning,
	 * that way it will never be moved to the dirty
	 * list because "mark_inode_dirty()" will think
	 * that it already _is_ on the dirty list.
	 */
	inode->i_state = I_DIRTY;
	inode->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

	return inode;

fail_iput:
	iput(inode);

fail_inode:
	return NULL;
}

/**
 * create_pipe_files - 创建管道文件对
 * @res: 用于返回文件指针的数组 (res[0]为读端，res[1]为写端)
 * @flags: 管道创建标志
 *
 * 创建一对文件描述符用于管道通信
 * res[0]是读端(O_RDONLY)，res[1]是写端(O_WRONLY)
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
int create_pipe_files(struct file **res, int flags)
{
	struct inode *inode = get_pipe_inode();  /* 获取管道inode */
	struct file *f;
	int error;

	if (!inode)
		return -ENFILE;  /* 无法创建inode */

	if (flags & O_NOTIFICATION_PIPE) {  /* 通知管道 */
		error = watch_queue_init(inode->i_pipe);  /* 初始化监视队列 */
		if (error) {
			free_pipe_info(inode->i_pipe);
			iput(inode);
			return error;
		}
	}

	/* 创建写端文件 */
	f = alloc_file_pseudo(inode, pipe_mnt, "",
				O_WRONLY | (flags & (O_NONBLOCK | O_DIRECT)),
				&pipefifo_fops);
	if (IS_ERR(f)) {
		free_pipe_info(inode->i_pipe);
		iput(inode);
		return PTR_ERR(f);
	}

	f->private_data = inode->i_pipe;  /* 关联管道信息 */

	/* 克隆文件创建读端 */
	res[0] = alloc_file_clone(f, O_RDONLY | (flags & O_NONBLOCK),
				  &pipefifo_fops);
	if (IS_ERR(res[0])) {
		put_pipe_info(inode, inode->i_pipe);
		fput(f);
		return PTR_ERR(res[0]);
	}
	res[0]->private_data = inode->i_pipe;  /* 关联管道信息 */
	res[1] = f;  /* 写端文件 */
	stream_open(inode, res[0]);  /* 设置为流模式 */
	stream_open(inode, res[1]);
	return 0;
}

/**
 * __do_pipe_flags - 创建管道的内部实现
 * @fd: 用于返回文件描述符的数组
 * @files: 用于返回文件指针的数组
 * @flags: 管道创建标志
 *
 * 创建管道并分配文件描述符，但不安装到进程的文件描述符表中
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
static int __do_pipe_flags(int *fd, struct file **files, int flags)
{
	int error;
	int fdw, fdr;

	/* 检查标志的有效性 */
	if (flags & ~(O_CLOEXEC | O_NONBLOCK | O_DIRECT | O_NOTIFICATION_PIPE))
		return -EINVAL;

	error = create_pipe_files(files, flags);  /* 创建管道文件对 */
	if (error)
		return error;

	/* 获取读端文件描述符 */
	error = get_unused_fd_flags(flags);
	if (error < 0)
		goto err_read_pipe;
	fdr = error;

	/* 获取写端文件描述符 */
	error = get_unused_fd_flags(flags);
	if (error < 0)
		goto err_fdr;
	fdw = error;

	audit_fd_pair(fdr, fdw);  /* 审计文件描述符对 */
	fd[0] = fdr;  /* 读端描述符 */
	fd[1] = fdw;  /* 写端描述符 */
	return 0;

 err_fdr:
	put_unused_fd(fdr);  /* 释放读端描述符 */
 err_read_pipe:
	fput(files[0]);  /* 释放文件引用 */
	fput(files[1]);
	return error;
}

/**
 * do_pipe_flags - 创建管道并安装文件描述符
 * @fd: 用于返回文件描述符的数组
 * @flags: 管道创建标志
 *
 * 创建管道并将文件描述符安装到当前进程的文件描述符表中
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
int do_pipe_flags(int *fd, int flags)
{
	struct file *files[2];
	int error = __do_pipe_flags(fd, files, flags);
	if (!error) {
		fd_install(fd[0], files[0]);  /* 安装读端描述符 */
		fd_install(fd[1], files[1]);  /* 安装写端描述符 */
	}
	return error;
}

/*
 * sys_pipe() 是创建管道的标准C调用约定。
 * 但这不是Unix传统的做法。
 */
/**
 * do_pipe2 - pipe2系统调用的内部实现
 * @fildes: 用户空间文件描述符数组指针
 * @flags: 管道创建标志
 *
 * 实现pipe2系统调用，支持额外的标志参数
 *
 * 返回值: 成功返回0，失败返回负数错误码
 */
static int do_pipe2(int __user *fildes, int flags)
{
	struct file *files[2];
	int fd[2];
	int error;

	error = __do_pipe_flags(fd, files, flags);
	if (!error) {
		/* 将文件描述符拷贝到用户空间 */
		if (unlikely(copy_to_user(fildes, fd, sizeof(fd)))) {
			fput(files[0]);  /* 拷贝失败，清理资源 */
			fput(files[1]);
			put_unused_fd(fd[0]);
			put_unused_fd(fd[1]);
			error = -EFAULT;
		} else {
			fd_install(fd[0], files[0]);  /* 安装文件描述符 */
			fd_install(fd[1], files[1]);
		}
	}
	return error;
}

/**
 * SYSCALL_DEFINE2(pipe2) - pipe2系统调用入口
 * @fildes: 用户空间文件描述符数组指针
 * @flags: 管道创建标志
 *
 * pipe2系统调用，支持O_CLOEXEC、O_NONBLOCK等标志
 */
SYSCALL_DEFINE2(pipe2, int __user *, fildes, int, flags)
{
	return do_pipe2(fildes, flags);
}

/**
 * SYSCALL_DEFINE1(pipe) - 传统pipe系统调用入口
 * @fildes: 用户空间文件描述符数组指针
 *
 * 传统的pipe系统调用，等同于flags为0的pipe2调用
 */
SYSCALL_DEFINE1(pipe, int __user *, fildes)
{
	return do_pipe2(fildes, 0);
}

/*
 * This is the stupid "wait for pipe to be readable or writable"
 * model.
 *
 * See pipe_read/write() for the proper kind of exclusive wait,
 * but that requires that we wake up any other readers/writers
 * if we then do not end up reading everything (ie the whole
 * "wake_next_reader/writer" logic in pipe_read/write()).
 */
void pipe_wait_readable(struct pipe_inode_info *pipe)
{
	pipe_unlock(pipe);
	wait_event_interruptible(pipe->rd_wait, pipe_readable(pipe));
	pipe_lock(pipe);
}

void pipe_wait_writable(struct pipe_inode_info *pipe)
{
	pipe_unlock(pipe);
	wait_event_interruptible(pipe->wr_wait, pipe_writable(pipe));
	pipe_lock(pipe);
}

/*
 * This depends on both the wait (here) and the wakeup (wake_up_partner)
 * holding the pipe lock, so "*cnt" is stable and we know a wakeup cannot
 * race with the count check and waitqueue prep.
 *
 * Normally in order to avoid races, you'd do the prepare_to_wait() first,
 * then check the condition you're waiting for, and only then sleep. But
 * because of the pipe lock, we can check the condition before being on
 * the wait queue.
 *
 * We use the 'rd_wait' waitqueue for pipe partner waiting.
 */
static int wait_for_partner(struct pipe_inode_info *pipe, unsigned int *cnt)
{
	DEFINE_WAIT(rdwait);
	int cur = *cnt;

	while (cur == *cnt) {
		prepare_to_wait(&pipe->rd_wait, &rdwait, TASK_INTERRUPTIBLE);
		pipe_unlock(pipe);
		schedule();
		finish_wait(&pipe->rd_wait, &rdwait);
		pipe_lock(pipe);
		if (signal_pending(current))
			break;
	}
	return cur == *cnt ? -ERESTARTSYS : 0;
}

static void wake_up_partner(struct pipe_inode_info *pipe)
{
	wake_up_interruptible_all(&pipe->rd_wait);
}

static int fifo_open(struct inode *inode, struct file *filp)
{
	struct pipe_inode_info *pipe;
	bool is_pipe = inode->i_sb->s_magic == PIPEFS_MAGIC;
	int ret;

	filp->f_version = 0;

	spin_lock(&inode->i_lock);
	if (inode->i_pipe) {
		pipe = inode->i_pipe;
		pipe->files++;
		spin_unlock(&inode->i_lock);
	} else {
		spin_unlock(&inode->i_lock);
		pipe = alloc_pipe_info();
		if (!pipe)
			return -ENOMEM;
		pipe->files = 1;
		spin_lock(&inode->i_lock);
		if (unlikely(inode->i_pipe)) {
			inode->i_pipe->files++;
			spin_unlock(&inode->i_lock);
			free_pipe_info(pipe);
			pipe = inode->i_pipe;
		} else {
			inode->i_pipe = pipe;
			spin_unlock(&inode->i_lock);
		}
	}
	filp->private_data = pipe;
	/* OK, we have a pipe and it's pinned down */

	__pipe_lock(pipe);

	/* We can only do regular read/write on fifos */
	stream_open(inode, filp);

	switch (filp->f_mode & (FMODE_READ | FMODE_WRITE)) {
	case FMODE_READ:
	/*
	 *  O_RDONLY
	 *  POSIX.1 says that O_NONBLOCK means return with the FIFO
	 *  opened, even when there is no process writing the FIFO.
	 */
		pipe->r_counter++;
		if (pipe->readers++ == 0)
			wake_up_partner(pipe);

		if (!is_pipe && !pipe->writers) {
			if ((filp->f_flags & O_NONBLOCK)) {
				/* suppress EPOLLHUP until we have
				 * seen a writer */
				filp->f_version = pipe->w_counter;
			} else {
				if (wait_for_partner(pipe, &pipe->w_counter))
					goto err_rd;
			}
		}
		break;

	case FMODE_WRITE:
	/*
	 *  O_WRONLY
	 *  POSIX.1 says that O_NONBLOCK means return -1 with
	 *  errno=ENXIO when there is no process reading the FIFO.
	 */
		ret = -ENXIO;
		if (!is_pipe && (filp->f_flags & O_NONBLOCK) && !pipe->readers)
			goto err;

		pipe->w_counter++;
		if (!pipe->writers++)
			wake_up_partner(pipe);

		if (!is_pipe && !pipe->readers) {
			if (wait_for_partner(pipe, &pipe->r_counter))
				goto err_wr;
		}
		break;

	case FMODE_READ | FMODE_WRITE:
	/*
	 *  O_RDWR
	 *  POSIX.1 leaves this case "undefined" when O_NONBLOCK is set.
	 *  This implementation will NEVER block on a O_RDWR open, since
	 *  the process can at least talk to itself.
	 */

		pipe->readers++;
		pipe->writers++;
		pipe->r_counter++;
		pipe->w_counter++;
		if (pipe->readers == 1 || pipe->writers == 1)
			wake_up_partner(pipe);
		break;

	default:
		ret = -EINVAL;
		goto err;
	}

	/* Ok! */
	__pipe_unlock(pipe);
	return 0;

err_rd:
	if (!--pipe->readers)
		wake_up_interruptible(&pipe->wr_wait);
	ret = -ERESTARTSYS;
	goto err;

err_wr:
	if (!--pipe->writers)
		wake_up_interruptible_all(&pipe->rd_wait);
	ret = -ERESTARTSYS;
	goto err;

err:
	__pipe_unlock(pipe);

	put_pipe_info(inode, pipe);
	return ret;
}

const struct file_operations pipefifo_fops = {
	.open		= fifo_open,
	.llseek		= no_llseek,
	.read_iter	= pipe_read,
	.write_iter	= pipe_write,
	.poll		= pipe_poll,
	.unlocked_ioctl	= pipe_ioctl,
	.release	= pipe_release,
	.fasync		= pipe_fasync,
};

/*
 * Currently we rely on the pipe array holding a power-of-2 number
 * of pages. Returns 0 on error.
 */
unsigned int round_pipe_size(unsigned long size)
{
	if (size > (1U << 31))
		return 0;

	/* Minimum pipe size, as required by POSIX */
	if (size < PAGE_SIZE)
		return PAGE_SIZE;

	return roundup_pow_of_two(size);
}

/*
 * Resize the pipe ring to a number of slots.
 */
int pipe_resize_ring(struct pipe_inode_info *pipe, unsigned int nr_slots)
{
	struct pipe_buffer *bufs;
	unsigned int head, tail, mask, n;

	/*
	 * We can shrink the pipe, if arg is greater than the ring occupancy.
	 * Since we don't expect a lot of shrink+grow operations, just free and
	 * allocate again like we would do for growing.  If the pipe currently
	 * contains more buffers than arg, then return busy.
	 */
	mask = pipe->ring_size - 1;
	head = pipe->head;
	tail = pipe->tail;
	n = pipe_occupancy(pipe->head, pipe->tail);
	if (nr_slots < n)
		return -EBUSY;

	bufs = kcalloc(nr_slots, sizeof(*bufs),
		       GFP_KERNEL_ACCOUNT | __GFP_NOWARN);
	if (unlikely(!bufs))
		return -ENOMEM;

	/*
	 * The pipe array wraps around, so just start the new one at zero
	 * and adjust the indices.
	 */
	if (n > 0) {
		unsigned int h = head & mask;
		unsigned int t = tail & mask;
		if (h > t) {
			memcpy(bufs, pipe->bufs + t,
			       n * sizeof(struct pipe_buffer));
		} else {
			unsigned int tsize = pipe->ring_size - t;
			if (h > 0)
				memcpy(bufs + tsize, pipe->bufs,
				       h * sizeof(struct pipe_buffer));
			memcpy(bufs, pipe->bufs + t,
			       tsize * sizeof(struct pipe_buffer));
		}
	}

	head = n;
	tail = 0;

	kfree(pipe->bufs);
	pipe->bufs = bufs;
	pipe->ring_size = nr_slots;
	if (pipe->max_usage > nr_slots)
		pipe->max_usage = nr_slots;
	pipe->tail = tail;
	pipe->head = head;

	/* This might have made more room for writers */
	wake_up_interruptible(&pipe->wr_wait);
	return 0;
}

/*
 * Allocate a new array of pipe buffers and copy the info over. Returns the
 * pipe size if successful, or return -ERROR on error.
 */
static long pipe_set_size(struct pipe_inode_info *pipe, unsigned long arg)
{
	unsigned long user_bufs;
	unsigned int nr_slots, size;
	long ret = 0;

#ifdef CONFIG_WATCH_QUEUE
	if (pipe->watch_queue)
		return -EBUSY;
#endif

	size = round_pipe_size(arg);
	nr_slots = size >> PAGE_SHIFT;

	if (!nr_slots)
		return -EINVAL;

	/*
	 * If trying to increase the pipe capacity, check that an
	 * unprivileged user is not trying to exceed various limits
	 * (soft limit check here, hard limit check just below).
	 * Decreasing the pipe capacity is always permitted, even
	 * if the user is currently over a limit.
	 */
	if (nr_slots > pipe->max_usage &&
			size > pipe_max_size && !capable(CAP_SYS_RESOURCE))
		return -EPERM;

	user_bufs = account_pipe_buffers(pipe->user, pipe->nr_accounted, nr_slots);

	if (nr_slots > pipe->max_usage &&
			(too_many_pipe_buffers_hard(user_bufs) ||
			 too_many_pipe_buffers_soft(user_bufs)) &&
			pipe_is_unprivileged_user()) {
		ret = -EPERM;
		goto out_revert_acct;
	}

	ret = pipe_resize_ring(pipe, nr_slots);
	if (ret < 0)
		goto out_revert_acct;

	pipe->max_usage = nr_slots;
	pipe->nr_accounted = nr_slots;
	return pipe->max_usage * PAGE_SIZE;

out_revert_acct:
	(void) account_pipe_buffers(pipe->user, nr_slots, pipe->nr_accounted);
	return ret;
}

/*
 * After the inode slimming patch, i_pipe/i_bdev/i_cdev share the same
 * location, so checking ->i_pipe is not enough to verify that this is a
 * pipe.
 */
struct pipe_inode_info *get_pipe_info(struct file *file, bool for_splice)
{
	struct pipe_inode_info *pipe = file->private_data;

	if (file->f_op != &pipefifo_fops || !pipe)
		return NULL;
#ifdef CONFIG_WATCH_QUEUE
	if (for_splice && pipe->watch_queue)
		return NULL;
#endif
	return pipe;
}

long pipe_fcntl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pipe_inode_info *pipe;
	long ret;

	pipe = get_pipe_info(file, false);
	if (!pipe)
		return -EBADF;

	__pipe_lock(pipe);

	switch (cmd) {
	case F_SETPIPE_SZ:
		ret = pipe_set_size(pipe, arg);
		break;
	case F_GETPIPE_SZ:
		ret = pipe->max_usage * PAGE_SIZE;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	__pipe_unlock(pipe);
	return ret;
}

static const struct super_operations pipefs_ops = {
	.destroy_inode = free_inode_nonrcu,
	.statfs = simple_statfs,
};

/*
 * pipefs should _never_ be mounted by userland - too much of security hassle,
 * no real gain from having the whole whorehouse mounted. So we don't need
 * any operations on the root directory. However, we need a non-trivial
 * d_name - pipe: will go nicely and kill the special-casing in procfs.
 */

static int pipefs_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, PIPEFS_MAGIC);
	if (!ctx)
		return -ENOMEM;
	ctx->ops = &pipefs_ops;
	ctx->dops = &pipefs_dentry_operations;
	return 0;
}

static struct file_system_type pipe_fs_type = {
	.name		= "pipefs",
	.init_fs_context = pipefs_init_fs_context,
	.kill_sb	= kill_anon_super,
};

static int __init init_pipe_fs(void)
{
	int err = register_filesystem(&pipe_fs_type);

	if (!err) {
		pipe_mnt = kern_mount(&pipe_fs_type);
		if (IS_ERR(pipe_mnt)) {
			err = PTR_ERR(pipe_mnt);
			unregister_filesystem(&pipe_fs_type);
		}
	}
	return err;
}

fs_initcall(init_pipe_fs);
