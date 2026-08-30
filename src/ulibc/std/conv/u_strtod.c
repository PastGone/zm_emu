#include "../../include/u_stdlib.h"

#include <stdlib.h>

#include "../../include/u_mem.h"

/**
 * @file u_strtod.c
 * @brief u_strtod / u_strtod_ex —— 字符串转 double
 *
 * 只抓取前导空白 + 一个固定窗口（U_STRTOD_WIN），够覆盖 applet 实际用法，
 * 同时避免客户机串损坏（无 '\0'）时读爆宿主缓冲。
 */

#define U_STRTOD_WIN 512u

double u_strtod_ex(uc_engine *uc, uint32_t s, uint32_t *end_out) {
  char buf[U_STRTOD_WIN];
  char *end = NULL;
  double v = 0.0;

  u_errno = 0;
  if (end_out)
    *end_out = s;
  if (!uc || s == 0)
    return 0.0;

  u_read_cstr(uc, s, buf, sizeof(buf));
  v = strtod(buf, &end);
  if (end && end_out)
    *end_out = s + (uint32_t)(end - buf);
  return v;
}

double u_strtod(uc_engine *uc, uint32_t s, uint32_t endptr_addr) {
  uint32_t end = s;
  double v = u_strtod_ex(uc, s, &end);
  if (endptr_addr)
    u_wr32(uc, endptr_addr, end);
  return v;
}
