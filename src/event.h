#ifndef EVENT_H
#define EVENT_H

#include "./emu.h"

// -------------------- applet 事件派发 --------------------

/**
 * @brief 调用 applet 事件 handler sub_A30(instance, event_type, x, y)
 *
 * 与 init 相同的陷入模式：设好 R0..R3 / SP / LR 后 uc_emu_start，
 * handler 执行到 bx lr（LR=STACK_TOP）时 PC 命中 end 地址自动停止。
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
 * 故一次鼠标点击需连续派发 case9 与 case10（同坐标）。
 */
void on_touch_click(uint32_t x, uint32_t y);

#endif