#include "../../include/u_time.h"

#include <string.h>
#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_gmtime.c
 * @brief u_gmtime —— 按 UTC 拆分时间戳（依赖 u_tm_store）
 *
 * 用 gmtime_r 可重入版本。
 */

uint32_t u_gmtime(uc_engine *uc, uint32_t timer_ptr, uint32_t tm_ptr) {
  time_t t = (time_t)(timer_ptr ? (int64_t)u_rd32(uc, timer_ptr)
                                : (int64_t)time(NULL));
  struct tm gt;
  memset(&gt, 0, sizeof(gt));
  gmtime_r(&t, &gt);
  u_tm_store(uc, tm_ptr, &gt);
  return tm_ptr;
}
