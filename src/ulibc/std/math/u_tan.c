#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_tan.c
 * @brief u_tan —— 【纯透传】转发宿主 libm 的 tan
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_tan(double x) {
	return tan(x);
}
