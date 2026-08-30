#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_ferror.c
 * @brief u_ferror / u_clearerr —— 错误标志查询与清除
 *
 * 二者操作同一个标志位，合并为一个翻译单元。
 */

int u_ferror(uc_engine *uc, uint32_t fp) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  return f ? ferror(f) : 1;
}

void u_clearerr(uc_engine *uc, uint32_t fp) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  if (f)
    clearerr(f);
}
