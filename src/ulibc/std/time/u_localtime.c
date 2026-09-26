#include "../../include/u_time.h"

#include <string.h>
#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_localtime.c
 * @brief u_localtime —— 按本地时区拆分时间戳（依赖 u_tm_store）
 *
 * 【为什么不用 localtime_r】
 * localtime_r 是 POSIX 扩展，Windows 工具链（MinGW/MSVC）上没有声明 →
 * `implicit declaration of function 'localtime_r'` 编译报错。
 *
 * 以前选 _r 版本的理由是"避免宿主静态缓冲被模拟器自身覆盖"——这个顾虑用
 * **取到即整份拷贝**同样能解决：localtime 返回后立刻 `lt = *p`，
 * 两条语句之间不调用任何其它函数（日志/其它 host 代码都插不进来），
 * 之后再慢慢写客户机内存。时间戳越界时 localtime 返回 NULL，保持全 0
 * 结构，与真机 localtime_r 失败一致。
 */
uint32_t u_localtime(uc_engine *uc, uint32_t timer_ptr, uint32_t tm_ptr) {
	time_t t = (time_t)(timer_ptr ? (int64_t)u_rd32(uc, timer_ptr) : (int64_t)time(NULL));
	struct tm lt;
	memset(&lt, 0, sizeof(lt));

	const struct tm *p = localtime(&t);
	if (p)
		lt = *p;

	u_tm_store(uc, tm_ptr, &lt);
	return tm_ptr;
}
