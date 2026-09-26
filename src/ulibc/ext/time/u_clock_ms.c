#include "../../include/u_time.h"

#include <time.h>

/**
 * @file u_clock_ms.c
 * @brief u_clock_ms —— 进程 CPU 时间（毫秒）
 *
 * 注意语义：clock() 测的是 CPU 时间，不是墙上时钟。
 * 需要单调计时请用 ext/time 的 u_tick_ms。
 */

int64_t u_clock_ms(void) {
	return (int64_t)(clock() * 1000 / CLOCKS_PER_SEC);
}
