#ifndef EVENT_H
#define EVENT_H

#include "./emu.h"
#include <stdbool.h>

// -------------------- applet 事件派发 --------------------

/**
 * @brief 固件（ZMAEE）派给 applet handler 的**事件码**
 *
 * 调用约定：handler(this=instance, evt, arg1, arg2)，见 dispatch_applet_event
 *   R0 = instance    R1 = evt（下面这些值）    R2/R3 = 参数    **R4 =
 * instance**
 *
 * 其中：
 *   evt=0 INIT     —— R2/R3 传**上下文指针**（INIT_CTX），不是坐标
 *   evt=5/6/7/8 按键 —— R2 = 键码（下面 ZMAEE_APPLET_INTERNAL_KEYCODE 的
 *                      KEYCODE_*，真值由 Android 键码推断，见 docs 的对应表）
 *   evt=9/10/11 触摸 —— R2 = x, R3 = y
 *
 * 【关于 9/10 的命名】实测 Evidence：
 *   · 00000506 的分发表：evt=9/10/11 → 同一个 loc_8478 →
 * obj->vt[0x1C](obj,x,y)； · case 9 把**按下点**记进 INSTANCE[25..26]，case 10
 * 再与测试用例的抬起校验 （sub_824 / sub_8B4），即 **9 = 按下、10 = 抬起**。
 * 旧注释曾把这两个名字写反（TOUCH_UP=9 ✗），这里按实测更正 ✓。
 */
enum APP_CMD : uint32_t {
	APP_CMD_INIT = 0,			/**< 创建/初始化（R2/R3 = 上下文指针 INIT_CTX） */
	APP_CMD_DESTROY = 1,		/**< 销毁：让 applet 收尾（停声音 + 存盘） */
	APP_CMD_PAUSE = 2,			/**< 切后台 */
	APP_CMD_RESUME = 3,			/**< 切回前台 */
	APP_CMD_REPAINT = 4,		/**< 要求重绘 */
	APP_CMD_KEY_DOWN = 5,		/**< 按键按下（R2 = KEYCODE_*） */
	APP_CMD_KEY_UP = 6,			/**< 按键抬起（R2 = KEYCODE_*） */
	APP_CMD_KEY_LONG_PRESS = 7, /**< 长按（按住超过 ~600ms，只发一次） */
	APP_CMD_KEY_MULTIPLE = 8,	/**< 连发（按住期间的自动重复） */
	APP_CMD_TOUCH_DOWN = 9,		/**< 笔按下（R2=x R3=y） */
	APP_CMD_TOUCH_UP = 10,		/**< 笔抬起（R2=x R3=y） */
	APP_CMD_TOUCH_MOVE = 11,	/**< 笔移动（拖动） */
};

//  ZMAEE 内部使用的键码，由安卓推断而出
enum ZMAEE_APPLET_INTERNAL_KEYCODE : uint32_t {
	// 数字键：Android 7~16 -> 0~9
	KEYCODE_0 = 0, // Android KEYCODE_0 (7)
	KEYCODE_1 = 1, // Android KEYCODE_1 (8)
	KEYCODE_2 = 2, // Android KEYCODE_2 (9)
	KEYCODE_3 = 3, // Android KEYCODE_3 (10)
	KEYCODE_4 = 4, // Android KEYCODE_4 (11)
	KEYCODE_5 = 5, // Android KEYCODE_5 (12)
	KEYCODE_6 = 6, // Android KEYCODE_6 (13)
	KEYCODE_7 = 7, // Android KEYCODE_7 (14)
	KEYCODE_8 = 8, // Android KEYCODE_8 (15)
	KEYCODE_9 = 9, // Android KEYCODE_9 (16)

	// 功能键
	KEYCODE_SOFT_LEFT = 10,	 // Android KEYCODE_SOFT_LEFT (1) / KEYCODE_MENU (82)
	KEYCODE_SOFT_RIGHT = 11, // Android KEYCODE_SOFT_RIGHT (2) / KEYCODE_BACK (4)
	KEYCODE_BACK = 11,		 // 别名：Android KEYCODE_BACK (4) 也映射到 11
	KEYCODE_CALL = 17,		 // Android KEYCODE_CALL (5)

	// 方向键
	KEYCODE_DPAD_UP = 13,	 // Android KEYCODE_DPAD_UP (19)
	KEYCODE_DPAD_DOWN = 14,	 // Android KEYCODE_DPAD_DOWN (20)
	KEYCODE_DPAD_LEFT = 15,	 // Android KEYCODE_DPAD_LEFT (21)
	KEYCODE_DPAD_RIGHT = 16, // Android KEYCODE_DPAD_RIGHT (22)

