#include "../../include/u_stdlib.h"

#include <limits.h>

#include "../../include/internal/u_conv_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_strtoll.c
 * @brief u_strtoll / u_strtoll_ex —— 字符串转 long long
 */

long long u_strtoll_ex(uc_engine *uc, uint32_t s, uint32_t *end_out,
                       int base) {
  uint32_t end = s;
  int neg = 0, ovf = 0;
  uint64_t acc = u_conv_scan_uint(uc, s, base, &end, &neg, &ovf);

  if (end_out)
    *end_out = end;
  if (!acc && end == s)
    return 0;

  if (neg) {
    uint64_t limit = (uint64_t)LLONG_MAX + 1ULL;
    if (ovf || acc > limit) {
      u_errno = U_ERANGE;
      return LLONG_MIN;
    }
    if (acc == limit)
      return LLONG_MIN;
    return -(long long)acc;
  }
  if (ovf || acc > (uint64_t)LLONG_MAX) {
    u_errno = U_ERANGE;
    return LLONG_MAX;
  }
  return (long long)acc;
}

long long u_strtoll(uc_engine *uc, uint32_t s, uint32_t endptr_addr,
                    int base) {
  uint32_t end = s;
  long long v = u_strtoll_ex(uc, s, &end, base);
  if (endptr_addr)
    u_wr32(uc, endptr_addr, end);
  return v;
}
