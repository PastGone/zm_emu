#include "./event.h"
#include "./log/log.h"
#include "./zmaee/core/zm_addrs.h" /* INIT_CTX */

void dispatch_applet_event(uint32_t evt, uint32_t x, uint32_t y) {
  if (!g_instance || !g_handler)
    return;
  uint32_t sp = STACK_TOP;
  uint32_t lr = STACK_TOP;
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uc_reg_write(uc, UC_ARM_REG_LR, &lr);
  uc_reg_write(uc, UC_ARM_REG_R0, &g_instance);
  uc_reg_write(uc, UC_ARM_REG_R1, &evt);
  uc_reg_write(uc, UC_ARM_REG_R2, &x);
  /* 00000405.app：init 事件（evt==0）需 r3=上下文指针（INIT_CTX 零缓冲），
   * 否则 sub_8433C 解引用 r3+0x100 触发 MEM unmapped。其它事件 r3=y。 */
  uint32_t r3 = (evt == 0) ? INIT_CTX : y;
  uc_reg_write(uc, UC_ARM_REG_R3, &r3);
  uc_emu_start(uc, g_handler, STACK_TOP, 0, 0);
}

void on_touch_click(uint32_t x, uint32_t y) {
  log_info("触摸事件: (%u, %u) -> handler=0x%X instance=0x%X", x, y, g_handler,
           g_instance);
  dispatch_applet_event(9, x, y);
  dispatch_applet_event(10, x, y);
}
