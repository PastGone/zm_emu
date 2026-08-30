#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_log.c
 * @brief u_log —— 【纯透传】转发宿主 libm 的 log
 *
 * 纯标量运算，不涉及客户机地址空间。
 */

double u_log(double x) { return log(x); }
