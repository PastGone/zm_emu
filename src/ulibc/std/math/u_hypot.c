#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_hypot.c
 * @brief u_hypot —— 【纯透传】转发宿主 libm 的 hypot（C99）
 */

double u_hypot(double x, double y) {
	return hypot(x, y);
}
