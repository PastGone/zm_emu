#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_sin.c
 * @brief u_sin —— 【纯透传】转发宿主 libm 的 sin
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_sin(double x) { return sin(x); }