	// 中心键、星号、井号
	KEYCODE_CENTER = 25, // Android KEYCODE_DPAD_CENTER (23)
	KEYCODE_STAR = 20,	 // Android KEYCODE_STAR (17)
	KEYCODE_POUND = 21,	 // Android KEYCODE_POUND (18)

	// 其他
	KEYCODE_HOME = 30,	 // Android KEYCODE_HOME (3)
	KEYCODE_SEARCH = 31, // Android KEYCODE_SEARCH (84)
};

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
 * @brief 把 SDL 鼠标拖动转发为 applet 触摸移动事件（APP_CMD_TOUCH_MOVE, evt=11）
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
 * 我们收到后：先派发 APP_CMD_DESTROY(evt=1) 让 applet 跑完自己的退出回调
 * （00000506 会停声音 + 写 data/farm 存档），收尾完由事件循环分支结束模拟。
 */
void zm_event_request_close(void);

/** @brief 是否已经收到过 CloseApplet（事件循环据此收工） */
bool zm_event_close_requested(void);

/* -------------------- 带语义的事件派发封装（APP_CMD 的实际用法）
 * --------------------
 *
 * 以前各处散的是魔法数（9/10/11、1、0 ✗），可读性差也容易改错；下面这些是面向
 * 语义的封装：内部只调到 dispatch_applet_event(cmd, arg1, arg2)。
 */

/**
 * @brief 通用派发：按 APP_CMD 的语义把事件送进 applet handler
 *
 * INIT(0) 会自动把 arg2 换成 INIT_CTX（上下文指针）—— 与固件一致：
 * 00000405 的 EVT_INIT 需要 r3=上下文指针，否则 sub_8433C 解引用 r3+0x100 会
 * 触发 MEM unmapped。
 */
void zm_event_send(uint32_t cmd, uint32_t arg1, uint32_t arg2);

/**
 * @brief 按键：down≠0 发 KEY_DOWN(5)，down==0 发 KEY_UP(6)
 *
 * keycode 取本文件的 ZMAEE_APPLET_INTERNAL_KEYCODE（由 Android 键码推断的真值，
 * 对应表见 docs/sdl 和内部键码的对应.md；旧的顺序枚举头已作废 ✗）。同时登记"按住"状态，供
 * zm_event_key_tick 升级成长按(7) / 连发(8)。焦点丢失时应把按住的键全部松开。
 */
void zm_event_key(uint32_t keycode, int down);

/**
 * @brief 每个事件循环周期调一次：把按住的键升级为 LONG_PRESS(7) / 连发(8)
 *
 * 时机参考真机：按住 ~600ms 后补一次**长按**，之后每 ~150ms 补一次**连发**。
 * @return true 表示本轮派发了事件（调用方应让模拟器去执行 handler）
 */
bool zm_event_key_tick(uint32_t now_ms);

/** @brief 把"按住中"的键**全部松开**（切后台/失焦时用，避免按键卡住） */
void zm_event_key_release_all(void);

/**
 * @brief SDL 按键 → ZMAEE 键码映射
 * @param sdl_sym SDL_Keycode（SDLK_*）
 * @param out_code 命中时写入 KEYCODE_*
 * @return true 表示该键有意义并已映射（字母按手机键盘折到数字键 2~9）
 */
bool zm_event_key_from_sdl(int sdl_sym, uint32_t *out_code);

/** @brief 切后台 → PAUSE(2)（并松开按住的键） */
void zm_event_pause(void);

/**
 * @brief 切回前台 → RESUME(3)
 *
 * 【不在这里补 REPAINT】dispatch 是"设寄存器"，同一时刻只能挂一个事件 ✗，
 * 连发两个后者会覆盖前者（applet 只收得到最后一个）。重绘交给 applet 自己的
 * Resume 分支；确实要强制重绘时单独调 zm_event_repaint。
 */
void zm_event_resume(void);

/** @brief 要求重绘 → REPAINT(4) */
void zm_event_repaint(void);

/**
 * @brief **通用**事件入队（自旋兜底路径）：把 (evt, arg1, arg2) 排进队列
 *
 * 谁取：① 正常 yield 的 applet —— zm_display_event_loop 每轮先取队列；
 *       ② 已自旋到不回事件循环的 applet —— 异步跳板 zm_event_async_poll。
 * zm_event_queue_touch 是它的触摸特例（evt=按下）。
 */
void zm_event_queue(uint32_t evt, uint32_t x, uint32_t y);

/** @brief 取一个排队的**任意**事件（evt/x/y 任一可为 NULL）；空则返回 false */
bool zm_event_take_queued_evt(uint32_t *evt, uint32_t *x, uint32_t *y);

#endif