#include "../../include/u_wcs.h"

#include <limits.h>
#include <stdlib.h>

#include "../../include/u_mem.h"

/**
 * @file u_wcstol.c
 * @brief u_wcstol / u_wcstoul —— 宽串转整数
 *
 * 做法：把宽串按 Latin-1 折叠成宿主窄串，再交给已有的 u_strtol_ex /
 * u_strtoul_ex，最后把"结束位置（字节偏移）"换算回"宽字符偏移"。
 * 折叠是安全的：数字字符与进制前缀都是 ASCII，Latin-1 折叠一一对应。
 */

#define U_WCONV_WIN 512u

/** 宽串 → 宿主窄串；返回宽字符数（不含终止符） */
static uint32_t fold(uc_engine *uc, uint32_t s, char *out, uint32_t cap) {
  uint32_t i = 0;
  while (i < cap) {
    uint16_t c = u_rd16(uc, s + i * 2u);
    if (c == 0)
      break;
    out[i] = (char)(c & 0xFF);
    i++;
  }
  out[i] = '\0';
  return i;
}

long u_wcstol(uc_engine *uc, uint32_t s, uint32_t endptr_addr, int base) {
  char buf[U_WCONV_WIN];
  if (!uc || s == 0)
    return 0;
  fold(uc, s, buf, U_WCONV_WIN - 1);

  char *e = NULL;
  long v = strtol(buf, &e, base);
  if (endptr_addr)
    u_wr32(uc, endptr_addr, s + (uint32_t)(e - buf) * 2u);
  return v;
}

unsigned long u_wcstoul(uc_engine *uc, uint32_t s, uint32_t endptr_addr,
                        int base) {
  char buf[U_WCONV_WIN];
  if (!uc || s == 0)
    return 0;
  fold(uc, s, buf, U_WCONV_WIN - 1);

  char *e = NULL;
  unsigned long v = strtoul(buf, &e, base);
  if (endptr_addr)
    u_wr32(uc, endptr_addr, s + (uint32_t)(e - buf) * 2u);
  return v;
}
