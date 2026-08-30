#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_sqrt.c
 * @brief u_sqrt —— 【纯透传】转发宿主 libm 的 sqrt
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_sqrt(double x) { return sqrt(x); }
