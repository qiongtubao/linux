// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/read_write.c
 *
 *  VFS读写系统调用实现
 *  提供read/write/pread/pwrite/readv/writev等系统调用的VFS层实现
 *  支持同步/异步I/O、向量I/O、splice操作等
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/sched/xacct.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/fsnotify.h>
#include <linux/security.h>
#include <linux/export.h>
#include <linux/syscalls.h>
#include <linux/pagemap.h>
#include <linux/splice.h>
#include <linux/compat.h>
#include <linux/mount.h>
#include <linux/fs.h>
#include "internal.h"

#include <linux/uaccess.h>
#include <asm/unistd.h>

/* 通用只读文件操作结构 - 用于只读文件系统和设备 */
const struct file_operations generic_ro_fops = {
	.llseek		= generic_file_llseek,     /* 文件定位 */
	.read_iter	= generic_file_read_iter,  /* 迭代读取 */
	.mmap		= generic_file_readonly_mmap, /* 只读内存映射 */
	.splice_read	= generic_file_splice_read,   /* splice读取 */
};

EXPORT_SYMBOL(generic_ro_fops);

/* 检查文件是否使用无符号偏移量 */
static inline bool unsigned_offsets(struct file *file)
{
	return file->f_mode & FMODE_UNSIGNED_OFFSET; /* 返回是否支持无符号偏移 */
}

/**
 * vfs_setpos - 更新文件seek偏移量
 * @file:	要操作的文件结构
 * @offset:	要设置的文件偏移量
 * @maxsize:	文件的最大大小限制
 *
 * 这是一个底层文件系统辅助函数，用于将文件偏移量更新为指定的@offset值，
 * 前提是给定的偏移量有效且不等于当前文件偏移量。
 *
 * 成功时返回指定的偏移量，偏移量无效时返回-EINVAL。
 */
loff_t vfs_setpos(struct file *file, loff_t offset, loff_t maxsize)
{
	if (offset < 0 && !unsigned_offsets(file)) /* 检查负偏移量 */
		return -EINVAL;
	if (offset > maxsize) /* 检查偏移量是否超过最大值 */
		return -EINVAL;

	if (offset != file->f_pos) { /* 只在偏移量发生变化时更新 */
		file->f_pos = offset; /* 设置新的文件位置 */
		file->f_version = 0; /* 重置版本号 */
	}
	return offset; /* 返回设置的偏移量 */
}
EXPORT_SYMBOL(vfs_setpos);

/**
 * generic_file_llseek_size - 通用文件定位实现
 * @file:	要定位的文件
 * @offset:	偏移量
 * @whence:	定位方式(SEEK_SET/SEEK_CUR/SEEK_END等)
 * @size:	文件大小
 * @eof:	文件结束位置
 *
 * 这是大多数文件系统使用的标准lseek实现。
 * 支持SEEK_SET、SEEK_CUR、SEEK_END、SEEK_DATA、SEEK_HOLE等定位方式。
 *
 * 返回新的文件位置，出错时返回负的错误码。
 */
loff_t
generic_file_llseek_size(struct file *file, loff_t offset, int whence,
		loff_t maxsize, loff_t eof)
{
	switch (whence) {
	case SEEK_END:
		offset += eof;
		break;
	case SEEK_CUR:
		/*
		 * Here we special-case the lseek(fd, 0, SEEK_CUR)
		 * position-querying operation.  Avoid rewriting the "same"
		 * f_pos value back to the file because a concurrent read(),
		 * write() or lseek() might have altered it
		 */
		if (offset == 0)
			return file->f_pos;
		/*
		 * f_lock protects against read/modify/write race with other
		 * SEEK_CURs. Note that parallel writes and reads behave
		 * like SEEK_SET.
		 */
		spin_lock(&file->f_lock);
		offset = vfs_setpos(file, file->f_pos + offset, maxsize);
		spin_unlock(&file->f_lock);
		return offset;
	case SEEK_DATA:
		/*
		 * In the generic case the entire file is data, so as long as
		 * offset isn't at the end of the file then the offset is data.
		 */
		if ((unsigned long long)offset >= eof)
			return -ENXIO;
		break;
	case SEEK_HOLE:
		/*
		 * There is a virtual hole at the end of the file, so as long as
		 * offset isn't i_size or larger, return i_size.
		 */
		if ((unsigned long long)offset >= eof)
			return -ENXIO;
		offset = eof;
		break;
	}

	return vfs_setpos(file, offset, maxsize);
}
EXPORT_SYMBOL(generic_file_llseek_size);

/**
 * generic_file_llseek - 常规文件的通用llseek实现
 * @file: 要定位的文件结构
 * @offset: 要定位到的文件偏移量
 * @whence: 定位类型
 *
 * 这是对所有普通本地文件系统都可用的->llseek的通用实现。
 * 它只是将文件偏移量更新为@offset和@whence指定的值。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
/**
 * generic_file_llseek - generic llseek implementation for regular files
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @whence:	type of seek
 *
 * This is a generic implemenation of ->llseek useable for all normal local
 * filesystems.  It just updates the file offset to the value specified by
 * @offset and @whence.
 */
loff_t generic_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	return generic_file_llseek_size(file, offset, whence,
					inode->i_sb->s_maxbytes,
					i_size_read(inode));
}
EXPORT_SYMBOL(generic_file_llseek);

/**
 * fixed_size_llseek - 固定大小设备的llseek实现
 * @file: 要定位的文件结构
 * @offset: 要定位到的文件偏移量
 * @whence: 定位类型
 * @size: 文件的大小
 *
 * 用于已知固定大小设备的llseek实现。只支持基本的SEEK_SET、
 * SEEK_CUR和SEEK_END操作，不支持SEEK_DATA和SEEK_HOLE。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
/**
 * fixed_size_llseek - llseek implementation for fixed-sized devices
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @whence:	type of seek
 * @size:	size of the file
 *
 */
