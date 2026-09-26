#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_log2.c
 * @brief u_log2 —— 【纯透传】转发宿主 libm 的 log2
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_log2(double x) {
	return log2(x);
}
