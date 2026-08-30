#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_getc.c
 * @brief u_getc / u_getchar —— 单字符读取
 *
 * 不触碰客户机内存，直接转发宿主。
 */

int u_getc(uc_engine *uc, uint32_t fp) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  return f ? getc(f) : EOF;
}

int u_getchar(uc_engine *uc) {
  (void)uc;
  return getchar();
}