loff_t fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size)
{
	switch (whence) {
	case SEEK_SET: case SEEK_CUR: case SEEK_END:
		return generic_file_llseek_size(file, offset, whence,
						size, size);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(fixed_size_llseek);

/**
 * no_seek_end_llseek - 不支持SEEK_END的llseek实现
 * @file: 要定位的文件结构
 * @offset: 要定位到的文件偏移量
 * @whence: 定位类型
 *
 * 用于不支持SEEK_END操作的设备的llseek实现。只支持SEEK_SET
 * 和SEEK_CUR操作，对于其他定位类型返回-EINVAL。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
/**
 * no_seek_end_llseek - llseek implementation for fixed-sized devices
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @whence:	type of seek
 *
 */
loff_t no_seek_end_llseek(struct file *file, loff_t offset, int whence)
{
	switch (whence) {
	case SEEK_SET: case SEEK_CUR:
		return generic_file_llseek_size(file, offset, whence,
						OFFSET_MAX, 0);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(no_seek_end_llseek);

/**
 * no_seek_end_llseek_size - 带大小限制且不支持SEEK_END的llseek实现
 * @file: 要定位的文件结构
 * @offset: 要定位到的文件偏移量
 * @whence: 定位类型
 * @size: 允许的最大偏移量
 *
 * 类似no_seek_end_llseek，但允许指定最大偏移量限制。
 * 只支持SEEK_SET和SEEK_CUR操作。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
/**
 * no_seek_end_llseek_size - llseek implementation for fixed-sized devices
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @whence:	type of seek
 * @size:	maximal offset allowed
 *
 */
loff_t no_seek_end_llseek_size(struct file *file, loff_t offset, int whence, loff_t size)
{
	switch (whence) {
	case SEEK_SET: case SEEK_CUR:
		return generic_file_llseek_size(file, offset, whence,
						size, 0);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(no_seek_end_llseek_size);

/**
 * noop_llseek - 无操作的llseek实现
 * @file: 要定位的文件结构
 * @offset: 要定位到的文件偏移量
 * @whence: 定位类型
 *
 * 这是一个用于特殊情况的->llseek实现，当用户空间期望seek操作成功
 * 但（设备）文件实际上无法执行seek时使用。在这种情况下使用noop_llseek()
 * 而不是回退到默认的->llseek实现。
 *
 * 返回值: 总是返回当前文件位置
 */
/**
 * noop_llseek - No Operation Performed llseek implementation
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @whence:	type of seek
 *
 * This is an implementation of ->llseek useable for the rare special case when
 * userspace expects the seek to succeed but the (device) file is actually not
 * able to perform the seek. In this case you use noop_llseek() instead of
 * falling back to the default implementation of ->llseek.
 */
loff_t noop_llseek(struct file *file, loff_t offset, int whence)
{
	return file->f_pos;
}
EXPORT_SYMBOL(noop_llseek);

/**
 * no_llseek - 禁止llseek操作
 * @file: 文件结构
 * @offset: 偏移量
 * @whence: 定位类型
 *
 * 用于不支持seek操作的文件或设备，如管道、套接字等。
 * 总是返回-ESPIPE错误，表示这是一个管道或类似的设备。
 *
 * 返回值: 总是返回-ESPIPE
 */
loff_t no_llseek(struct file *file, loff_t offset, int whence)
{
	return -ESPIPE;
}
EXPORT_SYMBOL(no_llseek);

/**
 * default_llseek - 默认的llseek实现
 * @file: 文件结构
 * @offset: 偏移量
 * @whence: 定位类型
 *
 * 这是默认的llseek实现，使用inode锁来保护文件位置的更新。
 * 支持所有标准的seek操作包括SEEK_DATA和SEEK_HOLE。
 * 比generic_file_llseek更安全但性能稍低。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
loff_t default_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);
	loff_t retval;

	inode_lock(inode);
	switch (whence) {
		case SEEK_END:
			offset += i_size_read(inode);
			break;
		case SEEK_CUR:
			if (offset == 0) {
				retval = file->f_pos;
				goto out;
			}
			offset += file->f_pos;
			break;
		case SEEK_DATA:
			/*
			 * In the generic case the entire file is data, so as
			 * long as offset isn't at the end of the file then the
			 * offset is data.
			 */
			if (offset >= inode->i_size) {
				retval = -ENXIO;
				goto out;
			}
			break;
		case SEEK_HOLE:
			/*
			 * There is a virtual hole at the end of the file, so
			 * as long as offset isn't i_size or larger, return
			 * i_size.
			 */
			if (offset >= inode->i_size) {
				retval = -ENXIO;
				goto out;
			}
			offset = inode->i_size;
			break;
	}
	retval = -EINVAL;
	if (offset >= 0 || unsigned_offsets(file)) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
	}
out:
	inode_unlock(inode);
	return retval;
}
EXPORT_SYMBOL(default_llseek);

/**
 * vfs_llseek - VFS层的llseek操作
 * @file: 文件结构
 * @offset: 偏移量
 * @whence: 定位类型
 *
 * VFS层的文件定位函数。根据文件的模式标志决定使用哪个llseek实现：
 * 如果文件支持LSEEK且定义了f_op->llseek则使用它，否则使用no_llseek。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
loff_t vfs_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t (*fn)(struct file *, loff_t, int);

	fn = no_llseek;
	if (file->f_mode & FMODE_LSEEK) {
		if (file->f_op->llseek)
			fn = file->f_op->llseek;
	}
	return fn(file, offset, whence);
}
EXPORT_SYMBOL(vfs_llseek);

/**
 * ksys_lseek - 内核lseek系统调用实现
 * @fd: 文件描述符
 * @offset: 偏移量
 * @whence: 定位类型
 *
 * 内核级别的lseek实现。获取文件描述符对应的文件结构，
 * 验证参数有效性，然后调用vfs_llseek执行实际的定位操作。
 *
 * 返回值: 成功返回新的文件位置，失败返回负错误码
 */
static off_t ksys_lseek(unsigned int fd, off_t offset, unsigned int whence)
{
	off_t retval;
	struct fd f = fdget_pos(fd);
	if (!f.file)
		return -EBADF;

	retval = -EINVAL;
	if (whence <= SEEK_MAX) {
		loff_t res = vfs_llseek(f.file, offset, whence);
		retval = res;
		if (res != (loff_t)retval)
			retval = -EOVERFLOW;	/* LFS: should only happen on 32 bit platforms */
	}
	fdput_pos(f);
	return retval;
}

/* lseek系统调用入口 - 修改文件读写位置 */
SYSCALL_DEFINE3(lseek, unsigned int, fd, off_t, offset, unsigned int, whence)
{
	return ksys_lseek(fd, offset, whence); /* 调用内核lseek实现 */
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE3(lseek, unsigned int, fd, compat_off_t, offset, unsigned int, whence)
{
	return ksys_lseek(fd, offset, whence);
}
#endif

#if !defined(CONFIG_64BIT) || defined(CONFIG_COMPAT) || \
	defined(__ARCH_WANT_SYS_LLSEEK)
SYSCALL_DEFINE5(llseek, unsigned int, fd, unsigned long, offset_high,
		unsigned long, offset_low, loff_t __user *, result,
		unsigned int, whence)
{
	int retval;
	struct fd f = fdget_pos(fd);
	loff_t offset;

	if (!f.file)
		return -EBADF;

	retval = -EINVAL;
	if (whence > SEEK_MAX)
		goto out_putf;

	offset = vfs_llseek(f.file, ((loff_t) offset_high << 32) | offset_low,
			whence);

	retval = (int)offset;
	if (offset >= 0) {
		retval = -EFAULT;
		if (!copy_to_user(result, &offset, sizeof(offset)))
			retval = 0;
	}
out_putf:
	fdput_pos(f);
	return retval;
}
#endif

/**
 * rw_verify_area - 验证读写操作的区域
 * @read_write: 读写操作类型(READ或WRITE)
 * @file: 文件结构
 * @ppos: 文件位置指针
 * @count: 要读写的字节数
 *
 * 在执行读写操作前验证参数的有效性和权限。检查文件锁、
 * 偏移量溢出、强制锁定区域等。这是所有读写操作的必要前置检查。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
int rw_verify_area(int read_write, struct file *file, const loff_t *ppos, size_t count)
{
	struct inode *inode;
	int retval = -EINVAL;

	inode = file_inode(file);
	if (unlikely((ssize_t) count < 0))
		return retval;

	/*
	 * ranged mandatory locking does not apply to streams - it makes sense
	 * only for files where position has a meaning.
	 */
	if (ppos) {
		loff_t pos = *ppos;

		if (unlikely(pos < 0)) {
			if (!unsigned_offsets(file))
				return retval;
			if (count >= -pos) /* both values are in 0..LLONG_MAX */
				return -EOVERFLOW;
		} else if (unlikely((loff_t) (pos + count) < 0)) {
			if (!unsigned_offsets(file))
				return retval;
		}

		if (unlikely(inode->i_flctx && mandatory_lock(inode))) {
			retval = locks_mandatory_area(inode, file, pos, pos + count - 1,
					read_write == READ ? F_RDLCK : F_WRLCK);
			if (retval < 0)
				return retval;
		}
	}

	return security_file_permission(file,
				read_write == READ ? MAY_READ : MAY_WRITE);
}

/**
 * new_sync_read - 新式同步读取实现
 * @filp: 文件指针
 * @buf: 用户缓冲区
 * @len: 读取长度
 * @ppos: 文件位置指针
 *
 * 通过read_iter接口实现的同步读取。将传统的read接口转换为
 * 现代的iter接口调用。用于文件操作结构中只定义了read_iter
 * 而没有定义read的情况。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
static ssize_t new_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = (ppos ? *ppos : 0);
	iov_iter_init(&iter, READ, &iov, 1, len);

	ret = call_read_iter(filp, &kiocb, &iter);
	BUG_ON(ret == -EIOCBQUEUED);
	if (ppos)
		*ppos = kiocb.ki_pos;
	return ret;
}

/**
 * warn_unsupported - 警告不支持的操作
 * @file: 文件指针
 * @op: 操作名称
 *
 * 当文件系统不支持某种操作时打印限速警告信息。
 * 帮助开发者和用户识别文件系统的限制。
 *
 * 返回值: 总是返回-EINVAL
 */
static int warn_unsupported(struct file *file, const char *op)
{
	pr_warn_ratelimited(
		"kernel %s not supported for file %pD4 (pid: %d comm: %.20s)\n",
		op, file, current->pid, current->comm);
	return -EINVAL;
}

/**
 * __kernel_read - 内核内部读取函数
 * @file: 文件指针
 * @buf: 内核缓冲区
 * @count: 要读取的字节数
 * @pos: 文件位置指针
 *
 * 内核内部使用的读取函数，调用者负责file_start_write/file_end_write。
 * 直接使用内核虚拟地址，无需用户空间地址检查。主要用于内核模块
 * 和内核内部的文件I/O操作。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
ssize_t __kernel_read(struct file *file, void *buf, size_t count, loff_t *pos)
{
	struct kvec iov = {
		.iov_base	= buf,
		.iov_len	= min_t(size_t, count, MAX_RW_COUNT),
	};
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	if (WARN_ON_ONCE(!(file->f_mode & FMODE_READ)))
		return -EINVAL;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;
	/*
	 * Also fail if ->read_iter and ->read are both wired up as that
	 * implies very convoluted semantics.
	 */
	if (unlikely(!file->f_op->read_iter || file->f_op->read))
		return warn_unsupported(file, "read");

	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = pos ? *pos : 0;
	iov_iter_kvec(&iter, READ, &iov, 1, iov.iov_len);
	ret = file->f_op->read_iter(&kiocb, &iter);
	if (ret > 0) {
		if (pos)
			*pos = kiocb.ki_pos;
		fsnotify_access(file);
		add_rchar(current, ret);
	}
	inc_syscr(current);
	return ret;
}

/**
 * kernel_read - 内核读取函数
 * @file: 文件指针
 * @buf: 内核缓冲区
 * @count: 要读取的字节数
 * @pos: 文件位置指针
 *
 * 提供给内核模块使用的标准读取接口。包含完整的权限检查
 * 和区域验证，然后调用__kernel_read执行实际读取。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
ssize_t kernel_read(struct file *file, void *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	ret = rw_verify_area(READ, file, pos, count);
	if (ret)
		return ret;
	return __kernel_read(file, buf, count, pos);
}
EXPORT_SYMBOL(kernel_read);

/**
 * vfs_read - VFS层读取函数
 * @file: 文件指针
 * @buf: 用户空间缓冲区
 * @count: 要读取的字节数
 * @pos: 文件位置指针
 *
 * VFS层的核心读取函数。执行完整的权限检查、地址验证和
 * 区域检查，然后根据文件操作结构选择合适的读取方法。
 * 这是所有用户空间读取操作的统一入口点。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;
	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;

	ret = rw_verify_area(READ, file, pos, count);
	if (ret)
		return ret;
	if (count > MAX_RW_COUNT)
		count =  MAX_RW_COUNT;

	if (file->f_op->read)
		ret = file->f_op->read(file, buf, count, pos);
	else if (file->f_op->read_iter)
		ret = new_sync_read(file, buf, count, pos);
	else
		ret = -EINVAL;
	if (ret > 0) {
		fsnotify_access(file);
		add_rchar(current, ret);
	}
	inc_syscr(current);
	return ret;
}

/**
 * new_sync_write - 新式同步写入实现
 * @filp: 文件指针
 * @buf: 用户缓冲区
 * @len: 写入长度
 * @ppos: 文件位置指针
 *
 * 通过write_iter接口实现的同步写入。将传统的write接口转换为
 * 现代的iter接口调用。用于文件操作结构中只定义了write_iter
 * 而没有定义write的情况。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
static ssize_t new_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = (void __user *)buf, .iov_len = len };
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = (ppos ? *ppos : 0);
	iov_iter_init(&iter, WRITE, &iov, 1, len);

	ret = call_write_iter(filp, &kiocb, &iter);
	BUG_ON(ret == -EIOCBQUEUED);
	if (ret > 0 && ppos)
		*ppos = kiocb.ki_pos;
	return ret;
}

/**
 * __kernel_write - 内核内部写入函数
 * @file: 文件指针
 * @buf: 内核缓冲区
 * @count: 要写入的字节数
 * @pos: 文件位置指针
 *
 * 内核内部使用的写入函数，调用者负责file_start_write/file_end_write。
 * 直接使用内核虚拟地址，无需用户空间地址检查。主要用于内核模块
 * 和内核内部的文件I/O操作。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
/* caller is responsible for file_start_write/file_end_write */
ssize_t __kernel_write(struct file *file, const void *buf, size_t count, loff_t *pos)
{
	struct kvec iov = {
		.iov_base	= (void *)buf,
		.iov_len	= min_t(size_t, count, MAX_RW_COUNT),
	};
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	if (WARN_ON_ONCE(!(file->f_mode & FMODE_WRITE)))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_WRITE))
		return -EINVAL;
	/*
	 * Also fail if ->write_iter and ->write are both wired up as that
	 * implies very convoluted semantics.
	 */
	if (unlikely(!file->f_op->write_iter || file->f_op->write))
		return warn_unsupported(file, "write");

	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = pos ? *pos : 0;
	iov_iter_kvec(&iter, WRITE, &iov, 1, iov.iov_len);
	ret = file->f_op->write_iter(&kiocb, &iter);
	if (ret > 0) {
		if (pos)
			*pos = kiocb.ki_pos;
		fsnotify_modify(file);
		add_wchar(current, ret);
	}
	inc_syscw(current);
	return ret;
}
/*
 * This "EXPORT_SYMBOL_GPL()" is more of a "EXPORT_SYMBOL_DONTUSE()",
 * but autofs is one of the few internal kernel users that actually
 * wants this _and_ can be built as a module. So we need to export
 * this symbol for autofs, even though it really isn't appropriate
 * for any other kernel modules.
 */
