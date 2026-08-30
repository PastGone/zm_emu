#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_fseek.c
 * @brief u_fseek —— 定位文件（SEEK_* 在 ARM newlib 与 Linux 上同为 0/1/2）
 */

int u_fseek(uc_engine *uc, uint32_t fp, int32_t off, int whence) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  if (!f)
    return -1;
  return fseek(f, (long)off, whence);
}
