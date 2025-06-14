#ifndef _TIME_H
#define _TIME_H

#ifndef _TIME_T
#define _TIME_T
typedef long time_t;
#endif

#ifndef _SIZE_T
#define _SIZE_T
typedef unsigned int size_t;
#endif

#define CLOCKS_PER_SEC 100

typedef long clock_t;

struct tm {
	int tm_sec;  // 秒 (0 - 60)
	int tm_min;	 // 分 (0 - 59)
	int tm_hour; // 小时 (0 - 23)
	int tm_mday; // 月份中的第几天 (1 - 31)
	int tm_mon;  // 月份 (0 - 11，0 表示 1 月)
	int tm_year; // 年份减去 1900（例如：2025 -> 125）
	int tm_wday; // 星期几 (0 - 6, 0 表示星期日)
	int tm_yday; // 一年中的第几天 (0 - 365)
	int tm_isdst;// 夏令时标志
};

clock_t clock(void);
time_t time(time_t * tp);
double difftime(time_t time2, time_t time1);
time_t mktime(struct tm * tp);

char * asctime(const struct tm * tp);
char * ctime(const time_t * tp);
struct tm * gmtime(const time_t *tp);
struct tm *localtime(const time_t * tp);
size_t strftime(char * s, size_t smax, const char * fmt, const struct tm * tp);
void tzset(void);

#endif