EXPORT_SYMBOL_GPL(__kernel_write);

/**
 * kernel_write - 内核写入函数
 * @file: 文件指针
 * @buf: 内核缓冲区
 * @count: 要写入的字节数
 * @pos: 文件位置指针
 *
 * 提供给内核模块使用的标准写入接口。包含完整的权限检查、
 * 区域验证和写入保护机制，然后调用__kernel_write执行实际写入。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
ssize_t kernel_write(struct file *file, const void *buf, size_t count,
			    loff_t *pos)
{
	ssize_t ret;

	ret = rw_verify_area(WRITE, file, pos, count);
	if (ret)
		return ret;

	file_start_write(file);
	ret =  __kernel_write(file, buf, count, pos);
	file_end_write(file);
	return ret;
}
EXPORT_SYMBOL(kernel_write);

/**
 * vfs_write - VFS层写入函数
 * @file: 文件指针
 * @buf: 用户空间缓冲区
 * @count: 要写入的字节数
 * @pos: 文件位置指针
 *
 * VFS层的核心写入函数。执行完整的权限检查、地址验证和
 * 区域检查，然后根据文件操作结构选择合适的写入方法。
 * 包含写入保护机制以防止并发写入冲突。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_WRITE))
		return -EINVAL;
	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;

	ret = rw_verify_area(WRITE, file, pos, count);
	if (ret)
		return ret;
	if (count > MAX_RW_COUNT)
		count =  MAX_RW_COUNT;
	file_start_write(file);
	if (file->f_op->write)
		ret = file->f_op->write(file, buf, count, pos);
	else if (file->f_op->write_iter)
		ret = new_sync_write(file, buf, count, pos);
	else
		ret = -EINVAL;
	if (ret > 0) {
		fsnotify_modify(file);
		add_wchar(current, ret);
	}
	inc_syscw(current);
	file_end_write(file);
	return ret;
}

/**
 * file_ppos - 获取文件位置指针
 * @file: 文件指针
 *
 * 返回文件位置指针。如果文件是流设备则返回NULL，
 * 否则返回&file->f_pos。流设备不维护文件位置概念。
 *
 * 返回值: 文件位置指针或NULL
 */
/* file_ppos returns &file->f_pos or NULL if file is stream */
static inline loff_t *file_ppos(struct file *file)
{
	return file->f_mode & FMODE_STREAM ? NULL : &file->f_pos;
}

