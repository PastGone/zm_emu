#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_pow.c
 * @brief u_pow —— 【纯透传】转发宿主 libm 的 pow
 */

double u_pow(double x, double y) { return pow(x, y); }
