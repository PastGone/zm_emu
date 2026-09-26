#include "../../include/u_time.h"

#include <string.h> /* strlen / memcpy：把 ctime 结果拷进本函数缓冲 */
#include <time.h>

#include "../../include/u_mem.h"

/**
 * @file u_ctime.c
 * @brief u_ctime —— 时间戳转固定格式串
 *
 * 【为什么不用 ctime_r】
 * ctime_r 是 **POSIX 扩展，不是标准 C**：Windows 工具链（MinGW/MSVC）上
 * 没有声明，编译直接报错
 *     error: implicit declaration of function 'ctime_r';
 *            did you mean 'ctime'? [-Wimplicit-function-declaration]
 * 换标准 C 的 ctime：它返回宿主**静态缓冲**，本函数紧跟着就整份拷走 ——
 * 两条语句之间不再调用任何其它函数，静态缓冲没有机会被覆盖（模拟器单线程）。
 * 长度上限固定（C 标准："Www Mmm dd hh:mm:ss yyyy\n" 共 25 字符 + NUL），
 * 64 字节缓冲足够。
 *
 * 同族的 asctime/gmtime/localtime 三个文件是同一处改动，理由相同。
 */
uint32_t u_ctime(uc_engine *uc, uint32_t timer_ptr, uint32_t buf) {
	time_t t = (time_t)(timer_ptr ? (int64_t)u_rd32(uc, timer_ptr) : (int64_t)time(NULL));
	char tmp[64];

	const char *s = ctime(&t);
	if (!s)
		return 0; /* 与实机同语义：ctime 只在时间戳超出可表示范围时返回 NULL */

	size_t n = strlen(s);
	if (n >= sizeof(tmp))
		n = sizeof(tmp) - 1; /* 防御：无论如何不溢出本函数的缓冲 */
	memcpy(tmp, s, n);
	tmp[n] = '\0';

	return u_write_cstr(uc, buf, tmp) ? buf : 0;
}
