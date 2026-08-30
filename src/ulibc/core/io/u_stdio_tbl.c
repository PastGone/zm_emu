#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_stdio_tbl.c
 * @brief 客户机 FILE* 句柄表（非公开，供所有 u_f* 函数共享）
 */

FILE *u_stdio_g_files[U_MAX_FILES];

void u_stdio_boot(void) {
  static int booted = 0;
  if (booted)
    return;
  u_stdio_g_files[U_STDIN] = stdin;
  u_stdio_g_files[U_STDOUT] = stdout;
  u_stdio_g_files[U_STDERR] = stderr;
  booted = 1;
}

FILE *u_stdio_slot(uint32_t guest_fp) {
  u_stdio_boot();
  if (guest_fp == 0 || guest_fp >= U_MAX_FILES)
    return NULL;
  return u_stdio_g_files[guest_fp];
}

int u_stdio_alloc(FILE *f) {
  u_stdio_boot();
  if (!f)
    return 0;
  for (uint32_t i = 4; i < U_MAX_FILES; i++) {
    if (u_stdio_g_files[i] == NULL) {
      u_stdio_g_files[i] = f;
      return (int)i;
    }
  }
  fclose(f);
  return 0;
}
