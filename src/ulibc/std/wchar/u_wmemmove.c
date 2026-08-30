#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wmemmove.c
 * @brief u_wmemmove —— 允许重叠的宽字符块复制（依赖 u_memmove）
 */

uint32_t u_wmemmove(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
  if (!uc || n == 0)
    return dst;
  u_memmove(uc, dst, src, n * 2u);
  return dst;
}
