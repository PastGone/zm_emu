#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_sinh.c
 * @brief u_sinh —— 【纯透传】转发宿主 libm 的 sinh
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_sinh(double x) { return sinh(x); }
