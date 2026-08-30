#include "../../include/u_fmt.h"

#include "../../include/u_stdio.h"

/**
 * @file u_fprintf.c
 * @brief u_fprintf / u_vfprintf_g —— 格式化输出到**客户机** FILE 句柄
 */

int u_vfprintf_g(uc_engine *uc, uint32_t guest_fp, uint32_t fmt, u_va *va) {
  FILE *fp = u_stdio_host_file(guest_fp);
  if (!fp)
    fp = stdout;
  return u_vfprintf(uc, fp, fmt, va);
}

int u_fprintf(uc_engine *uc, uint32_t guest_fp, uint32_t fmt, u_va *va) {
  return u_vfprintf_g(uc, guest_fp, fmt, va);
}
