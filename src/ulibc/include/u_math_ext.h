#ifndef U_MATH_EXT_H
#define U_MATH_EXT_H
/**
 * @file u_math_ext.h
 * @brief 定点数运算 —— **非标准**扩展
 *
 * 标准 C 里有浮点（float/double），没有定点数。这些是 zmaee 需要的：
 * 原始固件里有 zmaee_fixed_div / zmaee_f_assignint / zmaee_f_assignuint /
 * zmaee_f_cmp / zmaee_f_op 等定点运算接口，MT6250 这类不带 FPU 的
 * 基带芯片上普遍用定点替代浮点。
 *
 * frac_bits 表示小数部分占用的位数（常见 8 / 12 / 16）。
 */
#include <stdint.h>

/** 浮点 → 定点（四舍五入） */
int32_t u_float_to_fixed(double v, int frac_bits);

/** 定点 → 浮点 */
double u_fixed_to_float(int32_t v, int frac_bits);

/** 定点乘：结果右移 frac_bits */
int32_t u_fixed_mul(int32_t a, int32_t b, int frac_bits);

/** 定点除：先左移 frac_bits 再除；除数为 0 返回 0 */
int32_t u_fixed_div(int32_t a, int32_t b, int frac_bits);

#endif /* U_MATH_EXT_H */
