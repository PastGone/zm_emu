#ifndef U_TIME_EXT_H
#define U_TIME_EXT_H
/**
 * @file u_time_ext.h
 * @brief 时间**非标准**扩展
 *
 * u_tick_ms 对应 zmaee 的 ZMAEE_IShell_GetTickCount / SDL_GetTicks 语义：
 * 单调毫秒计时（不受系统时间调整影响），供动画、定时器、超时判定使用。
 * 标准 C 的 clock() 测的是 CPU 时间，语义不同，故单列。
 */
#include <stdint.h>

/** 单调毫秒计时（系统启动/epoch 起，不受 settime 影响） */
uint32_t u_tick_ms(void);

#endif /* U_TIME_EXT_H */
