#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_cosh.c
 * @brief u_cosh —— 【纯透传】转发宿主 libm 的 cosh
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_cosh(double x) { return cosh(x); }
