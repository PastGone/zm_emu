#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_ftell.c
 * @brief u_ftell / u_rewind —— 取得/复位文件位置
 *
 * rewind(fp) 等价于 fseek(fp,0,SEEK_SET) 并清除 EOF 标志，
 * 二者语义紧耦合，合并为一个翻译单元。
 */

int32_t u_ftell(uc_engine *uc, uint32_t fp) {
  (void)uc;
  FILE *f = u_stdio_slot(fp);
  if (!f)
    return -1;
  return (int32_t)ftell(f);
}

void u_rewind(uc_engine *uc, uint32_t fp) {
  FILE *f = u_stdio_slot(fp);
  if (f)
    rewind(f);
  (void)uc;
}
