/*
 *  linux/kernel/traps.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'Traps.c' handles hardware traps and faults after we have saved some
 * state in 'asm.s'. Currently mostly a debugging-aid, will be extended
 * to mainly kill the offending process (probably by giving it a signal,
 * but possibly by killing it outright if necessary).
 */
#include <string.h> 

#include <linux/head.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/io.h>

#define get_seg_byte(seg,addr) ({ \
register char __res; \
__asm__("push %%fs;mov %%ax,%%fs;movb %%fs:%2,%%al;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})

#define get_seg_long(seg,addr) ({ \
register unsigned long __res; \
__asm__("push %%fs;mov %%ax,%%fs;movl %%fs:%2,%%eax;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})

#define _fs() ({ \
register unsigned short __res; \
__asm__("mov %%fs,%%ax":"=a" (__res):); \
__res;})

int do_exit(long code);

void page_exception(void);

void divide_error(void);  //具体函数实现 kernel/asm.s
void debug(void);
void nmi(void);
void int3(void);
void overflow(void);
void bounds(void);
void invalid_op(void);
void device_not_available(void);
void double_fault(void);
void coprocessor_segment_overrun(void);
void invalid_TSS(void);
void segment_not_present(void);
void stack_segment(void);
void general_protection(void);
void page_fault(void);
void coprocessor_error(void);
void reserved(void);
void parallel_interrupt(void);
void irq13(void);

static void die(char * str,long esp_ptr,long nr)
{
	long * esp = (long *) esp_ptr;
	int i;

	printk("%s: %04x\n\r",str,nr&0xffff);
	printk("EIP:\t%04x:%p\nEFLAGS:\t%p\nESP:\t%04x:%p\n",
		esp[1],esp[0],esp[2],esp[4],esp[3]);
	printk("fs: %04x\n",_fs());
	printk("base: %p, limit: %p\n",get_base(current->ldt[1]),get_limit(0x17));
	if (esp[4] == 0x17) {
		printk("Stack: ");
		for (i=0;i<4;i++)
			printk("%p ",get_seg_long(0x17,i+(long *)esp[3]));
		printk("\n");
	}
	str(i);
	printk("Pid: %d, process nr: %d\n\r",current->pid,0xffff & i);
	for(i=0;i<10;i++)
		printk("%02x ",0xff & get_seg_byte(esp[1],(i+(char *)esp[0])));
	printk("\n\r");
	do_exit(11);		/* play segment exception */
}

void do_double_fault(long esp, long error_code)
{
	die("double fault",esp,error_code);
}

void do_general_protection(long esp, long error_code)
{
	die("general protection",esp,error_code);
}

void do_divide_error(long esp, long error_code)
{
	die("divide error",esp,error_code);
}

void do_int3(long * esp, long error_code,
		long fs,long es,long ds,
		long ebp,long esi,long edi,
		long edx,long ecx,long ebx,long eax)
{
	int tr;

	__asm__("str %%ax":"=a" (tr):"0" (0));
	printk("eax\t\tebx\t\tecx\t\tedx\n\r%8x\t%8x\t%8x\t%8x\n\r",
		eax,ebx,ecx,edx);
	printk("esi\t\tedi\t\tebp\t\tesp\n\r%8x\t%8x\t%8x\t%8x\n\r",
		esi,edi,ebp,(long) esp);
	printk("\n\rds\tes\tfs\ttr\n\r%4x\t%4x\t%4x\t%4x\n\r",
		ds,es,fs,tr);
	printk("EIP: %8x   CS: %4x  EFLAGS: %8x\n\r",esp[0],esp[1],esp[2]);
}

void do_nmi(long esp, long error_code)
{
	die("nmi",esp,error_code);
}

void do_debug(long esp, long error_code)
{
	die("debug",esp,error_code);
}

void do_overflow(long esp, long error_code)
{
	die("overflow",esp,error_code);
}

void do_bounds(long esp, long error_code)
{
	die("bounds",esp,error_code);
}

void do_invalid_op(long esp, long error_code)
{
	die("invalid operand",esp,error_code);
}

