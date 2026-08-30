#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_log10.c
 * @brief u_log10 —— 【纯透传】转发宿主 libm 的 log10
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_log10(double x) { return log10(x); }
