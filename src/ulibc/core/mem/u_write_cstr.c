#include "../../include/u_mem.h"

#include <string.h>

/**
 * @file u_write_cstr.c
 * @brief u_write_cstr —— 宿主 C 串写入客户机内存（含结尾 '\0'）
 */

uint32_t u_write_cstr(uc_engine *uc, uint32_t addr, const char *s) {
  if (!uc || addr == 0 || !s)
    return 0;
  size_t len = strlen(s);
  if (!u_write(uc, addr, s, len + 1))
    return 0;
  return (uint32_t)(len + 1);
}