/*
 * ksys_read - 内核read系统调用实现
 * @fd: 文件描述符
 * @buf: 用户空间缓冲区指针
 * @count: 要读取的字节数
 *
 * 从指定文件描述符读取数据到用户空间缓冲区。
 * 这是read()系统调用的内核实现函数。
 *
 * 返回实际读取的字节数，失败时返回负的错误码。
 */
ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
	struct fd f = fdget_pos(fd);
	ssize_t ret = -EBADF;

	if (f.file) {
		loff_t pos, *ppos = file_ppos(f.file);
		if (ppos) {
			pos = *ppos;
			ppos = &pos;
		}
		ret = vfs_read(f.file, buf, count, ppos);
		if (ret >= 0 && ppos)
			f.file->f_pos = pos;
		fdput_pos(f);
	}
	return ret;
}

/* read系统调用入口 - 从文件描述符读取数据 */
SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
	return ksys_read(fd, buf, count); /* 调用内核read实现 */
}

/*
 * ksys_write - 内核write系统调用实现
 * @fd: 文件描述符
 * @buf: 用户空间缓冲区指针
 * @count: 要写入的字节数
 *
 * 从用户空间缓冲区写入数据到指定文件描述符。
 * 这是write()系统调用的内核实现函数。
 *
 * 返回实际写入的字节数，失败时返回负的错误码。
 */
ssize_t ksys_write(unsigned int fd, const char __user *buf, size_t count)
{
	struct fd f = fdget_pos(fd);
	ssize_t ret = -EBADF;

	if (f.file) {
		loff_t pos, *ppos = file_ppos(f.file);
		if (ppos) {
			pos = *ppos;
			ppos = &pos;
		}
		ret = vfs_write(f.file, buf, count, ppos);
		if (ret >= 0 && ppos)
			f.file->f_pos = pos;
		fdput_pos(f);
	}

	return ret;
}

/* write系统调用入口 - 向文件描述符写入数据 */
SYSCALL_DEFINE3(write, unsigned int, fd, const char __user *, buf,
		size_t, count)
{
	return ksys_write(fd, buf, count); /* 调用内核write实现 */
}

/**
 * ksys_pread64 - 内核pread64系统调用实现
 * @fd: 文件描述符
 * @buf: 用户空间缓冲区
 * @count: 要读取的字节数
 * @pos: 读取的文件位置
 *
 * 从指定位置读取数据，不改变文件的当前位置。这是pread64()
 * 系统调用的内核实现，支持64位文件偏移量。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
ssize_t ksys_pread64(unsigned int fd, char __user *buf, size_t count,
		     loff_t pos)
{
	struct fd f;
	ssize_t ret = -EBADF;

	if (pos < 0)
		return -EINVAL;

	f = fdget(fd);
	if (f.file) {
		ret = -ESPIPE;
		if (f.file->f_mode & FMODE_PREAD)
			ret = vfs_read(f.file, buf, count, &pos);
		fdput(f);
	}

	return ret;
}

/* pread64系统调用入口 - 从指定偏移位置读取数据（不改变文件位置） */
SYSCALL_DEFINE4(pread64, unsigned int, fd, char __user *, buf,
			size_t, count, loff_t, pos)
{
	return ksys_pread64(fd, buf, count, pos); /* 调用内核pread实现 */
}

/**
 * ksys_pwrite64 - 内核pwrite64系统调用实现
 * @fd: 文件描述符
 * @buf: 用户空间缓冲区
 * @count: 要写入的字节数
 * @pos: 写入的文件位置
 *
 * 向指定位置写入数据，不改变文件的当前位置。这是pwrite64()
 * 系统调用的内核实现，支持64位文件偏移量。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
ssize_t ksys_pwrite64(unsigned int fd, const char __user *buf,
		      size_t count, loff_t pos)
{
	struct fd f;
	ssize_t ret = -EBADF;

	if (pos < 0)
		return -EINVAL;

	f = fdget(fd);
	if (f.file) {
		ret = -ESPIPE;
		if (f.file->f_mode & FMODE_PWRITE)  
			ret = vfs_write(f.file, buf, count, &pos);
		fdput(f);
	}

	return ret;
}

/* pwrite64系统调用入口 - 向指定偏移位置写入数据（不改变文件位置） */
SYSCALL_DEFINE4(pwrite64, unsigned int, fd, const char __user *, buf,
			 size_t, count, loff_t, pos)
{
	return ksys_pwrite64(fd, buf, count, pos); /* 调用内核pwrite实现 */
}

/**
 * do_iter_readv_writev - 执行迭代器读写操作
 * @filp: 文件指针
 * @iter: I/O迭代器
 * @ppos: 文件位置指针
 * @type: 操作类型(READ或WRITE)
 * @flags: 读写标志
 *
 * 使用现代的迭代器接口执行向量I/O操作。支持各种读写标志
 * 如RWF_HIPRI、RWF_NOWAIT等。这是高性能I/O的首选方法。
 *
 * 返回值: 实际读写的字节数，失败返回负错误码
 */
static ssize_t do_iter_readv_writev(struct file *filp, struct iov_iter *iter,
		loff_t *ppos, int type, rwf_t flags)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	ret = kiocb_set_rw_flags(&kiocb, flags);
	if (ret)
		return ret;
	kiocb.ki_pos = (ppos ? *ppos : 0);

	if (type == READ)
		ret = call_read_iter(filp, &kiocb, iter);
	else
		ret = call_write_iter(filp, &kiocb, iter);
	BUG_ON(ret == -EIOCBQUEUED);
	if (ppos)
		*ppos = kiocb.ki_pos;
	return ret;
}

/**
 * do_loop_readv_writev - 循环执行向量读写操作
 * @filp: 文件指针
 * @iter: I/O迭代器
 * @ppos: 文件位置指针
 * @type: 操作类型(READ或WRITE)
 * @flags: 读写标志
 *
 * 手动循环处理向量I/O，适用于只支持传统read/write接口的文件系统。
 * 逐个处理iovec结构中的每个缓冲区，直到全部处理完成或遇到错误。
 *
 * 返回值: 实际读写的字节数，失败返回负错误码
 */
/* Do it by hand, with file-ops */
static ssize_t do_loop_readv_writev(struct file *filp, struct iov_iter *iter,
		loff_t *ppos, int type, rwf_t flags)
{
	ssize_t ret = 0;

	if (flags & ~RWF_HIPRI)
		return -EOPNOTSUPP;

	while (iov_iter_count(iter)) {
		struct iovec iovec = iov_iter_iovec(iter);
		ssize_t nr;

		if (type == READ) {
			nr = filp->f_op->read(filp, iovec.iov_base,
					      iovec.iov_len, ppos);
		} else {
			nr = filp->f_op->write(filp, iovec.iov_base,
					       iovec.iov_len, ppos);
		}

		if (nr < 0) {
			if (!ret)
				ret = nr;
			break;
		}
		ret += nr;
		if (nr != iovec.iov_len)
			break;
		iov_iter_advance(iter, nr);
	}

	return ret;
}

