#ifndef _TIMES_H
#define _TIMES_H

#include <sys/types.h>

struct tms {
	time_t tms_utime;	// 用户态执行时间（user time）
	time_t tms_stime;	// 内核态执行时间（system time）
	time_t tms_cutime;	// 子进程的用户态时间（cumulative user time）
	time_t tms_cstime;	// 子进程的内核态时间（cumulative system time）
};

extern time_t times(struct tms * tp);

#endif
