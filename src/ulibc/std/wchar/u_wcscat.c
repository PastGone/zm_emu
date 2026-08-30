#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcscat.c
 * @brief u_wcscat —— 追加 UCS-2 宽串
 */

uint32_t u_wcscat(uc_engine *uc, uint32_t dst, uint32_t src) {
  if (!uc || dst == 0 || src == 0)
    return dst;
  uint32_t dlen = u_wcslen(uc, dst);
  uint32_t slen = u_wcslen(uc, src);
  u_memcpy(uc, dst + dlen * 2u, src, slen * 2u);
  u_wr16(uc, dst + (dlen + slen) * 2u, 0);
  return dst;
}
