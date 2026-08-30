#include "../../include/u_stdio.h"

#include <stdlib.h>

#include "../../include/internal/u_stdio_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_fputs.c
 * @brief u_fputs —— 把客户机 C 串写入流（不追加 '\n'）
 */

int u_fputs(uc_engine *uc, uint32_t s, uint32_t fp) {
  FILE *f = u_stdio_slot(fp);
  if (!f)
    return EOF;
  if (s == 0)
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

  int r = fputs(buf, f);
  if (heap)
    free(buf);
  return r;
}
