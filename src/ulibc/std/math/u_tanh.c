#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_tanh.c
 * @brief u_tanh —— 【纯透传】转发宿主 libm 的 tanh
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_tanh(double x) { return tanh(x); }
