/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ) // 主板上的晶体振荡器频率为 14.31818 MHz /12

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0;//一个全局变量，记录系统自启动以来经历的时钟中断次数。
long startup_time=0;
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) //遍历所有任务（从最后一个到第一个）
		if (*p) { //存在任务
			if ((*p)->alarm && (*p)->alarm < jiffies) { //已经到期 （小于jiffies)
					(*p)->signal |= (1<<(SIGALRM-1)); //发送SIGALRM
					(*p)->alarm = 0;				  //清空alarm
				}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE) //如果未被阻塞 （_BLOCKABLE）且当前状态是TASK_INTERRUPTIBLE
				(*p)->state=TASK_RUNNING; //修改状态 表示可以参与调度
		}

/* this is the scheduler proper: */

	while (1) { //找出当前拥有最多剩余时间片的任务
		c = -1;
		next = 0;
		i = NR_TASKS; 			//最后一个 往前找
		p = &task[NR_TASKS];	
		while (--i) {
			if (!*--p)		//task 是空 跳过
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c) //状态可运行 且 事件分片大于最大值
				c = (*p)->counter, next = i;	//更新最大的counter 和 任务索引
		}
		if (c) break; //找到了最大时间片 跳出循环
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)	//所有任务的时间分片为0。重制时间分片
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority; //时间分片 = counter >> 1 +  优先级
	}
	switch_to(next); //next 记录对应的进程编号
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;	//剩余滴答数
	void (*fn)();	//回调函数
	struct timer_list * next; //下一个
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl) //cpl：当前特权级（Current Privilege Level），表示当前运行的是用户态（3）还是内核态（0）
{
	extern int beepcount;		  // 用于控制 PC 扬声器发出蜂鸣的时间长度。
	extern void sysbeepstop(void);// 关闭蜂鸣的函数。

	if (beepcount)		//这是非常早期版本的简单蜂鸣机制，现代系统已经不再使用
		if (!--beepcount) //如果 beepcount > 0，则减一。
			sysbeepstop();//若减到 0，则调用 sysbeepstop() 关闭蜂鸣。

	if (cpl)
		current->utime++; //增加用户态运行时间 utime
	else
		current->stime++; //增加内核态运行时间 stime

	if (next_timer) {	//一个简单的定时器链表处理逻辑
		next_timer->jiffies--;	//将当前定时器的 jiffies 减一（倒计时）
		while (next_timer && next_timer->jiffies <= 0) { //如果 jiffies <= 0，说明该定时器已到期，调用其回调函数 fn()
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0) //current_DOR 是软盘控制器的数字输出寄存器（Digital Output Register）。
		do_floppy_timer();	//如果某些位被设置（如电机仍在运转），则调用 do_floppy_timer() 继续处理软盘状态
	if ((--current->counter)>0) return; //current->counter：当前进程剩余的时间片（timeslice）
	current->counter=0;
	if (!cpl) return;	//内核态不会被抢占
	schedule(); //如果是用户态，调用 schedule() 触发调度
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16) //因为在汇编或底层机制中，很多地方会硬编码偏移量来访问这个结构体字段。
		panic("Struct sigaction MUST be 16 bytes");
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss)); //gdt[4] 设置tss。
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt)); //gdt[5] 设置ldt 
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) { //task 保存所有进程的指针（最多 NR_TASKS 个，默认为 64）。
		task[i] = NULL; //清空task
		p->a=p->b=0;	//一个任务对应2个gdt对象
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl"); // 使用内联汇编清除 EFLAGS 中的 NT（Nested Task）位。 NT 位用于任务嵌套切换，不清除可能导致异常。
	ltr(0); //加载 TR（Task Register），指向 GDT 中第 0 个 TSS 描述符（其实是跳过前几个无效描述符后的第一个有效 TSS）。
	lldt(0);//加载 LDTR，指向第一个 LDT 描述符。
	outb_p(0x36,0x43);		//向 8253/8254 PIT 芯片发送命令，设置定时器 0 工作在模式 3（方波输出）。 /* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */ 
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt); //注册时钟中断处理函数（timer_interrupt）到0x20 当pit触发的时候跳转到timer_interrupt
	outb(inb_p(0x21)&~0x01,0x21);		//允许irq0事件 传入cpu
	set_system_gate(0x80,&system_call); //注册调用门 用户态也可调用 128中断事件 为系统调用函数
}
