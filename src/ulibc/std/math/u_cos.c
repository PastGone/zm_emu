#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_cos.c
 * @brief u_cos —— 【纯透传】转发宿主 libm 的 cos
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_cos(double x) { return cos(x); }
