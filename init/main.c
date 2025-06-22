/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline fork(void) __attribute__((always_inline));   //强制编译器 始终内联展开
static inline pause(void) __attribute__((always_inline));
static inline _syscall0(int,fork)		//生成fork函数
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;	//时间结构体

	do {
		time.tm_sec = CMOS_READ(0);	//读取CMOS 实时RTC硬件的数据
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));//如果执行后超过了1s,之后就重新获取一次
	BCD_TO_BIN(time.tm_sec);	//转换秒数据 CMOS RTC 使用 BCD（Binary-Coded Decimal） 格式存储时间数据（即每个数字占 4 位）。
	BCD_TO_BIN(time.tm_min);	//转换分钟数据
	BCD_TO_BIN(time.tm_hour);	//转换小时数据
	BCD_TO_BIN(time.tm_mday);	//转换天数据
	BCD_TO_BIN(time.tm_mon);	//转换月数据
	BCD_TO_BIN(time.tm_year);	//转换年数据
	time.tm_mon--; //月数据从0开始 修正
	startup_time = kernel_mktime(&time); //更新时间
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;

void main(void)		/* This really IS void, no error here. */ //系统的开始
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */

 	ROOT_DEV = ORIG_ROOT_DEV; //设置根文件系统的设备号（来自引导加载程序传递的信息）。
 	drive_info = DRIVE_INFO; //获取磁盘驱动器信息（同样由引导程序提供）。
	memory_end = (1<<20) + (EXT_MEM_K<<10); //计算总内存大小：EXT_MEM_K 是从 BIOS 获取的扩展内存大小（单位为 KB）
	memory_end &= 0xfffff000; //对齐到页边界（4KB 对齐）
	if (memory_end > 16*1024*1024) //最大限制为 16MB（因为当时硬件限制）
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) //根据总内存大小设置缓冲区大小（buffer）用于块设备缓存。
		buffer_memory_end = 4*1024*1024; // (12,16]MB。 块设备缓存4MB
	else if (memory_end > 6*1024*1024)	
		buffer_memory_end = 2*1024*1024; //(6，12]MB 块设备缓存2MB
	else
		buffer_memory_end = 1*1024*1024; //[0, 6] 块设备缓存1MB
	main_memory_start = buffer_memory_end; //剩余的内存用于主内存分配
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024); //如果启用了 RAMDISK，就预留一段内存作为虚拟磁盘。rd_init() 初始化 RAMDISK 并返回下一个可用内存地址。
#endif
	mem_init(main_memory_start,memory_end); //初始化内存管理子系统，标记哪些内存可以被动态分配。
	trap_init(); 	//中断陷阱处理初始化
	blk_dev_init(); // 块设备初始化
	chr_dev_init(); // 字符设备初始化
	tty_init();		// 终端设备初始化
	time_init();	// 时间/时钟初始化
	sched_init();	// 调度器初始化
	buffer_init(buffer_memory_end);	// 缓冲区初始化
	hd_init();		// 硬盘初始化
	floppy_init();	// 软驱初始化
	sti();			// 开启全局中断
	move_to_user_mode();	//通过修改标志寄存器（EFLAGS）中的 IOPL 字段，使 CPU 进入用户模式运行。尽管此时仍在内核空间执行，但模拟了用户态权限。
	if (!fork()) {		/* we count on this going ok */ //使用 fork() 创建一个新的进程（进程号为 1）。
		init();		//子进程中调用 init()，这是第一个用户级进程，负责启动后续的系统服务。
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause(); //父进程（进程号为 0）进入无限循环并调用 pause()。
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)  //1号线程会一直创建子进程 shell交互进程 不退出
{
	int pid,i;

	setup((void *) &drive_info);		//从BIOS获取磁盘信息 并挂在根文件系统 初始化设备信息
	(void) open("/dev/tty0",O_RDWR,0);	//打开第一个虚拟终端 /dev/tty0。作为标准输入 （fd=0）。
	(void) dup(0);						//会复制fd=0（标准输入） 得到fd=1（stdout）		
	(void) dup(0);						//再次复制。得到fd=2 (stderr)
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);//输出缓存
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);//输出内存信息
	if (!(pid=fork())) {	
		close(0);//子进程 关闭标准输出stdout。？
		if (open("/etc/rc",O_RDONLY,0)) //打开/etc/rc文件作为stdin 
			_exit(1); //exit 是C标准库函数。它会在退出前执行一些清理工作，_exit() 是系统调用（sys_exit) 直接终止进程，不进行任何清理
		execve("/bin/sh",argv_rc,envp_rc);	//使用shell脚本执行rc文件脚本 
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i)) //调用wait 等待子进程结束 一直阻塞
			/* nothing */;
	while (1) { //不断fork 并进行shell
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) { //子进程
			close(0);close(1);close(2); //关闭所有的stdin，stdout，stderror
			setsid();	//创建新的会话
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp)); //启动一个交互式 shell
		}
		while (1)
			if (pid == wait(&i)) //等待子进程退出
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i); //打印子进程状态码
		sync();	//强制将缓冲区数据写入磁盘
	}
	_exit(0);	//最终退出  不会执行到 /* NOTE! _exit, not exit() */
}
