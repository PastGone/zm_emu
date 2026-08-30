#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_ldexp.c
 * @brief u_ldexp —— 【纯透传】转发宿主 libm 的 ldexp
 */

double u_ldexp(double x, int e) { return ldexp(x, e); }
