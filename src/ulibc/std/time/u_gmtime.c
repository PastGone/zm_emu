#include "../../include/u_time.h"

#include <string.h>
#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_gmtime.c
 * @brief u_gmtime —— 按 UTC 拆分时间戳（依赖 u_tm_store）
 *
 * 【为什么不用 gmtime_r】
 * gmtime_r 是 POSIX 扩展，Windows 工具链（MinGW/MSVC）上没有声明 →
 * `implicit declaration of function 'gmtime_r'` 编译报错。
 * 标准 C 的 gmtime 返回宿主**静态缓冲**，这里取到后**立刻整份拷进局部变量**
 * 再写客户机内存，两条语句之间不调用任何其它函数 → 静态缓冲不会有机会被覆盖
 * （模拟器单线程；真要并行也只会各自拷贝后再用，不共享该缓冲）。
 *
 * 注：时间戳越界时 gmtime 返回 NULL，此时保持 memset 出来的全 0 结构，
 * 与真机 gmtime_r 失败的行为一致。
 */
uint32_t u_gmtime(uc_engine *uc, uint32_t timer_ptr, uint32_t tm_ptr) {
	time_t t = (time_t)(timer_ptr ? (int64_t)u_rd32(uc, timer_ptr) : (int64_t)time(NULL));
	struct tm gt;
	memset(&gt, 0, sizeof(gt));

	const struct tm *p = gmtime(&t);
	if (p)
		gt = *p;

	u_tm_store(uc, tm_ptr, &gt);
	return tm_ptr;
}
