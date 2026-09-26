#ifndef U_TIME_H
#define U_TIME_H
/**
 * @file u_time.h
 * @brief 时间函数 —— 标准 C（time.h）【结构体翻译类】
 *
 * 关键点：struct tm 在 32 位 ARM 与 64 位宿主上的字段宽度/布局不同，
 * 不能把宿主 struct tm 整块 memcpy 进客户机。本模块**逐字段**写入
 * 客户机内存，布局固定为 9 个 int32（不含 tm_gmtoff / tm_zone）：
 *
 *   +0  tm_sec    +4  tm_min    +8  tm_hour  +12 tm_mday
 *   +16 tm_mon    +20 tm_year   +24 tm_wday  +28 tm_yday
 *   +32 tm_isdst
 *
 * ⚠ 非标准扩展（u_tick_ms 单调计时）见 u_time_ext.h。
 */
#include <stdint.h>
#include <time.h>
#include <unicorn/unicorn.h>

/** 客户机 struct tm 的大小（字节） */
#define U_TM_SIZE 36u

/** time()：timer_ptr 非 0 时同时写回客户机，返回秒数 */
int64_t u_time(uc_engine *uc, uint32_t timer_ptr);

/** difftime：end - begin */
double u_difftime(int64_t end, int64_t begin);

/** localtime / gmtime：把 timer_ptr 指向的时间拆到 tm_ptr；返回 tm_ptr */
uint32_t u_localtime(uc_engine *uc, uint32_t timer_ptr, uint32_t tm_ptr);
uint32_t u_gmtime(uc_engine *uc, uint32_t timer_ptr, uint32_t tm_ptr);

/** mktime：把客户机 struct tm 转回时间戳 */
int64_t u_mktime(uc_engine *uc, uint32_t tm_ptr);

/** asctime / ctime：结果写入客户机 buf，返回 buf */
uint32_t u_asctime(uc_engine *uc, uint32_t tm_ptr, uint32_t buf);
uint32_t u_ctime(uc_engine *uc, uint32_t timer_ptr, uint32_t buf);

/**
 * strftime：按客户机格式串格式化到客户机 dst，最多 max 字节
 * @return 写入的字符数（不含 '\0'）
 */
uint32_t u_strftime(uc_engine *uc, uint32_t dst, uint32_t max, uint32_t fmt_ptr, uint32_t tm_ptr);

/** clock()：进程 CPU 时间（clock_t 滴答数） */
int64_t u_clock(void);

/** clock() 的毫秒版本（便利性非标准补充） */
int64_t u_clock_ms(void);

#endif /* U_TIME_H */