void do_device_not_available(long esp, long error_code)
{
	die("device not available",esp,error_code);
}

void do_coprocessor_segment_overrun(long esp, long error_code)
{
	die("coprocessor segment overrun",esp,error_code);
}

void do_invalid_TSS(long esp,long error_code)
{
	die("invalid TSS",esp,error_code);
}

void do_segment_not_present(long esp,long error_code)
{
	die("segment not present",esp,error_code);
}

void do_stack_segment(long esp,long error_code)
{
	die("stack segment",esp,error_code);
}

void do_coprocessor_error(long esp, long error_code)
{
	if (last_task_used_math != current)
		return;
	die("coprocessor error",esp,error_code);
}

void do_reserved(long esp, long error_code)
{
	die("reserved (15,17-47) error",esp,error_code);
}

void trap_init(void) //在 x86 架构中，CPU 使用 中断描述符表（Interrupt Descriptor Table, IDT） 来记录每个中断或异常对应的处理程序地址。每个表项是一个 门描述符（Gate Descriptor）
{
	int i;
	//set_trap_gate 陷阱门 只允许内核态。set_system_gate 调用门允许用户态
	set_trap_gate(0,&divide_error);	// 除法错误.
	set_trap_gate(1,&debug);		// 调试异常
	set_trap_gate(2,&nmi);			// 非屏蔽中断
	set_system_gate(3,&int3);	    // 断点指令 int3/* int3-5 can be called from all */ 
	set_system_gate(4,&overflow);	// 溢出中断 int4
	set_system_gate(5,&bounds);		// 边界检查中断 int5
	set_trap_gate(6,&invalid_op);	// 无效操作码
	set_trap_gate(7,&device_not_available);// 设备不可用（如协处理器）
	set_trap_gate(8,&double_fault);	// 双重故障
	set_trap_gate(9,&coprocessor_segment_overrun); // 协处理器段越界
	set_trap_gate(10,&invalid_TSS);	// TSS 无效
	set_trap_gate(11,&segment_not_present); // 段未加载
	set_trap_gate(12,&stack_segment);		// 堆栈段错误
	set_trap_gate(13,&general_protection);	// 一般保护异常
	set_trap_gate(14,&page_fault);			// 页面错误
	set_trap_gate(15,&reserved);			// 保留
	set_trap_gate(16,&coprocessor_error);	// 协处理器错误
	for (i=17;i<48;i++)						// 预留7-47 这些中断号尚未被使用，统一指向 reserved 处理函数。 后续可以动态注册外部设备中断。
		set_trap_gate(i,&reserved);
	set_trap_gate(45,&irq13);				// 把中断号 45（对应 IRQ13）设置为调用 irq13 处理函数。这个函数通常用于捕获协处理器（FPU）错误。 主 PIC（8259A）映射到中断号 0x20~0x27（即 32~39 IRQ0-7），从片映射到 0x28~0x2F（即 40~47 IRQ8-15）。  45=IRQ13
	outb_p(inb_p(0x21)&0xfb,0x21);			// 读取主 PIC 的中断屏蔽寄存器（IMR），清除第 2 位（bit 2），然后写回，从而启用 IRQ2 中断线。0x21 是 主 PIC 的 IMR（Interrupt Mask Register）端口地址。0xFB = 1111 1011，表示把第 2 位设为 0，其他位不变 即允许 IRQ2 中断（通常用于连接从片）。inb_p 读取 outb_p 写回+停顿
	outb(inb_p(0xA1)&0xdf,0xA1);			// 读取从 PIC 的中断屏蔽寄存器（IMR），清除第 3 位（bit 3），然后写回，从而启用 IRQ9 中断线。0xA1 是 从 PIC 的 IMR 端口地址。0xDF = 1101 1111，表示把第 3 位置为 0。即允许 IRQ11 中断。inb_p 读取 写回新的 IMR 值
	set_trap_gate(39,&parallel_interrupt);  // 39 = 0x27，属于主 PIC 的中断范围（0x20~0x27）。IRQ7  -> 并口中断
}
