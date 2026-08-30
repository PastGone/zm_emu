#include "../../include/u_stdlib.h"

/**
 * @file u_abs.c
 * @brief u_abs / u_labs / u_llabs —— 整数绝对值
 *
 * 自己实现而非转发宿主：避免 INT_MIN 上的未定义行为。
 */

int u_abs(int v) { return (v < 0) ? -v : v; }
long u_labs(long v) { return (v < 0) ? -v : v; }
long long u_llabs(long long v) { return (v < 0) ? -v : v; }
