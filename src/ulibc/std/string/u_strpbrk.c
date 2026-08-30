#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strpbrk.c
 * @brief u_strpbrk —— 返回首个出现在 accept 中字符的地址（依赖 u_strcspn）
 */

uint32_t u_strpbrk(uc_engine *uc, uint32_t s, uint32_t accept) {
  if (!uc || s == 0)
    return 0;
  uint32_t off = u_strcspn(uc, s, accept);
  uint32_t len = u_strlen(uc, s);
  if (off >= len)
    return 0;
  return s + off;
}
