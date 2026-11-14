// SPDX-License-Identifier: GPL-2.0
#include <linux/unistd.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/romfs_fs.h>
#include <linux/initrd.h>
#include <linux/sched.h>
#include <linux/freezer.h>
#include <linux/kmod.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"

unsigned long initrd_start, initrd_end;
int initrd_below_start_ok;
static unsigned int real_root_dev;	/* do_proc_dointvec cannot handle kdev_t */
/* noinitrd 可以把它关掉；默认允许尝试老式 initrd */
static int __initdata mount_initrd = 1;

phys_addr_t phys_initrd_start __initdata;
unsigned long phys_initrd_size __initdata;

#ifdef CONFIG_SYSCTL
static const struct ctl_table kern_do_mounts_initrd_table[] = {
	{
		.procname       = "real-root-dev",
		.data           = &real_root_dev,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
	},
};

static __init int kernel_do_mounts_initrd_sysctls_init(void)
{
	register_sysctl_init("kernel", kern_do_mounts_initrd_table);
	return 0;
}
late_initcall(kernel_do_mounts_initrd_sysctls_init);
#endif /* CONFIG_SYSCTL */

static int __init no_initrd(char *str)
{
	mount_initrd = 0;
	return 1;
}

__setup("noinitrd", no_initrd);

static int __init early_initrdmem(char *p)
{
	phys_addr_t start;
	unsigned long size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		phys_initrd_start = start;
		phys_initrd_size = size;
	}
	return 0;
}
early_param("initrdmem", early_initrdmem);

static int __init early_initrd(char *p)
{
	return early_initrdmem(p);
}
early_param("initrd", early_initrd);

/* /linuxrc 的执行环境：换到 initrd 自己的根，脱离内核的 fs 上下文 */
static int __init init_linuxrc(struct subprocess_info *info, struct cred *new)
{
	/* 复制一份 fs 上下文，后面的 chroot 只影响这个子进程 */
	ksys_unshare(CLONE_FS | CLONE_FILES);
	console_on_rootfs();
	/* move initrd over / and chdir/chroot in initrd root */
	init_chdir("/root");
	init_mount(".", "/", NULL, MS_MOVE, NULL);
	init_chroot(".");
	ksys_setsid();
	return 0;
}

/*
 * 老式 initrd 的执行体：把 ramdisk 挂到 /root，chroot 进去跑 /linuxrc，
 * 由它用 real-root-dev 告知真正的根在哪儿，再切回去挂真根。
 */
static void __init handle_initrd(char *root_device_name)
{
	struct subprocess_info *info;
	static char *argv[] = { "linuxrc", NULL, };
	extern char *envp_init[];
	int error;

	pr_warn("using deprecated initrd support, will be removed soon.\n");

	real_root_dev = new_encode_dev(ROOT_DEV);
	create_dev("/dev/root.old", Root_RAM0);
	/* mount initrd on rootfs' /root */
	mount_root_generic("/dev/root.old", root_device_name,
			   root_mountflags & ~MS_RDONLY);
	init_mkdir("/old", 0700);
	init_chdir("/old");

	info = call_usermodehelper_setup("/linuxrc", argv, envp_init,
					 GFP_KERNEL, init_linuxrc, NULL, NULL);
	if (!info)
		return;
	call_usermodehelper_exec(info, UMH_WAIT_PROC|UMH_FREEZABLE);

	/* move initrd to rootfs' /old */
	init_mount("..", ".", NULL, MS_MOVE, NULL);
	/* switch root and cwd back to / of rootfs */
	init_chroot("..");

	if (new_decode_dev(real_root_dev) == Root_RAM0) {
		init_chdir("/old");
		return;
	}

	init_chdir("/");
	ROOT_DEV = new_decode_dev(real_root_dev);
	mount_root(root_device_name);

	printk(KERN_NOTICE "Trying to move old root to /initrd ... ");
	error = init_mount("/old", "/root/initrd", NULL, MS_MOVE, NULL);
	if (!error)
		printk("okay\n");
	else {
		if (error == -ENOENT)
			printk("/initrd does not exist. Ignored.\n");
		else
			printk("failed\n");
		printk(KERN_NOTICE "Unmounting old root\n");
		init_umount("/old", MNT_DETACH);
	}
}

/*
 * 老式 initrd 入口：BLK_DEV_RAM 打开时才可能把镜像读进 /dev/ram0 并跑
 * /linuxrc。本次配置没开 BLK_DEV_RAM，rd_load_image() 是返回 0 的空桩，
 * 所以这里必然返回 false——initramfs 场景下它什么也不做。
 */
bool __init initrd_load(char *root_device_name)
{
	printk(KERN_INFO "[latte] initrd_load: mount_initrd=%d ROOT_DEV=%u\n",
	       mount_initrd, ROOT_DEV);
	if (mount_initrd) {
		create_dev("/dev/ram", Root_RAM0);
		/*
		 * Load the initrd data into /dev/ram0. Execute it as initrd
		 * unless /dev/ram0 is supposed to be our actual root device,
		 * in that case the ram disk is just set up here, and gets
		 * mounted in the normal path.
		 */
		/* 镜像能读成 ramdisk、且根不是 ram0，才走 linuxrc 流程 */
		if (rd_load_image("/initrd.image") && ROOT_DEV != Root_RAM0) {
			init_unlink("/initrd.image");
			handle_initrd(root_device_name);
			return true;
		}
	}
	/* 没有可用的老式 initrd：清掉占位文件，返回 false 让调用者挂真根 */
	init_unlink("/initrd.image");
	return false;
}
