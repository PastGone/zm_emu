#include "../../include/u_math_ext.h"

/**
 * @file u_fixed_to_float.c
 * @brief u_fixed_to_float —— 定点转浮点（非标准）
 */

double u_fixed_to_float(int32_t v, int frac_bits) {
  return (double)v / (double)(1 << frac_bits);
}
