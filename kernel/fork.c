/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <string.h>
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f); //0x0f=0x00001（索引1）1（0=gdt,1=ldt）11（3用户态） 当前进程的代码段大小 单位字节
	data_limit=get_limit(0x17); //0x17=0x00010（索引2）1（0=gdt,1=ldt) 11（3用户态） 当前进程的数据段大小 单位字节
	old_code_base = get_base(current->ldt[1]);  //代码段基地址
	old_data_base = get_base(current->ldt[2]);	//数据段基地址
	if (old_data_base != old_code_base) //如果代码段和数据段的基地址不是同一个 系统崩溃。目前不支持代码段和数据段分离
		panic("We don't support separate I&D");
	if (data_limit < code_limit) //数据段大小必须大于代码段大小 系统崩溃
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * 0x4000000; //每个进程分配64MB（即 0x4000000 字节）
	p->start_code = new_code_base;	//记录新进程的代码段起始地址
	set_base(p->ldt[1],new_code_base); 	//将新的基地址写入子进程的 LDT 描述符中
	set_base(p->ldt[2],new_data_base);	
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) { //复制父进程的页表到子进程, 页表复制 + 共享物理页面
		printk("free_page_tables: from copy_mem\n");
		free_page_tables(new_data_base,data_limit); //如果复制失败，则释放已分配的页表
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(int nr/*find_empty_process找到的函数索引*/,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss) //copy程序
{
	struct task_struct *p;
	int i;
	struct file *f;

	p = (struct task_struct *) get_free_page(); //申请页内存。类似于kmalloc(sizeof(task_struct))
	if (!p)
		return -EAGAIN;
	task[nr] = p;					//插入槽中
	
	// NOTE!: the following statement now work with gcc 4.3.2 now, and you
	// must compile _THIS_ memcpy without no -O of gcc.#ifndef GCC4_3
	*p = *current;	//拷贝大部分属性 栈不会拷贝 /* NOTE! this doesn't copy the supervisor stack */
	p->state = TASK_UNINTERRUPTIBLE; //状态设置为不可运行状态
	p->pid = last_pid;				 //使用之前分配的唯一 PID
	p->father = current->pid;		 //父pid为当前进程的pid
	p->counter = p->priority;		 //时间片=优先级
	p->signal = 0;					 //清除信号
	p->alarm = 0;					 //警报
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;		 //运行时间
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;		 //进程开始时间
	p->tss.back_link = 0;			 	//tss任务状态段
	p->tss.esp0 = PAGE_SIZE + (long) p; //内核栈的栈顶地址（每个进程有自己的内核栈）
	p->tss.ss0 = 0x10;					
	p->tss.eip = eip;					//程序执行起点（通常是系统调用返回后继续执行的位置）
	p->tss.eflags = eflags;
	p->tss.eax = 0;						//这是子进程 fork() 返回值的关键（fork() 返回 0）
	p->tss.ecx = ecx;					//其他寄存器
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)					//如果当前进程使用了协处理器，那就设置当前创建进程的协处理器（如浮点运算）
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387)); //
	if (copy_mem(nr,p)) {	//拷贝内存成功返回0 负责为子进程建立页表映射（可能是只读共享，之后写时复制）
		task[nr] = NULL;	
		free_page((long) p);
		return -EAGAIN;
	}
	for (i=0; i<NR_OPEN;i++) //继承父进程的打开的fd (fd的引用计数增加1）
		if ((f=p->filp[i]))
			f->f_count++;	
	if (current->pwd)	//当前目录 引用计数+1
		current->pwd->i_count++;
	if (current->root)	//根目录 引用计数+1
		current->root->i_count++;
	if (current->executable) //可执行文件 引用计数+1
		current->executable->i_count++;
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss)); //将子进程的 TSS 和 LDT 地址写入 GDT（全局描述符表）
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt)); // nr << 1 占用2个描述符
	p->state = TASK_RUNNING;	//设置完成设置进程 可运行状态 /* do this last, just in case */
	return last_pid;
}

int find_empty_process(void) //找空闲进程槽位
{
	int i;

	repeat: //刷新全局变量last_pid 用于后面创建进程pid
		if ((++last_pid)<0) last_pid=1; //超出最大long +1 会变负数。则重制为1
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i; //空闲进程槽位
	return -EAGAIN;
}