/**
 * do_iter_read - 执行迭代器读取操作
 * @file: 文件指针
 * @iter: I/O迭代器
 * @pos: 文件位置
 * @flags: 读取标志
 *
 * 执行基于迭代器的读取操作。根据文件是否支持read_iter接口
 * 选择使用现代迭代器方法或传统的循环读取方法。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
static ssize_t do_iter_read(struct file *file, struct iov_iter *iter,
		loff_t *pos, rwf_t flags)
{
	size_t tot_len;
	ssize_t ret = 0;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;

	tot_len = iov_iter_count(iter);
	if (!tot_len)
		goto out;
	ret = rw_verify_area(READ, file, pos, tot_len);
	if (ret < 0)
		return ret;

	if (file->f_op->read_iter)
		ret = do_iter_readv_writev(file, iter, pos, READ, flags);
	else
		ret = do_loop_readv_writev(file, iter, pos, READ, flags);
out:
	if (ret >= 0)
		fsnotify_access(file);
	return ret;
}

/**
 * vfs_iocb_iter_read - VFS层IOCB迭代器读取
 * @file: 文件指针
 * @iocb: I/O控制块
 * @iter: I/O迭代器
 *
 * 使用IOCB和迭代器进行VFS层读取操作。支持异步I/O和高级I/O特性。
 * 这是现代异步I/O框架的核心读取函数。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
ssize_t vfs_iocb_iter_read(struct file *file, struct kiocb *iocb,
			   struct iov_iter *iter)
{
	size_t tot_len;
	ssize_t ret = 0;

	if (!file->f_op->read_iter)
		return -EINVAL;
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;

	tot_len = iov_iter_count(iter);
	if (!tot_len)
		goto out;
	ret = rw_verify_area(READ, file, &iocb->ki_pos, tot_len);
	if (ret < 0)
		return ret;

	ret = call_read_iter(file, iocb, iter);
out:
	if (ret >= 0)
		fsnotify_access(file);
	return ret;
}
EXPORT_SYMBOL(vfs_iocb_iter_read);

/**
 * vfs_iter_read - VFS层迭代器读取
 * @file: 文件指针
 * @iter: I/O迭代器
 * @ppos: 文件位置指针
 * @flags: 读取标志
 *
 * VFS层的迭代器读取接口。要求文件必须支持read_iter操作。
 * 提供统一的迭代器读取入口，支持各种读取标志。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
ssize_t vfs_iter_read(struct file *file, struct iov_iter *iter, loff_t *ppos,
		rwf_t flags)
{
	if (!file->f_op->read_iter)
		return -EINVAL;
	return do_iter_read(file, iter, ppos, flags);
}
EXPORT_SYMBOL(vfs_iter_read);

/**
 * do_iter_write - 执行迭代器写入操作
 * @file: 文件指针
 * @iter: I/O迭代器
 * @pos: 文件位置
 * @flags: 写入标志
 *
 * 执行基于迭代器的写入操作。根据文件是否支持write_iter接口
 * 选择使用现代迭代器方法或传统的循环写入方法。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
static ssize_t do_iter_write(struct file *file, struct iov_iter *iter,
		loff_t *pos, rwf_t flags)
{
	size_t tot_len;
	ssize_t ret = 0;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_WRITE))
		return -EINVAL;

	tot_len = iov_iter_count(iter);
	if (!tot_len)
		return 0;
	ret = rw_verify_area(WRITE, file, pos, tot_len);
	if (ret < 0)
		return ret;

	if (file->f_op->write_iter)
		ret = do_iter_readv_writev(file, iter, pos, WRITE, flags);
	else
		ret = do_loop_readv_writev(file, iter, pos, WRITE, flags);
	if (ret > 0)
		fsnotify_modify(file);
	return ret;
}

/**
 * vfs_iocb_iter_write - VFS层IOCB迭代器写入
 * @file: 文件指针
 * @iocb: I/O控制块
 * @iter: I/O迭代器
 *
 * 使用IOCB和迭代器进行VFS层写入操作。支持异步I/O和高级I/O特性。
 * 这是现代异步I/O框架的核心写入函数。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
ssize_t vfs_iocb_iter_write(struct file *file, struct kiocb *iocb,
			    struct iov_iter *iter)
{
	size_t tot_len;
	ssize_t ret = 0;

	if (!file->f_op->write_iter)
		return -EINVAL;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_WRITE))
		return -EINVAL;

	tot_len = iov_iter_count(iter);
	if (!tot_len)
		return 0;
	ret = rw_verify_area(WRITE, file, &iocb->ki_pos, tot_len);
	if (ret < 0)
		return ret;

	ret = call_write_iter(file, iocb, iter);
	if (ret > 0)
		fsnotify_modify(file);

	return ret;
}
EXPORT_SYMBOL(vfs_iocb_iter_write);

/**
 * vfs_iter_write - VFS层迭代器写入
 * @file: 文件指针
 * @iter: I/O迭代器
 * @ppos: 文件位置指针
 * @flags: 写入标志
 *
 * VFS层的迭代器写入接口。要求文件必须支持write_iter操作。
 * 提供统一的迭代器写入入口，支持各种写入标志。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
ssize_t vfs_iter_write(struct file *file, struct iov_iter *iter, loff_t *ppos,
		rwf_t flags)
{
	if (!file->f_op->write_iter)
		return -EINVAL;
	return do_iter_write(file, iter, ppos, flags);
}
EXPORT_SYMBOL(vfs_iter_write);

/**
 * vfs_readv - VFS层向量读取
 * @file: 文件指针
 * @vec: 用户空间iovec数组
 * @vlen: iovec数组长度
 * @pos: 文件位置
 * @flags: 读取标志
 *
 * VFS层的向量读取函数。将用户空间的iovec数组导入为内核
 * 迭代器，然后执行迭代器读取操作。支持分散读取模式。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
static ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
		  unsigned long vlen, loff_t *pos, rwf_t flags)
{
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov = iovstack;
	struct iov_iter iter;
	ssize_t ret;

	ret = import_iovec(READ, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
	if (ret >= 0) {
		ret = do_iter_read(file, &iter, pos, flags);
		kfree(iov);
	}

	return ret;
}

/**
 * vfs_writev - VFS层向量写入
 * @file: 文件指针
 * @vec: 用户空间iovec数组
 * @vlen: iovec数组长度
 * @pos: 文件位置
 * @flags: 写入标志
 *
 * VFS层的向量写入函数。将用户空间的iovec数组导入为内核
 * 迭代器，然后执行迭代器写入操作。包含写入保护机制。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
static ssize_t vfs_writev(struct file *file, const struct iovec __user *vec,
		   unsigned long vlen, loff_t *pos, rwf_t flags)
{
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov = iovstack;
	struct iov_iter iter;
	ssize_t ret;

	ret = import_iovec(WRITE, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
	if (ret >= 0) {
		file_start_write(file);
		ret = do_iter_write(file, &iter, pos, flags);
		file_end_write(file);
		kfree(iov);
	}
	return ret;
}

/**
 * do_readv - 执行向量读取操作
 * @fd: 文件描述符
 * @vec: 用户空间iovec数组
 * @vlen: iovec数组长度
 * @flags: 读取标志
 *
 * 向量读取的核心实现函数。获取文件结构，处理文件位置，
 * 然后调用vfs_readv执行实际的向量读取操作。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
static ssize_t do_readv(unsigned long fd, const struct iovec __user *vec,
			unsigned long vlen, rwf_t flags)
{
	struct fd f = fdget_pos(fd);
	ssize_t ret = -EBADF;

	if (f.file) {
		loff_t pos, *ppos = file_ppos(f.file);
		if (ppos) {
			pos = *ppos;
			ppos = &pos;
		}
		ret = vfs_readv(f.file, vec, vlen, ppos, flags);
		if (ret >= 0 && ppos)
			f.file->f_pos = pos;
		fdput_pos(f);
	}

	if (ret > 0)
		add_rchar(current, ret);
	inc_syscr(current);
	return ret;
}

/**
 * do_writev - 执行向量写入操作
 * @fd: 文件描述符
 * @vec: 用户空间iovec数组
 * @vlen: iovec数组长度
 * @flags: 写入标志
 *
 * 向量写入的核心实现函数。获取文件结构，处理文件位置，
 * 然后调用vfs_writev执行实际的向量写入操作。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
static ssize_t do_writev(unsigned long fd, const struct iovec __user *vec,
			 unsigned long vlen, rwf_t flags)
{
	struct fd f = fdget_pos(fd);
	ssize_t ret = -EBADF;

	if (f.file) {
		loff_t pos, *ppos = file_ppos(f.file);
		if (ppos) {
			pos = *ppos;
			ppos = &pos;
		}
		ret = vfs_writev(f.file, vec, vlen, ppos, flags);
		if (ret >= 0 && ppos)
			f.file->f_pos = pos;
		fdput_pos(f);
	}

	if (ret > 0)
		add_wchar(current, ret);
	inc_syscw(current);
	return ret;
}

/**
 * pos_from_hilo - 从高低位构造64位偏移量
 * @high: 高32位
 * @low: 低32位
 *
 * 将32位的高位和低位值合并成一个64位的文件偏移量。
 * 主要用于32位系统上处理64位文件偏移量的系统调用。
 *
 * 返回值: 64位文件偏移量
 */
static inline loff_t pos_from_hilo(unsigned long high, unsigned long low)
{
#define HALF_LONG_BITS (BITS_PER_LONG / 2)
	return (((loff_t)high << HALF_LONG_BITS) << HALF_LONG_BITS) | low;
}

