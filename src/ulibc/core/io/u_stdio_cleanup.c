#include "../../include/u_stdio.h"

#include "../../include/internal/u_stdio_impl.h"

/**
 * @file u_stdio_cleanup.c
 * @brief u_stdio_cleanup —— 关闭所有非标准流（模拟器复位时调用）
 */

void u_stdio_cleanup(void) {
  u_stdio_boot();
  for (uint32_t i = 4; i < U_MAX_FILES; i++) {
    if (u_stdio_g_files[i]) {
      fclose(u_stdio_g_files[i]);
      u_stdio_g_files[i] = NULL;
    }
  }
}
