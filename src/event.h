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

/**
 * @brief 把一个点击放进"待派发触摸"队列（自旋兜底路径用）
 *
 * 谁调：zm_display_pump_events —— applet 长时间不回事件循环时，宿主的周期泵
 * 把 SDL 队列里的点击收进来（那条路不能直接派发：此刻 guest 正在跑，
 * 直接改 PC 会把它打断在半路）。
 * 谁取：① 正常路径 —— zm_display_event_loop 每轮先取队列（yield 型 applet）；
 *       ② 饥饿路径 —— zm_event_async_poll 经异步跳板派发（自旋型 applet）。
 */
void zm_event_queue_touch(uint32_t x, uint32_t y);

/** @brief 从待派发触摸队列取一个（FIFO）；空则返回 false */
bool zm_event_take_queued_touch(uint32_t *x, uint32_t *y);

/**
 * @brief 自旋型 applet 的触摸异步派发（hook_code 周期性调用）
 *
 * applet 一旦停在"0ms 定时器/自旋"主循环里就再也不回事件循环，触摸会被
 * 无限期饿死（实测 0000048a：点"加入残局"后停在难度列表，32 秒里 5 个点击
 * 一个都没派发）。本函数在 starved（zm_timer_is_starved）成立时，用定时器
 * 同一条指令级跳板把 evt=9 送进 handler，下一拍再补 evt=10（成对）。
 *
 * @param resume_pc 被打断的指令地址（跳板恢复时从这里继续）
 * @param starved   是否已超过饥饿门限（由调用方查 zm_timer_is_starved）
 * @return true 表示已改写 PC/LR（调用方应立即 return，让 Unicorn 去跑 handler）
 */
bool zm_event_async_poll(uc_engine *uc, uint32_t resume_pc, bool starved);

/**
 * @brief applet 调用 IShell.CloseApplet（vtable +0x24）→ 它请求关闭自己
 *
 * 固件侧是 sub_3482C，日志串 "CloseApplet: bRetToIdle = %d"。
 * 我们收到后：先派发 EV_STOP(evt=1) 让 applet 跑完自己的退出回调
 * （00000506 会停声音 + 写 data/farm 存档），收尾完由事件循环分支结束模拟。
 */
void zm_event_request_close(void);

/** @brief 是否已经收到过 CloseApplet（事件循环据此收工） */
bool zm_event_close_requested(void);

#endif