/**
 * do_preadv - 执行位置向量读取操作
 * @fd: 文件描述符
 * @vec: 用户空间iovec数组
 * @vlen: iovec数组长度
 * @pos: 读取位置
 * @flags: 读取标志
 *
 * 从指定位置执行向量读取，不改变文件当前位置。这是preadv
 * 系统调用族的核心实现函数。
 *
 * 返回值: 实际读取的字节数，失败返回负错误码
 */
static ssize_t do_preadv(unsigned long fd, const struct iovec __user *vec,
			 unsigned long vlen, loff_t pos, rwf_t flags)
{
	struct fd f;
	ssize_t ret = -EBADF;

	if (pos < 0)
		return -EINVAL;

	f = fdget(fd);
	if (f.file) {
		ret = -ESPIPE;
		if (f.file->f_mode & FMODE_PREAD)
			ret = vfs_readv(f.file, vec, vlen, &pos, flags);
		fdput(f);
	}

	if (ret > 0)
		add_rchar(current, ret);
	inc_syscr(current);
	return ret;
}

/**
 * do_pwritev - 执行位置向量写入操作
 * @fd: 文件描述符
 * @vec: 用户空间iovec数组
 * @vlen: iovec数组长度
 * @pos: 写入位置
 * @flags: 写入标志
 *
 * 向指定位置执行向量写入，不改变文件当前位置。这是pwritev
 * 系统调用族的核心实现函数。
 *
 * 返回值: 实际写入的字节数，失败返回负错误码
 */
static ssize_t do_pwritev(unsigned long fd, const struct iovec __user *vec,
			  unsigned long vlen, loff_t pos, rwf_t flags)
{
	struct fd f;
	ssize_t ret = -EBADF;

	if (pos < 0)
		return -EINVAL;

	f = fdget(fd);
	if (f.file) {
		ret = -ESPIPE;
		if (f.file->f_mode & FMODE_PWRITE)
			ret = vfs_writev(f.file, vec, vlen, &pos, flags);
		fdput(f);
	}

	if (ret > 0)
		add_wchar(current, ret);
	inc_syscw(current);
	return ret;
}

/* readv系统调用入口 - 向量读取（从多个缓冲区读取数据） */
SYSCALL_DEFINE3(readv, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen)
{
	return do_readv(fd, vec, vlen, 0); /* 调用向量读取实现 */
}

/* writev系统调用入口 - 向量写入（向多个缓冲区写入数据） */
SYSCALL_DEFINE3(writev, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen)
{
	return do_writev(fd, vec, vlen, 0); /* 调用向量写入实现 */
}

SYSCALL_DEFINE5(preadv, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);

	return do_preadv(fd, vec, vlen, pos, 0);
}

SYSCALL_DEFINE6(preadv2, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h,
		rwf_t, flags)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);

	if (pos == -1)
		return do_readv(fd, vec, vlen, flags);

	return do_preadv(fd, vec, vlen, pos, flags);
}

SYSCALL_DEFINE5(pwritev, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);

	return do_pwritev(fd, vec, vlen, pos, 0);
}

SYSCALL_DEFINE6(pwritev2, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h,
		rwf_t, flags)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);

	if (pos == -1)
		return do_writev(fd, vec, vlen, flags);

	return do_pwritev(fd, vec, vlen, pos, flags);
}

/*
 * Various compat syscalls.  Note that they all pretend to take a native
 * iovec - import_iovec will properly treat those as compat_iovecs based on
 * in_compat_syscall().
 */
#ifdef CONFIG_COMPAT
#ifdef __ARCH_WANT_COMPAT_SYS_PREADV64
COMPAT_SYSCALL_DEFINE4(preadv64, unsigned long, fd,
		const struct iovec __user *, vec,
		unsigned long, vlen, loff_t, pos)
{
	return do_preadv(fd, vec, vlen, pos, 0);
}
#endif

COMPAT_SYSCALL_DEFINE5(preadv, compat_ulong_t, fd,
		const struct iovec __user *, vec,
		compat_ulong_t, vlen, u32, pos_low, u32, pos_high)
{
	loff_t pos = ((loff_t)pos_high << 32) | pos_low;

	return do_preadv(fd, vec, vlen, pos, 0);
}

#ifdef __ARCH_WANT_COMPAT_SYS_PREADV64V2
COMPAT_SYSCALL_DEFINE5(preadv64v2, unsigned long, fd,
		const struct iovec __user *, vec,
		unsigned long, vlen, loff_t, pos, rwf_t, flags)
{
	if (pos == -1)
		return do_readv(fd, vec, vlen, flags);
	return do_preadv(fd, vec, vlen, pos, flags);
}
#endif

COMPAT_SYSCALL_DEFINE6(preadv2, compat_ulong_t, fd,
		const struct iovec __user *, vec,
		compat_ulong_t, vlen, u32, pos_low, u32, pos_high,
		rwf_t, flags)
{
	loff_t pos = ((loff_t)pos_high << 32) | pos_low;

	if (pos == -1)
		return do_readv(fd, vec, vlen, flags);
	return do_preadv(fd, vec, vlen, pos, flags);
}

#ifdef __ARCH_WANT_COMPAT_SYS_PWRITEV64
COMPAT_SYSCALL_DEFINE4(pwritev64, unsigned long, fd,
		const struct iovec __user *, vec,
		unsigned long, vlen, loff_t, pos)
{
	return do_pwritev(fd, vec, vlen, pos, 0);
}
#endif

COMPAT_SYSCALL_DEFINE5(pwritev, compat_ulong_t, fd,
		const struct iovec __user *,vec,
		compat_ulong_t, vlen, u32, pos_low, u32, pos_high)
{
	loff_t pos = ((loff_t)pos_high << 32) | pos_low;

	return do_pwritev(fd, vec, vlen, pos, 0);
}

#ifdef __ARCH_WANT_COMPAT_SYS_PWRITEV64V2
COMPAT_SYSCALL_DEFINE5(pwritev64v2, unsigned long, fd,
		const struct iovec __user *, vec,
		unsigned long, vlen, loff_t, pos, rwf_t, flags)
{
	if (pos == -1)
		return do_writev(fd, vec, vlen, flags);
	return do_pwritev(fd, vec, vlen, pos, flags);
}
#endif

COMPAT_SYSCALL_DEFINE6(pwritev2, compat_ulong_t, fd,
		const struct iovec __user *,vec,
		compat_ulong_t, vlen, u32, pos_low, u32, pos_high, rwf_t, flags)
{
	loff_t pos = ((loff_t)pos_high << 32) | pos_low;

	if (pos == -1)
		return do_writev(fd, vec, vlen, flags);
	return do_pwritev(fd, vec, vlen, pos, flags);
}
#endif /* CONFIG_COMPAT */

/**
 * do_sendfile - 执行sendfile操作
 * @out_fd: 输出文件描述符
 * @in_fd: 输入文件描述符
 * @ppos: 输入文件位置指针
 * @count: 要传输的字节数
 * @max: 最大传输字节数
 *
 * 在两个文件描述符之间直接传输数据，无需在用户空间缓冲。
 * 这是零拷贝数据传输的核心实现，通常用于高效的网络服务。
 *
 * 返回值: 实际传输的字节数，失败返回负错误码
 */
