#include "../../include/u_time.h"

#include <time.h>

#include "../../include/u_mem.h"

/**
 * @file u_time.c
 * @brief u_time —— 取当前秒级时间戳
 */

int64_t u_time(uc_engine *uc, uint32_t timer_ptr) {
  int64_t t = (int64_t)time(NULL);
  if (uc && timer_ptr)
    u_wr32(uc, timer_ptr, (uint32_t)t);
  return t;
}
