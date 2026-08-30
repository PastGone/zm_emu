#include "../../include/u_time.h"

#include <time.h>

#include "../../include/u_mem.h"

/**
 * @file u_ctime.c
 * @brief u_ctime —— 时间戳转固定格式串
 */

uint32_t u_ctime(uc_engine *uc, uint32_t timer_ptr, uint32_t buf) {
  time_t t = (time_t)(timer_ptr ? (int64_t)u_rd32(uc, timer_ptr)
                                : (int64_t)time(NULL));
  char tmp[64];
  ctime_r(&t, tmp);
  return u_write_cstr(uc, buf, tmp) ? buf : 0;
}
