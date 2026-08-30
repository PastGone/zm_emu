#include "../../include/u_time_ext.h"

#include <stddef.h>
#include <sys/time.h>

/**
 * @file u_tick_ms.c
 * @brief u_tick_ms —— 单调毫秒计时（非标准）
 *
 * 对应 zmaee 的 ZMAEE_IShell_GetTickCount / SDL_GetTicks 语义。
 * 标准 C 的 clock() 测的是 CPU 时间，语义不同，故单列。
 */

uint32_t u_tick_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t)(tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL);
}
