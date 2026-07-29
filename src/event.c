#include "./event.h"
#include "./log/log.h"

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
  uc_reg_write(uc, UC_ARM_REG_R3, &y);
  uc_emu_start(uc, g_handler, STACK_TOP, 0, 0);
}

void on_touch_click(uint32_t x, uint32_t y) {
  log_info("触摸事件: (%u, %u) -> handler=0x%X instance=0x%X", x, y, g_handler,
           g_instance);
  dispatch_applet_event(9, x, y);
  dispatch_applet_event(10, x, y);
}