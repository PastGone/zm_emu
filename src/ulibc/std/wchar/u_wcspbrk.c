#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcspbrk.c
 * @brief u_wcspbrk —— 首个出现在 accept 中字符的地址（依赖 u_wcscspn）
 */

uint32_t u_wcspbrk(uc_engine *uc, uint32_t s, uint32_t accept) {
  if (!uc || s == 0)
    return 0;
  uint32_t off = u_wcscspn(uc, s, accept);
  uint32_t len = u_wcslen(uc, s);
  if (off >= len)
    return 0;
  return s + off * 2u;
}
