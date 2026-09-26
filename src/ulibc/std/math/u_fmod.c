#include "../../include/u_math.h"

#include <math.h>

/**
 * @file u_fmod.c
 * @brief u_fmod —— 【纯透传】转发宿主 libm 的 fmod
 */

double u_fmod(double x, double y) {
	return fmod(x, y);
}
