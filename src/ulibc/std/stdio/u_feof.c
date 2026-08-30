#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_feof.c
 * @brief u_feof / u_fflush —— 流状态查询与刷缓冲
 */

int u_feof(uc_engine *uc, uint32_t fp) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  return f ? feof(f) : 1;
}

int u_fflush(uc_engine *uc, uint32_t fp) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  if (!f)
    return EOF;
  return fflush(f);
}
