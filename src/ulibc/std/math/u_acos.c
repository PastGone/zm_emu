#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_acos.c
 * @brief u_acos —— 【纯透传】转发宿主 libm 的 acos
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_acos(double x) { return acos(x); }
