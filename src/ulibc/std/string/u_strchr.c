#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strchr.c
 * @brief u_strchr —— 查找字符首次出现
 *
 * 标准：查找 '\0' 时返回指向终止符的指针。
 */

uint32_t u_strchr(uc_engine *uc, uint32_t s, int c) {
  if (!uc || s == 0)
    return 0;
  uint32_t len = u_strlen(uc, s);
  uint32_t r = u_memchr(uc, s, c, len);
  if (r)
    return r;
  if ((c & 0xFF) == 0)
    return s + len;
  return 0;
}
