#include "../../include/u_time.h"

#include <time.h>

#include "../../include/internal/u_time_impl.h"

/**
 * @file u_mktime.c
 * @brief u_mktime —— 客户机 struct tm 转回时间戳（依赖 u_tm_load）
 */

int64_t u_mktime(uc_engine *uc, uint32_t tm_ptr) {
  struct tm t;
  u_tm_load(uc, tm_ptr, &t);
  return (int64_t)mktime(&t);
}