static ssize_t do_sendfile(int out_fd, int in_fd, loff_t *ppos,
		  	   size_t count, loff_t max)
{
	struct fd in, out;
	struct inode *in_inode, *out_inode;
	loff_t pos;
	loff_t out_pos;
	ssize_t retval;
	int fl;

	/*
	 * Get input file, and verify that it is ok..
	 */
	retval = -EBADF;
	in = fdget(in_fd);
	if (!in.file)
		goto out;
	if (!(in.file->f_mode & FMODE_READ))
		goto fput_in;
	retval = -ESPIPE;
	if (!ppos) {
		pos = in.file->f_pos;
	} else {
		pos = *ppos;
		if (!(in.file->f_mode & FMODE_PREAD))
			goto fput_in;
	}
	retval = rw_verify_area(READ, in.file, &pos, count);
	if (retval < 0)
		goto fput_in;
	if (count > MAX_RW_COUNT)
		count =  MAX_RW_COUNT;

	/*
	 * Get output file, and verify that it is ok..
	 */
	retval = -EBADF;
	out = fdget(out_fd);
	if (!out.file)
		goto fput_in;
	if (!(out.file->f_mode & FMODE_WRITE))
		goto fput_out;
	in_inode = file_inode(in.file);
	out_inode = file_inode(out.file);
	out_pos = out.file->f_pos;
	retval = rw_verify_area(WRITE, out.file, &out_pos, count);
	if (retval < 0)
		goto fput_out;

	if (!max)
		max = min(in_inode->i_sb->s_maxbytes, out_inode->i_sb->s_maxbytes);

	if (unlikely(pos + count > max)) {
		retval = -EOVERFLOW;
		if (pos >= max)
			goto fput_out;
		count = max - pos;
	}

	fl = 0;
#if 0
	/*
	 * We need to debate whether we can enable this or not. The
	 * man page documents EAGAIN return for the output at least,
	 * and the application is arguably buggy if it doesn't expect
	 * EAGAIN on a non-blocking file descriptor.
	 */
	if (in.file->f_flags & O_NONBLOCK)
		fl = SPLICE_F_NONBLOCK;
#endif
	file_start_write(out.file);
	retval = do_splice_direct(in.file, &pos, out.file, &out_pos, count, fl);
	file_end_write(out.file);

	if (retval > 0) {
		add_rchar(current, retval);
		add_wchar(current, retval);
		fsnotify_access(in.file);
		fsnotify_modify(out.file);
		out.file->f_pos = out_pos;
		if (ppos)
			*ppos = pos;
		else
			in.file->f_pos = pos;
	}

	inc_syscr(current);
	inc_syscw(current);
	if (pos > max)
		retval = -EOVERFLOW;

fput_out:
	fdput(out);
fput_in:
	fdput(in);
out:
	return retval;
}

