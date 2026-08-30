#include "../../include/u_wcs.h"

#include "../../include/u_mem.h"

/**
 * @file u_wcscspn.c
 * @brief u_wcscspn —— 首个出现在 reject 中字符之前的长度
 *
 * u_wcspbrk 依赖本函数。
 */

uint32_t u_wcscspn(uc_engine *uc, uint32_t s, uint32_t reject) {
  if (!uc || s == 0)
    return 0;
  uint32_t nlen = u_wcslen(uc, reject);
  uint32_t len = u_wcslen(uc, s);

  for (uint32_t i = 0; i < len; i++) {
    uint16_t c = u_rd16(uc, s + i * 2u);
    for (uint32_t j = 0; j < nlen; j++) {
      if (u_rd16(uc, reject + j * 2u) == c)
        return i;
    }
  }
  return len;
}
