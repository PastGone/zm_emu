#include "../../include/u_math_ext.h"

/**
 * @file u_float_to_fixed.c
 * @brief u_float_to_fixed —— 浮点转定点（四舍五入，饱和处理）
 *
 * 非标准：对应 zmaee 的 zmaee_f_assignint / zmaee_f_assignuint。
 */

int32_t u_float_to_fixed(double v, int frac_bits) {
	double s = (double)(1 << frac_bits);
	double r = v * s;
	if (r >= 2147483647.0)
		return 2147483647;
	if (r <= -2147483648.0)
		return (int32_t)(-2147483647 - 1);
	return (int32_t)(r >= 0 ? r + 0.5 : r - 0.5);
}
