#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcsrchr.c
 * @brief u_wcsrchr —— 查找宽字符最后一次出现（依赖 u_wcslen）
 */

uint32_t u_wcsrchr(uc_engine *uc, uint32_t s, u_wchar c) {
  if (!uc || s == 0)
    return 0;
  uint32_t len = u_wcslen(uc, s);
  if (c == 0)
    return s + len * 2u; /* 查终止符 → 返回终止符位置 */

  for (uint32_t i = len; i > 0;) {
    i--;
    if (u_rd16(uc, s + i * 2u) == c)
      return s + i * 2u;
  }
  return 0;
}
