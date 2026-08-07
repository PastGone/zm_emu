#ifndef ZM_KEY_CODE_H
#define ZM_KEY_CODE_H

/**
 * @file zm_key_code.h
 * @brief ZM 按键码（keycode）定义（供 applet 使用）
 *
 * keycode 作为按键事件（down/up）的 R2 参数传递给 applet 入口函数。
 *
 * @note 数值采用 MediaTek AEE 风格：数字键 '0'-'9' 即 ASCII 码 0x30-0x39，
 *       '*'=0x2A、'#'=0x23（与真实设备 DTMF/ASCII 布局一致）；功能键
 *       （方向/软键/拨号/挂机/确认/返回）为专用小整数值。
 *       由于 AEE 精确枚举无法在此环境完全确认，功能键数值统一集中在此处，
 *       便于后续依据真实 applet 反汇编校正。
 *
 * @note 事件码约定（用户确认）：5=按键按下(keyDown)，6=按键抬起(keyUp)。
 */

/* ---------- 按键事件码 ---------- */
#define ZM_EV_KEY_DOWN 0x05U /* 按键按下：R2=keycode */
#define ZM_EV_KEY_UP   0x06U /* 按键抬起：R2=keycode */

/* ---------- 数字键（ASCII） ---------- */
#define ZM_KEY_0 0x30U /* '0' */
#define ZM_KEY_1 0x31U /* '1' */
#define ZM_KEY_2 0x32U /* '2' */
#define ZM_KEY_3 0x33U /* '3' */
#define ZM_KEY_4 0x34U /* '4' */
#define ZM_KEY_5 0x35U /* '5' */
#define ZM_KEY_6 0x36U /* '6' */
#define ZM_KEY_7 0x37U /* '7' */
#define ZM_KEY_8 0x38U /* '8' */
#define ZM_KEY_9 0x39U /* '9' */

/* ---------- 符号键 ---------- */
#define ZM_KEY_STAR  0x2AU /* '*' */
#define ZM_KEY_POUND 0x23U /* '#' */

/* ---------- 功能键（专用小值，集中可配置） ---------- */
#define ZM_KEY_UP         0x0DU /* 上（W） */
#define ZM_KEY_DOWN       0x0EU /* 下（S） */
#define ZM_KEY_LEFT       0x0FU /* 左（A） */
#define ZM_KEY_RIGHT      0x10U /* 右（D） */
#define ZM_KEY_SOFT_LEFT  0x11U /* 左确认（Q） */
#define ZM_KEY_SOFT_RIGHT 0x12U /* 右返回（E） */
#define ZM_KEY_CALL       0x13U /* 拨号键（Z） */
#define ZM_KEY_END        0x14U /* 挂机键（C） */

/* 无法识别的按键返回该值 */
#define ZM_KEY_UNKNOWN 0xFFFFU

#endif /* ZM_KEY_CODE_H_ */
