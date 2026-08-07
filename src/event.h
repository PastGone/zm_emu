#ifndef EVENT_H
#define EVENT_H

#include "./emu.h"
#include <stdbool.h>

// -------------------- applet 事件派发 --------------------

/**
 * @brief 调用 applet 事件 handler sub_A30(instance, event_type, x, y)
 *
 * 设好 R0..R3 / LR 后由调用者（handle_trap 返回后的模拟器）继续执行。
 * 不调用 uc_emu_start，依赖当前正在运行的模拟器上下文。
 *
 * @param evt 事件类型（9=penDown, 10=penUp 等）
 * @param x   触摸 x 坐标
 * @param y   触摸 y 坐标
 */
void dispatch_applet_event(uint32_t evt, uint32_t x, uint32_t y);

/**
 * @brief 把一次点击转发为 applet 触摸事件（penDown + 排队 penUp）
 *
 * applet 的 tap 检测需要成对的 case9(pen down)+case10(pen up)：
 *   case9 把按下点记录到 INSTANCE 内；
 *   case10 比较按下点与抬起点是否落在同一按钮，是则触发按钮动作。
 *
 * 本函数只派发 case 9，case 10 排队，由下一轮事件循环派发。
 */
void on_touch_click(uint32_t x, uint32_t y);

/**
 * @brief 检查并派发待处理的触摸事件（case 10 penUp）
 * @return true  有待处理事件，已设置寄存器
 */
bool zm_event_dispatch_pending(void);

/**
 * @brief 事件循环单步：决定"继续执行 applet"还是"结束模拟"
 *
 * 统一了三条路径：
 *   1. 有排队中的 penUp → 立即派发；
 *   2. 无头模式（--headless）→ 按 --clicks 数量注入合成点击，
 *      用完后返回 false 结束；
 *   3. 有窗口模式 → 走 SDL 事件循环，等待真实点击 / 关窗 / 超时。
 *
 * 同时统计 g_event_rounds，并在超过 g_max_events 时收敛结束，
 * 保证批量测试不会永远跑下去。
 *
 * @return true  应让模拟器继续执行 applet 的 handler
 * @return false 应结束模拟（uc_emu_stop）
 */
bool zm_event_loop_step(void);

/* 无头模式下待注入的合成点击次数（由 main.c 的 --clicks 设置） */
extern uint32_t g_auto_clicks;

#endif
