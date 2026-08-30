#include "../../include/u_str_ext.h"

#include "../../include/u_heap.h"
#include "../../include/u_mem.h"

/**
 * @file u_strndup.c
 * @brief u_strndup —— 从客户机堆复制最多 n 字符（依赖 u_malloc）
 *
 * POSIX 而非 C 标准。
 */

uint32_t u_strndup(uc_engine *uc, uint32_t s, uint32_t n) {
  if (!uc || s == 0)
    return 0;
  uint32_t len = u_strnlen(uc, s, n);
  uint32_t p = u_malloc(uc, len + 1);
  if (!p)
    return 0;
  if (len)
    u_memcpy(uc, p, s, len);
  u_wr8(uc, p + len, 0);
  return p;
}
