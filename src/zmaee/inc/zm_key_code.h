#ifndef ZM_KEY_CODE_H
#define ZM_KEY_CODE_H

/**
 * @file zm_key_code.h
 * @brief ZM 按键码定义（供 applet 使用）
 * 这些数值作为事件码 5~8 的附带参数传递给 applet 入口函数。
 *
 * @note 这个地方还没有真实实现,枚举的这几个名称来源于
 * java游戏模拟器和真实设备上的按键布局
 */

enum ZMAEE_KEYCODE {
  // 数字键
  KEYCODE_1,     /**< 1 键 */
  KEYCODE_2,     /**< 2 键 */
  KEYCODE_3,     /**< 3 键 */
  KEYCODE_4,     /**< 4 键 */
  KEYCODE_5,     /**< 5 键 */
  KEYCODE_6,     /**< 6 键 */
  KEYCODE_7,     /**< 7 键 */
  KEYCODE_8,     /**< 8 键 */
  KEYCODE_9,     /**< 9 键 */
  KEYCODE_STAR,  /**< * 键 */
  KEYCODE_0,     /**< 0 键 */
  KEYCODE_POUND, /**< # 键 */
  // 功能键
  KEYCODE_SOFT_RIGHT, /**< 右软键（通常用于确认/前进） */
  KEYCODE_SOFT_LEFT,  /**< 左软键（通常用于后退/上一页） */
  KEYCODE_CALL,       /**打电话 */
  KEYCODE_DECALL,     /**挂断电话 */
  // 方向键
  KEYCODE_DPAD_UP,    /**< 上方向键 键 */
  KEYCODE_DPAD_DOWN,  /**< 下方向键 键 */
  KEYCODE_DPAD_LEFT,  /**< 左方向键 键 */
  KEYCODE_DPAD_RIGHT, /**< 右方向键 键 */

  // 中心键以及特殊的返回键
  KEYCODE_CENTER, /**< 中心方向键 键,也是选择也是进入也是确认 */
  KEYCODE_BACK,   /**< 返回键  */

};

#endif /* ZM_KEY_CODE_H_ */