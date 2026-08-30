#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcsncat.c
 * @brief u_wcsncat —— 最多追加 n 个宽字符，并补终止符
 */

uint32_t u_wcsncat(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
  if (!uc || dst == 0 || src == 0 || n == 0)
    return dst;
  uint32_t dlen = u_wcslen(uc, dst);

  uint32_t slen = 0;
  while (slen < n && u_rd16(uc, src + slen * 2u) != 0)
    slen++;

  if (slen)
    u_memcpy(uc, dst + dlen * 2u, src, slen * 2u);
  u_wr16(uc, dst + (dlen + slen) * 2u, 0);
  return dst;
}
