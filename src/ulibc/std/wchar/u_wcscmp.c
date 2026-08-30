#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcscmp.c
 * @brief u_wcscmp —— 比较 UCS-2 宽串
 */

int u_wcscmp(uc_engine *uc, uint32_t a, uint32_t b) {
  if (!uc || a == b)
    return 0;
  uint32_t la = u_wcslen(uc, a);
  uint32_t lb = u_wcslen(uc, b);
  uint32_t m = (la < lb) ? la : lb;
  for (uint32_t i = 0; i < m; i++) {
    uint16_t x = u_rd16(uc, a + i * 2u);
    uint16_t y = u_rd16(uc, b + i * 2u);
    if (x != y)
      return (int)x - (int)y;
  }
  if (la == lb)
    return 0;
  return (la < lb) ? -1 : 1;
}
