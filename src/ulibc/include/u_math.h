#ifndef U_MATH_H
#define U_MATH_H
/**
 * @file u_math.h
 * @brief 数学函数 —— 标准 C（math.h）【纯透传类】
 *
 * 纯标量运算（double 进 double 出）不涉及地址空间，可直接转发宿主 libm。
 *
 * 例外：modf / frexp 带**客户机指针**出参，必须在宿主侧取到结果后写回
 * 客户机内存，因此这两个函数带 uc 参数。
 *
 * ⚠ 非标准扩展（定点数换算 u_fixed_* / u_float_to_fixed）见 u_math_ext.h。
 */
#include <stdint.h>
#include <unicorn/unicorn.h>

double u_sqrt(double x);
double u_pow(double x, double y);
double u_hypot(double x, double y);
double u_fabs(double x);
double u_floor(double x);
double u_ceil(double x);
double u_fmod(double x, double y);

double u_sin(double x);
double u_cos(double x);
double u_tan(double x);
double u_asin(double x);
double u_acos(double x);
double u_atan(double x);
double u_atan2(double y, double x);
double u_sinh(double x);
double u_cosh(double x);
double u_tanh(double x);

double u_exp(double x);
double u_log(double x);
double u_log10(double x);
double u_log2(double x);  /* C99 */
double u_ldexp(double x, int e);

/** modf：小数部分作为返回值，整数部分（double）写回客户机 iptr */
double u_modf(uc_engine *uc, double x, uint32_t iptr);

/** frexp：尾数作为返回值，指数写回客户机 exptr（4 字节 int） */
double u_frexp(uc_engine *uc, double x, uint32_t exptr);

#endif /* U_MATH_H */
