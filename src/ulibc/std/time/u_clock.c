#include "../../include/u_time.h"

#include <time.h>

/**
 * @file u_clock.c
 * @brief u_clock —— 进程 CPU 时间（clock_t 滴答数）
 *
 * 标准 clock() 返回 CLOCKS_PER_SEC 为单位的滴答数。
 * 另有 u_clock_ms() 提供毫秒版本（见 std/time/u_clock_ms.c）。
 */

int64_t u_clock(void) { return (int64_t)clock(); }
