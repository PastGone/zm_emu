#ifndef _ZM_EVENT_CODE_H_
#define _ZM_EVENT_CODE_H_

/* ZM 框架标准事件码
 * 来源：zmsys001.dll.lst 中 charge 模块事件处理器 (sub_1AC4)
 * 调试字符串直接标注：ZMAEE_EV_STOP / SUSPEND / RESUME / PEN_UP / USER
 */

enum ZMAEE_EVENT {
  ZMAEE_EV_CREATE = 0x00,   // 创建 (CREATE) - 推断，entry 返回 0 不处理
  ZMAEE_EV_STOP = 0x01,     // 销毁/停止 (字符串: "ZMAEE_EV_STOP 111")
  ZMAEE_EV_SUSPEND = 0x02,  // 挂起 (字符串: "ZMAEE_EV_SUSPEND")
  ZMAEE_EV_RESUME = 0x03,   // 恢复 (字符串: "ZMAEE_EV_RESUME")
  ZMAEE_EV_REPAINT = 0x04,  // 重绘 (对应 sub_1980 + "zmaeecharge_repaint")
  ZMAEE_EV_KEY_BASE = 0x05, // 按键事件起始 (5~8 为按键类)
  ZMAEE_EV_PEN_DOWN = 0x09, // 触摸按下 (与 PEN_UP 同分支)
  ZMAEE_EV_PEN_UP = 0x0A,   // 触摸抬起 (字符串: "ZMAEE_EV_PEN_UP")
  ZMAEE_EV_PEN_MOVE = 0x0B, // 触摸移动 (与 PEN_UP 同分支)

  // 这下面的更像是applet自定义所以也是存在疑问的,反正有可能是自定义时件的回调或者电时器回调什么的我个人是更偏向于自定义时件的回调
  ZMAEE_EV_USER = 0x7000,  // 用户自定义事件 (字符串:
                           // "ZMAEE_EV_USER"说是用户这里可能是 applet,存疑
  ZMAEE_EV_TIMER = 0x7002, // 这里是猜测的，也存在疑问，定时器事件 (框架 API:
                           // AEE_IShell_SetTimer 产生)
};

#endif /* _ZMAEE_EVENTS_H_ */