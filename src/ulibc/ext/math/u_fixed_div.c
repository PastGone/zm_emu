#include "../../include/u_math_ext.h"

/**
 * @file u_fixed_div.c
 * @brief u_fixed_div —— 定点除，先左移再除；除数为 0 返回 0（非标准）
 *
 * 对应 zmaee_fixed_div。
 */

int32_t u_fixed_div(int32_t a, int32_t b, int frac_bits) {
	if (b == 0)
		return 0;
	return (int32_t)(((int64_t)a << frac_bits) / (int64_t)b);
}
