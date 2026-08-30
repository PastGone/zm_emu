#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strncpy.c
 * @brief u_strncpy —— 限定长度的拷贝
 *
 * 与标准一致：源串短于 n 时补 '\0' 填满 n；拷满 n 则不以 '\0' 结尾。
 */

uint32_t u_strncpy(uc_engine *uc, uint32_t dst, uint32_t src, uint32_t n) {
  if (!uc || dst == 0 || n == 0)
    return dst;
  if (src == 0) {
    u_memset(uc, dst, 0, n);
    return dst;
  }
  uint32_t len = u_strnlen(uc, src, n);
  u_memcpy(uc, dst, src, len);
  if (len < n)
    u_memset(uc, dst + len, 0, n - len);
  return dst;
}
