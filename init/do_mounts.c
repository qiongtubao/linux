// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/fd.h>
#include <linux/tty.h>
#include <linux/suspend.h>
#include <linux/root_dev.h>
#include <linux/security.h>
#include <linux/delay.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/initrd.h>
#include <linux/async.h>
#include <linux/fs_struct.h>
#include <linux/slab.h>
#include <linux/ramfs.h>
#include <linux/shmem_fs.h>
#include <linux/ktime.h>

#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_mount.h>
#include <linux/raid/detect.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"

int root_mountflags = MS_RDONLY | MS_SILENT;
static char __initdata saved_root_name[64];
static int root_wait;

dev_t ROOT_DEV;

static int __init load_ramdisk(char *str)
{
	pr_warn("ignoring the deprecated load_ramdisk= option\n");
	return 1;
}
__setup("load_ramdisk=", load_ramdisk);

static int __init readonly(char *str)
{
	if (*str)
		return 0;
	root_mountflags |= MS_RDONLY;
	return 1;
}

static int __init readwrite(char *str)
{
	if (*str)
		return 0;
	root_mountflags &= ~MS_RDONLY;
	return 1;
}

__setup("ro", readonly);
__setup("rw", readwrite);

static int __init root_dev_setup(char *line)
{
	strscpy(saved_root_name, line, sizeof(saved_root_name));
	return 1;
}

__setup("root=", root_dev_setup);

static int __init rootwait_setup(char *str)
{
	if (*str)
		return 0;
	root_wait = -1;
	return 1;
}

__setup("rootwait", rootwait_setup);

static int __init rootwait_timeout_setup(char *str)
{
	int sec;

	if (kstrtoint(str, 0, &sec) || sec < 0) {
		pr_warn("ignoring invalid rootwait value\n");
		goto ignore;
	}

	if (check_mul_overflow(sec, MSEC_PER_SEC, &root_wait)) {
		pr_warn("ignoring excessive rootwait value\n");
		goto ignore;
	}

	return 1;

ignore:
	/* Fallback to indefinite wait */
	root_wait = -1;

	return 1;
}

__setup("rootwait=", rootwait_timeout_setup);

static char * __initdata root_mount_data;
static int __init root_data_setup(char *str)
{
	root_mount_data = str;
	return 1;
}

static char * __initdata root_fs_names;
static int __init fs_names_setup(char *str)
{
	root_fs_names = str;
	return 1;
}

static unsigned int __initdata root_delay;
static int __init root_delay_setup(char *str)
{
	root_delay = simple_strtoul(str, NULL, 0);
	return 1;
}

__setup("rootflags=", root_data_setup);
__setup("rootfstype=", fs_names_setup);
__setup("rootdelay=", root_delay_setup);

/* This can return zero length strings. Caller should check */
/* 把 "ext4,ramfs" 这类 rootfstype 列表拆成首尾相连的字符串数组 */
static int __init split_fs_names(char *page, size_t size)
{
	int count = 1;
	char *p = page;

	strscpy(p, root_fs_names, size);
	while (*p++) {
		if (p[-1] == ',') {
			p[-1] = '\0';
			count++;
		}
	}

	return count;
}

