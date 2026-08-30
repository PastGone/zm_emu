#include "../../include/u_time.h"

#include <string.h>
#include <time.h>

#include "../../include/internal/u_time_impl.h"
#include "../../include/u_mem.h"

/**
 * @file u_localtime.c
 * @brief u_localtime —— 按本地时区拆分时间戳（依赖 u_tm_store）
 *
 * 用 localtime_r 可重入版本，避免宿主静态缓冲被模拟器自身覆盖。
 */

uint32_t u_localtime(uc_engine *uc, uint32_t timer_ptr, uint32_t tm_ptr) {
  time_t t = (time_t)(timer_ptr ? (int64_t)u_rd32(uc, timer_ptr)
                                : (int64_t)time(NULL));
  struct tm lt;
  memset(&lt, 0, sizeof(lt));
  localtime_r(&t, &lt);
  u_tm_store(uc, tm_ptr, &lt);
  return tm_ptr;
}
