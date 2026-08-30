#include "../../include/u_stdio.h"

#include <stdlib.h>

#include "../../include/u_mem.h"

/**
 * @file u_puts.c
 * @brief u_puts —— 把客户机 C 串写到 stdout 并追加 '\n'
 *
 * 标准 puts 固定写 stdout、不带 fp 参数（要写任意流请用 u_fputs）。
 */

int u_puts(uc_engine *uc, uint32_t s) {
  if (!uc || s == 0)
    return EOF;

  char stack_buf[512];
  uint32_t len = u_strlen(uc, s);
  char *buf = stack_buf;
  int heap = 0;
  if (len + 1u > sizeof(stack_buf)) {
    buf = (char *)malloc((size_t)len + 1u);
    if (!buf)
      return EOF;
    heap = 1;
  }
  u_read(uc, s, buf, len);
  buf[len] = '\0';

  int r = fputs(buf, stdout);
  if (r >= 0)
    r = fputc('\n', stdout);
  if (heap)
    free(buf);
  return r;
}
