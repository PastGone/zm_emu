#include "../../include/u_str.h"

#include "../../include/u_mem.h"

/**
 * @file u_strcpy.c
 * @brief u_strcpy —— 拷贝客户机 C 串（含 '\0'）
 */

uint32_t u_strcpy(uc_engine *uc, uint32_t dst, uint32_t src) {
  if (!uc || dst == 0)
    return dst;
  if (src == 0) {
    u_wr8(uc, dst, 0);
    return dst;
  }
  uint32_t len = u_strlen(uc, src);
  u_memcpy(uc, dst, src, len);
  u_wr8(uc, dst + len, 0);
  return dst;
}