/* sendfile系统调用入口 - 在两个文件描述符之间传输数据 */
SYSCALL_DEFINE4(sendfile, int, out_fd, int, in_fd, off_t __user *, offset, size_t, count)
{
	loff_t pos;
	off_t off;
	ssize_t ret;

	if (offset) {
		if (unlikely(get_user(off, offset)))
			return -EFAULT;
		pos = off;
		ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

SYSCALL_DEFINE4(sendfile64, int, out_fd, int, in_fd, loff_t __user *, offset, size_t, count)
{
	loff_t pos;
	ssize_t ret;

	if (offset) {
		if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
			return -EFAULT;
		ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(sendfile, int, out_fd, int, in_fd,
		compat_off_t __user *, offset, compat_size_t, count)
{
	loff_t pos;
	off_t off;
	ssize_t ret;

	if (offset) {
		if (unlikely(get_user(off, offset)))
			return -EFAULT;
		pos = off;
		ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

COMPAT_SYSCALL_DEFINE4(sendfile64, int, out_fd, int, in_fd,
		compat_loff_t __user *, offset, compat_size_t, count)
{
	loff_t pos;
	ssize_t ret;

	if (offset) {
		if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
			return -EFAULT;
		ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}
#endif

/**
 * generic_copy_file_range - 在两个文件之间复制数据
 * @file_in: 源文件结构
 * @pos_in: 源文件读取偏移量
 * @file_out: 目标文件结构
 * @pos_out: 目标文件写入偏移量
 * @len: 要复制的数据量
 * @flags: 复制标志
 *
 * 这是一个通用的文件系统辅助函数，用于在文件之间复制数据。
 * 对源文件或目标文件的所有者没有约束 - 文件可以属于不同的
 * 超级块和不同的文件系统类型。允许短复制。
 *
 * 应该从@file_out文件系统调用，按照->copy_file_range()方法。
 *
 * 返回值: 复制的字节数或表示失败的负错误码
 */
/**
 * generic_copy_file_range - copy data between two files
 * @file_in:	file structure to read from
 * @pos_in:	file offset to read from
 * @file_out:	file structure to write data to
 * @pos_out:	file offset to write data to
 * @len:	amount of data to copy
 * @flags:	copy flags
 *
 * This is a generic filesystem helper to copy data from one file to another.
 * It has no constraints on the source or destination file owners - the files
 * can belong to different superblocks and different filesystem types. Short
 * copies are allowed.
 *
 * This should be called from the @file_out filesystem, as per the
 * ->copy_file_range() method.
 *
 * Returns the number of bytes copied or a negative error indicating the
 * failure.
 */

ssize_t generic_copy_file_range(struct file *file_in, loff_t pos_in,
				struct file *file_out, loff_t pos_out,
				size_t len, unsigned int flags)
{
	return do_splice_direct(file_in, &pos_in, file_out, &pos_out,
				len > MAX_RW_COUNT ? MAX_RW_COUNT : len, 0);
}
EXPORT_SYMBOL(generic_copy_file_range);

/**
 * do_copy_file_range - 执行文件复制操作
 * @file_in: 源文件
 * @pos_in: 源文件位置
 * @file_out: 目标文件
 * @pos_out: 目标文件位置
 * @len: 复制长度
 * @flags: 复制标志
 *
 * 选择最合适的文件复制方法。如果两个文件使用相同的copy_file_range
 * 实现，则使用文件系统特定的方法，否则回退到通用实现。
 *
 * 返回值: 复制的字节数，失败返回负错误码
 */
static ssize_t do_copy_file_range(struct file *file_in, loff_t pos_in,
				  struct file *file_out, loff_t pos_out,
				  size_t len, unsigned int flags)
{
	/*
	 * Although we now allow filesystems to handle cross sb copy, passing
	 * a file of the wrong filesystem type to filesystem driver can result
	 * in an attempt to dereference the wrong type of ->private_data, so
	 * avoid doing that until we really have a good reason.  NFS defines
	 * several different file_system_type structures, but they all end up
	 * using the same ->copy_file_range() function pointer.
	 */
	if (file_out->f_op->copy_file_range &&
	    file_out->f_op->copy_file_range == file_in->f_op->copy_file_range)
		return file_out->f_op->copy_file_range(file_in, pos_in,
						       file_out, pos_out,
						       len, flags);

	return generic_copy_file_range(file_in, pos_in, file_out, pos_out, len,
				       flags);
}

/**
 * generic_copy_file_checks - 执行文件复制前的必要检查
 * @file_in: 源文件
 * @pos_in: 源文件位置
 * @file_out: 目标文件
 * @pos_out: 目标文件位置
 * @req_count: 请求复制的字节数（可调整）
 * @flags: 复制标志
 *
 * 在执行文件复制前进行各种检查和验证。检查文件权限、偏移量溢出、
 * 文件大小限制、重叠复制等。可以调整实际复制的字节数。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/*
 * Performs necessary checks before doing a file copy
 *
 * Can adjust amount of bytes to copy via @req_count argument.
 * Returns appropriate error code that caller should return or
 * zero in case the copy should be allowed.
 */
static int generic_copy_file_checks(struct file *file_in, loff_t pos_in,
				    struct file *file_out, loff_t pos_out,
				    size_t *req_count, unsigned int flags)
{
	struct inode *inode_in = file_inode(file_in);
	struct inode *inode_out = file_inode(file_out);
	uint64_t count = *req_count;
	loff_t size_in;
	int ret;

	ret = generic_file_rw_checks(file_in, file_out);
	if (ret)
		return ret;

	/* Don't touch certain kinds of inodes */
	if (IS_IMMUTABLE(inode_out))
		return -EPERM;

	if (IS_SWAPFILE(inode_in) || IS_SWAPFILE(inode_out))
		return -ETXTBSY;

	/* Ensure offsets don't wrap. */
	if (pos_in + count < pos_in || pos_out + count < pos_out)
		return -EOVERFLOW;

	/* Shorten the copy to EOF */
	size_in = i_size_read(inode_in);
	if (pos_in >= size_in)
		count = 0;
	else
		count = min(count, size_in - (uint64_t)pos_in);

	ret = generic_write_check_limits(file_out, pos_out, &count);
	if (ret)
		return ret;

	/* Don't allow overlapped copying within the same file. */
	if (inode_in == inode_out &&
	    pos_out + count > pos_in &&
	    pos_out < pos_in + count)
		return -EINVAL;

	*req_count = count;
	return 0;
}

/**
 * vfs_copy_file_range - VFS层文件范围复制
 * @file_in: 源文件
 * @pos_in: 源文件位置
 * @file_out: 目标文件
 * @pos_out: 目标文件位置
 * @len: 复制长度
 * @flags: 复制标志
 *
 * VFS层的文件范围复制接口。首先尝试使用文件系统的克隆功能
 * (remap_file_range)进行高效复制，如果不支持则回退到传统复制。
 * copy_file_range()与常规文件读写的不同之处在于它明确允许
 * 返回部分成功。
 *
 * 返回值: 复制的字节数，失败返回负错误码
 */
/*
 * copy_file_range() differs from regular file read and write in that it
 * specifically allows return partial success.  When it does so is up to
 * the copy_file_range method.
 */
ssize_t vfs_copy_file_range(struct file *file_in, loff_t pos_in,
			    struct file *file_out, loff_t pos_out,
			    size_t len, unsigned int flags)
{
	ssize_t ret;

	if (flags != 0)
		return -EINVAL;

	ret = generic_copy_file_checks(file_in, pos_in, file_out, pos_out, &len,
				       flags);
	if (unlikely(ret))
		return ret;

	ret = rw_verify_area(READ, file_in, &pos_in, len);
	if (unlikely(ret))
		return ret;

	ret = rw_verify_area(WRITE, file_out, &pos_out, len);
	if (unlikely(ret))
		return ret;

	if (len == 0)
		return 0;

	file_start_write(file_out);

	/*
	 * Try cloning first, this is supported by more file systems, and
	 * more efficient if both clone and copy are supported (e.g. NFS).
	 */
	if (file_in->f_op->remap_file_range &&
	    file_inode(file_in)->i_sb == file_inode(file_out)->i_sb) {
		loff_t cloned;

		cloned = file_in->f_op->remap_file_range(file_in, pos_in,
				file_out, pos_out,
				min_t(loff_t, MAX_RW_COUNT, len),
				REMAP_FILE_CAN_SHORTEN);
		if (cloned > 0) {
			ret = cloned;
			goto done;
		}
	}

	ret = do_copy_file_range(file_in, pos_in, file_out, pos_out, len,
				flags);
	WARN_ON_ONCE(ret == -EOPNOTSUPP);
done:
	if (ret > 0) {
		fsnotify_access(file_in);
		add_rchar(current, ret);
		fsnotify_modify(file_out);
		add_wchar(current, ret);
	}

	inc_syscr(current);
	inc_syscw(current);

	file_end_write(file_out);

	return ret;
}
EXPORT_SYMBOL(vfs_copy_file_range);

/* copy_file_range系统调用入口 - 在两个文件之间复制数据范围 */
SYSCALL_DEFINE6(copy_file_range, int, fd_in, loff_t __user *, off_in,
		int, fd_out, loff_t __user *, off_out,
		size_t, len, unsigned int, flags)
{
	loff_t pos_in;
	loff_t pos_out;
	struct fd f_in;
	struct fd f_out;
	ssize_t ret = -EBADF;

	f_in = fdget(fd_in);
	if (!f_in.file)
		goto out2;

	f_out = fdget(fd_out);
	if (!f_out.file)
		goto out1;

	ret = -EFAULT;
	if (off_in) {
		if (copy_from_user(&pos_in, off_in, sizeof(loff_t)))
			goto out;
	} else {
		pos_in = f_in.file->f_pos;
	}

	if (off_out) {
		if (copy_from_user(&pos_out, off_out, sizeof(loff_t)))
			goto out;
	} else {
		pos_out = f_out.file->f_pos;
	}

	ret = vfs_copy_file_range(f_in.file, pos_in, f_out.file, pos_out, len,
				  flags);
	if (ret > 0) {
		pos_in += ret;
		pos_out += ret;

		if (off_in) {
			if (copy_to_user(off_in, &pos_in, sizeof(loff_t)))
				ret = -EFAULT;
		} else {
			f_in.file->f_pos = pos_in;
		}

		if (off_out) {
			if (copy_to_user(off_out, &pos_out, sizeof(loff_t)))
				ret = -EFAULT;
		} else {
			f_out.file->f_pos = pos_out;
		}
	}

out:
	fdput(f_out);
out1:
	fdput(f_in);
out2:
	return ret;
}

/**
 * generic_write_check_limits - 检查写入限制
 * @file: 文件指针
 * @pos: 写入位置
 * @count: 要写入的字节数（可调整）
 *
 * 检查写入操作是否超过各种限制，包括页缓存支持范围、LFS限制、
 * 文件大小限制等。如果位置在限制范围内会进行短访问，
 * 如果超过限制则返回-EFBIG。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/*
 * Don't operate on ranges the page cache doesn't support, and don't exceed the
 * LFS limits.  If pos is under the limit it becomes a short access.  If it
 * exceeds the limit we return -EFBIG.
 */
int generic_write_check_limits(struct file *file, loff_t pos, loff_t *count)
{
	struct inode *inode = file->f_mapping->host;
	loff_t max_size = inode->i_sb->s_maxbytes;
	loff_t limit = rlimit(RLIMIT_FSIZE);

	if (limit != RLIM_INFINITY) {
		if (pos >= limit) {
			send_sig(SIGXFSZ, current, 0);
			return -EFBIG;
		}
		*count = min(*count, limit - pos);
	}

	if (!(file->f_flags & O_LARGEFILE))
		max_size = MAX_NON_LFS;

	if (unlikely(pos >= max_size))
		return -EFBIG;

	*count = min(*count, max_size - pos);

	return 0;
}

/**
 * generic_write_checks - 执行写入前的必要检查
 * @iocb: I/O控制块
 * @from: I/O迭代器
 *
 * 在执行写入前进行各种检查。可以调整写入位置或写入字节数。
 * 检查交换文件、追加模式、NOWAIT标志组合、文件大小限制等。
 *
 * 返回值: 调整后允许写入的字节数，失败返回负错误码
 */
/*
 * Performs necessary checks before doing a write
 *
 * Can adjust writing position or amount of bytes to write.
 * Returns appropriate error code that caller should return or
 * zero in case that write should be allowed.
 */
ssize_t generic_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	loff_t count;
	int ret;

	if (IS_SWAPFILE(inode))
		return -ETXTBSY;

	if (!iov_iter_count(from))
		return 0;

	/* FIXME: this is for backwards compatibility with 2.4 */
	if (iocb->ki_flags & IOCB_APPEND)
		iocb->ki_pos = i_size_read(inode);

	if ((iocb->ki_flags & IOCB_NOWAIT) && !(iocb->ki_flags & IOCB_DIRECT))
		return -EINVAL;

	count = iov_iter_count(from);
	ret = generic_write_check_limits(file, iocb->ki_pos, &count);
	if (ret)
		return ret;

	iov_iter_truncate(from, count);
	return iov_iter_count(from);
}
EXPORT_SYMBOL(generic_write_checks);

/**
 * generic_file_rw_checks - 执行文件复制/克隆前的通用检查
 * @file_in: 源文件
 * @file_out: 目标文件
 *
 * 在进行文件复制/克隆操作前执行通用检查。验证文件类型（不能是
 * 目录、管道、套接字等），检查文件权限（源文件可读、目标文件可写），
 * 确保目标文件不是以追加模式打开。
 *
 * 返回值: 成功返回0，失败返回负错误码
 */
/*
 * Performs common checks before doing a file copy/clone
 * from @file_in to @file_out.
 */
int generic_file_rw_checks(struct file *file_in, struct file *file_out)
{
	struct inode *inode_in = file_inode(file_in);
	struct inode *inode_out = file_inode(file_out);

	/* Don't copy dirs, pipes, sockets... */
	if (S_ISDIR(inode_in->i_mode) || S_ISDIR(inode_out->i_mode))
		return -EISDIR;
	if (!S_ISREG(inode_in->i_mode) || !S_ISREG(inode_out->i_mode))
		return -EINVAL;

	if (!(file_in->f_mode & FMODE_READ) ||
	    !(file_out->f_mode & FMODE_WRITE) ||
	    (file_out->f_flags & O_APPEND))
		return -EBADF;

	return 0;
}
