#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"
#include "../../include/u_mem_ext.h"

/**
 * @file u_wmemset.c
 * @brief u_wmemset —— 按宽字符填充（依赖 u_memset16）
 */

uint32_t u_wmemset(uc_engine *uc, uint32_t dst, u_wchar c, uint32_t n) {
  if (!uc || n == 0)
    return dst;
  u_memset16(uc, dst, (uint16_t)c, n);
  return dst;
}
