#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strncmp.c
 * @brief u_strncmp —— 限定长度的比较
 */

int u_strncmp(uc_engine *uc, uint32_t a, uint32_t b, uint32_t n) {
  if (!uc || n == 0)
    return 0;
  if (a == b)
    return 0;
  uint32_t la = u_strnlen(uc, a, n);
  uint32_t lb = u_strnlen(uc, b, n);
  uint32_t m = (la < lb) ? la : lb;
  if (m) {
    int r = u_memcmp(uc, a, b, m);
    if (r)
      return r;
  }
  if (la == lb)
    return 0;
  return (la < lb) ? -1 : 1;
}
