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
 * @brief 把 SDL 鼠标点击转发为 applet 触摸事件
 *
 * applet 的 tap 检测需要成对的 case9(pen down)+case10(pen up)：
 *   case9(sub_824) 把按下点记录到 INSTANCE[25..26]；
 *   case10(sub_8B4) 比较按下点与抬起点是否落在同一按钮，是则
 *   从 .zmr 读出该按钮的 MP3 资源并 ap.play 播放。
 *
 * 本函数只派发 case 9，case 10 排队到 g_pending_touch，
 * 由 zm_event_dispatch_pending() 在下一轮事件循环中派发。
 */
void on_touch_click(uint32_t x, uint32_t y);

/**
 * @brief 把 SDL 鼠标拖动转发为 applet 触摸移动事件（evt=11 PEN_MOVE）
 *
 * 事件码语义（实测 00000506 sub_80FC 的分发表）：
 *   evt=9/10/11 都落到 loc_8478 → obj->vt[0x1C](obj, x, y)
 * 即按下/抬起/移动走同一个处理函数，applet 自己按坐标变化判断拖动。
 */
void on_touch_move(uint32_t x, uint32_t y);

/**
 * @brief 检查并派发待处理的触摸事件（case 10 penUp）
 * @return true  有待处理事件，已设置寄存器，调用者应让模拟器执行 handler
 * @return false 无待处理事件
 */
bool zm_event_dispatch_pending(void);

#endif