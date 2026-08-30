#include "../../include/u_time.h"

#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_asctime.c
 * @brief u_asctime —— struct tm 转固定格式串（依赖 u_tm_load）
 */

uint32_t u_asctime(uc_engine *uc, uint32_t tm_ptr, uint32_t buf) {
  struct tm t;
  char tmp[64];
  u_tm_load(uc, tm_ptr, &t);
  asctime_r(&t, tmp);
  return u_write_cstr(uc, buf, tmp) ? buf : 0;
}
