#include "../../include/u_math_ext.h"

/**
 * @file u_fixed_mul.c
 * @brief u_fixed_mul —— 定点乘，结果右移 frac_bits（非标准）
 */

int32_t u_fixed_mul(int32_t a, int32_t b, int frac_bits) {
  return (int32_t)(((int64_t)a * (int64_t)b) >> frac_bits);
}
