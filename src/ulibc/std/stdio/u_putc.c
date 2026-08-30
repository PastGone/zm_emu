#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_putc.c
 * @brief u_putc / u_putchar —— 单字符写出
 */

int u_putc(uc_engine *uc, uint32_t fp, int c) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  return f ? putc(c, f) : EOF;
}

int u_putchar(uc_engine *uc, int c) {
  (void)uc;
  return putchar(c);
}