/* 真挂一个 fs 到 /root；挂成后把 ROOT_DEV 更新成 superblock 的实际设备号 */
static int __init do_mount_root(const char *name, const char *fs,
				 const int flags, const void *data)
{
	struct super_block *s;
	struct page *p = NULL;
	char *data_page = NULL;
	int ret;

	if (data) {
		/* init_mount() requires a full page as fifth argument */
		p = alloc_page(GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		data_page = page_address(p);
		strscpy_pad(data_page, data, PAGE_SIZE);
	}

	/* 先挂到 /root，挂成了才挪到 /，避免半成品盖住当前的根 */
	ret = init_mount(name, "/root", fs, flags, data_page);
	if (ret)
		goto out;

	init_chdir("/root");
	s = current->fs->pwd.dentry->d_sb;
	ROOT_DEV = s->s_dev;
	/* 这条 VFS: Mounted root ... 就是根挂成功的标志 */
	printk(KERN_INFO
	       "VFS: Mounted root (%s filesystem)%s on device %u:%u.\n",
	       s->s_type->name,
	       sb_rdonly(s) ? " readonly" : "",
	       MAJOR(ROOT_DEV), MINOR(ROOT_DEV));

out:
	if (p)
		put_page(p);
	return ret;
}

/*
 * 通用挂载：先按 rootfstype（没给就列出所有块设备 fs）逐个试，
 * -EACCES/-EINVAL 当成“这个 fs 不认”换下一个，全失败才 panic。
 */
void __init mount_root_generic(char *name, char *pretty_name, int flags)
{
	printk(KERN_INFO "[latte] mount_root_generic: ROOT_DEV=%u\n", ROOT_DEV);
	struct page *page = alloc_page(GFP_KERNEL);
	char *fs_names = page_address(page);
	char *p;
	char b[BDEVNAME_SIZE];
	int num_fs, i;

	scnprintf(b, BDEVNAME_SIZE, "unknown-block(%u,%u)",
		  MAJOR(ROOT_DEV), MINOR(ROOT_DEV));
	if (root_fs_names)
		num_fs = split_fs_names(fs_names, PAGE_SIZE);
	else
		num_fs = list_bdev_fs_names(fs_names, PAGE_SIZE);
retry:
	for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1) {
		int err;

		if (!*p)
			continue;
		err = do_mount_root(name, p, flags, root_mount_data);
		switch (err) {
			case 0:
				goto out;
			case -EACCES:
			case -EINVAL:
#ifdef CONFIG_BLOCK
				init_flush_fput();
#endif
				continue;
		}
	        /*
		 * Allow the user to distinguish between failed sys_open
		 * and bad superblock on root device.
		 * and give them a list of the available devices
		 */
		printk("VFS: Cannot open root device \"%s\" or %s: error %d\n",
				pretty_name, b, err);
		printk("Please append a correct \"root=\" boot option; here are the available partitions:\n");
		printk_all_partitions();

		if (root_fs_names)
			num_fs = list_bdev_fs_names(fs_names, PAGE_SIZE);
		if (!num_fs)
			pr_err("Can't find any bdev filesystem to be used for mount!\n");
		else {
			pr_err("List of all bdev filesystems:\n");
			for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1)
				pr_err(" %s", p);
			pr_err("\n");
		}

		/* 设备在、fs 也认，但就是挂不上：无路可退，panic */
		panic("VFS: Unable to mount root fs on %s", b);
	}
	if (!(flags & SB_RDONLY)) {
		flags |= SB_RDONLY;
		goto retry;
	}

	printk("List of all partitions:\n");
	printk_all_partitions();
	printk("No filesystem could mount root, tried: ");
	for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1)
		printk(" %s", p);
	printk("\n");
	panic("VFS: Unable to mount root fs on \"%s\" or %s", pretty_name, b);
out:
	put_page(page);
}
 
#ifdef CONFIG_ROOT_NFS

#define NFSROOT_TIMEOUT_MIN	5
#define NFSROOT_TIMEOUT_MAX	30
#define NFSROOT_RETRY_MAX	5

static void __init mount_nfs_root(void)
{
	char *root_dev, *root_data;
	unsigned int timeout;
	int try;

	if (nfs_root_data(&root_dev, &root_data))
		goto fail;

	/*
	 * The server or network may not be ready, so try several
	 * times.  Stop after a few tries in case the client wants
	 * to fall back to other boot methods.
	 */
	timeout = NFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		if (!do_mount_root(root_dev, "nfs", root_mountflags, root_data))
			return;
		if (try > NFSROOT_RETRY_MAX)
			break;

		/* Wait, in case the server refused us immediately */
		ssleep(timeout);
		timeout <<= 1;
		if (timeout > NFSROOT_TIMEOUT_MAX)
			timeout = NFSROOT_TIMEOUT_MAX;
	}
fail:
	pr_err("VFS: Unable to mount root fs via NFS.\n");
}
#else
static inline void mount_nfs_root(void)
{
}
#endif /* CONFIG_ROOT_NFS */

#ifdef CONFIG_CIFS_ROOT

#define CIFSROOT_TIMEOUT_MIN	5
#define CIFSROOT_TIMEOUT_MAX	30
#define CIFSROOT_RETRY_MAX	5

