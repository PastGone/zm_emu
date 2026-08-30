#include "../../include/u_stdlib.h"

#include <limits.h>

#include "../../include/internal/u_conv_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_strtoul.c
 * @brief u_strtoul / u_strtoul_ex —— 字符串转 unsigned long
 *
 * 标准：接受前导负号并按无符号取负。
 */

unsigned long u_strtoul_ex(uc_engine *uc, uint32_t s, uint32_t *end_out,
                           int base) {
  uint32_t end = s;
  int neg = 0, ovf = 0;
  uint64_t acc = u_conv_scan_uint(uc, s, base, &end, &neg, &ovf);

  if (end_out)
    *end_out = end;
  if (!acc && end == s)
    return 0;

  if (neg)
    acc = (uint64_t)(0ULL - acc);
  if (ovf || acc > (uint64_t)ULONG_MAX) {
    u_errno = U_ERANGE;
    return ULONG_MAX;
  }
  return (unsigned long)acc;
}

unsigned long u_strtoul(uc_engine *uc, uint32_t s, uint32_t endptr_addr,
                        int base) {
  uint32_t end = s;
  unsigned long v = u_strtoul_ex(uc, s, &end, base);
  if (endptr_addr)
    u_wr32(uc, endptr_addr, end);
  return v;
}
