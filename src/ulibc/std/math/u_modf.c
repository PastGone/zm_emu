#include "../../include/u_math.h"

#include <math.h>

#include "../../include/u_mem.h"

/**
 * @file u_modf.c
 * @brief u_modf —— 拆分整数/小数部分
 *
 * 与其余 math 函数不同：整数部分通过**客户机指针**写回，
 * 故必须带 uc 参数（标准 math.h 里 modf 无 uc，这里是等价的客户机版本）。
 */

double u_modf(uc_engine *uc, double x, uint32_t iptr) {
  double ipart = 0.0;
  double fpart = modf(x, &ipart);
  if (uc && iptr) {
    /* 显式按小端写 8 字节，避免宿主/客户机端序或 double 表示差异 */
    union {
      double d;
      uint64_t u;
    } cvt;
    cvt.d = ipart;
    uint64_t bits = cvt.u;
    u_wr32(uc, iptr, (uint32_t)(bits & 0xFFFFFFFFu));
    u_wr32(uc, iptr + 4, (uint32_t)(bits >> 32));
  }
  return fpart;
}