static void __init mount_cifs_root(void)
{
	char *root_dev, *root_data;
	unsigned int timeout;
	int try;

	if (cifs_root_data(&root_dev, &root_data))
		goto fail;

	timeout = CIFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		if (!do_mount_root(root_dev, "cifs", root_mountflags,
				   root_data))
			return;
		if (try > CIFSROOT_RETRY_MAX)
			break;

		ssleep(timeout);
		timeout <<= 1;
		if (timeout > CIFSROOT_TIMEOUT_MAX)
			timeout = CIFSROOT_TIMEOUT_MAX;
	}
fail:
	pr_err("VFS: Unable to mount root fs via SMB.\n");
}
#else
static inline void mount_cifs_root(void)
{
}
#endif /* CONFIG_CIFS_ROOT */

static bool __init fs_is_nodev(char *fstype)
{
	struct file_system_type *fs = get_fs_type(fstype);
	bool ret = false;

	if (fs) {
		ret = !(fs->fs_flags & FS_REQUIRES_DEV);
		put_filesystem(fs);
	}

	return ret;
}

/* 不依赖设备的 fs（tmpfs/ramfs 这类）路径：只试显式给的 rootfstype */
static int __init mount_nodev_root(char *root_device_name)
{
	char *fs_names, *fstype;
	int err = -EINVAL;
	int num_fs, i;

	fs_names = (void *)__get_free_page(GFP_KERNEL);
	if (!fs_names)
		return -EINVAL;
	num_fs = split_fs_names(fs_names, PAGE_SIZE);

	for (i = 0, fstype = fs_names; i < num_fs;
	     i++, fstype += strlen(fstype) + 1) {
		if (!*fstype)
			continue;
		if (!fs_is_nodev(fstype))
			continue;
		err = do_mount_root(root_device_name, fstype, root_mountflags,
				    root_mount_data);
		if (!err)
			break;
	}

	free_page((unsigned long)fs_names);
	return err;
}

#ifdef CONFIG_BLOCK
/* 块设备路径：先造出 /dev/root 节点，再交给通用挂载逐个 fs 试 */
static void __init mount_block_root(char *root_device_name)
{
	int err = create_dev("/dev/root", ROOT_DEV);

	if (err < 0)
		pr_emerg("Failed to create /dev/root: %d\n", err);
	printk(KERN_INFO "[latte] mount_block_root: /dev/root\n");
	mount_root_generic("/dev/root", root_device_name, root_mountflags);
}
#else
static inline void mount_block_root(char *root_device_name)
{
}
#endif /* CONFIG_BLOCK */

/*
 * 按 ROOT_DEV 分派：NFS/CIFS 走网络，Root_Generic 走通用查找；
 * 设备号是 0（没给 root=）时先试不依赖设备的 fs，最后退回块设备路径。
 */
void __init mount_root(char *root_device_name)
{
	printk(KERN_INFO "[latte] mount_root: ROOT_DEV=%u\n", ROOT_DEV);
	switch (ROOT_DEV) {
	case Root_NFS:
		mount_nfs_root();
		break;
	case Root_CIFS:
		mount_cifs_root();
		break;
	case Root_Generic:
		mount_root_generic(root_device_name, root_device_name,
				   root_mountflags);
		break;
	case 0:
		/* 没给 root=：只有显式指定 rootfstype 才试 nodev 那类 */
		if (root_device_name && root_fs_names &&
		    mount_nodev_root(root_device_name) == 0)
			break;
		fallthrough;
	default:
		/* 兜底：当成块设备挂（/dev/root），失败就在里面 panic */
		mount_block_root(root_device_name);
		break;
	}
}

/* wait for any asynchronous scanning to complete */
/* rootwait：轮询等根设备出现，等到了再交给 mount_root，超时也继续往下走 */
static void __init wait_for_root(char *root_device_name)
{
	ktime_t end;

	if (ROOT_DEV != 0)
		return;

	pr_info("Waiting for root device %s...\n", root_device_name);

	end = ktime_add_ms(ktime_get_raw(), root_wait);

	while (!driver_probe_done() ||
	       early_lookup_bdev(root_device_name, &ROOT_DEV) < 0) {
		msleep(5);
		if (root_wait > 0 && ktime_after(ktime_get_raw(), end))
			break;
	}

	async_synchronize_full();

}

