/*
 *  linux/kernel/mktime.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <time.h>

/*
 * This isn't the library routine, it is only used in the kernel.
 * as such, we don't care about years<1970 etc, but assume everything
 * is ok. Similarly, TZ etc is happily ignored. We just do everything
 * as easily as possible. Let's find something public for the library
 * routines (although I think minix times is public).
 */
/*
 * PS. I hate whoever though up the year 1970 - couldn't they have gotten
 * a leap-year instead? I also hate Gregorius, pope or no. I'm grumpy.
 */
#define MINUTE 60
#define HOUR (60*MINUTE)
#define DAY (24*HOUR)
#define YEAR (365*DAY)

/* interestingly, we assume leap-years */
static int month[12] = {	//闰年
	0,
	DAY*(31),                              //1月之前
	DAY*(31+29),
	DAY*(31+29+31),
	DAY*(31+29+31+30),
	DAY*(31+29+31+30+31),
	DAY*(31+29+31+30+31+30),
	DAY*(31+29+31+30+31+30+31),
	DAY*(31+29+31+30+31+30+31+31),
	DAY*(31+29+31+30+31+30+31+31+30),
	DAY*(31+29+31+30+31+30+31+31+30+31),
	DAY*(31+29+31+30+31+30+31+31+30+31+30)	//12月前
};

long kernel_mktime(struct tm * tm) //将一个表示日期和时间的 struct tm 结构体（年、月、日、时、分、秒等）转换为从 1970 年 1 月 1 日 00:00:00 UTC 开始累计的秒数（Unix 时间戳），便于在内核中进行时间计算。
{
	long res; //最终返回的时间戳（单位为秒）
	int year; //调整后相对于 1970 年的年数
	if (tm->tm_year >= 70) //tm_year 是从1900年开始的
	  year = tm->tm_year - 70;
	else
	  year = tm->tm_year + 100 -70; /* Y2K bug fix by hellotigercn 20110803 */ // < 1970年做兼容
/* magic offsets (y+1) needed to get leapyears right.*/
	res = YEAR*year + DAY*((year+1)/4); //计算年份的天数（含闰年）
	res += month[tm->tm_mon];			//计算月份的天数
/* and (y+2) here. If it wasn't a leap-year, we have to adjust */
	if (tm->tm_mon>1 && ((year+2)%4))  	//非闰年 且 过了2月份之后 减去-1天
		res -= DAY;
	res += DAY*(tm->tm_mday-1);		   	// 加上当前月的天数（从 1 开始）
	res += HOUR*tm->tm_hour;		   	// 加上小时	
	res += MINUTE*tm->tm_min;		   	// 加上分钟
	res += tm->tm_sec;				   	// 加上秒
	return res;
}
