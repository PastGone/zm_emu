#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_floor.c
 * @brief u_floor —— 【纯透传】转发宿主 libm 的 floor
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_floor(double x) {
	return floor(x);
}
