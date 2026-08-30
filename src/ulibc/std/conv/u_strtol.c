#include "../../include/u_stdlib.h"

#include <limits.h>

#include "../../include/internal/u_conv_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_strtol.c
 * @brief u_strtol / u_strtol_ex —— 字符串转 long
 *
 * _ex 变体是非标准便利接口：结束位置直接返回到宿主变量，
 * 不触碰客户机内存。
 */

long u_strtol_ex(uc_engine *uc, uint32_t s, uint32_t *end_out, int base) {
  uint32_t end = s;
  int neg = 0, ovf = 0;
  uint64_t acc = u_conv_scan_uint(uc, s, base, &end, &neg, &ovf);

  if (end_out)
    *end_out = end;
  if (!acc && end == s)
    return 0;

  if (neg) {
    uint64_t limit = (uint64_t)LONG_MAX + 1ULL;
    if (ovf || acc > limit) {
      u_errno = U_ERANGE;
      return LONG_MIN;
    }
    if (acc == limit)
      return LONG_MIN;
    return -(long)acc;
  }
  if (ovf || acc > (uint64_t)LONG_MAX) {
    u_errno = U_ERANGE;
    return LONG_MAX;
  }
  return (long)acc;
}

long u_strtol(uc_engine *uc, uint32_t s, uint32_t endptr_addr, int base) {
  uint32_t end = s;
  long v = u_strtol_ex(uc, s, &end, base);
  if (endptr_addr)
    u_wr32(uc, endptr_addr, end);
  return v;
}
