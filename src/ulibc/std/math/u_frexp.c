#include "../../include/u_math.h"

#include <math.h>

#include "../../include/u_mem.h"

/**
 * @file u_frexp.c
 * @brief u_frexp —— 拆分为尾数与指数
 *
 * 指数通过**客户机指针**写回（4 字节 int）。
 */

double u_frexp(uc_engine *uc, double x, uint32_t exptr) {
  int e = 0;
  double m = frexp(x, &e);
  if (uc && exptr)
    u_wr32(uc, exptr, (uint32_t)e);
  return m;
}
