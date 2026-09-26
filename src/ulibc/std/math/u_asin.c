#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_asin.c
 * @brief u_asin —— 【纯透传】转发宿主 libm 的 asin
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_asin(double x) {
	return asin(x);
}
