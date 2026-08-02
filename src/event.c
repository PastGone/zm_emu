#include "./event.h"
#include "./log/log.h"
#include "unicorn/arm.h"
#include <stdbool.h>

/* 待处理的触摸事件队列（仅支持 1 个，case 10 penUp 跟在 case 9 之后） */
static struct {
  bool has_pending;
  uint32_t evt;
  uint32_t x, y;
} g_pending_touch = {false, 0, 0, 0};

void dispatch_applet_event(uint32_t evt, uint32_t x, uint32_t y) {
  if (!g_instance || !g_handler)
    return;

  uint32_t lr = TR_enter_event_loop;
  uc_reg_write(g_uc, UC_ARM_REG_LR, &lr);

  uc_reg_write(g_uc, UC_ARM_REG_R0, &g_instance);
  uc_reg_write(g_uc, UC_ARM_REG_R1, &evt);
  uc_reg_write(g_uc, UC_ARM_REG_R2, &x);
  /* 00000405.app：init 事件（evt==0）需 r3=上下文指针（INIT_CTX 零缓冲），
   * 否则 sub_8433C 解引用 r3+0x100 触发 MEM unmapped。其它事件 r3=y。 */
  uint32_t r3 = (evt == 0) ? INIT_CTX : y;
  uc_reg_write(g_uc, UC_ARM_REG_R3, &r3);

  uc_reg_write(g_uc, UC_ARM_REG_PC, &g_handler);
}

void on_touch_click(uint32_t x, uint32_t y) {
  log_info("触摸事件: (%u, %u) -> handler=0x%X instance=0x%X", x, y, g_handler,
           g_instance);
  /* 派发 case 9 (penDown)：记录按下点到 INSTANCE[25..26] */
  dispatch_applet_event(9, x, y);
  /* 排队 case 10 (penUp)：等 handler 执行完 case 9 后，下一轮事件循环再派发 */
  g_pending_touch.has_pending = true;
  g_pending_touch.evt = 10;
  g_pending_touch.x = x;
  g_pending_touch.y = y;
}

bool zm_event_dispatch_pending(void) {
  if (!g_pending_touch.has_pending)
    return false;
  dispatch_applet_event(g_pending_touch.evt, g_pending_touch.x,
                        g_pending_touch.y);
  g_pending_touch.has_pending = false;
  return true;
}