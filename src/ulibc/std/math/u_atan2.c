#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_atan2.c
 * @brief u_atan2 —— 【纯透传】转发宿主 libm 的 atan2
 */

double u_atan2(double y, double x) { return atan2(y, x); }