/* 把 root= 的字符串变成 ROOT_DEV：特殊名字给伪值，其余查真实设备号 */
static dev_t __init parse_root_device(char *root_device_name)
{
	int error;
	dev_t dev;

	if (!strncmp(root_device_name, "mtd", 3) ||
	    !strncmp(root_device_name, "ubi", 3))
		return Root_Generic;
	if (strcmp(root_device_name, "/dev/nfs") == 0)
		return Root_NFS;
	if (strcmp(root_device_name, "/dev/cifs") == 0)
		return Root_CIFS;
	if (strcmp(root_device_name, "/dev/ram") == 0)
		return Root_RAM0;

	error = early_lookup_bdev(root_device_name, &dev);
	if (error) {
		if (error == -EINVAL && root_wait) {
			pr_err("Disabling rootwait; root= is invalid.\n");
			root_wait = 0;
		}
		return 0;
	}
	return dev;
}

/*
 * 挂真正的根：等设备探测完、必要时等根设备出现、挂上根，最后把它
 * 挪到 "/" 并 chroot 进去。只有 initramfs 里没有可用 init 时才会被
 * kernel_init_freeable() 调到（有 /init 就直接 execve 它了）。
 */
/*
 * Prepare the namespace - decide what/where to mount, load ramdisks, etc.
 */
void __init prepare_namespace(void)
{
	printk(KERN_INFO "[latte] prepare_namespace: root=%s\n",
	       saved_root_name[0] ? saved_root_name : "(none)");
	/* root_delay=N：给慢速设备留时间，N 秒后才开始找根 */
	if (root_delay) {
		printk(KERN_INFO "Waiting %d sec before mounting root device...\n",
		       root_delay);
		ssleep(root_delay);
	}

	/*
	 * wait for the known devices to complete their probing
	 *
	 * Note: this is a potential source of long boot delays.
	 * For example, it is not atypical to wait 5 seconds here
	 * for the touchpad of a laptop to initialize.
	 */
	/* 等已知设备探测完，否则根设备可能还没出现 */
	wait_for_device_probe();

	/* 根在 RAID 上时先把阵列组起来 */
	md_run_setup();

	/* root= 解析成 ROOT_DEV：块设备号，或 Root_NFS 这类伪值 */
	if (saved_root_name[0])
		ROOT_DEV = parse_root_device(saved_root_name);

	/* 老式 initrd：镜像里有 /linuxrc 就交给它跑，跑完再挂真根 */
	printk(KERN_INFO "[latte] prepare_namespace: initrd_load\n");
	if (initrd_load(saved_root_name))
		goto out;

	/* rootwait：设备可能慢，再轮询等一会儿根设备 */
	if (root_wait)
		wait_for_root(saved_root_name);
	/* 真正去挂根；挂不上会在 mount_root_generic() 里直接 panic */
	printk(KERN_INFO "[latte] prepare_namespace: mount_root\n");
	mount_root(saved_root_name);
out:
	/* 换根三步：挂 devtmpfs、把当前 rootfs 挪到 /、chroot 进去 */
	printk(KERN_INFO "[latte] prepare_namespace: devtmpfs + switch root\n");
	devtmpfs_mount();
	init_mount(".", "/", NULL, MS_MOVE, NULL);
	init_chroot(".");
}

static bool is_tmpfs;
static int rootfs_init_fs_context(struct fs_context *fc)
{
	if (IS_ENABLED(CONFIG_TMPFS) && is_tmpfs)
		return shmem_init_fs_context(fc);

	return ramfs_init_fs_context(fc);
}

struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.init_fs_context = rootfs_init_fs_context,
	.kill_sb	= kill_litter_super,
};

/*
 * 给 rootfs 选后端：既没 root= 也没 rootfstype 时用 tmpfs，否则退回
 * ramfs。initramfs 解压出来的东西就落在这个 rootfs 上。
 */
void __init init_rootfs(void)
{
	if (IS_ENABLED(CONFIG_TMPFS)) {
		if (!saved_root_name[0] && !root_fs_names)
			is_tmpfs = true;
		else if (root_fs_names && !!strstr(root_fs_names, "tmpfs"))
			is_tmpfs = true;
	}
}
